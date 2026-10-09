#ifdef GUACAMOLE_CARRIER_DEBUG
#include "auto_slot0_device.h"
#include "auto_feature_set.h"
#include "auto_experiment_journal.h"
#include "presence_diagnostics.h"
namespace guacamole_pdc {
namespace {
class DeviceAutoSlot0Operations final : public IAutoSlot0Operations {
 public:
  DeviceAutoSlot0Operations(IAutoExperimentState& state,const std::function<void(bool)>& permit)
      : state_(state),permit_(permit) {}
  GetSelectedConfigOutcome Selected() override {
    QcciShim vendor; PresenceQcciGuard guard(vendor); PdcClient client(guard);
    std::string log;
    if (!client.Init({{1,15,6}},&log)) return {};
    return client.GetSelectedConfig(ConfigType::kSoftware,5000);
  }
  FeatureReadOutcome Feature(FeatureSlot slot) override {
    QcciShim vendor; FeatureReadQcciGuard guard(vendor,slot); FeatureReadClient client(guard);
    return client.Read(slot,5000);
  }
  GetConfigInfoOutcome Info(CarrierTarget target) override {
    QcciShim vendor; PresenceQcciGuard guard(vendor); PdcClient client(guard);
    std::string log;
    if (!client.Init({{1,15,6}},&log)) return {};
    return client.GetConfigInfo(ConfigType::kSoftware,CarrierId(target),5000);
  }
  AutoFeatureSetResult EnableSlot0(const AutoSlot0Preflight& pre,const std::string& confirmation) override {
    QcciShim vendor; AutoFeatureSetQcciGuard guard(vendor);
    const bool armed=guard.Arm(pre,confirmation,state_);
    if (permit_) permit_(armed);
    if (!armed) return {};
    AutoFeatureSetClient client(guard);
    return client.EnableSlot0(5000);
    // client is destroyed/released before caller can start the post-read clients.
  }
 private:
  IAutoExperimentState& state_;
  const std::function<void(bool)>& permit_;
};
}
AutoSlot0Report RunDeviceAutoSlot0(const std::vector<std::string>& args,
    const std::function<void(const AutoSlot0Report&)>& before,
    const std::function<void(bool)>& permit) {
  if (!IsAutoSlot0Command(args)) return {}; // no storage/QCCI for wrong confirmation
  return RunAutoSlot0DeviceGate([&] {
    AutoExperimentJournal journal(OpenDeviceAutoExperimentDirectory(),0);
    DeviceAutoSlot0Operations ops(journal,permit);
    return RunAutoSlot0Experiment(ops,journal,args,before);
  });
}
}
#endif
