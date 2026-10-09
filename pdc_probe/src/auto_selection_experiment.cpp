#include "auto_selection_experiment.h"
namespace guacamole_pdc {
std::vector<uint8_t> BuildAutoFeatureRequest(FeatureSlot slot, bool enable) {
  if (slot != FeatureSlot::kSlot0 && slot != FeatureSlot::kSlot1) return {};
  // Stock helper enable -> HYBRID/true, disable -> DISABLE/false.
  // Deliberately no token, Selection Carrier or Refresh Mode TLV.
  return {0x01,4,0,static_cast<uint8_t>(slot),0,0,0,
          0x11,4,0,static_cast<uint8_t>(enable ? 3 : 0),0,0,0,
          0x13,1,0,static_cast<uint8_t>(enable)};
}
std::vector<uint8_t> BuildObservedAutoEnableRequest(ObservedAutoSelector selector) {
  switch (selector) {
    case ObservedAutoSelector::kZero: return BuildAutoFeatureRequest(FeatureSlot::kSlot0, true);
    case ObservedAutoSelector::kOne: return BuildAutoFeatureRequest(FeatureSlot::kSlot1, true);
    default: return {};
  }
}
AutoFeatureState ClassifyAutoFeature(std::optional<uint32_t> mode, std::optional<bool> flex) {
  if (!mode || !flex) return AutoFeatureState::kUnknown;
  if (*mode == 0 && !*flex) return AutoFeatureState::kDisabled;
  if (*mode >= 1 && *mode <= 3 && *flex) return AutoFeatureState::kEnabled;
  return AutoFeatureState::kUnknown;  // absent, unsupported or mixed settings
}
AutoSyncResult ParseObservedAutoSync(const std::vector<uint8_t>& b) {
  if(b.size()!=7 || b[0]!=2 || b[1]!=4 || b[2]!=0) return {};
  return {true,static_cast<uint16_t>(b[3]|uint16_t(b[4])<<8),
      static_cast<uint16_t>(b[5]|uint16_t(b[6])<<8)};
}
AutoIndicationResult ParseObservedAutoIndication(const std::vector<uint8_t>& b) {
  AutoIndicationResult r;bool result=false;
  for(size_t i=0;i<b.size();) {
    if(b.size()-i<3) return {};
    const uint8_t type=b[i];const size_t n=b[i+1]|size_t(b[i+2])<<8;i+=3;
    if(n>b.size()-i) return {};
    if(type==1 && n==2 && !result) {r.error=b[i]|uint16_t(b[i+1])<<8;result=true;}
    else if(type==0x10 && n==4 && !r.token) {
      r.token=uint32_t(b[i])|uint32_t(b[i+1])<<8|uint32_t(b[i+2])<<16|uint32_t(b[i+3])<<24;
    } else return {};
    i+=n;
  }
  r.valid=result;return r;
}
bool IsClosedAutoExperimentCommand(const std::vector<std::string>& args) {
  return args==std::vector<std::string>{"enable-auto-selection-once","--confirm",kAutoExperimentConfirmation};
}
}
