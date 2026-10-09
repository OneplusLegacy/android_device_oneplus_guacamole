#include "original_auto_state.h"

#include "hash.h"

namespace guacamole_efs {
namespace {
const OriginalAutoFile* Find(const OriginalAutoPreimage& value, EfsTarget target) {
  const OriginalAutoFile* found = nullptr;
  for (const auto& file : value.files) {
    if (file.target == target) {
      if (found) return nullptr;
      found = &file;
    }
  }
  return found;
}
}

bool ValidateOriginalAutoPreimage(const OriginalAutoPreimage& preimage) {
  if (preimage.files.size() != kOriginalMcfgAutoStateTargets.size()) return false;
  std::string capture;
  for (const auto target : kOriginalMcfgAutoStateTargets) {
    const auto* file = Find(preimage, target);
    if (!file || !file->full_read_verified || file->provenance.empty() ||
        file->capture_id.empty()) return false;
    if (capture.empty()) capture = file->capture_id;
    if (capture != file->capture_id) return false;
    const size_t expected = (target == EfsTarget::kMcfgSetting ||
                            target == EfsTarget::kMcfgSetting1) ? 41 : 1;
    if (file->size != expected || file->bytes.size() != expected ||
        file->sha256 != Sha256(file->bytes.data(), file->bytes.size())) return false;
  }
  return true;
}

AutoSelectionState CompareOriginalAutoState(
    const OriginalAutoPreimage& original,
    const OriginalAutoPreimage& observation) {
  if (!ValidateOriginalAutoPreimage(original) ||
      !ValidateOriginalAutoPreimage(observation)) return AutoSelectionState::kUnknown;
  for (const auto target : kOriginalMcfgAutoStateTargets) {
    if (Find(original, target)->bytes != Find(observation, target)->bytes)
      return AutoSelectionState::kChangedFromOriginal;
  }
  return AutoSelectionState::kOriginalBaselineMatch;
}
AutoExperimentOutcome AssessAutoRestoreExperiment(const OriginalAutoPreimage& original,
    const OriginalAutoPreimage& before, const OriginalAutoPreimage& after,
    const AutoExperimentObservation& o) {
  if (o.pre_selection != PdcSelectionState::kCommon || !o.pre_pending_none ||
      CompareOriginalAutoState(original,before) != AutoSelectionState::kChangedFromOriginal ||
      !o.protocol_accepted || o.post_selection != PdcSelectionState::kCommon || !o.post_pending_none)
    return AutoExperimentOutcome::kReviewRequired;
  const auto match = CompareOriginalAutoState(original,after);
  if (match == AutoSelectionState::kUnknown) return AutoExperimentOutcome::kReviewRequired;
  if (match == AutoSelectionState::kChangedFromOriginal)
    return AutoExperimentOutcome::kProtocolAcceptedButOriginalNotRestored;
  return o.post_reboot_verified ? AutoExperimentOutcome::kAutoRestoreConfirmed
      : AutoExperimentOutcome::kAwaitingRebootVerification;
}
}  // namespace guacamole_efs
