/****************************************************************************
 *   Copyright (C) 2006-2013 by Jason Ansel, Kapil Arya, and Gene Cooperman *
 *   jansel@csail.mit.edu, kapil@ccs.neu.edu, gene@ccs.neu.edu              *
 *                                                                          *
 *  This file is part of DMTCP.                                             *
 *                                                                          *
 *  DMTCP is free software: you can redistribute it and/or                  *
 *  modify it under the terms of the GNU Lesser General Public License as   *
 *  published by the Free Software Foundation, either version 3 of the      *
 *  License, or (at your option) any later version.                         *
 *                                                                          *
 *  DMTCP is distributed in the hope that it will be useful,                *
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of          *
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the           *
 *  GNU Lesser General Public License for more details.                     *
 *                                                                          *
 *  You should have received a copy of the GNU Lesser General Public        *
 *  License along with DMTCP:dmtcp/src.  If not, see                        *
 *  <http://www.gnu.org/licenses/>.                                         *
 ****************************************************************************/

#include <sys/syscall.h>
#include "../jalib/jassert.h"
#include "../jalib/jconvert.h"
#include "../jalib/jfilesystem.h"
#include "constants.h"
#include "coordinatorapi.h"
#include "dmtcpworker.h"
#include "pluginmanager.h"
#include "processinfo.h"
#include "shareddata.h"
#include "tokenize.h"
#include "syscallwrappers.h"
#include "syslogwrappers.h"
#include "threadlist.h"
#include "threadsync.h"
#include "uniquepid.h"
#include "util.h"

#define INITIAL_ARGV_MAX 128
#define MAX_EXTRA_ARGS 32
#define MAX_EXTRA_ENV 32

using namespace dmtcp;

#ifdef LOGGING
const static bool dbg = true;
#else // ifdef LOGGING
const static bool dbg = false;
#endif // ifdef LOGGING

static bool pthread_atfork_enabled = false;
static uint64_t child_time;
static int childCoordinatorSocket = -1;

extern "C" int
dmtcp_execlpe(const char *filename,
              const char *arg,
              va_list args,
              char *const envp[]);

extern "C" int
dmtcp_execvpe(const char *filename, char *const argv[], char *const envp[]);

// Allow plugins to call fork/exec/system to perform specific tasks during
// preCKpt/postCkpt/PostRestart etc. event.
static bool
isPerformingCkptRestart()
{
  if (WorkerState::currentState() != WorkerState::UNKNOWN &&
      WorkerState::currentState() != WorkerState::RUNNING) {
    return true;
  }
  return false;
}

LIB_PRIVATE void
pthread_atfork_prepare()
{
  /* FIXME: The user process might register a fork prepare handler with
   * pthread_atfork. That handler will be called _after_ we have acquired the
   * wrapper-exec lock in exclusive mode. This can lead to a deadlock situation
   * if the user process decides to do some operations that require calling a
   * dmtcp wrapper that require the wrapper-exec lock.
   *
   * Also, the preparation that dmtcp needs to do for fork() should be done
   * right before the child process is created, i.e. after all the user
   * handlers have been invoked. Fortunately, pthread_atfork prepare handlers
   * are called in reverse order or registration (as opposed to parent and
   * child handlers which are called in the order of registration), thus our
   * prepare handle will be called at the very last.
   *
   * FIXME: PID-conflict detection poses yet another serious problem. On a
   * pid-conflict, _real_fork() will be called more than once, resulting in
   * multiple calls of user-defined prepare handlers. This is undesired and can
   * cause several issues. One solution to this problem is to call the fork
   * system call directly whenever a tid-conflict is detected, however, it
   * might have some other side-effects.  Another possible solution would be to
   * have pid-virtualization plugin, which always assigns virtual pids, to the
   * newly created processes, and thus avoiding the pid-conflict totally.
   */
}

LIB_PRIVATE void
pthread_atfork_parent()
{}

LIB_PRIVATE void
pthread_atfork_child()
{
  if (!pthread_atfork_enabled) {
    return;
  }
  pthread_atfork_enabled = false;

  uint64_t host = UniquePid::ThisProcess().hostid();
  UniquePid parent = UniquePid::ThisProcess();
  UniquePid child = UniquePid(host, getpid(), child_time);
  string child_name = jalib::Filesystem::GetProgramName() + "_(forked)";
  ThreadSync::resetLocks();

  UniquePid::resetOnFork(child);
  Util::initializeLogFile(dmtcp_get_tmpdir(), child_name.c_str(), NULL);

  ProcessInfo::instance().resetOnFork();

  JTRACE("fork()ed [CHILD]") (child) (parent);
  CoordinatorAPI::resetOnFork(childCoordinatorSocket);
  DmtcpWorker::resetOnFork();
}

extern "C" pid_t
fork()
{
  if (isPerformingCkptRestart()) {
#ifndef __aarch64__
    return _real_syscall(SYS_fork);
#else
    return _real_fork();
#endif
  }

  /* Acquire the wrapperExeution lock to prevent checkpoint to happen while
   * processing this system call.
   */
  WRAPPER_EXECUTION_GET_EXCL_LOCK();
  PluginManager::eventHook(DMTCP_EVENT_ATFORK_PREPARE, NULL);

  /* Little bit cheating here: child_time should be same for both parent and
   * child, thus we compute it before forking the child. */
  child_time = time(NULL);
  uint64_t host = UniquePid::ThisProcess().hostid();
  UniquePid parent = UniquePid::ThisProcess();
  string child_name = jalib::Filesystem::GetProgramName() + "_(forked)";

  childCoordinatorSocket =
    CoordinatorAPI::createNewConnectionBeforeFork(child_name);

  // Enable the pthread_atfork child call
  pthread_atfork_enabled = true;
  pid_t childPid = _real_fork();

  if (childPid == -1) {
  } else if (childPid == 0) { /* child process */
    // ThreadList::resetOnFork calls pthread_create which in turn calls
    // malloc/calloc, etc. This can result in a deadlock if the parent process
    // was holding malloc-lock while forking and might reset the lock only
    // during atfork_child handler. Because our own atfork_child handler is
    // called at the very beginning, the parent process won't have a chance to
    // reset the lock. Calling ThreadList::resetOnFork here ensures that any
    // such locks would have been reset by the caller and hence it's safe to
    // call pthread_creat at this point.
    ThreadList::resetOnFork();

    /* NOTE: Any work that needs to be done for the newly created child
     * should be put into pthread_atfork_child() function. That function is
     * hooked to the libc:fork() and will be called right after the new
     * process is created and before the fork() returns.
     *
     * pthread_atfork_child is registered by calling pthread_atfork() from
     * within the DmtcpWorker constructor to make sure that this is the first
     * registered handle.
     */
    UniquePid child = UniquePid(host, getpid(), child_time);
    JTRACE("fork() done [CHILD]") (child) (parent);
  } else if (childPid > 0) { /* Parent Process */
    UniquePid child = UniquePid(host, childPid, child_time);
    ProcessInfo::instance().insertChild(childPid, child);
    JTRACE("fork()ed [PARENT] done") (child);
  }

  pthread_atfork_enabled = false;

  if (childPid != 0) {
    _real_close(childCoordinatorSocket);
    PluginManager::eventHook(DMTCP_EVENT_ATFORK_PARENT, NULL);
    WRAPPER_EXECUTION_RELEASE_EXCL_LOCK();
  }
  return childPid;
}

extern "C"
int
daemon(int nochdir, int noclose)
{
  int fd;

  switch (fork()) {
  case -1:
    return -1;

  case 0:
    break;
  default:
    _exit(0);
  }

  if (setsid() == -1) {
    return -1;
  }

  if (!nochdir) {
    JASSERT(chdir("/") == 0);
  }

  if (!noclose) {
    fd = open("/dev/null", O_RDWR, 0);
    if (fd != -1) {
      dup2(fd, STDIN_FILENO);
      dup2(fd, STDOUT_FILENO);
      dup2(fd, STDERR_FILENO);
      if (fd > 2) {
        close(fd);
      }
    } else {
      errno = ENODEV;
      return -1;
    }
  }
  return 0;
}

extern "C" pid_t
vfork()
{
  JTRACE("vfork wrapper calling fork");

  // This might not preserve the full semantics of vfork.
  // Used for checkpointing gdb.
  return fork();
}

// Special short-lived processes from executables like /lib/libc.so.6
// and man setuid/setgid executables cannot be loaded with LD_PRELOAD.
// Since they're short-lived, we execute them while holding a lock
// delaying checkpointing.
static void
execShortLivedProcessAndExit(const char *path, const char *argv[])
{
  unsetenv("LD_PRELOAD"); // /lib/ld.so won't let us preload if exec'ing lib
  const unsigned int bufSize = 100000;
  char *buf = (char *)JALLOC_HELPER_MALLOC(bufSize);
  memset(buf, 0, bufSize);
  FILE *output;
  if (argv[0] == NULL) {
    output = _real_popen(path, "r");
  } else {
    string command = path;
    for (int i = 1; argv[i] != NULL; i++) {
      command = command + " " + argv[i];
    }
    output = _real_popen(command.c_str(), "r");
  }
  int numRead = fread(buf, 1, bufSize - 1, output);
  numRead++, numRead--; // suppress unused-var warning
  buf[bufSize - 1] = '\0'; // NULL terminate in case the last char is not null

  pclose(output); // /lib/libXXX process is now done; can checkpoint now
  // FIXME:  code currently allows wrapper to proceed without lock if
  // it was busy because of a writer.  The unlock will then fail below.
  bool __wrapperExecutionLockAcquired = true; // needed for LOCK_UNLOCK macro
  WRAPPER_EXECUTION_RELEASE_EXCL_LOCK();

  // We  are now the new /lib/libXXX process, and it's safe for DMTCP to ckpt
  // us.
  printf("%s", buf); // print buf, which is what /lib/libXXX would print
  JALLOC_HELPER_FREE(buf);

  // Avoid running exit handlers of the parent process by calling _exit.
  _exit(0);
}

// FIXME:  Unify this code with code prior to execvp in dmtcp_launch.cpp
// Can use argument to dmtcpPrepareForExec() or getenv("DMTCP_...")
// from DmtcpWorker constructor, to distinguish the two cases.
static void
dmtcpPrepareForExec(const char *path,
                    const char *argv[],
                    const char **filename,
                    const char ***newArgv)
{
  JTRACE("Preparing for Exec") (path);

  const char *libPrefix = "/lib/lib";
  const char *lib64Prefix = "/lib64/lib";
  if (path != NULL && Util::strStartsWith(path, libPrefix)) {
    execShortLivedProcessAndExit(path, argv);
  }
  if (path != NULL && Util::strStartsWith(path, lib64Prefix)) {
    execShortLivedProcessAndExit(path, argv);
  }

  // Needed for /usr/libexec/utempter/utempter and other short-lived
  // setuid/setgid processes.
  // FIXME:  USE THIS FOR ALL setuid/setgid PROCESSES EXCEPT ONES THAT
  // WE DIRECTLY HANDLE, LIKE 'screen'.  (Need to name special routine,
  // execScreenProcess() ??)
  if (path != NULL && Util::strEndsWith(path, "/utempter")) {
    JTRACE("Trying to exec: utempter")(path)(argv[0])(argv[1]);
    string realPtsNameStr;

    // utempter takes a pts slave name as an argument. Since we virtualize
    // ptys, the slave name points to a virtual slave name, thus we need to
    // replace it with the real one.
    for (size_t i = 0; argv[i] != NULL; i++) {
      if (Util::strStartsWith(argv[i], VIRT_PTS_PREFIX_STR)) {
        // FIXME: Potential memory leak if exec() fails.
        char *realPtsNameStr = (char *)JALLOC_HELPER_MALLOC(PTS_PATH_MAX);
        SharedData::getRealPtyName(argv[i], realPtsNameStr,
                                   PTS_PATH_MAX);

        // Override const restriction
        argv[i] = realPtsNameStr;
      }
    }
    execShortLivedProcessAndExit(path, argv);
  }

  // FIXME:  SEE COMMENTS IN dmtcp_launch.cpp, rev. 1087; AND CHANGE THIS.
  if (path != NULL && Util::isSetuid(path)) {
    if (Util::isScreen(path)) {
      Util::setScreenDir();
    }

    // THIS NEXT LINE IS DANGEROUS.  MOST setuid PROGRAMS CAN'T RUN UNPRIVILEGED
    Util::patchArgvIfSetuid(path, argv, newArgv);

    // BUG:  Util::patchArgvIfSetuid() DOES NOT SET newArgv WHEN COPYING
    // BINARY IN CODE RE-FACTORING FROM REVISION 911.
    *filename = (*newArgv)[0];
  } else {
    *filename = (char *)path;
    *newArgv = argv;
  }

  JTRACE("Will exec filename instead of path") (path) (*filename);

  Util::adjustRlimitStack();

  // Remove FD_CLOEXEC flag from protected file descriptors.
  for (size_t i = PROTECTED_FD_START; i < PROTECTED_FD_END; i++) {
    int flags = fcntl(i, F_GETFD, NULL);
    if (flags != -1) {
      fcntl(i, F_SETFD, flags & ~FD_CLOEXEC);
    }
  }
  JTRACE("Prepared for Exec") (getenv("LD_PRELOAD"));
}

static void
dmtcpProcessFailedExec(const char *path, const char *newArgv[])
{
  int saved_errno = errno;

  if (Util::isSetuid(path)) {
    Util::freePatchedArgv(newArgv);
  }

  restoreUserLDPRELOAD();

  JTRACE("Processed failed Exec Attempt") (path) (getenv("LD_PRELOAD"));
  errno = saved_errno;
  JASSERT(_real_close(PROTECTED_LIFEBOAT_FD) == 0) (JASSERT_ERRNO);
}

static string
getUpdatedLdPreload(const char *filename, const char *currLdPreload)
{
  string preload = getenv(ENV_VAR_HIJACK_LIBS);

  bool isElf = false;
  bool is32bitElf = false;

  if (getenv(ENV_VAR_HIJACK_LIBS_M32) != NULL &&
      Util::elfType(filename, &isElf, &is32bitElf) != -1 &&
      isElf &&
      is32bitElf) {
    preload = getenv(ENV_VAR_HIJACK_LIBS_M32);
  }

  vector<string>pluginLibraries = tokenizeString(preload, ":");
  for (size_t i = 0; i < pluginLibraries.size(); i++) {
    // If the plugin doesn't exist, try to search it in the current install
    // directory.
    if (!jalib::Filesystem::FileExists(pluginLibraries[i])) {
      pluginLibraries[i] =
        Util::getPath(jalib::Filesystem::BaseName(pluginLibraries[i]).c_str(),
                      is32bitElf);
    }
  }

  const char *preloadEnv = getenv("LD_PRELOAD");
  if (currLdPreload != NULL && strlen(currLdPreload) > 0) {
    pluginLibraries.push_back(currLdPreload);
    setenv(ENV_VAR_ORIG_LD_PRELOAD, currLdPreload, 1);
  } else if (preloadEnv != NULL && strlen(preloadEnv) > 0) {
    pluginLibraries.push_back(preloadEnv);
    setenv(ENV_VAR_ORIG_LD_PRELOAD, preloadEnv, 1);
  }

  string newPreload;
  if (pluginLibraries.size() > 0) {
    newPreload = pluginLibraries[0];
    for (size_t i = 1; i < pluginLibraries.size(); i++) {
      newPreload += ":" + pluginLibraries[i];
    }
  }

  return newPreload;
}

static vector<string>
copyEnv(char *const envp[])
{
  vector<string>result;
  if (envp) {
    for (size_t i = 0; envp[i] != NULL; i++) {
      result.push_back(envp[i]);
    }
  }
  return result;
}

static const char **
stringVectorToPointerArray(const vector<string> &s, size_t len)
{
  JASSERT(len >= s.size());

  const char **result = (const char **) JALLOC_MALLOC(len * sizeof (char*));
  JASSERT(result != NULL);

  // Now get the pointers.
  for (size_t i = 0; i < s.size(); i++) {
    result[i] = s[i].c_str();
  }

  result[s.size()] = NULL;

  return result;
}

static const char *ourImportantEnvs[] =
{
  ENV_VARS_ALL // expands to a long list
};
#define ourImportantEnvsCnt (sizeof(ourImportantEnvs) / sizeof(char *))

static bool
isImportantEnv(string str)
{
  str = str.substr(0, str.find("="));

  for (size_t i = 0; i < ourImportantEnvsCnt; ++i) {
    if (str == ourImportantEnvs[i]) {
      return true;
    }
  }
  return false;
}

static vector<string>
patchUserEnv(const char *env[], const char *filename)
{
  vector<string>result;
  string userPreloadStr;

  ostringstream out;
  out << "non-DMTCP env vars:\n";

  for (size_t i = 0; env[i] != NULL; i++) {
    if (isImportantEnv(env[i])) {
      if (dbg) {
        out << "     skipping: " << env[i] << '\n';
      }
      continue;
    }
    if (Util::strStartsWith(env[i], "LD_PRELOAD=")) {
      userPreloadStr = &env[i][strlen("LD_PRELOAD=")];
      continue;
    }

    result.push_back(env[i]);
    if (dbg) {
      out << "     addenv[user]:" << result.back() << '\n';
    }
  }
  JTRACE("Creating a copy of (non-DMTCP) user env vars...") (out.str());

  // pack up our ENV into the new ENV
  out.str("DMTCP env vars:\n");
  for (size_t i = 0; i < ourImportantEnvsCnt; ++i) {
    const char *v = getenv(ourImportantEnvs[i]);
    const string e = ourImportantEnvs[i];
    if (e == ENV_VAR_ORIG_LD_PRELOAD && !userPreloadStr.empty()) {
      result.push_back(e + "=" + userPreloadStr);
    } else if (v != NULL) {
      result.push_back(e + '=' + v);
      if (dbg) {
        out << "     addenv[dmtcp]:" << result.back() << '\n';
      }
    }
  }

  string ldPreloadStr = "LD_PRELOAD=";
  ldPreloadStr += getUpdatedLdPreload(filename, userPreloadStr.c_str());

  result.push_back(ldPreloadStr);
  if (dbg) {
    out << "     addenv[dmtcp]:" << result.back() << '\n';
  }

  JTRACE("Patched user envp...")  (out.str());

  return result;
}

static
int getLifeboatFd()
{
  char buf[PATH_MAX] = {0};
  snprintf(buf, sizeof(buf) - 1, "%s/LifeBoat.XXXXXX", dmtcp_get_tmpdir());
  int fd = _real_mkostemps(buf, 0, 0);
  JASSERT(fd != -1) (JASSERT_ERRNO);
  JASSERT(unlink(buf) == 0) (JASSERT_ERRNO);
  Util::changeFd(fd, PROTECTED_LIFEBOAT_FD);
  return PROTECTED_LIFEBOAT_FD;
}

extern "C" int
execve(const char *filename, char *const argv[], char *const envp[])
{
  return dmtcp_execvpe(filename, argv, envp);
}

extern "C" int
execv(const char *path, char *const argv[])
{
  return dmtcp_execvpe(path, argv, environ);
}

extern "C" int
execvp(const char *filename, char *const argv[])
{
  return dmtcp_execvpe(filename, argv, environ);
}

extern "C" int
fexecve(int fd, char *const argv[], char *const envp[])
{
  // TODO: Add dmtcp_execveat and use that.
  JASSERT(false) .Text("Not Implemented");
  return -1;
}

// This function first appeared in glibc 2.11
extern "C" int
execvpe(const char *filename, char *const argv[], char *const envp[])
{
  return dmtcp_execvpe(filename, argv, envp);
}

extern "C" int
execl(const char *path, const char *arg, ...)
{
  JTRACE("execl() wrapper") (path);

  va_list args;
  va_start(args, arg);
  int ret = dmtcp_execlpe(path, arg, args, environ);
  va_end(args);

  return ret;
}

extern "C" int
execlp(const char *file, const char *arg, ...)
{
  JTRACE("execlp() wrapper") (file);

  va_list args;
  va_start(args, arg);
  int ret = dmtcp_execlpe(file, arg, args, environ);
  va_end(args);

  return ret;
}

extern "C" int
execle(const char *path, const char *arg, ...)
{
  JTRACE("execle() wrapper") (path);

  va_list args;
  va_start(args, arg);
  int ret = dmtcp_execlpe(path, arg, args, NULL);
  va_end(args);
  return ret;
}

// See comment in glibcsystem.cpp for why this exists and how it works.
extern int do_system(const char *line);

extern "C" int
system(const char *line)
{
  JTRACE("before system(), checkpointing may not work")
    (line) (getenv(ENV_VAR_HIJACK_LIBS)) (getenv("LD_PRELOAD"));

  if (line == NULL) {
    /* Check that we have a command processor available.  It might
       not be available after a chroot(), for example.  */
    return do_system("exit 0") == 0;
  }

  int result = do_system(line);

  JTRACE("after system()");

  return result;
}

extern "C" int
dmtcp_execlpe(const char *filename,
              const char *arg,
              va_list args,
              char *const envp[])
{
  size_t argv_max = INITIAL_ARGV_MAX;
  const char *initial_argv[INITIAL_ARGV_MAX];
  const char **argv = initial_argv;
  argv[0] = arg;

  unsigned int i = 0;
  while (argv[i++] != NULL) {
    if (i == argv_max) {
      argv_max *= 2;
      const char **nptr = (const char **)realloc(
          argv == initial_argv ? NULL : argv,
          argv_max *
          sizeof(char *));
      if (nptr == NULL) {
        if (argv != initial_argv) {
          free(argv);
        }
        return -1;
      }
      if (argv == initial_argv) {
        /* We have to copy the already filled-in data ourselves.  */
        memcpy(nptr, argv, i * sizeof(char *));
      }

      argv = nptr;
    }

    argv[i] = va_arg(args, const char *);
  }

  if (envp == NULL) {
    envp = va_arg(args, char *const *);
  }

  int ret = dmtcp_execvpe(filename, (char *const *)argv, (char *const *)envp);
  if (argv != initial_argv) {
    free(argv);
  }

  return ret;
}

extern "C" int
dmtcp_execvpe(const char *filename, char *const argv[], char *const envp[])
{
  if (isPerformingCkptRestart()) {
    return _real_execvpe(filename, argv, envp);
  }

  string programName = jalib::Filesystem::BaseName(filename);

  JASSERT(programName != "dmtcp_coordinator" &&
          programName != "dmtcp_launch" &&
          programName != "dmtcp_restart" &&
          programName != "mtcp_restart")
    (programName).Text("This program should not be run under ckpt control");

  if (programName == "dmtcp_command") {
    // make sure coordinator connection is closed
    _real_close(PROTECTED_COORD_FD);

    pid_t cpid = _real_fork();
    JASSERT(cpid != -1);
    if (cpid != 0) {
      _real_exit(0);
    }
    return _real_execvpe(filename, argv, envp);
  }

  /* Acquire the wrapperExeution lock to prevent checkpoint to happen while
   * processing this system call.
   */
  WRAPPER_EXECUTION_GET_EXCL_LOCK();

  // Make a copy of the argv and environ coz it might change after a setenv().
  char filenameCopy[PATH_MAX] = {0};
  strncpy(filenameCopy, filename, sizeof(filenameCopy));

  const vector<string>argvCopy = copyEnv(argv);
  size_t maxArgs = argvCopy.size() + MAX_EXTRA_ARGS;
  const char **argvCopyCStr = stringVectorToPointerArray(argvCopy, maxArgs);

  const vector<string>envpCopy = copyEnv(envp);
  size_t maxEnv = envpCopy.size() + MAX_EXTRA_ENV;
  const char **envpCopyCStr = stringVectorToPointerArray(envpCopy, maxEnv);

  DmtcpEventData_t data;

  data.preExec.filename = filenameCopy;
  data.preExec.maxArgs = maxArgs;
  data.preExec.argv = argvCopyCStr;
  data.preExec.maxEnv = maxEnv;
  data.preExec.envp = envpCopyCStr;
  data.preExec.serializationFd = getLifeboatFd();

  UniquePid::serialize(data.preExec.serializationFd);

  PluginManager::eventHook(DMTCP_EVENT_PRE_EXEC, &data);

  programName = jalib::Filesystem::BaseName(data.preExec.filename);

  if (programName == "dmtcp_nocheckpoint" || programName == "dmtcp_command" ||
      programName == "ssh" || programName == "rsh" ) {
    return _real_execvpe(data.preExec.filename,
                         (char* const*) data.preExec.argv,
                         (char* const*) data.preExec.envp);
  }

  const char *newFilename;
  const char **newArgv;
  dmtcpPrepareForExec(data.preExec.filename,
                      data.preExec.argv,
                      &newFilename,
                      &newArgv);

  const vector<string>newEnvStrings =
    patchUserEnv(data.preExec.envp, data.preExec.filename);

  const char **newEnv = stringVectorToPointerArray(newEnvStrings, maxEnv);

  int retVal = _real_execvpe(newFilename,
                             (char* const*) newArgv,
                             (char*const*) newEnv);

  dmtcpProcessFailedExec(data.preExec.filename, newArgv);

  WRAPPER_EXECUTION_RELEASE_EXCL_LOCK();

  return retVal;
}
