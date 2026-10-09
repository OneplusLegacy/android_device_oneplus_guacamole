#pragma once
#include <functional>
#include <optional>
#include <string>
#include <vector>
namespace guacamole_pdc {
struct InitReportCommand {
  std::string label;
  std::vector<std::string> args;
};
// Only five fixed init invocations, no arbitrary argv, report path or slot.
std::optional<InitReportCommand> ParseInitReportCommand(const std::vector<std::string>& args);
// Caller must already pass root/build/context gates. Creates a fresh root-only
// report before invoking entry, without touching the mutation journal directory.
int RunInitExperimentReport(const InitReportCommand&, const std::function<int()>& entry);
}
