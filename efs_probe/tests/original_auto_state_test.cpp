#include "original_auto_state.h"
#include "hash.h"
#include <gtest/gtest.h>
using namespace guacamole_efs;
namespace {
OriginalAutoPreimage Snapshot() {
  OriginalAutoPreimage value;
  for (auto target : kOriginalMcfgAutoStateTargets) {
    OriginalAutoFile file;
    file.target = target;
    file.full_read_verified = true;
    file.bytes.assign((target == EfsTarget::kMcfgSetting ||
                       target == EfsTarget::kMcfgSetting1) ? 41 : 1, 0x17);
    file.size = file.bytes.size();
    file.sha256 = Sha256(file.bytes.data(), file.bytes.size());
    file.capture_id = "original-capture";
    file.provenance = "verified read-only original before manual selection";
    value.files.push_back(file);
  }
  return value;
}
}
TEST(OriginalAutoState, AllFourExactRequired) {
  auto original = Snapshot();
  EXPECT_EQ(CompareOriginalAutoState(original, original),
            AutoSelectionState::kOriginalBaselineMatch);
  for (size_t i = 0; i < 4; ++i) {
    auto changed = original;
    changed.files[i].bytes[0] ^= 1;
    changed.files[i].sha256 = Sha256(changed.files[i].bytes.data(), changed.files[i].bytes.size());
    EXPECT_EQ(CompareOriginalAutoState(original, changed),
              AutoSelectionState::kChangedFromOriginal);
  }
}
TEST(OriginalAutoState, MissingOrUnknownWinsOverChanged) {
  auto original = Snapshot();
  auto changed = original;
  changed.files[0].bytes[0] ^= 1;
  changed.files[0].sha256 = Sha256(changed.files[0].bytes.data(), changed.files[0].bytes.size());
  changed.files.pop_back();
  EXPECT_EQ(CompareOriginalAutoState(original, changed), AutoSelectionState::kUnknown);
}
TEST(OriginalAutoState, IntegrityAndProvenanceRequired) {
  auto original = Snapshot();
  auto bad = original;
  bad.files[0].sha256[0] ^= 1;
  EXPECT_FALSE(ValidateOriginalAutoPreimage(bad));
  bad = original; bad.files[0].provenance.clear();
  EXPECT_FALSE(ValidateOriginalAutoPreimage(bad));
  bad = original; bad.files[0].capture_id = "different";
  EXPECT_FALSE(ValidateOriginalAutoPreimage(bad));
  bad = original; bad.files[0].size = 2;
  EXPECT_FALSE(ValidateOriginalAutoPreimage(bad));
  bad = original; bad.files[0].full_read_verified = false;
  EXPECT_FALSE(ValidateOriginalAutoPreimage(bad));
  bad = original; bad.files[0] = bad.files[1];
  EXPECT_FALSE(ValidateOriginalAutoPreimage(bad));
}
TEST(OriginalAutoState, CommonAndRebootDoNotRestoreOrAssignByteSemantics) {
  auto original = Snapshot();
  auto changed = original;
  changed.files[0].bytes[0] = 0x80;
  changed.files[0].sha256 = Sha256(changed.files[0].bytes.data(), 1);
  // No PDC selection or reboot input can override exact-byte evidence.
  EXPECT_EQ(CompareOriginalAutoState(original, changed), AutoSelectionState::kChangedFromOriginal);
  EXPECT_FALSE(kAutoRestorePathOpen);
  EXPECT_STREQ(kAutoRestoreProtocol, "AUTO_FEATURE_PERSISTENT_PRE_SKT_EFS_NOT_RESTORED");
}

namespace {
OriginalAutoPreimage ChangedSnapshot() {
  auto value = Snapshot();
  value.files[0].bytes[0] ^= 1;
  value.files[0].sha256 = Sha256(value.files[0].bytes.data(), value.files[0].bytes.size());
  return value;
}
AutoExperimentObservation AcceptedObservation() {
  AutoExperimentObservation o;
  o.pre_selection = o.post_selection = PdcSelectionState::kCommon;
  o.pre_pending_none = o.post_pending_none = true;
  o.protocol_accepted = true;
  return o;
}
}
TEST(AutoRestoreExperiment, ProtocolSuccessAndCommonAreNotRestoration) {
  auto original = Snapshot();
  auto changed = ChangedSnapshot();
  auto observation = AcceptedObservation();
  observation.post_reboot_verified = true;
  EXPECT_EQ(AssessAutoRestoreExperiment(original, changed, changed, observation),
            AutoExperimentOutcome::kProtocolAcceptedButOriginalNotRestored);
}
TEST(AutoRestoreExperiment, AllFourExactAndRebootEvidenceRequired) {
  auto original = Snapshot();
  auto changed = ChangedSnapshot();
  auto observation = AcceptedObservation();
  EXPECT_EQ(AssessAutoRestoreExperiment(original, changed, original, observation),
            AutoExperimentOutcome::kAwaitingRebootVerification);
  observation.post_reboot_verified = true;
  EXPECT_EQ(AssessAutoRestoreExperiment(original, changed, original, observation),
            AutoExperimentOutcome::kAutoRestoreConfirmed);
  for (size_t i = 0; i < original.files.size(); ++i) {
    auto missing = original;
    missing.files.erase(missing.files.begin() + i);
    EXPECT_EQ(AssessAutoRestoreExperiment(original, changed, missing, observation),
              AutoExperimentOutcome::kReviewRequired);
  }
}
TEST(AutoRestoreExperiment, UnknownPrestateOrSelectionCannotConfirm) {
  auto original = Snapshot();
  auto changed = ChangedSnapshot();
  auto observation = AcceptedObservation();
  observation.post_reboot_verified = true;
  EXPECT_EQ(AssessAutoRestoreExperiment(original, original, original, observation),
            AutoExperimentOutcome::kReviewRequired);
  observation.post_selection = PdcSelectionState::kSkt;
  EXPECT_EQ(AssessAutoRestoreExperiment(original, changed, original, observation),
            AutoExperimentOutcome::kReviewRequired);
  observation = AcceptedObservation(); observation.post_pending_none = false;
  EXPECT_EQ(AssessAutoRestoreExperiment(original, changed, original, observation),
            AutoExperimentOutcome::kReviewRequired);
  observation = AcceptedObservation(); observation.protocol_accepted = false;
  EXPECT_EQ(AssessAutoRestoreExperiment(original, changed, original, observation),
            AutoExperimentOutcome::kReviewRequired);
  EXPECT_FALSE(kAutoRestorePathOpen);
}

TEST(OriginalAutoState, B4PersistentHybridEffectsDoNotMatchPreSktStockBaseline) {
  // Synthetic complete fixtures encode reported deltas, not actual raw captures.
  auto pre_skt = Snapshot();
  for (auto& f : pre_skt.files) {
    if (f.bytes.size()==1) f.bytes[0]=0x05;
    else { f.bytes[4]=3; f.bytes[5]=0; f.bytes[8]=1; }
    f.sha256=Sha256(f.bytes.data(),f.bytes.size());
  }
  auto immediate=pre_skt;
  immediate.files[0].bytes[0]=0x87;
  immediate.files[1].bytes[0]=0x80;
  immediate.files[2].bytes[4]=3;immediate.files[2].bytes[5]=1;immediate.files[2].bytes[8]=0;
  immediate.files[3].bytes[4]=0;immediate.files[3].bytes[5]=0;immediate.files[3].bytes[8]=0;
  for (auto& f:immediate.files) f.sha256=Sha256(f.bytes.data(),f.bytes.size());
  auto rebooted=immediate;
  rebooted.files[3].bytes=rebooted.files[2].bytes;
  rebooted.files[3].sha256=Sha256(rebooted.files[3].bytes.data(),rebooted.files[3].bytes.size());
  EXPECT_EQ(CompareOriginalAutoState(pre_skt,immediate),AutoSelectionState::kChangedFromOriginal);
  EXPECT_EQ(CompareOriginalAutoState(pre_skt,rebooted),AutoSelectionState::kChangedFromOriginal);
  auto observation=AcceptedObservation();observation.post_reboot_verified=true;
  EXPECT_EQ(AssessAutoRestoreExperiment(pre_skt,immediate,rebooted,observation),
            AutoExperimentOutcome::kProtocolAcceptedButOriginalNotRestored);
  EXPECT_FALSE(kAutoRestorePathOpen);
}
