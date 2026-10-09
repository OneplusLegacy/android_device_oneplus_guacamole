#include "production_worker.h"
#include "w1_artifact.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#if defined(__BIONIC__)
#include <sys/random.h>
#else
#include <sys/stat.h>
#endif
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace guacamole_production {
using namespace guacamole_efs;
using namespace guacamole_efs::w1;
bool CoordinatorIdentity(){return geteuid()==kDaemonUid&&getegid()==kDaemonUid&&Context()==kDaemonContext;}
namespace {
using Clock = std::chrono::steady_clock;
class Fd {
 public:
  explicit Fd(int fd = -1) : fd_(fd) {}
  ~Fd() { if (fd_ >= 0) close(fd_); }
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  int get() const { return fd_; }
  void Close() { if (fd_ >= 0) { close(fd_); fd_ = -1; } }
 private:
  int fd_;
};
bool RandomNonce(WorkerNonce* nonce) {
  // Fail closed on interrupted/short entropy reads too; no command retry.
#if defined(__BIONIC__)
  return getrandom(nonce->data(), nonce->size(), GRND_NONBLOCK) ==
      static_cast<ssize_t>(nonce->size());
#else
  // Soong host tests use a glibc 2.17 sysroot without sys/random.h or
  // the getrandom libc wrapper. Use the host kernel CSPRNG device instead.
  // ANDROID is also defined for host targets, so distinguish libc here.
  Fd random(open("/dev/urandom", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
  struct stat st{};
  if (random.get() < 0 || fstat(random.get(), &st) != 0 || !S_ISCHR(st.st_mode))
    return false;
  return read(random.get(), nonce->data(), nonce->size()) ==
      static_cast<ssize_t>(nonce->size());
#endif
}
void StoreExit(int status, ProcessResult* r) {
  r->reaped = true;
  if (WIFEXITED(status)) r->exit_code = WEXITSTATUS(status);
  if (WIFSIGNALED(status)) r->signal = WTERMSIG(status);
}
// Kill only our still-owned child. Never kill after waitpid reports ECHILD.
void StopChild(pid_t pid, ProcessResult* r) {
  if (r->reaped) return;
  int status = 0;
  const auto found = waitpid(pid, &status, WNOHANG);
  if (found == pid) { StoreExit(status, r); return; }
  if (found < 0 && errno != EINTR) return;
  if (kill(pid, SIGKILL) < 0 && errno != ESRCH) return;
  const auto deadline = Clock::now() + std::chrono::seconds(2);
  do {
    const auto waited = waitpid(pid, &status, WNOHANG);
    if (waited == pid) { StoreExit(status, r); return; }
    if (waited < 0 && errno != EINTR) return;
    poll(nullptr, 0, 10);
  } while (Clock::now() < deadline);
  // Kernel-uninterruptible child: report not reaped, NEVER launch phase two.
}
}
ProcessResult ProductionLauncher::Run(const ProcessRequest& request) {
  static std::mutex lifecycle_mutex;
  static bool child_unreaped=false;
  std::lock_guard<std::mutex> lifecycle_guard(lifecycle_mutex);
  if(child_unreaped) { ProcessResult blocked; blocked.error="unreaped worker HARD_STOP"; return blocked; }
  const bool is_read=request.operation==ProcessOperation::kRead;
  const bool rollback=request.transaction_id=="rollback";
  if (!CoordinatorIdentity() || !journal_.Validate() || journal_.lock_fd()<0 ||
      !ValidProcessRequest(request) || request.target!=EfsTarget::kMcfgSelDb ||
      (!is_read && request.transaction_id!="apply" && !rollback) ||
      (!is_read && request.bytes!=(rollback?PackagedOriginal():PackagedProposal()))) {
    ProcessResult blocked;blocked.error="fixed W1 request/environment rejected";return blocked;
  }
  const std::chrono::milliseconds timeout_(120000);
  const std::string executable_=kWorkerExecutable;
  ProcessResult r;
  WorkerNonce nonce{};
  if (timeout_.count() <= 0 || !RandomNonce(&nonce) || nonce==WorkerNonce{}) {
    r.error = "invalid deadline or nonce generation failed"; return r;
  }
  int fds[2];
  if (pipe2(fds, O_CLOEXEC) < 0) { r.error = "pipe creation failed"; return r; }
  Fd reader(fds[0]);
  const int safe_write = fcntl(fds[1], F_DUPFD_CLOEXEC, 10);
  close(fds[1]);
  if (safe_write < 0) { r.error = "pipe duplication failed"; return r; }
  Fd writer(safe_write);
  const int flags = fcntl(reader.get(), F_GETFL);
  if (flags < 0 || fcntl(reader.get(), F_SETFL, flags | O_NONBLOCK) < 0) {
    r.error = "nonblocking pipe setup failed"; return r;
  }
  // Separate inherited capability channel, never a CLI payload. Lock descriptor
  // survives child exec/parent death and prevents another coordinator overlap.
  int auth[2];
  if(pipe2(auth,O_CLOEXEC)<0){r.error="auth pipe failed";return r;}
  Fd auth_writer(auth[1]);
  const int auth_safe=fcntl(auth[0],F_DUPFD_CLOEXEC,10);close(auth[0]);
  Fd auth_reader(auth_safe);
  Fd lock_copy(fcntl(journal_.lock_fd(),F_DUPFD_CLOEXEC,10));
  const std::string operation=is_read?"read":(rollback?"rollback":"apply");
  const std::string capability=WorkerNonceHex(nonce)+"\n"+operation+"\n"+request.transaction_id+"\n";
  if(auth_reader.get()<0||lock_copy.get()<0||write(auth_writer.get(),capability.data(),capability.size())!=static_cast<ssize_t>(capability.size())) {
    r.error="capability creation failed";return r;
  }
  auth_writer.Close();
  posix_spawn_file_actions_t actions;
  posix_spawnattr_t attr;
  int rc = posix_spawn_file_actions_init(&actions);
  if (rc != 0) { r.error = "spawn actions init failed"; return r; }
  rc = posix_spawnattr_init(&attr);
  if (rc != 0) {
    posix_spawn_file_actions_destroy(&actions);
    r.error = "spawn attributes init failed"; return r;
  }
  sigset_t mask, defaults;
  sigemptyset(&mask);
  sigemptyset(&defaults);
  sigaddset(&defaults, SIGPIPE);
  short spawn_flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
  spawn_flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
  // Close first: reader could be fd 3. safe_write is guaranteed >= 10.
  if ((rc = posix_spawn_file_actions_addclose(&actions, reader.get())) == 0)
    rc = posix_spawn_file_actions_adddup2(&actions, writer.get(), kDiagWorkerResultFd);
  if (rc == 0) rc = posix_spawn_file_actions_addclose(&actions, writer.get());
  if (rc == 0) rc = posix_spawn_file_actions_adddup2(&actions, auth_reader.get(), 4);
  if (rc == 0) rc = posix_spawn_file_actions_adddup2(&actions, lock_copy.get(), 5);
  if (rc == 0) rc = posix_spawnattr_setsigmask(&attr, &mask);
  if (rc == 0) rc = posix_spawnattr_setsigdefault(&attr, &defaults);
  if (rc == 0) rc = posix_spawnattr_setflags(&attr, spawn_flags);
  // No CLI operation/nonce/path/payload. Only inherited fixed capability FD.
  char* const args[] = {const_cast<char*>(executable_.c_str()), nullptr};
  pid_t child = -1;
  const auto deadline = Clock::now() + timeout_;
  if (rc == 0) rc = posix_spawn(&child, executable_.c_str(), &actions, &attr, args, environ);
  posix_spawnattr_destroy(&attr);
  posix_spawn_file_actions_destroy(&actions);
  // Parent must close its pipe writer now, not at function exit (EOF proof).
  // Keep RAII on all setup errors too.
  writer.Close();
  if (rc != 0) { r.error = std::string("spawn/exec failed: ") + std::strerror(rc); return r; }
  r.spawned = true;
  r.pid = child;
  std::vector<uint8_t> frame;
  bool eof = false;
  bool child_owned = true;
  while (!(eof && r.reaped)) {
    if (Clock::now() >= deadline) {
      r.timed_out = true; r.error = "worker deadline exceeded"; break;
    }
    if (!r.reaped) {
      int status = 0;
      const auto waited = waitpid(child, &status, WNOHANG);
      if (waited == child) StoreExit(status, &r);
      else if (waited < 0 && errno != EINTR) {
        child_owned = false; r.error = "lost child wait ownership"; break;
      }
    }
    uint8_t buffer[4096];
    const ssize_t n = read(reader.get(), buffer, sizeof(buffer));
    if (n > 0) {
      if (frame.size() + static_cast<size_t>(n) > kProcessDataCap + 280) {
        r.error = "oversized worker frame"; break;
      }
      frame.insert(frame.end(), buffer, buffer + n);
    } else if (n == 0) {
      eof = true;
    } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
      r.error = "worker pipe read failed"; break;
    }
    if (!(eof && r.reaped)) {
      pollfd pfd{reader.get(), POLLIN, 0};
      // EOF without child exit must not busy-spin on POLLHUP.
      poll(eof ? nullptr : &pfd, eof ? 0 : 1, 10);
    }
  }
  if (!r.error.empty()) {
    if (child_owned) StopChild(child, &r);
    if (!r.reaped) child_unreaped=true;
    return r;
  }
  r.report_valid = DecodeProcessReport(frame, request, nonce,
                                     &r.report, &r.error);
  if (!(r.report_valid && r.reaped && r.exit_code == 0 && r.signal == 0 &&
      r.report.outcome == ProcessOutcome::kConfirmed && r.report.teardown_completed) && r.error.empty())
    r.error = "worker read/teardown/exit failed";
  return r;
}
}
