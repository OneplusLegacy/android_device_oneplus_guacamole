#include "diag_process_worker.h"
#include "diag_process_init_gate.h"
#include "hash.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <limits.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace guacamole_efs;
namespace {
WorkerReadReport GoodReport(uint8_t nonce = 1) {
  WorkerReadReport r;
  r.pid = 123;
  r.nonce.fill(nonce);
  r.status = WorkerReadStatus::kOk;
  r.teardown_completed = true;
  r.bytes.assign(32, 0x42);
  r.sha256 = Sha256(r.bytes.data(), r.bytes.size());
  return r;
}
void Resign(std::vector<uint8_t>* frame) {
  const auto hash = Sha256(frame->data(), 104);
  std::copy(hash.begin(), hash.end(), frame->begin() + 104);
}
TEST(DiagWorkerProtocol, ExactRoundTrip) {
  const auto in = GoodReport();
  WorkerReadReport out;
  ASSERT_TRUE(DecodeWorkerReport(EncodeWorkerReport(in), in.pid, in.nonce, &out, nullptr));
  EXPECT_EQ(out.bytes, in.bytes);
  EXPECT_EQ(out.sha256, in.sha256);
  EXPECT_TRUE(out.teardown_completed);
}
TEST(DiagWorkerProtocol, RejectsTruncatedAndTrailing) {
  const auto r = GoodReport();
  auto frame = EncodeWorkerReport(r);
  WorkerReadReport out;
  frame.pop_back();
  EXPECT_FALSE(DecodeWorkerReport(frame, r.pid, r.nonce, &out, nullptr));
  frame.resize(kWorkerFrameSize + 1);
  EXPECT_FALSE(DecodeWorkerReport(frame, r.pid, r.nonce, &out, nullptr));
}
TEST(DiagWorkerProtocol, RejectsIdentityAndChecksum) {
  const auto r = GoodReport();
  auto frame = EncodeWorkerReport(r);
  WorkerReadReport out;
  EXPECT_FALSE(DecodeWorkerReport(frame, r.pid + 1, r.nonce, &out, nullptr));
  EXPECT_FALSE(DecodeWorkerReport(frame, r.pid, WorkerNonce{}, &out, nullptr));
  frame.back() ^= 1;
  EXPECT_FALSE(DecodeWorkerReport(frame, r.pid, r.nonce, &out, nullptr));
}
TEST(DiagWorkerProtocol, RejectsSemanticFaultEvenWithValidChecksum) {
  const auto r = GoodReport();
  for (const size_t offset : {size_t(0), size_t(28), size_t(32), size_t(36), size_t(72)}) {
    auto frame = EncodeWorkerReport(r);
    frame[offset] ^= 0x80;
    Resign(&frame);
    WorkerReadReport out;
    EXPECT_FALSE(DecodeWorkerReport(frame, r.pid, r.nonce, &out, nullptr)) << offset;
  }
}
TEST(DiagWorkerProtocol, FailureHasNoSuccessPayload) {
  auto r = GoodReport();
  r.status = WorkerReadStatus::kReadFailed;
  EXPECT_TRUE(EncodeWorkerReport(r).empty());
  r.bytes.clear();
  WorkerReadReport out;
  ASSERT_TRUE(DecodeWorkerReport(EncodeWorkerReport(r), r.pid, r.nonce, &out, nullptr));
  EXPECT_EQ(out.status, WorkerReadStatus::kReadFailed);
  EXPECT_TRUE(out.bytes.empty());
}
TEST(DiagWorkerProtocol, NonceStrictLowerHex) {
  WorkerNonce nonce{};
  EXPECT_TRUE(ParseWorkerNonce(std::string(32, 'a'), &nonce));
  EXPECT_EQ(WorkerNonceHex(nonce), std::string(32, 'a'));
  EXPECT_FALSE(ParseWorkerNonce(std::string(32, 'A'), &nonce));
  EXPECT_FALSE(ParseWorkerNonce(std::string(31, '0'), &nonce));
  EXPECT_FALSE(ParseWorkerNonce(std::string(32, 'g'), &nonce));
}
class FakeLauncher : public IReadWorkerLauncher {
 public:
  int calls = 0;
  bool fail = false, unreaped = false, mismatch = false, replay = false;
  WorkerProcessResult Run() override {
    ++calls;
    WorkerProcessResult r;
    r.spawned = true; r.reaped = !unreaped;
    r.success = !fail; r.report_valid = true; r.exit_code = 0; r.pid = 123;
    r.report = GoodReport(replay ? 1 : calls);
    if (mismatch && calls == 2) {
      r.report.bytes[0] ^= 1;
      r.report.sha256 = Sha256(r.report.bytes.data(), r.report.bytes.size());
    }
    return r;
  }
};
TEST(DiagProcessCycle, FreshNonceAndExactDataAllowsReusedNumericPid) {
  FakeLauncher launcher;
  EXPECT_TRUE(RunDiagProcessCycleReadonly(launcher).exact_equal);
  EXPECT_EQ(launcher.calls, 2);
}
TEST(DiagProcessCycle, FailurePreventsSecondSpawn) {
  FakeLauncher launcher; launcher.fail = true;
  EXPECT_FALSE(RunDiagProcessCycleReadonly(launcher).exact_equal);
  EXPECT_EQ(launcher.calls, 1);
}
TEST(DiagProcessCycle, NoOverlapWithoutReapedPredecessor) {
  FakeLauncher launcher; launcher.unreaped = true;
  EXPECT_FALSE(RunDiagProcessCycleReadonly(launcher).exact_equal);
  EXPECT_EQ(launcher.calls, 1);
}
TEST(DiagProcessCycle, MismatchFails) {
  FakeLauncher launcher; launcher.mismatch = true;
  EXPECT_FALSE(RunDiagProcessCycleReadonly(launcher).exact_equal);
  EXPECT_EQ(launcher.calls, 2);
}
TEST(DiagProcessCycle, ReplayedNonceFails) {
  FakeLauncher launcher; launcher.replay = true;
  EXPECT_FALSE(RunDiagProcessCycleReadonly(launcher).exact_equal);
}
TEST(DiagProcessInitGate, ConsumedPermanentlyEvenIfVendorInitFailed) {
  DiagProcessInitGate gate;
  EXPECT_TRUE(gate.Consume());
  // There is deliberately no success/failure-specific release or reset.
  EXPECT_FALSE(gate.Consume());
  EXPECT_FALSE(gate.Consume());
}
// Real spawn+exec+pipe+wait exercised only when the USER runs the host tests.
class WorkerSpawnTest : public ::testing::Test {
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
TEST_F(WorkerSpawnTest, TwoExecsReadAndReapBeforeNextPhase) {
  PosixReadWorkerLauncher launcher(Fixture("ok"));
  const auto r = RunDiagProcessCycleReadonly(launcher);
  EXPECT_TRUE(r.exact_equal);
  for (const auto& w : r.workers) {
    EXPECT_TRUE(w.success); EXPECT_TRUE(w.reaped);
    int status = 0;
    EXPECT_EQ(waitpid(w.pid, &status, WNOHANG), -1);
    EXPECT_EQ(errno, ECHILD);
  }
}
TEST_F(WorkerSpawnTest, TimeoutKillsReapsAndDoesNotStartSecond) {
  PosixReadWorkerLauncher launcher(Fixture("timeout"), std::chrono::milliseconds(300));
  const auto r = RunDiagProcessCycleReadonly(launcher);
  EXPECT_TRUE(r.workers[0].timed_out);
  EXPECT_TRUE(r.workers[0].reaped);
  EXPECT_FALSE(r.workers[1].spawned);
  EXPECT_FALSE(r.exact_equal);
}
TEST_F(WorkerSpawnTest, CompleteFrameWithoutProcessExitIsNotSuccess) {
  PosixReadWorkerLauncher launcher(Fixture("teardown-hang"), std::chrono::milliseconds(300));
  const auto r = launcher.Run();
  EXPECT_TRUE(r.timed_out); EXPECT_TRUE(r.reaped); EXPECT_FALSE(r.success);
}
TEST_F(WorkerSpawnTest, PartialOversizedCorruptOrBadExitFailClosed) {
  for (const char* mode : {"partial", "oversized", "checksum", "badexit"}) {
    PosixReadWorkerLauncher launcher(Fixture(mode));
    const auto r = RunDiagProcessCycleReadonly(launcher);
    EXPECT_FALSE(r.workers[0].success) << mode;
    EXPECT_TRUE(r.workers[0].reaped) << mode;
    EXPECT_FALSE(r.workers[1].spawned) << mode;
  }
}
TEST_F(WorkerSpawnTest, MissingExecutableNeverStartsSecond) {
  PosixReadWorkerLauncher launcher(directory + "/does-not-exist");
  const auto r = RunDiagProcessCycleReadonly(launcher);
  EXPECT_FALSE(r.workers[0].spawned);
  EXPECT_FALSE(r.workers[1].spawned);
}
}
