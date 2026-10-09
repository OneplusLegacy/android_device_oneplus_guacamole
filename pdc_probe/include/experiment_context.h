#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace guacamole_pdc {
inline constexpr char kGuacamoleProbeContext[] = "u:r:guacamole_pdc_probe:s0";
using SelfContextReader = std::function<bool(std::string*)>;
// Device binds getcon/freecon. Host tests inject success/failure and exact bytes.
// Must be called inside the existing compile-time and root/debug entry gates.
int RunWithGuacamoleProbeContext(const SelfContextReader& read,
                                const std::function<int()>& entry);
// Diagnostic only: no operation continuation, journal, or QCCI dependency.
int ReportGuacamoleSelfContext(uint32_t euid, const SelfContextReader& read);
}
