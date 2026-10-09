#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace guacamole_pdc {
// Host-injectable boundary. Device entry points bind geteuid/GetProperty.
// Compilation of the entry points remains protected by each probe's DEBUG macro.
// The continuation owns ALL argument/file/QCCI operations, so a rejected gate
// cannot construct or initialize QCCI. Returns exit=4 on rejection.
int RunGuacamoleManualExperiment(
    const std::function<uint32_t()>& effective_uid,
    const std::function<std::string(const std::string&)>& property,
    const std::function<int()>& entry);
}
