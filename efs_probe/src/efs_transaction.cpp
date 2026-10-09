#include <utility>
#include "efs_transaction.h"
#include "process_efs_executor.h"

#include <atomic>

#include "efs_reader.h"
#include "efs_writer.h"
#include "hash.h"

namespace guacamole_efs {

PatchPlanBuildResult BuildPatchPlan(
    EfsTarget target, const std::vector<uint8_t>& preimage_bytes,
    const std::vector<uint8_t>& proposed_bytes) {
  PatchPlan plan;
  plan.target = target;

  plan.preimage.target = target;
  plan.preimage.bytes = preimage_bytes;
  plan.preimage.sha256 =
      Sha256(preimage_bytes.data(), preimage_bytes.size());

  plan.proposed.bytes = proposed_bytes;
  plan.proposed.sha256 =
      Sha256(proposed_bytes.data(), proposed_bytes.size());

  plan.rollback.bytes = preimage_bytes;  // exact copy, by construction.
  plan.rollback.sha256 = plan.preimage.sha256;

  PatchPlanBuildResult result;
  result.state = PatchPlanBuildState::kOk;
  result.plan = std::move(plan);
  return result;
}

VerificationResult VerifyDigest(const std::vector<uint8_t>& actual_bytes,
                                 const std::array<uint8_t, 32>& expected_sha256) {
  auto actual = Sha256(actual_bytes.data(), actual_bytes.size());
  VerificationResult r;
  if (actual == expected_sha256) {
    r.outcome = VerificationOutcome::kMatch;
  } else {
    r.outcome = VerificationOutcome::kMismatch;
    r.message = "sha256 mismatch";
  }
  return r;
}

VerificationResult VerifyPatchPlanConsistency(const PatchPlan& plan) {
  auto pre_check = VerifyDigest(plan.preimage.bytes, plan.preimage.sha256);
  if (pre_check.outcome != VerificationOutcome::kMatch) {
    return {VerificationOutcome::kMismatch,
            "preimage digest inconsistent with preimage bytes"};
  }
  auto prop_check = VerifyDigest(plan.proposed.bytes, plan.proposed.sha256);
  if (prop_check.outcome != VerificationOutcome::kMatch) {
    return {VerificationOutcome::kMismatch,
            "proposed digest inconsistent with proposed bytes"};
  }
  if (plan.rollback.bytes != plan.preimage.bytes) {
    return {VerificationOutcome::kMismatch,
            "rollback bytes != preimage bytes"};
  }
  if (plan.rollback.sha256 != plan.preimage.sha256) {
    return {VerificationOutcome::kMismatch,
            "rollback digest != preimage digest"};
  }
  return {VerificationOutcome::kMatch, ""};
}

VerificationResult VerifyPreimageStillCurrent(
    const PatchPlan& plan, const std::vector<uint8_t>& current_bytes) {
  auto check = VerifyDigest(current_bytes, plan.preimage.sha256);
  if (check.outcome != VerificationOutcome::kMatch) {
    return {VerificationOutcome::kMismatch,
            "current target bytes no longer match the plan's preimage - "
            "refuse to apply"};
  }
  if (current_bytes != plan.preimage.bytes) {
    // Should be unreachable if the digest matched - a SHA-256 collision
    // would be required - but checked explicitly anyway, never trust
    // the digest alone for something this consequential.
    return {VerificationOutcome::kMismatch,
            "current bytes differ from preimage despite matching digest "
            "(unreachable in practice)"};
  }
  return {VerificationOutcome::kMatch, ""};
}

bool IsResolvedTransactionState(TransactionState state) {
  switch (state) {
    case TransactionState::kSucceeded:
    case TransactionState::kSucceededAfterAmbiguousResponse:
    case TransactionState::kUnchangedAfterAmbiguousResponse:
    case TransactionState::kUnchangedAfterWrite:
    case TransactionState::kRolledBackSuccessfully:
    case TransactionState::kWriteSessionUnavailable:
      return true;
    default:
      return false;
  }
}

namespace {
std::atomic<bool> gMutationActive{false};

EfsReadFileResult ReadFresh(IEfsSessionFactory& sessions, const std::string& path) {
  std::string error;
  auto session = sessions.OpenRead(&error);
  if (!session) {
    EfsReadFileResult failed;
    failed.message = "fresh read session unavailable: " + error;
    return failed;
  }
  // session is destroyed (including callback quiescence/Deinit) on return.
  return ReadWholeFile(&session->client(), path);
}
bool Matches(const EfsReadFileResult& read, const std::vector<uint8_t>& bytes,
    const std::array<uint8_t, 32>& digest) {
  return read.state == EfsReadFileState::kDone && read.data == bytes &&
      VerifyDigest(read.data, digest).outcome == VerificationOutcome::kMatch;
}
}  // namespace

TransactionOutcome ApplyPatchPlan(IEfsSessionFactory& sessions,
    EfsWriteTarget write_target, const PatchPlan& plan,
    const std::string& transaction_id, int64_t timestamp, ITransactionLog& log,
    RollbackPolicy rollback) {
  TransactionOutcome out;
  bool expected_idle = false;
  if (!gMutationActive.compare_exchange_strong(expected_idle, true)) {
    out.state = TransactionState::kBusy;
    out.message = "another transaction is in progress; no I/O attempted";
    return out;
  }
  struct MutationLease {
    ~MutationLease() { gMutationActive.store(false); }
  } mutation_lease;
  const std::string& path = TargetPath(write_target);
  if (path.empty() || CorrespondingReadTarget(write_target) != plan.target ||
      plan.preimage.target != plan.target) {
    out.state = TransactionState::kPlanTargetMismatch;
    out.message = "plan and write target disagree";
    return out;
  }
  auto consistency = VerifyPatchPlanConsistency(plan);
  if (transaction_id.empty() || timestamp < 0 ||
      consistency.outcome != VerificationOutcome::kMatch ||
      plan.proposed.bytes.empty()) {
    out.state = TransactionState::kPlanInconsistent;
    out.message = "invalid id/time/plan or empty proposed image";
    return out;
  }
  auto recovery = log.LoadPendingTransactions();
  if (!recovery.AllowsNewTransaction()) {
    out.state = TransactionState::kRecoveryRequired;
    out.message = "journal requires recovery; no DIAG I/O: " + recovery.error;
    return out;
  }
  EfsReadFileResult current = ReadFresh(sessions, path);
  if (current.state != EfsReadFileState::kDone) {
    out.state = TransactionState::kReadCurrentFailed;
    out.message = current.message;
    return out;
  }
  if (VerifyPreimageStillCurrent(plan, current.data).outcome != VerificationOutcome::kMatch) {
    out.state = TransactionState::kPreimageMismatch;
    out.message = "live baseline changed; no mutation";
    return out;
  }
  auto backup = log.PersistPreimageBackup(transaction_id, current.data);
  if (backup.state != BackupPersistState::kOk) {
    out.state = TransactionState::kDurableCheckpointFailed;
    out.message = backup.message;
    return out;
  }
  TransactionRecord record;
  record.transaction_id = transaction_id;
  record.target = path;
  record.timestamp = timestamp;
  record.preimage_sha256 = plan.preimage.sha256;
  record.proposed_sha256 = plan.proposed.sha256;
  record.state = TransactionState::kMutationOutcomeUnknown;
  record.verification_result = "PENDING; durable backup verified, before mutation";
  auto pending = log.RecordPending(record);
  if (pending.state != PendingRecordState::kOk) {
    out.state = TransactionState::kDurableCheckpointFailed;
    out.message = pending.message;
    return out;
  }
  auto finish = [&]() {
    record.state = out.state;
    record.verification_result = out.message;
    record.rollback_state = out.rollback_attempted
        ? (out.rollback_readback_matched_preimage ? "verified preimage" : "unresolved") : "";
    if (!log.RecordTransaction(record)) {
      out.state = TransactionState::kFinalRecordFailed;
      out.message = "final record not durably committed; recovery required";
    }
    return out;
  };
  {
    std::string error;
    auto session = sessions.OpenWrite(&error);
    if (!session) {
      out.state = TransactionState::kWriteSessionUnavailable;
      out.message = "no mutation attempted: " + error;
      return finish();
    }
    out.write_attempted = true;
    auto write = WriteWholeFile(&session->client(), write_target, plan.proposed.bytes);
    out.write_mutation_outcome_ambiguous = write.mutation_outcome_ambiguous;
  }  // Write client and DIAG session MUST die before readback opens.
  auto readback = ReadFresh(sessions, path);
  if (readback.state != EfsReadFileState::kDone) {
    out.state = TransactionState::kReadbackFailed;
    out.message = "fresh readback failed; no rollback/retry: " + readback.message;
    return finish();
  }
  if (Matches(readback, plan.proposed.bytes, plan.proposed.sha256)) {
    out.state = out.write_mutation_outcome_ambiguous
        ? TransactionState::kSucceededAfterAmbiguousResponse : TransactionState::kSucceeded;
    out.readback_matched_proposed = true;
    out.message = "fresh readback matches proposed bytes and SHA-256";
    return finish();
  }
  if (Matches(readback, plan.preimage.bytes, plan.preimage.sha256)) {
    out.state = out.write_mutation_outcome_ambiguous
        ? TransactionState::kUnchangedAfterAmbiguousResponse : TransactionState::kUnchangedAfterWrite;
    out.message = "fresh readback confirms preimage unchanged; no rollback";
    return finish();
  }
  out.state = TransactionState::kContentMismatch;
  out.message = "fresh read succeeded but content matches neither image";
  if (rollback != RollbackPolicy::kRestorePreimageOnVerifiedMismatch) return finish();

  // Explicitly authorized recovery after a successful read only. Exactly one
  // new write session, then a separate new read session; no same-session reuse.
  {
    std::string error;
    auto session = sessions.OpenWrite(&error);
    if (!session) {
      out.message += "; fresh recovery session unavailable: " + error;
      return finish();
    }
    out.rollback_attempted = true;
    out.rollback_used_fresh_session = true;
    auto restored = WriteWholeFile(&session->client(), write_target, plan.rollback.bytes);
    out.rollback_write_succeeded = restored.state == EfsWriteFileState::kDone;
  }
  auto restored = ReadFresh(sessions, path);
  out.rollback_readback_matched_preimage = Matches(restored, plan.rollback.bytes,
      plan.rollback.sha256);
  out.state = out.rollback_readback_matched_preimage
      ? TransactionState::kRolledBackSuccessfully : TransactionState::kRollbackFailedHardStop;
  out.message = out.rollback_readback_matched_preimage
      ? "fresh recovery readback verified preimage" : "recovery did not verify; HARD_STOP";
  return finish();
}

namespace {
bool ProcessReadOk(const ProcessResult& r) {
  return r.spawned && r.reaped && !r.timed_out && r.exit_code==0 && r.signal==0 &&
      r.report_valid && r.report.teardown_completed && r.report.outcome==ProcessOutcome::kConfirmed &&
      r.report.failure==ProcessFailure::kNone &&
      VerifyDigest(r.report.bytes,r.report.sha256).outcome==VerificationOutcome::kMatch;
}
bool ProcessMatches(const ProcessResult& r,const std::vector<uint8_t>& bytes,
    const std::array<uint8_t,32>& hash) {
  return ProcessReadOk(r) && r.report.bytes==bytes && r.report.sha256==hash;
}
}
TransactionOutcome ApplyProcessPatchPlan(IProcessEfsExecutor& executor,
    EfsWriteTarget write_target, const PatchPlan& plan,
    const std::string& transaction_id, int64_t timestamp, ITransactionLog& log,
    RollbackPolicy rollback) {
  TransactionOutcome out;
  bool expected_idle = false;
  if (!gMutationActive.compare_exchange_strong(expected_idle, true)) {
    out.state = TransactionState::kBusy;
    out.message = "another transaction is in progress; no I/O attempted";
    return out;
  }
  struct MutationLease {
    ~MutationLease() { gMutationActive.store(false); }
  } mutation_lease;
  const std::string& path = TargetPath(write_target);
  if (path.empty() || CorrespondingReadTarget(write_target) != plan.target ||
      plan.preimage.target != plan.target) {
    out.state = TransactionState::kPlanTargetMismatch;
    out.message = "plan and write target disagree";
    return out;
  }
  auto consistency = VerifyPatchPlanConsistency(plan);
  if (transaction_id.empty() || timestamp < 0 ||
      consistency.outcome != VerificationOutcome::kMatch ||
      plan.proposed.bytes.empty()) {
    out.state = TransactionState::kPlanInconsistent;
    out.message = "invalid id/time/plan or empty proposed image";
    return out;
  }
  auto recovery = log.LoadPendingTransactions();
  if (!recovery.AllowsNewTransaction()) {
    out.state = TransactionState::kRecoveryRequired;
    out.message = "journal requires recovery; no DIAG I/O: " + recovery.error;
    return out;
  }
  auto current = executor.ReadTarget(plan.target, transaction_id);
  if (!ProcessReadOk(current)) {
    out.state = TransactionState::kReadCurrentFailed;
    out.message = current.error;
    return out;
  }
  if (VerifyPreimageStillCurrent(plan, current.report.bytes).outcome != VerificationOutcome::kMatch) {
    out.state = TransactionState::kPreimageMismatch;
    out.message = "live baseline changed; no mutation";
    return out;
  }
  auto backup = log.PersistPreimageBackup(transaction_id, current.report.bytes);
  if (backup.state != BackupPersistState::kOk) {
    out.state = TransactionState::kDurableCheckpointFailed;
    out.message = backup.message;
    return out;
  }
  TransactionRecord record;
  record.transaction_id = transaction_id;
  record.target = path;
  record.timestamp = timestamp;
  record.preimage_sha256 = plan.preimage.sha256;
  record.proposed_sha256 = plan.proposed.sha256;
  record.state = TransactionState::kMutationOutcomeUnknown;
  record.verification_result = "PENDING; durable backup verified, before mutation";
  auto pending = log.RecordPending(record);
  if (pending.state != PendingRecordState::kOk) {
    out.state = TransactionState::kDurableCheckpointFailed;
    out.message = pending.message;
    return out;
  }
  auto finish = [&]() {
    record.state = out.state;
    record.verification_result = out.message;
    record.rollback_state = out.rollback_attempted
        ? (out.rollback_readback_matched_preimage ? "verified preimage" : "unresolved") : "";
    if (!log.RecordTransaction(record)) {
      out.state = TransactionState::kFinalRecordFailed;
      out.message = "final record not durably committed; recovery required";
    }
    return out;
  };
  auto write = executor.WriteTarget(write_target, plan.proposed.bytes, transaction_id);
  out.write_attempted = write.spawned;
  out.write_mutation_outcome_ambiguous = write.report.outcome == ProcessOutcome::kAmbiguous;
  if (!write.spawned) {
    out.state=TransactionState::kWriteSessionUnavailable;
    out.message="device writer CLOSED/unavailable; no mutation attempted";
    return finish();
  }
  if (!write.reaped) {
    out.state=TransactionState::kMutationOutcomeUnknown;
    out.message="write worker not reaped; HARD_STOP, no further worker";
    return finish();
  }
  auto readback = executor.ReadTarget(plan.target, transaction_id);
  if (!ProcessReadOk(readback)) {
    out.state = TransactionState::kReadbackFailed;
    out.message = "fresh readback failed; no rollback/retry: " + readback.error;
    return finish();
  }
  if (ProcessMatches(readback, plan.proposed.bytes, plan.proposed.sha256)) {
    out.state = out.write_mutation_outcome_ambiguous
        ? TransactionState::kSucceededAfterAmbiguousResponse : TransactionState::kSucceeded;
    out.readback_matched_proposed = true;
    out.message = "fresh readback matches proposed bytes and SHA-256";
    return finish();
  }
  if (ProcessMatches(readback, plan.preimage.bytes, plan.preimage.sha256)) {
    out.state = out.write_mutation_outcome_ambiguous
        ? TransactionState::kUnchangedAfterAmbiguousResponse : TransactionState::kUnchangedAfterWrite;
    out.message = "fresh readback confirms preimage unchanged; no rollback";
    return finish();
  }
  out.state = TransactionState::kContentMismatch;
  out.message = "fresh read succeeded but content matches neither image";
  if (rollback != RollbackPolicy::kRestorePreimageOnVerifiedMismatch) return finish();

  // Explicitly authorized recovery after a successful read only. Exactly one
  // new write session, then a separate new read session; no same-session reuse.
  auto recovery_write=executor.WriteTarget(write_target, plan.rollback.bytes, transaction_id);
  out.rollback_attempted=recovery_write.spawned;
  out.rollback_used_fresh_session=recovery_write.spawned;
  out.rollback_write_succeeded=recovery_write.report_valid && recovery_write.report.outcome==ProcessOutcome::kConfirmed;
  if (!recovery_write.spawned || !recovery_write.reaped) {
    out.state=TransactionState::kRollbackFailedHardStop;
    out.message="recovery worker unavailable/not reaped; HARD_STOP";
    return finish();
  }
  auto restored = executor.ReadTarget(plan.target, transaction_id);
  out.rollback_readback_matched_preimage = ProcessMatches(restored, plan.rollback.bytes,
      plan.rollback.sha256);
  out.state = out.rollback_readback_matched_preimage
      ? TransactionState::kRolledBackSuccessfully : TransactionState::kRollbackFailedHardStop;
  out.message = out.rollback_readback_matched_preimage
      ? "fresh recovery readback verified preimage" : "recovery did not verify; HARD_STOP";
  return finish();
}

}  // namespace guacamole_efs
