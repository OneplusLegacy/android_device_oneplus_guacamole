#pragma once
#include "auto_slot0_experiment.h"
namespace guacamole_pdc::auto_test {
inline GetSelectedConfigOutcome Common() {
  GetSelectedConfigOutcome o;o.status=ProbeStatus::kOk;o.data.ok=true;
  o.data.active_id=CarrierId(CarrierTarget::kCommon);return o;
}
inline FeatureReadOutcome Feature(uint32_t mode=0,bool flex=false) {
  FeatureReadOutcome o;o.status=ProbeStatus::kOk;o.data.valid=true;o.data.token=1;
  o.data.selection_mode=mode;o.data.flex_mapping=flex;o.data.selection_carrier=0;o.data.refresh_mode=0;return o;
}
inline GetConfigInfoOutcome Info(size_t i) {
  GetConfigInfoOutcome o;o.status=ProbeStatus::kOk;o.data.ok=true;
  o.data.has_total_size=o.data.has_description=o.data.has_version=true;
  const uint32_t sizes[]{24508,41172,39544,41088};
  const uint32_t versions[]{134285321,134293258,134293504,134293000};
  const char* names[]{"Common-Commercial","Commercial-SKT","Commercial-KT","Commercial-LGU"};
  o.data.total_size=sizes[i];o.data.version=versions[i];o.data.description=names[i];return o;
}
inline AutoSlot0Preflight Pre() {
  AutoSlot0Preflight p;p.selected=Common();p.slots={Feature(),Feature()};
  for(size_t i=0;i<4;++i) p.info[i]=Info(i);
  return p;
}
inline AutoFeatureSetResult Success() {
  AutoFeatureSetResult r;r.mutation_attempted=true;r.status=ProbeStatus::kOk;
  r.sync.valid=true;r.indication.valid=true;return r;
}
inline std::vector<std::string> Command() {
  return {"enable-auto-selection-slot0-once","--confirm",kAutoSlot0Confirmation};
}
class State : public IAutoExperimentState {
 public:
  bool clean=true,record_ok=true,durable=false;
  int acquires=0,records=0;
  bool AcquireClean() override {++acquires;return clean;}
  bool RecordPending() override {++records;clean=false;durable=record_ok;return record_ok;}
  bool PendingDurable() const override {return durable;}
};
class Ops : public IAutoSlot0Operations {
 public:
  AutoSlot0Preflight pre=Pre();
  std::array<FeatureReadOutcome,2> post{auto_test::Feature(3,true),auto_test::Feature()};
  GetSelectedConfigOutcome post_selected=Common();
  AutoFeatureSetResult set=Success();
  int selected=0,sets=0;
  std::array<int,2> feature_calls{};
  std::array<int,4> info_calls{};
  std::vector<std::string> order;
  GetSelectedConfigOutcome Selected() override {order.push_back("selected");return ++selected==1?pre.selected:post_selected;}
  FeatureReadOutcome Feature(FeatureSlot slot) override {
    const size_t i=static_cast<size_t>(slot);order.push_back(i?"slot1":"slot0");
    return ++feature_calls[i]==1?pre.slots[i]:post[i];
  }
  GetConfigInfoOutcome Info(CarrierTarget target) override {
    const size_t i=static_cast<size_t>(target);++info_calls[i];order.push_back("info");return pre.info[i];
  }
  AutoFeatureSetResult EnableSlot0(const AutoSlot0Preflight&,const std::string&) override {
    ++sets;order.push_back("set0");return set;
  }
};
}
