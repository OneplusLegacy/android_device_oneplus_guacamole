#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "efs_targets.h"

namespace guacamole_efs {

// "Original" in these legacy API names means the saved pre-SKT stock EFS
// baseline; its GetFeature state was never measured. It is not a HYBRID schema.
// Selection and exact preimage equality are independent facts. Neither byte
// value (including 05/80) nor Common selection encodes an AUTO semantic here.
enum class PdcSelectionState { kCommon, kSkt, kKt, kLgu, kUnknown };
enum class AutoSelectionState {
  kOriginalBaselineMatch, kChangedFromOriginal, kUnknown
};
enum class OriginalRestoreState {
  kNotStarted, kPdcOnlyRestored, kEfsPreimageRestored,
  kVerifiedAfterReboot, kUnknown, kHardStop
};
inline constexpr std::array<EfsTarget, 4> kOriginalMcfgAutoStateTargets = {
    EfsTarget::kMcfgAutoselectByUim,
    EfsTarget::kMcfgAutoselectByUimSubscription01,
    EfsTarget::kMcfgSetting, EfsTarget::kMcfgSetting1};

// These targets are observation/backup targets, NOT an EFS write allowlist.
struct OriginalAutoFile {
  EfsTarget target = EfsTarget::kMcfgDir;
  bool full_read_verified = false;
  std::vector<uint8_t> bytes;
  uint64_t size = 0;
  std::array<uint8_t, 32> sha256{};
  std::string capture_id;
  std::string provenance;
  std::optional<uint64_t> timestamp_unix_seconds;
};
struct OriginalAutoPreimage {
  std::vector<OriginalAutoFile> files;
};

// Validates all four exact targets, full reads, sizes, hashes, provenance and
// capture identity before accepting a candidate for durable transaction backup.
bool ValidateOriginalAutoPreimage(const OriginalAutoPreimage& preimage);
AutoSelectionState CompareOriginalAutoState(
    const OriginalAutoPreimage& original,
    const OriginalAutoPreimage& observation);
enum class AutoExperimentOutcome {
  kReviewRequired, kProtocolAcceptedButOriginalNotRestored,
  kAwaitingRebootVerification, kAutoRestoreConfirmed
};
struct AutoExperimentObservation {
  PdcSelectionState pre_selection = PdcSelectionState::kUnknown;
  PdcSelectionState post_selection = PdcSelectionState::kUnknown;
  bool pre_pending_none = false, post_pending_none = false;
  bool protocol_accepted = false;
  bool post_reboot_verified = false;
};
// Evidence evaluator only: no I/O, no restore operation and no send capability.
AutoExperimentOutcome AssessAutoRestoreExperiment(const OriginalAutoPreimage& original,
    const OriginalAutoPreimage& before, const OriginalAutoPreimage& after,
    const AutoExperimentObservation& observation);
// B.4 observed persistent slot0 feature enable, NOT restoration of the pre-SKT
// stock EFS baseline. Legacy symbol name retained; path remains CLOSED.
inline constexpr const char* kAutoRestoreProtocol = "AUTO_FEATURE_PERSISTENT_PRE_SKT_EFS_NOT_RESTORED";
inline constexpr bool kAutoRestorePathOpen = false;

}  // namespace guacamole_efs
