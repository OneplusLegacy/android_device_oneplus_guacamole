#include "experiment_environment.h"
#include <cstdio>

namespace guacamole_pdc {
int RunGuacamoleManualExperiment(
    const std::function<uint32_t()>& effective_uid,
    const std::function<std::string(const std::string&)>& property,
    const std::function<int()>& entry) {
  if (!effective_uid || !property || !entry) {
    std::fprintf(stderr, "EXPERIMENT_BLOCKED: environment provider required\n");
    return 4;
  }
  if (effective_uid() != 0) {
    std::fprintf(stderr, "EXPERIMENT_BLOCKED: root execution required\n");
    return 4;
  }
  const auto debug_type = [](const std::string& value) {
    return value == "userdebug" || value == "eng";
  };
  const auto system = property("ro.system.build.type");
  const auto vendor = property("ro.vendor.build.type");
  if (!debug_type(system) || !debug_type(vendor)) {
    std::fprintf(stderr, "EXPERIMENT_BLOCKED: system/vendor debug build required\n");
    return 4;
  }
  // ro.debuggable/ro.build.type are intentionally not consulted for this mixed
  // PixelOS product. No override can bypass root + BOTH partition build types.
  return entry();
}
}
