#pragma once
#include <array>
#include <functional>
#include <string>
#include <vector>
#include "efs_sessions.h"

namespace guacamole_efs {
struct ReadCyclePhase {
  std::string status = "NOT_STARTED";
  std::string error;
  bool teardown_completed = false;  // owning destructor returned, not a vendor return code
  std::vector<uint8_t> bytes;
  std::array<uint8_t, 32> sha256{};
};
struct ReadCycleResult {
  std::array<ReadCyclePhase, 2> sessions;
  bool exact_equal = false;
};
// One fixed digest read; owning teardown returns before the result is emitted.
ReadCyclePhase RunSingleDiagDigestRead(DiagEfsSessionFactory& factory,
    const std::function<void(const std::string&)>& progress = {});
// Object-lifecycle helper retained for fake host tests ONLY. Real H.41 must use
// diag-process-cycle-readonly: DeInit does not establish reusable process state.
// Hook is a progress log so a hung vendor DeInit remains diagnosable.
ReadCycleResult RunDiagSessionCycleReadonly(DiagEfsSessionFactory& factory,
    const std::function<void(const std::string&)>& progress = {});
}
