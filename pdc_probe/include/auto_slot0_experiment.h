#pragma once
#include "carrier_experiment.h"
#include "feature_readonly.h"
#include <array>
#include <functional>

namespace guacamole_pdc {
// B.4: experiment concluded. Device send is CLOSED regardless of marker state.
inline constexpr bool kDeviceAutoSlot0SendOpen = false;
inline constexpr char kAutoSlot0Confirmation[] = "GUACAMOLE_AUTO_SLOT0_ENABLE_EXPERIMENT_V1";
bool IsAutoSlot0Command(const std::vector<std::string>& args);
bool IsClosedAutoSlot1Command(const std::vector<std::string>& args);
struct AutoSlot0Preflight {
  GetSelectedConfigOutcome selected;
  std::array<FeatureReadOutcome,2> slots;
  std::array<GetConfigInfoOutcome,4> info;
};
bool AutoSlot0PreflightVerified(const AutoSlot0Preflight& pre);
struct AutoFeatureSetResult {
  ProbeStatus status = ProbeStatus::kInitFailed;
  bool mutation_attempted = false;
  bool ambiguous = false;
  AutoSyncResult sync;
  AutoIndicationResult indication;
};
class IAutoExperimentState {
 public:
  virtual ~IAutoExperimentState() = default;
  // Hold exclusive process lock until destruction. Any existing attempt or
  // unexpected state (including torn/corrupt files) blocks, never implies done.
  virtual bool AcquireClean() = 0;
  virtual bool RecordPending() = 0; // file + parent fsync, before permit
  virtual bool PendingDurable() const = 0;
};
class IAutoSlot0Operations {
 public:
  virtual ~IAutoSlot0Operations() = default;
  virtual GetSelectedConfigOutcome Selected() = 0;
  virtual FeatureReadOutcome Feature(FeatureSlot slot) = 0;
  virtual GetConfigInfoOutcome Info(CarrierTarget target) = 0;
  virtual AutoFeatureSetResult EnableSlot0(const AutoSlot0Preflight& pre,
                                          const std::string& confirmation) = 0;
};
struct AutoSlot0Report {
  std::string outcome = "EXPERIMENT_BLOCKED";
  std::string reason;
  std::string assessment = "NOT_ATTEMPTED";
  bool pre_observed = false, checkpoint_durable = false, post_observed = false;
  AutoSlot0Preflight pre;
  AutoFeatureSetResult set;
  std::array<FeatureReadOutcome,2> post_slots;
  GetSelectedConfigOutcome post_selected;
};
AutoSlot0Report RunAutoSlot0DeviceGate(const std::function<AutoSlot0Report()>& entry);
// Host simulation below retains historical protocol tests; device uses the closed gate.
// No EFS, Load, Select, Activate, disable, reboot or recovery method exists here.
AutoSlot0Report RunAutoSlot0Experiment(IAutoSlot0Operations& ops,
    IAutoExperimentState& state, const std::vector<std::string>& args,
    const std::function<void(const AutoSlot0Report&)>& before_mutation = {});
}
