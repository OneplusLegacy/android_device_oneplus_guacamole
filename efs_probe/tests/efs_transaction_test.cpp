#include "efs_transaction.h"

#include <algorithm>
#include <functional>
#include <optional>
#include <type_traits>

#include <gtest/gtest.h>

using namespace guacamole_efs;

namespace {
std::vector<uint8_t> Bytes(const char* s) {
  return std::vector<uint8_t>(s, s + std::char_traits<char>::length(s));
}
}  // namespace

TEST(EfsTransaction, BuildPatchPlanComputesBothDigestsIndependently) {
  auto pre = Bytes("old-content");
  auto proposed = Bytes("new-content");

  auto r = BuildPatchPlan(EfsTarget::kMcfgSelDb, pre, proposed);
  ASSERT_EQ(r.state, PatchPlanBuildState::kOk);
  ASSERT_TRUE(r.plan.has_value());

  EXPECT_EQ(r.plan->preimage.bytes, pre);
  EXPECT_EQ(r.plan->proposed.bytes, proposed);
  EXPECT_EQ(VerifyDigest(pre, r.plan->preimage.sha256).outcome,
            VerificationOutcome::kMatch);
  EXPECT_EQ(VerifyDigest(proposed, r.plan->proposed.sha256).outcome,
            VerificationOutcome::kMatch);
  // The two digests must differ for two different byte buffers.
  EXPECT_NE(r.plan->preimage.sha256, r.plan->proposed.sha256);
}

TEST(EfsTransaction, RollbackIsExactCopyOfPreimage) {
  auto pre = Bytes("exact-preimage-bytes");
  auto proposed = Bytes("something-else-entirely");

  auto r = BuildPatchPlan(EfsTarget::kMcfgAutoselectByUim, pre, proposed);
  ASSERT_EQ(r.state, PatchPlanBuildState::kOk);

  EXPECT_EQ(r.plan->rollback.bytes, pre);
  EXPECT_EQ(r.plan->rollback.sha256, r.plan->preimage.sha256);
  // Rollback must never accidentally equal the proposed image.
  EXPECT_NE(r.plan->rollback.bytes, proposed);
}

TEST(EfsTransaction, VerifyDigestDetectsMismatch) {
  auto original = Bytes("original");
  auto tampered = Bytes("tampered!");

  auto digest = BuildPatchPlan(EfsTarget::kMcfgDebugLog, original, original)
                    .plan->preimage.sha256;

  EXPECT_EQ(VerifyDigest(original, digest).outcome,
            VerificationOutcome::kMatch);
  EXPECT_EQ(VerifyDigest(tampered, digest).outcome,
            VerificationOutcome::kMismatch);
}

TEST(EfsTransaction, VerifyPatchPlanConsistencySucceedsForBuiltPlan) {
  auto r = BuildPatchPlan(EfsTarget::kMcfgRfsSwDigestVersion, Bytes("a"),
                           Bytes("b"));
  ASSERT_EQ(r.state, PatchPlanBuildState::kOk);

  auto v = VerifyPatchPlanConsistency(*r.plan);
  EXPECT_EQ(v.outcome, VerificationOutcome::kMatch);
}

TEST(EfsTransaction, VerifyPatchPlanConsistencyDetectsForgedPreimageDigest) {
  auto r = BuildPatchPlan(EfsTarget::kMcfgSelDb, Bytes("real"), Bytes("new"));
  ASSERT_EQ(r.state, PatchPlanBuildState::kOk);
  PatchPlan plan = *r.plan;
  plan.preimage.sha256[0] ^= 0xFF;  // corrupt the stored digest.

  auto v = VerifyPatchPlanConsistency(plan);
  EXPECT_EQ(v.outcome, VerificationOutcome::kMismatch);
}

TEST(EfsTransaction, VerifyPatchPlanConsistencyDetectsForgedProposedDigest) {
  auto r = BuildPatchPlan(EfsTarget::kMcfgSelDb, Bytes("real"), Bytes("new"));
  ASSERT_EQ(r.state, PatchPlanBuildState::kOk);
  PatchPlan plan = *r.plan;
  plan.proposed.sha256[0] ^= 0xFF;

  auto v = VerifyPatchPlanConsistency(plan);
  EXPECT_EQ(v.outcome, VerificationOutcome::kMismatch);
}

TEST(EfsTransaction, VerifyPatchPlanConsistencyDetectsTamperedRollbackBytes) {
  auto r = BuildPatchPlan(EfsTarget::kMcfgSelDb, Bytes("real"), Bytes("new"));
  ASSERT_EQ(r.state, PatchPlanBuildState::kOk);
  PatchPlan plan = *r.plan;
  plan.rollback.bytes = Bytes("not-the-preimage-at-all");

  auto v = VerifyPatchPlanConsistency(plan);
  EXPECT_EQ(v.outcome, VerificationOutcome::kMismatch);
}

TEST(EfsTransaction, VerifyPatchPlanConsistencyDetectsTamperedRollbackDigest) {
  auto r = BuildPatchPlan(EfsTarget::kMcfgSelDb, Bytes("real"), Bytes("new"));
  ASSERT_EQ(r.state, PatchPlanBuildState::kOk);
  PatchPlan plan = *r.plan;
  plan.rollback.sha256[0] ^= 0xFF;

  auto v = VerifyPatchPlanConsistency(plan);
  EXPECT_EQ(v.outcome, VerificationOutcome::kMismatch);
}

TEST(EfsTransaction, VerifyPreimageStillCurrentSucceedsWhenUnchanged) {
  auto pre = Bytes("still-the-same");
  auto r = BuildPatchPlan(EfsTarget::kMcfgSelDb, pre, Bytes("new"));
  ASSERT_EQ(r.state, PatchPlanBuildState::kOk);

  auto v = VerifyPreimageStillCurrent(*r.plan, pre);
  EXPECT_EQ(v.outcome, VerificationOutcome::kMatch);
}

TEST(EfsTransaction, VerifyPreimageStillCurrentFailsWhenTargetChangedSince) {
  auto pre = Bytes("what-the-plan-was-built-from");
  auto r = BuildPatchPlan(EfsTarget::kMcfgSelDb, pre, Bytes("new"));
  ASSERT_EQ(r.state, PatchPlanBuildState::kOk);

  auto changed_on_device = Bytes("something-changed-it-since-the-snapshot");
  auto v = VerifyPreimageStillCurrent(*r.plan, changed_on_device);
  EXPECT_EQ(v.outcome, VerificationOutcome::kMismatch);
}

TEST(EfsTransaction, EmptyPreimageAndProposedHandledSafely) {
  auto r = BuildPatchPlan(EfsTarget::kMcfgCards, {}, {});
  ASSERT_EQ(r.state, PatchPlanBuildState::kOk);
  EXPECT_EQ(VerifyPatchPlanConsistency(*r.plan).outcome,
            VerificationOutcome::kMatch);
  EXPECT_EQ(VerifyPreimageStillCurrent(*r.plan, {}).outcome,
            VerificationOutcome::kMatch);
}

namespace {
class FakeReadClient : public IEfsClient {
 public:
  std::vector<uint8_t> content;
  bool fail_stat = false;
  int stat_calls = 0;

  EfsStatResult Stat(const std::string&) override {
    stat_calls++;
    EfsStatResult s;
    if (fail_stat) {
      s.ok = false;
      return s;
    }
    s.ok = true;
    s.diag_errno = 0;
    s.size = static_cast<int32_t>(content.size());
    s.mode = 0100644;
    return s;
  }
  EfsOpenResult OpenReadOnly(const std::string&) override {
    EfsOpenResult r;
    r.ok = true;
    r.diag_errno = 0;
    r.fd = 42;
    return r;
  }
  EfsReadResult Read(int32_t fd, uint32_t nbyte, uint32_t offset) override {
    EfsReadResult r;
    if (offset > content.size()) {
      r.ok = false;
      return r;
    }
    uint32_t avail = static_cast<uint32_t>(content.size()) - offset;
    uint32_t n = nbyte < avail ? nbyte : avail;
    r.ok = true;
    r.fd = fd;
    r.offset = offset;
    r.diag_errno = 0;
    r.bytes_read = static_cast<int32_t>(n);
    r.data.assign(content.begin() + offset, content.begin() + offset + n);
    return r;
  }
  EfsCloseResult Close(int32_t) override {
    EfsCloseResult r;
    r.ok = true;
    r.errno_value = 0;
    return r;
  }
  EfsOpenDirResult OpenDir(const std::string&) override { return {}; }
  EfsReadDirResult ReadDir(uint32_t, int32_t) override { return {}; }
  EfsCloseDirResult CloseDir(uint32_t) override { return {}; }
};

// Minimal fake IEfsWriteClient: every Write() call actually mutates a
// shared `*content` pointer (so the paired FakeReadClient sees the
// effect) - this is what lets a test express "the write really did
// change the file" vs. "the write call happened but the device
// silently ignored it" (content pointer left unmodified). Content
// mutation is applied REGARDLESS of `*_transport_status` - it
// represents ground truth on the (simulated) device, deliberately
// independent of what the caller is TOLD via the returned
// TransportStatus, exactly the real-world scenario an ambiguous
// timeout represents (the request may have been fully processed with
// no confirmed response).
class FakeWriteClient : public IEfsWriteClient {
 public:
  std::vector<uint8_t>* content = nullptr;  // points at the paired
                                              // FakeReadClient's content.
  bool fail_open = false;
  bool silently_ignore_write = false;  // ground truth: *content is
                                         // never actually updated,
                                         // regardless of what transport
                                         // status is reported.
  std::optional<std::vector<uint8_t>> corrupt_to;  // ground truth:
                                                      // every write
                                                      // forces *content
                                                      // to this instead.

  TransportStatus open_transport_status = TransportStatus::kOk;
  TransportStatus write_transport_status = TransportStatus::kOk;
  TransportStatus close_transport_status = TransportStatus::kOk;

  int open_calls = 0;
  int write_calls = 0;
  int close_calls = 0;

  // If set, OpenWrite() appends "write_open" to this shared log - lets
  // a test assert ordering relative to an ITransactionLog's own calls
  // (see MutationBeginsOnlyAfterDurableCheckpoint below).
  std::vector<std::string>* call_order = nullptr;

  MutationOpenResult OpenWrite(EfsWriteTarget) override {
    open_calls++;
    if (call_order) call_order->push_back("write_open");
    if (!silently_ignore_write && !corrupt_to.has_value() && content) {
      content->clear();  // OPEN truncates, matching kOflagWriteTruncate -
                          // ground truth, independent of transport_status.
    }
    MutationOpenResult r;
    r.transport_status = open_transport_status;
    if (open_transport_status != TransportStatus::kOk) return r;
    if (fail_open) {
      r.parsed.ok = false;
      return r;
    }
    r.parsed.ok = true;
    r.parsed.diag_errno = 0;
    r.parsed.fd = 7;
    return r;
  }
  MutationWriteResult Write(int32_t fd, uint32_t offset,
                             const std::vector<uint8_t>& data) override {
    write_calls++;
    if (corrupt_to.has_value() && content) {
      *content = *corrupt_to;
    } else if (!silently_ignore_write && content) {
      if (content->size() < offset + data.size()) {
        content->resize(offset + data.size());
      }
      std::copy(data.begin(), data.end(), content->begin() + offset);
    }
    MutationWriteResult r;
    r.transport_status = write_transport_status;
    if (write_transport_status != TransportStatus::kOk) return r;
    r.parsed.ok = true;
    r.parsed.fd = fd;
    r.parsed.offset = offset;
    r.parsed.bytes_written = static_cast<int32_t>(data.size());
    r.parsed.diag_errno = 0;
    return r;
  }
  MutationCloseResult Close(int32_t) override {
    close_calls++;
    MutationCloseResult r;
    r.transport_status = close_transport_status;
    if (close_transport_status != TransportStatus::kOk) return r;
    r.parsed.ok = true;
    r.parsed.errno_value = 0;
    return r;
  }
};


struct SessionFactory;
class OwnedRead : public IEfsReadSession {
 public:
  OwnedRead(SessionFactory* owner, const std::vector<uint8_t>& bytes, bool fail);
  ~OwnedRead() override;
  IEfsClient& client() override { return read_; }
 private:
  SessionFactory* owner_;
  FakeReadClient read_;
};
class OwnedWrite : public IEfsWriteSession {
 public:
  explicit OwnedWrite(SessionFactory* owner, int ordinal);
  ~OwnedWrite() override;
  IEfsWriteClient& client() override { return write_; }
 private:
  SessionFactory* owner_;
  FakeWriteClient write_;
};
struct SessionFactory : IEfsSessionFactory {
  std::vector<uint8_t> content = Bytes("original");
  std::vector<std::string> events;
  int read_sessions = 0, write_sessions = 0, live = 0, max_live = 0;
  int fail_read_on = 0, deny_write_on = 0;
  int open_calls = 0, write_calls = 0, close_calls = 0;
  bool corrupt_first = false, ambiguous_first = false, preserve_first = false;
  std::function<void()> on_write_session;
  std::unique_ptr<IEfsReadSession> OpenRead(std::string*) override {
    ++read_sessions;
    return std::make_unique<OwnedRead>(this, content, read_sessions == fail_read_on);
  }
  std::unique_ptr<IEfsWriteSession> OpenWrite(std::string*) override {
    ++write_sessions;
    if (on_write_session) on_write_session();
    if (write_sessions == deny_write_on) return nullptr;
    return std::make_unique<OwnedWrite>(this, write_sessions);
  }
};
OwnedRead::OwnedRead(SessionFactory* owner, const std::vector<uint8_t>& bytes, bool fail)
    : owner_(owner) {
  owner_->events.push_back("read_init");
  owner_->max_live = std::max(owner_->max_live, ++owner_->live);
  read_.content = bytes;
  read_.fail_stat = fail;
}
OwnedRead::~OwnedRead() { owner_->events.push_back("read_deinit"); --owner_->live; }
OwnedWrite::OwnedWrite(SessionFactory* owner, int ordinal) : owner_(owner) {
  owner_->events.push_back("write_init");
  owner_->max_live = std::max(owner_->max_live, ++owner_->live);
  write_.content = &owner_->content;
  write_.call_order = &owner_->events;
  if (ordinal == 1) {
    write_.silently_ignore_write = owner_->preserve_first;
    if (owner_->corrupt_first) write_.corrupt_to = Bytes("partial");
    if (owner_->ambiguous_first) write_.write_transport_status = TransportStatus::kTimeout;
  }
}
OwnedWrite::~OwnedWrite() {
  owner_->events.push_back("write_deinit"); --owner_->live;
  owner_->open_calls += write_.open_calls;
  owner_->write_calls += write_.write_calls;
  owner_->close_calls += write_.close_calls;
}
class TestLog : public InMemoryTransactionLog {
 public:
  bool fail_backup = false, fail_pending = false, fail_final = false, fail_scan = false;
  std::vector<std::string>* events = nullptr;
  BackupPersistResult PersistPreimageBackup(const std::string& id,
      const std::vector<uint8_t>& bytes) override {
    if (events) events->push_back("backup");
    if (fail_backup) return {BackupPersistState::kFsyncFailed, "injected"};
    return InMemoryTransactionLog::PersistPreimageBackup(id, bytes);
  }
  PendingRecordResult RecordPending(const TransactionRecord& record) override {
    if (events) events->push_back("pending");
    if (fail_pending) return {};
    return InMemoryTransactionLog::RecordPending(record);
  }
  bool RecordTransaction(const TransactionRecord& record) override {
    if (events) events->push_back("final");
    return !fail_final && InMemoryTransactionLog::RecordTransaction(record);
  }
  RecoveryScan LoadPendingTransactions() override {
    if (fail_scan) return {};
    return InMemoryTransactionLog::LoadPendingTransactions();
  }
};
class TransactionTest : public ::testing::Test {
 protected:
  PatchPlan Plan() { return *BuildPatchPlan(EfsTarget::kMcfgSelDb, Bytes("original"), Bytes("proposed")).plan; }
  TransactionOutcome Apply(RollbackPolicy policy = RollbackPolicy::kNever) {
    return ApplyPatchPlan(sessions, EfsWriteTarget::kMcfgSelDb, Plan(), "id", 123, log, policy);
  }
  SessionFactory sessions;
  TestLog log;
};
}  // namespace

static_assert(std::is_same<decltype(&IEfsWriteClient::OpenWrite),
    MutationOpenResult (IEfsWriteClient::*)(EfsWriteTarget)>::value,
    "No raw write path API");

TEST_F(TransactionTest, DurableCheckpointAndNonoverlappingSessionsInExactOrder) {
  log.events = &sessions.events;
  auto out = Apply();
  EXPECT_EQ(out.state, TransactionState::kSucceeded);
  EXPECT_EQ(sessions.max_live, 1);
  EXPECT_EQ(sessions.live, 0);
  EXPECT_EQ(sessions.events, (std::vector<std::string>{"read_init", "read_deinit",
      "backup", "pending", "write_init", "write_open", "write_deinit",
      "read_init", "read_deinit", "final"}));
}
TEST_F(TransactionTest, EmptyImageAndTargetMismatchNeverOpenSession) {
  auto plan = *BuildPatchPlan(EfsTarget::kMcfgSelDb, Bytes("original"), {}).plan;
  auto out = ApplyPatchPlan(sessions, EfsWriteTarget::kMcfgSelDb, plan, "id", 123, log);
  EXPECT_EQ(out.state, TransactionState::kPlanInconsistent);
  plan = Plan(); plan.target = EfsTarget::kMcfgCards;
  out = ApplyPatchPlan(sessions, EfsWriteTarget::kMcfgSelDb, plan, "id", 123, log);
  EXPECT_EQ(out.state, TransactionState::kPlanTargetMismatch);
  EXPECT_TRUE(sessions.events.empty());
}
TEST_F(TransactionTest, StalePreimageNeverWrites) {
  sessions.content = Bytes("different baseline");
  EXPECT_EQ(Apply().state, TransactionState::kPreimageMismatch);
  EXPECT_EQ(sessions.write_sessions, 0);
}
TEST_F(TransactionTest, BaselineReadFailureNeverWrites) {
  sessions.fail_read_on = 1;
  EXPECT_EQ(Apply().state, TransactionState::kReadCurrentFailed);
  EXPECT_EQ(sessions.write_sessions, 0);
}
TEST_F(TransactionTest, BackupFailureNeverOpensWriteSession) {
  log.fail_backup = true;
  EXPECT_EQ(Apply().state, TransactionState::kDurableCheckpointFailed);
  EXPECT_EQ(sessions.write_sessions, 0);
}
TEST_F(TransactionTest, PendingFailureNeverOpensWriteSession) {
  log.fail_pending = true;
  EXPECT_EQ(Apply().state, TransactionState::kDurableCheckpointFailed);
  EXPECT_EQ(sessions.write_sessions, 0);
}
TEST_F(TransactionTest, ReadbackFailureNeverRollsBackEvenWithOptIn) {
  sessions.fail_read_on = 2;
  auto out = Apply(RollbackPolicy::kRestorePreimageOnVerifiedMismatch);
  EXPECT_EQ(out.state, TransactionState::kReadbackFailed);
  EXPECT_FALSE(out.rollback_attempted);
  EXPECT_EQ(sessions.write_sessions, 1);
  EXPECT_FALSE(log.LoadPendingTransactions().AllowsNewTransaction());
}
TEST_F(TransactionTest, MismatchDefaultsToRecoveryWithoutRollback) {
  sessions.corrupt_first = true;
  auto out = Apply();
  EXPECT_EQ(out.state, TransactionState::kContentMismatch);
  EXPECT_EQ(sessions.write_sessions, 1);
  EXPECT_FALSE(out.rollback_attempted);
}
TEST_F(TransactionTest, ExplicitRollbackUsesNewWriteAndNewReadSessions) {
  sessions.corrupt_first = true;
  auto out = Apply(RollbackPolicy::kRestorePreimageOnVerifiedMismatch);
  EXPECT_EQ(out.state, TransactionState::kRolledBackSuccessfully);
  EXPECT_TRUE(out.rollback_used_fresh_session);
  EXPECT_EQ(sessions.read_sessions, 3);
  EXPECT_EQ(sessions.write_sessions, 2);
  EXPECT_EQ(sessions.max_live, 1);
  EXPECT_EQ(sessions.content, Bytes("original"));
}
TEST_F(TransactionTest, FailedRecoveryReadPersistsHardStop) {
  sessions.corrupt_first = true; sessions.fail_read_on = 3;
  EXPECT_EQ(Apply(RollbackPolicy::kRestorePreimageOnVerifiedMismatch).state,
      TransactionState::kRollbackFailedHardStop);
  EXPECT_EQ(sessions.write_sessions, 2);
  auto scan = log.LoadPendingTransactions();
  ASSERT_EQ(scan.records.size(), 1u);
  EXPECT_EQ(scan.records[0].state, TransactionState::kRollbackFailedHardStop);
}
TEST_F(TransactionTest, AmbiguousWriteIsNeverRetriedAndReadbackIsAuthoritative) {
  sessions.ambiguous_first = true;
  auto out = Apply();
  EXPECT_EQ(out.state, TransactionState::kSucceededAfterAmbiguousResponse);
  EXPECT_EQ(sessions.write_calls, 1);
  EXPECT_EQ(sessions.close_calls, 0);
  EXPECT_EQ(sessions.read_sessions, 2);
}
TEST_F(TransactionTest, ConfirmedUnchangedNeedsNoRollback) {
  sessions.preserve_first = true;
  EXPECT_EQ(Apply().state, TransactionState::kUnchangedAfterWrite);
  EXPECT_EQ(sessions.write_sessions, 1);
}
TEST_F(TransactionTest, AmbiguousUnchangedNeedsNoRollback) {
  sessions.preserve_first = true; sessions.ambiguous_first = true;
  EXPECT_EQ(Apply().state, TransactionState::kUnchangedAfterAmbiguousResponse);
  EXPECT_EQ(sessions.write_sessions, 1);
}
TEST_F(TransactionTest, UnavailableWriteSessionDoesNotMutate) {
  sessions.deny_write_on = 1;
  auto out = Apply();
  EXPECT_EQ(out.state, TransactionState::kWriteSessionUnavailable);
  EXPECT_FALSE(out.write_attempted);
  EXPECT_EQ(sessions.open_calls, 0);
}
TEST_F(TransactionTest, FinalCommitFailureIsVisibleAndPendingRemains) {
  log.fail_final = true;
  EXPECT_EQ(Apply().state, TransactionState::kFinalRecordFailed);
  EXPECT_FALSE(log.LoadPendingTransactions().AllowsNewTransaction());
}
TEST_F(TransactionTest, RecoveryScanErrorBlocksBeforeAnyDiagSession) {
  log.fail_scan = true;
  EXPECT_EQ(Apply().state, TransactionState::kRecoveryRequired);
  EXPECT_TRUE(sessions.events.empty());
}
TEST_F(TransactionTest, UnresolvedEarlierTransactionBlocksNextAttempt) {
  sessions.corrupt_first = true;
  ASSERT_EQ(Apply().state, TransactionState::kContentMismatch);
  int opens = sessions.write_sessions;
  EXPECT_EQ(Apply().state, TransactionState::kRecoveryRequired);
  EXPECT_EQ(sessions.write_sessions, opens);
}
TEST_F(TransactionTest, ProcessWideGuardRejectsReentrantDifferentFactory) {
  SessionFactory second;
  TestLog second_log;
  sessions.on_write_session = [&] {
    auto nested = ApplyPatchPlan(second, EfsWriteTarget::kMcfgSelDb, Plan(),
        "other-id", 124, second_log);
    EXPECT_EQ(nested.state, TransactionState::kBusy);
  };
  EXPECT_EQ(Apply().state, TransactionState::kSucceeded);
  EXPECT_TRUE(second.events.empty());
}

TEST_F(TransactionTest, PartialAmbiguousMultiChunkWriteStopsWithoutRetry) {
  sessions.ambiguous_first = true;
  auto plan = *BuildPatchPlan(EfsTarget::kMcfgSelDb, Bytes("original"),
      std::vector<uint8_t>(3000, 0x41)).plan;
  auto out = ApplyPatchPlan(sessions, EfsWriteTarget::kMcfgSelDb, plan, "id", 123, log);
  EXPECT_EQ(out.state, TransactionState::kContentMismatch);
  EXPECT_EQ(sessions.write_calls, 1);
  EXPECT_EQ(sessions.close_calls, 0);
  EXPECT_EQ(sessions.read_sessions, 2);
  EXPECT_FALSE(out.rollback_attempted);
}
