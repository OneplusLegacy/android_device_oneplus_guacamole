#include <cstdio>
#ifdef GUACAMOLE_MAPPING_DEBUG
#include <android-base/properties.h>
#include <unistd.h>
#include "experiment_environment.h"
#include "mapping_experiment.h"
#include "mapping_transport.h"

namespace {
using namespace guacamole_pdc;
std::string Hex(const std::vector<uint8_t>& data) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string s;
  for (uint8_t b : data) { s += digits[b >> 4]; s += digits[b & 15]; }
  return s;
}
std::string Id(const std::optional<std::vector<uint8_t>>& id) {
  return id ? (id->empty() ? "EMPTY_INVALID" : Hex(*id)) : "NONE";
}
void Selected(const char* prefix, const GetSelectedConfigOutcome& o) {
  std::printf("%s_status=%s\n%s_error_status=%u\n%s_error_code=%u\n", prefix,
      ProbeStatusToString(o.status), prefix, o.data.error_status, prefix, o.data.error_code);
  const bool valid = o.status == ProbeStatus::kOk && o.data.ok && o.data.error_status == 0;
  std::printf("%s_active=%s\n%s_pending=%s\n", prefix,
      valid ? Id(o.data.active_id).c_str() : "UNKNOWN", prefix,
      valid ? Id(o.data.pending_id).c_str() : "UNKNOWN");
}
void Info(const char* name, const GetConfigInfoOutcome& o) {
  std::printf("%s_status=%s\n%s_error_status=%u\n%s_error_code=%u\n", name,
      ProbeStatusToString(o.status), name, o.data.error_status, name, o.data.error_code);
  std::printf("%s_has_size=%d\n%s_size=%u\n%s_has_version=%d\n%s_version=%u\n",
      name, o.data.has_total_size, name, o.data.total_size,
      name, o.data.has_version, name, o.data.version);
  const std::vector<uint8_t> description(o.data.description.begin(), o.data.description.end());
  std::printf("%s_has_description=%d\n%s_description_hex=%s\n", name,
      o.data.has_description, name, Hex(description).c_str());
}
}
static int RunMappingMain(int argc, char** argv) {
  using namespace guacamole_pdc;
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
  const auto request = ParseMappingArguments(args);
  if (!request) {
    std::fprintf(stderr, "Invalid command/confirmation. Commands: preflight, select-skt-once, select-common-once. See ROUND3_README before mutation.\n");
    return 2;  // no QMI initialization or send
  }
  std::optional<MappingTarget> target;
  if (request->command == MappingCommand::kSelectSkt) target = MappingTarget::kSkt;
  if (request->command == MappingCommand::kSelectCommon) target = MappingTarget::kCommon;
  QcciShim vendor;
  MappingQcciGuard guard(vendor, target);
  PdcClient client(guard);
  std::string log;
  if (!client.Init({{1, 15, 6}}, &log)) {
    std::fprintf(stderr, "EXPERIMENT_BLOCKED: QMI init failed: %s\n", log.c_str()); return 5;
  }
  std::printf("requested_target=%s\n", target ? Hex(MappingId(*target)).c_str() : "READ_ONLY");
  std::printf("automatic_retry=false\nautomatic_recovery=false\n"); std::fflush(stdout);
  MappingClientAdapter adapter(client);
  const auto report = RunMappingExperiment(adapter, *request, [](const MappingReport& pre) {
    std::printf("phase=BEFORE_SINGLE_SETSELECTED\n");
    Selected("pre", pre.pre); Info("common", pre.common); Info("skt", pre.skt);
    std::fflush(stdout);
  });
  std::printf("phase=FINAL_REPORT\n");
  std::printf("preflight_observed=%s\n", report.preflight_observed ? "true" : "false");
  if (report.preflight_observed) {
    Selected("pre", report.pre); Info("common", report.common); Info("skt", report.skt);
  }
  std::printf("selection_attempted=%s\n", report.selection_attempted ? "true" : "false");
  if (report.selection_attempted) {
    std::printf("setselected_result=%s\nsetselected_error_status=%u\nsetselected_error_code=%u\n",
        ProbeStatusToString(report.selection.status), report.selection.data.error_status,
        report.selection.data.error_code);
  } else std::printf("setselected_result=NOT_ATTEMPTED\n");
  if (report.post_observed) Selected("post", report.post);
  std::printf("outcome=%s\nreason=%s\n", report.outcome.c_str(), report.reason.c_str());
  if (report.selection_attempted) return 10;  // human review even on observed success
  return report.outcome == "PREFLIGHT_ALLOWED" || report.outcome == "NOOP_OK" ? 0 : 3;
}
int main(int argc, char** argv) {
  return guacamole_pdc::RunGuacamoleManualExperiment(
      [] { return static_cast<uint32_t>(geteuid()); },
      [](const std::string& key) { return android::base::GetProperty(key, ""); },
      [&] { return RunMappingMain(argc, argv); });
}
#else
int main() {
  std::fprintf(stderr, "EXPERIMENT_BLOCKED: compiled without debug experiment support\n");
  return 4;
}
#endif
