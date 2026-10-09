#include "diag_session_cycle.h"
#include "efs_targets.h"
#include "hash.h"

namespace guacamole_efs {
namespace {
bool ReadDigest(IEfsClient& client, ReadCyclePhase* out) {
  const auto& path = TargetPath(EfsTarget::kMcfgRfsSwDigestVersion);
  const auto stat = client.Stat(path);
  if (!stat.ok || stat.diag_errno != 0 || stat.size != 32 ||
      (stat.mode & 0170000) != 0100000) {
    out->error = "STAT failed or fixed digest is not a 32-byte regular file";
    return false;
  }
  const auto opened = client.OpenReadOnly(path);
  if (!opened.ok || opened.diag_errno != 0 || opened.fd < 0) {
    out->error = "O_RDONLY open failed";
    return false;
  }
  const auto read = client.Read(opened.fd, 32, 0);
  const auto closed = client.Close(opened.fd);  // cleanup, never retries read
  if (!read.ok || read.diag_errno != 0 || read.fd != opened.fd ||
      read.offset != 0 || read.bytes_read != 32 || read.data.size() != 32) {
    out->error = "read failed/partial/integrity error; no retry";
    return false;
  }
  if (!closed.ok || closed.errno_value != 0) {
    out->error = "CLOSE failed";
    return false;
  }
  out->bytes = read.data;
  out->sha256 = Sha256(out->bytes.data(), out->bytes.size());
  return true;
}
}
ReadCyclePhase RunSingleDiagDigestRead(DiagEfsSessionFactory& factory,
    const std::function<void(const std::string&)>& progress) {
  ReadCyclePhase phase;
  auto log = [&](const std::string& event) { if (progress) progress(event); };
  log("init_begin");
  auto session = factory.OpenRead(&phase.error);
  if (!session) {
    phase.status = "INIT_FAILED";
    log("init_failed");
    return phase;
  }
  log("init_completed");
  const bool ok = ReadDigest(session->client(), &phase);
  phase.status = ok ? "READ_OK" : "READ_FAILED";
  log(phase.status);
  log("teardown_begin");
  session.reset();
  phase.teardown_completed = true;
  log("teardown_completed");
  return phase;
}
ReadCycleResult RunDiagSessionCycleReadonly(DiagEfsSessionFactory& factory,
    const std::function<void(const std::string&)>& progress) {
  ReadCycleResult result;
  for (size_t i = 0; i < result.sessions.size(); ++i) {
    const auto tag = "session" + std::to_string(i + 1);
    result.sessions[i] = RunSingleDiagDigestRead(factory, [&](const std::string& event) {
      if (progress) progress(tag + "." + event);
    });
    if (result.sessions[i].status != "READ_OK") return result;
  }
  result.exact_equal = result.sessions[0].bytes == result.sessions[1].bytes &&
      result.sessions[0].sha256 == result.sessions[1].sha256;
  return result;
}
}
