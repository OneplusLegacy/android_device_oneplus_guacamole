#pragma once
#include "auto_slot0_experiment.h"
namespace guacamole_pdc {
AutoSlot0Report RunDeviceAutoSlot0(const std::vector<std::string>& args,
    const std::function<void(const AutoSlot0Report&)>& before,
    const std::function<void(bool)>& permit);
}
