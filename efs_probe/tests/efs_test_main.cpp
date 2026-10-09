// Host-only subprocess fixtures. Never links vendor libdiag or touches a modem.
#include "diag_process_worker.h"
#include "process_efs_executor.h"
#include <signal.h>
#include "hash.h"
#include <gtest/gtest.h>
#include <cerrno>
#include <string>
#include <unistd.h>

int main(int argc, char** argv) {
  using namespace guacamole_efs;
  if (argc == 3 && std::string(argv[1]) == kDiagWorkerCommand) {
    WorkerReadReport r;
    if (!ParseWorkerNonce(argv[2], &r.nonce)) return 2;
    r.pid = static_cast<uint32_t>(getpid());
    r.status = WorkerReadStatus::kOk;
    r.teardown_completed = true;
    r.bytes.assign(32, 0x42);
    r.sha256 = Sha256(r.bytes.data(), r.bytes.size());
    const std::string path = argv[0];
    const auto mode = path.substr(path.find_last_of('/') + 1);
    if (mode == "timeout") { for (;;) pause(); }
    auto frame = EncodeWorkerReport(r);
    if (mode == "partial") frame.resize(10);
    if (mode == "oversized") frame.push_back(0);
    if (mode == "checksum") frame.back() ^= 1;
    size_t offset = 0;
    while (offset < frame.size()) {
      const auto n = write(kDiagWorkerResultFd, frame.data() + offset, frame.size() - offset);
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) return 3;
      offset += static_cast<size_t>(n);
    }
    close(kDiagWorkerResultFd);
    if (mode == "teardown-hang") { for (;;) pause(); }
    return mode == "badexit" ? 4 : 0;
  }
  if(argc==5 && std::string(argv[1])==kProcessReadCommand) {
    ProcessReport r;
    if(!ParseWorkerNonce(argv[2],&r.nonce) || std::string(argv[3])!=std::to_string(static_cast<uint32_t>(EfsTarget::kMcfgSelDb))) return 2;
    r.identity={ProcessOperation::kRead,EfsTarget::kMcfgSelDb,argv[4],{}};
    r.outcome=ProcessOutcome::kConfirmed; r.teardown_completed=true;
    r.bytes.assign(57655,0x42); r.sha256=Sha256(r.bytes.data(),r.bytes.size());
    const std::string path=argv[0]; const auto mode=path.substr(path.find_last_of('/')+1);
    if(mode=="timeout") { for(;;) pause(); }
    if(mode=="killed") { raise(SIGKILL); return 5; }
    auto frame=EncodeProcessReport(r);
    if(mode=="partial") frame.resize(20);
    if(mode=="checksum") frame.back()^=1;
    size_t offset=0;
    while(offset<frame.size()) {
      auto n=write(kDiagWorkerResultFd,frame.data()+offset,frame.size()-offset);
      if(n<0 && errno==EINTR) continue;
      if(n<=0) return 3;
      offset+=static_cast<size_t>(n);
    }
    close(kDiagWorkerResultFd);
    if(mode=="teardown-hang") { for(;;) pause(); }
    return mode=="badexit"?4:0;
  }
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
