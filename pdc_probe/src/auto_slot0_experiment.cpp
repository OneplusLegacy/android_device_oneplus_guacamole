#include "auto_slot0_experiment.h"
namespace guacamole_pdc {
namespace {
bool CommonIdle(const GetSelectedConfigOutcome& o) {
  return o.status == ProbeStatus::kOk && o.data.ok && !o.data.error_status && !o.data.error_code &&
      o.data.active_id == CarrierId(CarrierTarget::kCommon) && !o.data.pending_id;
}
bool GoodFeature(const FeatureReadOutcome& o) {
  return o.status == ProbeStatus::kOk && !o.sync_result && !o.sync_error &&
      o.data.valid && !o.data.error;
}
bool ExactInfo(CarrierTarget t, const GetConfigInfoOutcome& o) {
  return CarrierInfoVerified(t,o); // canonical exact metadata for every profile
}
}
bool IsAutoSlot0Command(const std::vector<std::string>& a) {
  return a == std::vector<std::string>{"enable-auto-selection-slot0-once","--confirm",kAutoSlot0Confirmation};
}
bool IsClosedAutoSlot1Command(const std::vector<std::string>& a) {
  return a == std::vector<std::string>{"enable-auto-selection-slot1-once","--confirm",
      "GUACAMOLE_AUTO_SLOT1_ENABLE_EXPERIMENT_V1"};
}
bool AutoSlot0PreflightVerified(const AutoSlot0Preflight& p) {
  if (!CommonIdle(p.selected) || !GoodFeature(p.slots[0]) || !GoodFeature(p.slots[1]) ||
      p.slots[0].data.selection_mode != uint32_t(SelectionMode::kDisable) ||
      p.slots[0].data.flex_mapping != false) return false;
  constexpr CarrierTarget targets[]{CarrierTarget::kCommon,CarrierTarget::kSkt,CarrierTarget::kKt,CarrierTarget::kLgu};
  for (size_t i=0; i<4; ++i) if (!ExactInfo(targets[i],p.info[i])) return false;
  return true;
}
AutoSlot0Report RunAutoSlot0DeviceGate(const std::function<AutoSlot0Report()>& entry) {
  if (!kDeviceAutoSlot0SendOpen || !entry) {
    AutoSlot0Report r;
    r.outcome="AUTO_EXPERIMENT_CLOSED";
    r.reason="B.4 concluded: preserve slot0.pending permanently; no retry/disable/slot1";
    return r;
  }
  return entry();
}
AutoSlot0Report RunAutoSlot0Experiment(IAutoSlot0Operations& ops, IAutoExperimentState& state,
    const std::vector<std::string>& args, const std::function<void(const AutoSlot0Report&)>& before) {
  AutoSlot0Report r;
  if (!IsAutoSlot0Command(args)) { r.reason="exact slot0 confirmation required"; return r; }
  if (!state.AcquireClean()) { r.reason="experimental state unavailable/busy/unresolved; no retry or clearing"; return r; }
  r.pre_observed=true;
  r.pre.selected=ops.Selected();
  r.pre.slots[0]=ops.Feature(FeatureSlot::kSlot0);
  r.pre.slots[1]=ops.Feature(FeatureSlot::kSlot1);
  constexpr CarrierTarget targets[]{CarrierTarget::kCommon,CarrierTarget::kSkt,CarrierTarget::kKt,CarrierTarget::kLgu};
  for (size_t i=0; i<4; ++i) r.pre.info[i]=ops.Info(targets[i]);
  if (!AutoSlot0PreflightVerified(r.pre)) { r.reason="same-invocation preflight failed"; return r; }
  if (!state.RecordPending() || !state.PendingDurable()) {
    r.reason="durable pending checkpoint failed; any partial marker requires review"; return r;
  }
  r.checkpoint_durable=true;
  if (before) before(r);
  r.set=ops.EnableSlot0(r.pre,args[2]); // Exactly one operation; no retry/disable fallback.
  if (!r.set.mutation_attempted) {
    r.reason="SetFeature not attempted; persistent checkpoint still requires review"; return r;
  }
  // Each real operation constructs/releases a fresh independent QCCI client.
  r.post_slots[0]=ops.Feature(FeatureSlot::kSlot0);
  r.post_slots[1]=ops.Feature(FeatureSlot::kSlot1);
  r.post_selected=ops.Selected();
  r.post_observed=true;
  r.outcome="USER_REVIEW_REQUIRED";
  const bool accepted=r.set.status==ProbeStatus::kOk && !r.set.ambiguous &&
      r.set.sync.valid && !r.set.sync.result && !r.set.sync.error &&
      r.set.indication.valid && !r.set.indication.error;
  if (accepted && GoodFeature(r.post_slots[0]) &&
      r.post_slots[0].data.selection_mode==uint32_t(SelectionMode::kHybrid) &&
      r.post_slots[0].data.flex_mapping==true && CommonIdle(r.post_selected))
    r.assessment="AUTO_SLOT0_FEATURE_ENABLE_CONFIRMED";
  else if (accepted) r.assessment="PROTOCOL_ACCEPTED_UNEXPECTED_STATE";
  else r.assessment=r.set.ambiguous ? "AUTO_SET_AMBIGUOUS_REVIEW_REQUIRED" : "REVIEW_REQUIRED";
  r.reason="no EFS restore claim; preserve post observations and pending marker; stop for human review";
  return r;
}
}
