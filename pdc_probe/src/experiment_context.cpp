#include "experiment_context.h"
#include <cstdio>

namespace guacamole_pdc {
int RunWithGuacamoleProbeContext(const SelfContextReader& read,
                                const std::function<int()>& entry) {
  std::string context;
  if (!read || !read(&context) || context != kGuacamoleProbeContext || !entry) {
    std::fprintf(stderr, "EXPERIMENT_BLOCKED: wrong SELinux execution domain\n");
    return 4;
  }
  return entry(); // journal creation and ALL QCCI operations belong inside here
}
int ReportGuacamoleSelfContext(uint32_t euid, const SelfContextReader& read) {
  std::string context;
  const bool readable = read && read(&context);
  const bool verified = readable && context == kGuacamoleProbeContext;
  std::printf("euid=%u\nselinux_context=%s\nexpected_context=%s\ncontext_verified=%s\n",
      euid, readable ? context.c_str() : "UNAVAILABLE", kGuacamoleProbeContext,
      verified ? "true" : "false");
  return verified ? 0 : 4;
}
}
