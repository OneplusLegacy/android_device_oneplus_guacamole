#pragma once
#include <array>
#include <cstddef>
#include <utility>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace guacamole_efs {
inline constexpr char kDiagWorkerCommand[] = "--diag-read-worker-v1";
inline constexpr int kDiagWorkerResultFd = 3;
using WorkerNonce = std::array<uint8_t, 16>;
enum class WorkerReadStatus : uint32_t { kOk = 0, kInitFailed = 1, kReadFailed = 2 };
struct WorkerReadReport {
  uint32_t pid = 0;
  WorkerNonce nonce{};
  WorkerReadStatus status = WorkerReadStatus::kReadFailed;
  bool teardown_completed = false;
  std::vector<uint8_t> bytes;
  std::array<uint8_t, 32> sha256{};
};
inline constexpr size_t kWorkerFrameSize = 136;
std::vector<uint8_t> EncodeWorkerReport(const WorkerReadReport& report);
bool DecodeWorkerReport(const std::vector<uint8_t>& frame, uint32_t pid,
    const WorkerNonce& nonce, WorkerReadReport* report, std::string* error);
std::string WorkerNonceHex(const WorkerNonce& nonce);
bool ParseWorkerNonce(const std::string& text, WorkerNonce* nonce);

struct WorkerProcessResult {
  bool spawned = false;
  bool reaped = false;
  bool timed_out = false;
  bool report_valid = false;
  bool success = false;
  int pid = -1;
  int exit_code = -1;
  int signal = 0;
  std::string error;
  WorkerReadReport report;
};
class IReadWorkerLauncher {
 public:
  virtual ~IReadWorkerLauncher() = default;
  virtual WorkerProcessResult Run() = 0;  // fixed read only, no path/phase command
};
// Default executable is fixed. Override exists for host fixture tests only;
// neither CLI nor Binder exposes it. Parent performs no DIAG initialization.
class PosixReadWorkerLauncher final : public IReadWorkerLauncher {
 public:
  explicit PosixReadWorkerLauncher(
      std::string executable = "/vendor/bin/guacamole_efs_probe",
      std::chrono::milliseconds timeout = std::chrono::milliseconds(15000))
      : executable_(std::move(executable)), timeout_(timeout) {}
  WorkerProcessResult Run() override;
 private:
  std::string executable_;
  std::chrono::milliseconds timeout_;
};
struct ProcessReadCycleResult {
  std::array<WorkerProcessResult, 2> workers;
  bool exact_equal = false;
};
ProcessReadCycleResult RunDiagProcessCycleReadonly(IReadWorkerLauncher& launcher,
    const std::function<void(const std::string&)>& progress = {});
}
