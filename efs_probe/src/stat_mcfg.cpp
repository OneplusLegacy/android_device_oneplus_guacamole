#include "stat_mcfg.h"

#include "efs_targets.h"

namespace guacamole_efs {

StatMcfgResult RunStatMcfg(DiagTransport* transport,
                            std::chrono::milliseconds timeout) {
  StatMcfgResult result;

  // The ONLY path this function will ever send - resolved through the
  // fixed whitelist, never accepted as a parameter. See efs_targets.h.
  const std::string& path = TargetPath(EfsTarget::kMcfgDir);
  result.request = BuildStatRequest(path);
  if (result.request.empty()) {
    // BuildStatRequest() only returns empty for a degenerate input
    // (empty/oversized path) - TargetPath(kMcfgDir) is neither, so this
    // is not expected to happen. Fail closed rather than silently
    // proceeding with no request.
    result.transport.status = TransportStatus::kInitFailed;
    return result;
  }

  result.transport = transport->SendSubsysCommand(/*proc=*/0, result.request,
                                                    timeout);
  if (result.transport.status != TransportStatus::kOk) {
    return result;
  }

  result.stat = ParseStatResponse(result.transport.response.data(),
                                   result.transport.response.size());
  result.parsed = result.stat.ok;
  return result;
}

}  // namespace guacamole_efs
