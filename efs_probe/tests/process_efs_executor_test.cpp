#include "process_efs_executor.h"
#include "hash.h"
#include <gtest/gtest.h>
#include <deque>
#include <climits>
#include <cstdlib>
#include <cerrno>
#include <unistd.h>
#include <sys/wait.h>
namespace guacamole_efs {
namespace {
class FakeWorkers : public IProcessWorkerLauncher {
 public:
  std::vector<ProcessRequest> calls;
  std::deque<std::vector<uint8_t>> reads;
  size_t unreaped_call=0;
  std::vector<std::string> events;
  bool timeout=false, kill=false, corrupt=false, unreaped=false, repeat_nonce=false;
  bool readback_fail=false, ambiguous=false;
  ProcessResult Run(const ProcessRequest& request) override {
    calls.push_back(request);
    events.push_back("spawn"+std::to_string(calls.size()));
    ProcessResult r; r.spawned=true; r.reaped=!unreaped && calls.size()!=unreaped_call; r.exit_code=0;
    r.pid=123; // Same numeric PID intentionally legal after reap.
    r.report_valid=!corrupt; r.report.identity=request;
    r.report.nonce[0]=repeat_nonce?1:static_cast<uint8_t>(calls.size());
    r.report.teardown_completed=true;
    r.report.outcome=ProcessOutcome::kConfirmed;
    if(request.operation==ProcessOperation::kRead) {
      if(!reads.empty()) { r.report.bytes=reads.front(); reads.pop_front(); }
      if(readback_fail&&calls.size()>1) { r.report_valid=false; r.exit_code=1; }
    } else {
      r.timed_out=timeout; r.signal=kill?9:0;
      if(ambiguous) r.report.outcome=ProcessOutcome::kAmbiguous;
    }
    r.report.sha256=Sha256(r.report.bytes.data(),r.report.bytes.size());
    if(r.reaped) events.push_back("reap"+std::to_string(calls.size()));
    return r;
  }
};
PatchPlan Plan() { return *BuildPatchPlan(EfsTarget::kMcfgSelDb,{1},{2}).plan; }
TransactionOutcome Apply(FakeWorkers& workers,InMemoryTransactionLog& log,
    RollbackPolicy rollback=RollbackPolicy::kNever) {
  ProcessEfsExecutor executor(workers);
  return ApplyProcessPatchPlan(executor,EfsWriteTarget::kMcfgSelDb,Plan(),"tx",1,log,rollback);
}
TEST(ProcessExecutor, FreshWorkersEachPhaseWithPidReuse) {
  FakeWorkers w; w.reads={{1},{2}}; InMemoryTransactionLog log;
  EXPECT_EQ(Apply(w,log).state,TransactionState::kSucceeded);
  ASSERT_EQ(w.calls.size(),3u);
  EXPECT_EQ(w.events,(std::vector<std::string>{"spawn1","reap1","spawn2","reap2","spawn3","reap3"}));
  EXPECT_EQ(w.calls[0].operation,ProcessOperation::kRead);
  EXPECT_EQ(w.calls[1].operation,ProcessOperation::kWrite);
  EXPECT_EQ(w.calls[2].operation,ProcessOperation::kRead);
}
TEST(ProcessExecutor, UnreapedBaselineBlocksNextWorker) {
  FakeWorkers w; w.unreaped=true; w.reads={{1}}; InMemoryTransactionLog log;
  EXPECT_EQ(Apply(w,log).state,TransactionState::kReadCurrentFailed); EXPECT_EQ(w.calls.size(),1u);
}
TEST(ProcessExecutor, UnreapedWriteNeverStartsReadback) {
  FakeWorkers w; w.unreaped_call=2; w.reads={{1}}; InMemoryTransactionLog log;
  EXPECT_EQ(Apply(w,log).state,TransactionState::kMutationOutcomeUnknown);
  EXPECT_EQ(w.calls.size(),2u);
}
TEST(ProcessExecutor, UnreapedRollbackNeverStartsVerification) {
  FakeWorkers w; w.unreaped_call=4; w.reads={{1},{3}}; InMemoryTransactionLog log;
  EXPECT_EQ(Apply(w,log,RollbackPolicy::kRestorePreimageOnVerifiedMismatch).state,TransactionState::kRollbackFailedHardStop);
  EXPECT_EQ(w.calls.size(),4u);
}
TEST(ProcessExecutor, NonceReuseFailsReadback) {
  FakeWorkers w; w.repeat_nonce=true; w.reads={{1},{2}}; InMemoryTransactionLog log;
  EXPECT_EQ(Apply(w,log).state,TransactionState::kReadbackFailed);
}
TEST(ProcessExecutor, WriteTimeoutNoRetryFreshReadbackProvesProposed) {
  FakeWorkers w; w.timeout=true; w.reads={{1},{2}}; InMemoryTransactionLog log;
  EXPECT_EQ(Apply(w,log).state,TransactionState::kSucceededAfterAmbiguousResponse); EXPECT_EQ(w.calls.size(),3u);
}
TEST(ProcessExecutor, KilledWriteReadbackProvesOriginal) {
  FakeWorkers w; w.kill=true; w.reads={{1},{1}}; InMemoryTransactionLog log;
  EXPECT_EQ(Apply(w,log).state,TransactionState::kUnchangedAfterAmbiguousResponse); EXPECT_EQ(w.calls.size(),3u);
}
TEST(ProcessExecutor, AmbiguousWriteProposed) {
  FakeWorkers w; w.ambiguous=true; w.reads={{1},{2}}; InMemoryTransactionLog log;
  EXPECT_EQ(Apply(w,log).state,TransactionState::kSucceededAfterAmbiguousResponse);
}
TEST(ProcessExecutor, FailedReadbackNeverRollsBack) {
  FakeWorkers w; w.readback_fail=true; w.reads={{1},{2}}; InMemoryTransactionLog log;
  auto r=Apply(w,log,RollbackPolicy::kRestorePreimageOnVerifiedMismatch);
  EXPECT_EQ(r.state,TransactionState::kReadbackFailed); EXPECT_FALSE(r.rollback_attempted);
  EXPECT_EQ(w.calls.size(),3u); EXPECT_FALSE(log.LoadPendingTransactions().AllowsNewTransaction());
}
TEST(ProcessExecutor, MismatchDefaultLeavesRecoveryPending) {
  FakeWorkers w; w.reads={{1},{3}}; InMemoryTransactionLog log;
  EXPECT_EQ(Apply(w,log).state,TransactionState::kContentMismatch); EXPECT_EQ(w.calls.size(),3u);
}
TEST(ProcessExecutor, OptInRollbackUsesTwoAdditionalFreshWorkers) {
  FakeWorkers w; w.reads={{1},{3},{1}}; InMemoryTransactionLog log;
  EXPECT_EQ(Apply(w,log,RollbackPolicy::kRestorePreimageOnVerifiedMismatch).state,TransactionState::kRolledBackSuccessfully);
  ASSERT_EQ(w.calls.size(),5u); EXPECT_EQ(w.calls[3].bytes,std::vector<uint8_t>({1}));
  EXPECT_EQ(w.calls[4].operation,ProcessOperation::kRead);
}
TEST(ProcessExecutor, CorruptBaselineNoMutation) {
  FakeWorkers w; w.corrupt=true; w.reads={{1}}; InMemoryTransactionLog log;
  EXPECT_EQ(Apply(w,log).state,TransactionState::kReadCurrentFailed); EXPECT_EQ(w.calls.size(),1u);
}
TEST(ProcessExecutor, DeviceWriteEntryIsUnconditionallyClosed) {
  PosixProcessWorkerLauncher launcher;
  auto r=launcher.Run({ProcessOperation::kWrite,EfsTarget::kMcfgSelDb,"tx",{1}});
  EXPECT_FALSE(r.spawned); EXPECT_EQ(r.report.failure,ProcessFailure::kClosed);
}
TEST(ProcessExecutor, InvalidTargetAndTransactionNeverLaunch) {
  FakeWorkers w; ProcessEfsExecutor executor(w);
  EXPECT_FALSE(executor.ReadTarget(EfsTarget::kMcfgDir,"tx").spawned);
  EXPECT_FALSE(executor.ReadTarget(EfsTarget::kMcfgSelDb,"../tx").spawned);
  EXPECT_TRUE(w.calls.empty());
}
TEST(ProcessProtocol, ExactIdentityBytesHashAndPartialCorruption) {
  ProcessRequest q{ProcessOperation::kRead,EfsTarget::kMcfgSelDb,"tx",{}};
  ProcessReport r; r.identity=q; r.nonce[0]=9; r.outcome=ProcessOutcome::kConfirmed;
  r.teardown_completed=true; r.bytes={1,2,3}; r.sha256=Sha256(r.bytes.data(),r.bytes.size());
  auto frame=EncodeProcessReport(r); ASSERT_FALSE(frame.empty());
  ProcessReport parsed; std::string error;
  EXPECT_TRUE(DecodeProcessReport(frame,q,r.nonce,&parsed,&error)); EXPECT_EQ(parsed.bytes,r.bytes);
  auto wrong=q; wrong.transaction_id="ty";
  EXPECT_FALSE(DecodeProcessReport(frame,wrong,r.nonce,&parsed,&error));
  wrong=q; wrong.target=EfsTarget::kMcfgSetting;
  EXPECT_FALSE(DecodeProcessReport(frame,wrong,r.nonce,&parsed,&error));
  WorkerNonce other{};
  EXPECT_FALSE(DecodeProcessReport(frame,q,other,&parsed,&error));
  auto corrupt=frame; corrupt[85]^=1;
  EXPECT_FALSE(DecodeProcessReport(corrupt,q,r.nonce,&parsed,&error));
  frame.pop_back(); EXPECT_FALSE(DecodeProcessReport(frame,q,r.nonce,&parsed,&error));
}
TEST(ProcessProtocol, WriteReportBindsExactProposedBytesAndLength) {
  ProcessRequest q{ProcessOperation::kWrite,EfsTarget::kMcfgSelDb,"tx",{1,2}};
  ProcessReport r; r.identity=q; r.nonce[0]=7;
  r.outcome=ProcessOutcome::kAmbiguous; r.failure=ProcessFailure::kTransport;
  r.teardown_completed=true; r.sha256=Sha256(nullptr,0);
  auto frame=EncodeProcessReport(r); ASSERT_FALSE(frame.empty());
  ProcessReport parsed; std::string error;
  EXPECT_TRUE(DecodeProcessReport(frame,q,r.nonce,&parsed,&error));
  q.bytes={1,3}; EXPECT_FALSE(DecodeProcessReport(frame,q,r.nonce,&parsed,&error));
  q.bytes={1,2,3}; EXPECT_FALSE(DecodeProcessReport(frame,q,r.nonce,&parsed,&error));
}
TEST(ProcessProtocol, FailedReportCannotContainSuccessBytes) {
  ProcessReport r; r.identity={ProcessOperation::kRead,EfsTarget::kMcfgSelDb,"tx",{}};
  r.bytes={1}; r.sha256=Sha256(r.bytes.data(),r.bytes.size());
  EXPECT_TRUE(EncodeProcessReport(r).empty());
}
class ProcessSpawnTest : public ::testing::Test {
 protected:
  std::string directory;
  std::vector<std::string> links;
  void SetUp() override {
    char pattern[] = "/tmp/guacamole-worker-test-XXXXXX";
    const auto* p = mkdtemp(pattern);
    ASSERT_NE(p, nullptr);
    directory = p;
  }
  void TearDown() override {
    for (const auto& p : links) unlink(p.c_str());
    if (!directory.empty()) rmdir(directory.c_str());
  }
  std::string Fixture(const char* mode) {
    char self[PATH_MAX];
    const auto n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n <= 0) { ADD_FAILURE() << "cannot resolve test executable"; return {}; }
    self[n] = '\0';
    const auto path = directory + "/" + mode;
    if (symlink(self, path.c_str()) != 0) { ADD_FAILURE() << "fixture symlink failed"; return {}; }
    links.push_back(path);
    return path;
  }
};
TEST_F(ProcessSpawnTest, RealExecReapedBeforeNextWithUniqueNonce) {
  PosixProcessWorkerLauncher launcher(Fixture("ok"));
  ProcessEfsExecutor executor(launcher);
  auto a=executor.ReadTarget(EfsTarget::kMcfgSelDb,"tx");
  ASSERT_TRUE(a.report_valid); ASSERT_TRUE(a.reaped);
  int status=0; EXPECT_EQ(waitpid(a.pid,&status,WNOHANG),-1); EXPECT_EQ(errno,ECHILD);
  auto b=executor.ReadTarget(EfsTarget::kMcfgSelDb,"tx");
  ASSERT_TRUE(b.report_valid); ASSERT_TRUE(b.reaped);
  EXPECT_EQ(waitpid(b.pid,&status,WNOHANG),-1); EXPECT_EQ(errno,ECHILD);
  EXPECT_NE(a.report.nonce,b.report.nonce); EXPECT_EQ(a.report.bytes,b.report.bytes);
  EXPECT_EQ(a.report.bytes.size(),57655u);
}
TEST_F(ProcessSpawnTest, RealTimeoutAndTeardownHangReapedFailClosed) {
  for(const char* mode:{"timeout","teardown-hang"}) {
    PosixProcessWorkerLauncher launcher(Fixture(mode),std::chrono::milliseconds(300));
    ProcessEfsExecutor executor(launcher);
    auto r=executor.ReadTarget(EfsTarget::kMcfgSelDb,"tx");
    EXPECT_TRUE(r.timed_out); EXPECT_TRUE(r.reaped); EXPECT_FALSE(r.report_valid);
  }
}
TEST_F(ProcessSpawnTest, KilledPartialCorruptAndBadExitRejected) {
  for(const char* mode:{"killed","partial","checksum","badexit"}) {
    PosixProcessWorkerLauncher launcher(Fixture(mode)); ProcessEfsExecutor executor(launcher);
    auto r=executor.ReadTarget(EfsTarget::kMcfgSelDb,"tx");
    EXPECT_TRUE(r.spawned); EXPECT_TRUE(r.reaped); EXPECT_FALSE(r.report_valid) << mode;
  }
}

}
}
