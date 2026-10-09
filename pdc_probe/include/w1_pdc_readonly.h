#pragma once
#include "feature_readonly.h"
namespace guacamole_pdc {
struct W1RadioObservation {
  bool common_idle=false, profiles=false, slot0=false, slot1=false;
  bool active_known=false, pending_none=false;
  std::string detail;
  std::string selected="UNKNOWN", auto0="UNKNOWN", auto1="UNKNOWN";
};
// Only GetSelected/GetInfo/GetFeature. Exact profile identities, no mutation API.
W1RadioObservation ObserveW1Radio();
}
