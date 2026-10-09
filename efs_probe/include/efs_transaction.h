// Transaction safety primitives. No production daemon/CLI calls ApplyPatchPlan.
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "efs_sessions.h"
#include "efs_targets.h"
#include "efs_write_targets.h"

namespace guacamole_efs {

struct ExpectedPreimage {
  EfsTarget target = EfsTarget::kMcfgDir;
  std::vector<uint8_t> bytes;
  std::array<uint8_t, 32> sha256{};
};
struct ProposedImage {
  std::vector<uint8_t> bytes;
  std::array<uint8_t, 32> sha256{};
};
struct RollbackImage {
  std::vector<uint8_t> bytes;
  std::array<uint8_t, 32> sha256{};
};
struct PatchPlan {
  EfsTarget target = EfsTarget::kMcfgDir;
  ExpectedPreimage preimage;
  ProposedImage proposed;
  RollbackImage rollback;
};
enum class PatchPlanBuildState { kOk, kFailed };
struct PatchPlanBuildResult {
  PatchPlanBuildState state = PatchPlanBuildState::kFailed;
  std::string message;
  std::optional<PatchPlan> plan;
};
PatchPlanBuildResult BuildPatchPlan(EfsTarget target,
    const std::vector<uint8_t>& preimage_bytes,
    const std::vector<uint8_t>& proposed_bytes);
enum class VerificationOutcome { kMatch, kMismatch };
struct VerificationResult {
  VerificationOutcome outcome = VerificationOutcome::kMismatch;
  std::string message;
};
VerificationResult VerifyDigest(const std::vector<uint8_t>& actual_bytes,
    const std::array<uint8_t, 32>& expected_sha256);
VerificationResult VerifyPatchPlanConsistency(const PatchPlan& plan);
VerificationResult VerifyPreimageStillCurrent(const PatchPlan& plan,
    const std::vector<uint8_t>& current_bytes);

// Keep existing numeric values stable. Legacy records without a v2 checksum
// are not upgraded by guessing: the backend reports a recovery error.
enum class TransactionState {
  kPlanTargetMismatch = 0,
  kPlanInconsistent = 1,
  kPreimageMismatch = 2,
  kReadCurrentFailed = 3,
  kSucceeded = 4,
  kSucceededAfterAmbiguousResponse = 5,
  kUnchangedAfterAmbiguousResponse = 6,
  kRolledBackSuccessfully = 7,
  kRollbackFailedHardStop = 8,
  kMutationOutcomeUnknown = 9,
  kDurableCheckpointFailed = 10,
  kBusy = 11,
  kRecoveryRequired = 12,
  kReadbackFailed = 13,
  kContentMismatch = 14,
  kWriteSessionUnavailable = 15,
  kFinalRecordFailed = 16,
  kUnchangedAfterWrite = 17,
};

bool IsResolvedTransactionState(TransactionState state);

struct TransactionOutcome {
  TransactionState state = TransactionState::kPlanTargetMismatch;
  std::string message;
  bool write_attempted = false;
  bool write_mutation_outcome_ambiguous = false;
  bool readback_matched_proposed = false;
  bool rollback_attempted = false;
  bool rollback_used_fresh_session = false;
  bool rollback_write_succeeded = false;
  bool rollback_readback_matched_preimage = false;
};
struct TransactionRecord {
  std::string target;
  std::array<uint8_t, 32> preimage_sha256{};
  std::array<uint8_t, 32> proposed_sha256{};
  int64_t timestamp = 0;
  std::string transaction_id;
  TransactionState state = TransactionState::kMutationOutcomeUnknown;
  std::string verification_result;
  std::string rollback_state;
};
struct RecoveryScan {
  bool ok = false;
  std::vector<TransactionRecord> records;
  std::string error;
  bool AllowsNewTransaction() const { return ok && records.empty(); }
};
enum class BackupPersistState {
  kOk, kPersistFailed, kFsyncFailed, kRereadMismatchFailed,
};
struct BackupPersistResult {
  BackupPersistState state = BackupPersistState::kPersistFailed;
  std::string message;
};
enum class PendingRecordState { kOk, kFailed };
struct PendingRecordResult {
  PendingRecordState state = PendingRecordState::kFailed;
  std::string message;
};

class ITransactionLog {
 public:
  virtual ~ITransactionLog() = default;
  virtual BackupPersistResult PersistPreimageBackup(const std::string& id,
      const std::vector<uint8_t>& preimage) = 0;
  virtual PendingRecordResult RecordPending(const TransactionRecord& record) = 0;
  // A failed final commit must be observable, not silently discarded.
  virtual bool RecordTransaction(const TransactionRecord& record) = 0;
  // ok=false is an error/unknown state, never an empty, clean journal.
  virtual RecoveryScan LoadPendingTransactions() = 0;
  virtual std::optional<std::vector<uint8_t>> LoadBackup(const std::string& id) = 0;
};

// Host-only test double. Deliberately not a durable production backend.
class InMemoryTransactionLog : public ITransactionLog {
 public:
  BackupPersistResult PersistPreimageBackup(const std::string& id,
      const std::vector<uint8_t>& bytes) override {
    if (id.empty() || backups_.count(id)) return {};
    backups_.emplace(id, bytes);
    return {BackupPersistState::kOk, ""};
  }
  PendingRecordResult RecordPending(const TransactionRecord& r) override {
    if (!backups_.count(r.transaction_id) || pending_.count(r.transaction_id)) return {};
    pending_.emplace(r.transaction_id, r);
    return {PendingRecordState::kOk, ""};
  }
  bool RecordTransaction(const TransactionRecord& r) override {
    if (!pending_.count(r.transaction_id) || final_.count(r.transaction_id)) return false;
    final_.emplace(r.transaction_id, r);
    records_.push_back(r);
    return true;
  }
  RecoveryScan LoadPendingTransactions() override {
    RecoveryScan scan;
    scan.ok = true;
    for (const auto& [id, r] : pending_) {
      auto f = final_.find(id);
      if (f == final_.end()) scan.records.push_back(r);
      else if (!IsResolvedTransactionState(f->second.state)) scan.records.push_back(f->second);
    }
    for (const auto& entry : backups_) {
      if (!pending_.count(entry.first)) {
        scan.ok = false;
        scan.error = "orphan backup";
      }
    }
    return scan;
  }
  std::optional<std::vector<uint8_t>> LoadBackup(const std::string& id) override {
    auto it = backups_.find(id);
    if (it == backups_.end()) return std::nullopt;
    return it->second;
  }
  const std::vector<TransactionRecord>& Records() const { return records_; }
 private:
  std::map<std::string, std::vector<uint8_t>> backups_;
  std::map<std::string, TransactionRecord> pending_, final_;
  std::vector<TransactionRecord> records_;
};

enum class RollbackPolicy {
  kNever,
  // Explicit opt-in, only after a successful fresh read proves content mismatch.
  // Read failure never authorizes rollback, regardless of this policy.
  kRestorePreimageOnVerifiedMismatch,
};

// Process-wide nonblocking serialization covers recovery scan through final log.
// Owns each session in a separate scope: baseline read -> destroy -> checkpoint
// -> fresh write -> destroy -> fresh readback. No raw pre-existing client API.
// No retries. Default policy leaves mismatch pending for operator recovery.
// This is NOT caller authorization, mapping proof or a baseline allowlist.
TransactionOutcome ApplyPatchPlan(IEfsSessionFactory& sessions,
    EfsWriteTarget target, const PatchPlan& plan, const std::string& id,
    int64_t timestamp, ITransactionLog& log,
    RollbackPolicy rollback = RollbackPolicy::kNever);

}  // namespace guacamole_efs
