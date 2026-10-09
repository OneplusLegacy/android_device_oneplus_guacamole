#pragma once
#include <set>
#include <mutex>
#include "diag_process_worker.h"
#include "efs_transaction.h"
namespace guacamole_efs {
inline constexpr char kProcessReadCommand[] = "--diag-target-read-worker-v2";
inline constexpr size_t kProcessDataCap = 16 * 1024 * 1024;
enum class ProcessOperation : uint32_t { kRead = 0, kWrite = 1 };
enum class ProcessOutcome : uint32_t { kConfirmed = 0, kAmbiguous = 1, kNotAttempted = 2 };
enum class ProcessFailure : uint32_t { kNone = 0, kTransport = 1, kProtocol = 2, kTimeout = 3, kClosed = 4 };
struct ProcessRequest {
  ProcessOperation operation = ProcessOperation::kRead;
  EfsTarget target = EfsTarget::kMcfgSelDb;
  std::string transaction_id;
  std::vector<uint8_t> bytes;
};
struct ProcessReport {
  ProcessRequest identity;
  WorkerNonce nonce{};
  ProcessOutcome outcome = ProcessOutcome::kNotAttempted;
  ProcessFailure failure = ProcessFailure::kNone;
  bool teardown_completed = false;
  std::vector<uint8_t> bytes;
  std::array<uint8_t,32> sha256{};
};
struct ProcessResult {
  bool spawned = false, reaped = false, timed_out = false, report_valid = false;
  int pid = -1, exit_code = -1, signal = 0;
  std::string error;
  ProcessReport report;
};
bool ValidProcessRequest(const ProcessRequest& request);
std::vector<uint8_t> EncodeProcessReport(const ProcessReport& report);
bool DecodeProcessReport(const std::vector<uint8_t>& frame, const ProcessRequest& request,
    const WorkerNonce& nonce, ProcessReport* report, std::string* error);
class IProcessWorkerLauncher {
 public:
  virtual ~IProcessWorkerLauncher() = default;
  // Contract: spawn+exec exactly once, wait/reap before return; never retry.
  virtual ProcessResult Run(const ProcessRequest&) = 0;
};
class PosixProcessWorkerLauncher final : public IProcessWorkerLauncher {
 public:
  PosixProcessWorkerLauncher() = default;
#if !defined(__BIONIC__)
  // Host fixture override cannot exist in the Android device binary.
  explicit PosixProcessWorkerLauncher(std::string executable,
      std::chrono::milliseconds timeout = std::chrono::milliseconds(15000))
      : executable_(std::move(executable)), timeout_(timeout) {}
#endif
  ProcessResult Run(const ProcessRequest&) override;
 private:
  const std::string executable_ = "/vendor/bin/guacamole_efs_probe";
  const std::chrono::milliseconds timeout_{15000};
};
class IProcessEfsExecutor {
 public:
  virtual ~IProcessEfsExecutor() = default;
  virtual ProcessResult ReadTarget(EfsTarget, const std::string& id) = 0;
  virtual ProcessResult WriteTarget(EfsWriteTarget, const std::vector<uint8_t>&,
      const std::string& id) = 0;
};
class ProcessEfsExecutor final : public IProcessEfsExecutor {
 public:
  explicit ProcessEfsExecutor(IProcessWorkerLauncher& launcher) : launcher_(launcher) {}
  ProcessResult ReadTarget(EfsTarget, const std::string&) override;
  ProcessResult WriteTarget(EfsWriteTarget, const std::vector<uint8_t>&, const std::string&) override;
 private:
  ProcessResult Run(ProcessRequest);
  IProcessWorkerLauncher& launcher_;
  std::mutex lock_;
  std::set<WorkerNonce> seen_;
  bool poisoned_ = false;
};
TransactionOutcome ApplyProcessPatchPlan(IProcessEfsExecutor&, EfsWriteTarget,
    const PatchPlan&, const std::string&, int64_t, ITransactionLog&,
    RollbackPolicy = RollbackPolicy::kNever);
}
