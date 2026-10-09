#include <cstdio>
#ifdef GUACAMOLE_CARRIER_DEBUG
#include <android-base/properties.h>
#include <unistd.h>
#include <selinux/selinux.h>
#include "experiment_context.h"
#include "init_experiment_report.h"
#include "experiment_environment.h"
#include "carrier_transport.h"
#include "presence_diagnostics.h"
#include "auto_selection_experiment.h"
#include "feature_readonly.h"
#include "auto_slot0_device.h"
#include "hash.h"
namespace {
using namespace guacamole_pdc;
std::string Hex(const std::vector<uint8_t>& v) {
  constexpr char hex[] = "0123456789abcdef";
  std::string out;
  for (uint8_t b : v) { out += hex[b >> 4]; out += hex[b & 15]; }
  return out;
}
std::string SafeId(const std::optional<std::vector<uint8_t>>& id) {
  if (!id) return "NONE";
  for (auto t : {CarrierTarget::kCommon, CarrierTarget::kSkt, CarrierTarget::kKt, CarrierTarget::kLgu})
    if (*id == CarrierId(t)) return Hex(*id);
  return "UNRECOGNIZED_REDACTED";
}
void PrintSelected(const char* label, const GetSelectedConfigOutcome& o) {
  std::printf("%s_status=%s\n%s_error_status=%u\n%s_error_code=%u\n", label,
      ProbeStatusToString(o.status), label, o.data.error_status, label, o.data.error_code);
  const bool ok = o.status == ProbeStatus::kOk && o.data.ok && !o.data.error_status && !o.data.error_code;
  std::printf("%s_active=%s\n%s_pending=%s\n", label, ok ? SafeId(o.data.active_id).c_str() : "UNKNOWN",
      label, ok ? SafeId(o.data.pending_id).c_str() : "UNKNOWN");
}
void PrintExactInfo(const std::string& prefix, const char* name,
    const std::vector<uint8_t>& id, bool verified, const GetConfigInfoOutcome& o) {
  const char* p = prefix.c_str();
  std::printf("%s_carrier=%s\n%s_expected_config_id=%s\n", p, name, p, Hex(id).c_str());
  std::printf("%s_status=%s\n%s_error_status=%u\n%s_error_code=%u\n", p,
      ProbeStatusToString(o.status), p, o.data.error_status, p, o.data.error_code);
  const bool indication = o.status == ProbeStatus::kIndicationRejected;
  std::printf("%s_protocol_error_source=%s\n%s_protocol_error=%u\n", p,
      indication ? "INDICATION_RESULT" : o.status == ProbeStatus::kRequestRejected ? "SYNC_OPERATION_RESULT" : "NONE_OR_UNKNOWN",
      p, indication ? o.data.error_status : o.data.error_code);
  std::printf("%s_presence=%s\n%s_runtime_verified=%s\n", p,
      ConfigPresenceToString(CarrierPresence(o)), p, verified ? "true" : "false");
  std::printf("%s_has_size=%d\n%s_size=%u\n%s_has_version=%d\n%s_version=%u\n", p,
      o.data.has_total_size, p, o.data.total_size, p, o.data.has_version, p, o.data.version);
  const std::vector<uint8_t> description(o.data.description.begin(), o.data.description.end());
  std::printf("%s_has_description=%d\n%s_description_hex=%s\n", p,
      o.data.has_description, p, Hex(description).c_str());
}
void PrintInfo(const std::string& prefix, CarrierTarget t, const GetConfigInfoOutcome& o) {
  PrintExactInfo(prefix, CarrierName(t), CarrierId(t), CarrierInfoVerified(t, o), o);
}
void PrintAutoSlot(const char* prefix,const FeatureReadOutcome& r) {
  std::printf("%s_status=%s\n%s_sync_result=%u\n%s_sync_error=%u\n%s_indication_valid=%s\n%s_indication_error=%u\n",
      prefix,ProbeStatusToString(r.status),prefix,r.sync_result,prefix,r.sync_error,
      prefix,r.data.valid ? "true" : "false",prefix,r.data.error);
  auto field=[&](const char* name,std::optional<uint32_t> value) {
    if (value) std::printf("%s_%s=%u\n",prefix,name,*value);
    else std::printf("%s_%s=UNKNOWN\n",prefix,name);
  };
  field("selection_mode",r.data.selection_mode);
  field("selection_carrier",r.data.selection_carrier);
  field("refresh_mode",r.data.refresh_mode);
  std::printf("%s_flex_mapping=%s\n",prefix,r.data.flex_mapping ?
      (*r.data.flex_mapping ? "true" : "false") : "UNKNOWN");
}
void PrintAutoPre(const AutoSlot0Report& r) {
  if (!r.pre_observed) return;
  PrintSelected("pre_selected",r.pre.selected);
  PrintAutoSlot("pre_slot0",r.pre.slots[0]);PrintAutoSlot("pre_slot1",r.pre.slots[1]);
  for (auto target : {CarrierTarget::kCommon,CarrierTarget::kSkt,CarrierTarget::kKt,CarrierTarget::kLgu})
    PrintInfo(std::string("pre_")+CarrierName(target),target,r.pre.info[static_cast<size_t>(target)]);
}
int RunAutoSlot0(const std::vector<std::string>& args) {
  const auto r=RunDeviceAutoSlot0(args,[](const AutoSlot0Report& pre) {
    std::printf("phase=BEFORE_MUTATION\n");PrintAutoPre(pre);
    std::printf("pending_checkpoint=DURABLE\n");std::fflush(stdout);
  },[](bool armed) {
    std::printf("transport_permit=%s\n",armed ? "ARMED" : "BLOCKED");std::fflush(stdout);
  });
  std::printf("phase=FINAL_REPORT\n");PrintAutoPre(r);
  std::printf("mutation_attempted=%s\nset_status=%s\nsync_valid=%s\nsync_result=%u\nsync_error=%u\nindication_valid=%s\nindication_error=%u\nambiguous=%s\n",
      r.set.mutation_attempted ? "true" : "false",ProbeStatusToString(r.set.status),
      r.set.sync.valid ? "true" : "false",r.set.sync.result,r.set.sync.error,
      r.set.indication.valid ? "true" : "false",r.set.indication.error,r.set.ambiguous ? "true" : "false");
  if (r.post_observed) {
    PrintAutoSlot("post_slot0",r.post_slots[0]);PrintAutoSlot("post_slot1",r.post_slots[1]);
    PrintSelected("post_selected",r.post_selected);
  }
  std::printf("assessment=%s\noutcome=%s\nreason=%s\noriginal_auto_efs_state=NOT_OBSERVED\n",
      r.assessment.c_str(),r.outcome.c_str(),r.reason.c_str());
  return r.set.mutation_attempted ? 10 : 3;
}
int RunFeatureReadOnly(FeatureSlot slot) {
  QcciShim vendor;
  FeatureReadQcciGuard guard(vendor, slot);
  FeatureReadClient client(guard);
  const auto result = client.Read(slot, 5000);
  const auto& d = result.data;
  std::printf("slot=%u\nstatus=%s\nsync_result=%u\nsync_error=%u\nindication_valid=%s\nindication_error=%u\n",
      static_cast<unsigned>(slot), ProbeStatusToString(result.status), result.sync_result,
      result.sync_error, d.valid ? "true" : "false", d.error);
  auto print = [](const char* name, std::optional<uint32_t> value) {
    if (value) std::printf("%s=%u\n", name, *value);
    else std::printf("%s=UNKNOWN\n", name);
  };
  print("selection_mode", d.selection_mode);
  print("selection_carrier", d.selection_carrier);
  print("refresh_mode", d.refresh_mode);
  if (d.flex_mapping) std::printf("flex_mapping=%s\n", *d.flex_mapping ? "true" : "false");
  else std::printf("flex_mapping=UNKNOWN\n");
  const auto state = result.status == ProbeStatus::kOk ?
      ClassifyAutoFeature(d.selection_mode, d.flex_mapping) : AutoFeatureState::kUnknown;
  std::printf("auto_feature_state=%s\noriginal_auto_efs_state=ORIGINAL_AUTO_EFS_UNKNOWN\n",
      state == AutoFeatureState::kEnabled ? "AUTO_FEATURE_ENABLED" :
      state == AutoFeatureState::kDisabled ? "AUTO_FEATURE_DISABLED" : "AUTO_FEATURE_UNKNOWN");
  std::printf("h41_extension_count=%zu\nmutation_attempted=false\n", d.extension_count);
  return result.status == ProbeStatus::kOk ? 0 : 3;
}
int RunPresenceDiagnostics() {
  QcciShim vendor;
  PresenceQcciGuard guard(vendor); // no Arm / mutation permit API
  PdcClient client(guard);
  std::string log;
  if (!client.Init({{1,15,6}}, &log)) {
    std::fprintf(stderr, "outcome=INIT_FAILED\n"); return 5;
  }
  const auto r = DiagnosePresence(client);
  PrintSelected("selected", r.selected);
  std::printf("list_observed=%s\nstopped_on_transport_failure=%s\n",
      r.list_observed ? "true" : "false", r.stopped_on_transport_failure ? "true" : "false");
  std::printf("list_status=%s\nlist_error_status=%u\nlist_error_code=%u\nlist_ok=%d\nlist_truncated=%d\nlist_count=%zu\n",
      ProbeStatusToString(r.list.status), r.list.data.error_status, r.list.data.error_code,
      r.list.data.ok, r.list.data.truncated, r.list.data.configs.size());
  std::printf("list_membership_policy=ADVISORY_ONLY_OMISSION_IS_NOT_ABSENCE\n");
  constexpr CarrierTarget targets[]{CarrierTarget::kCommon, CarrierTarget::kSkt,
      CarrierTarget::kKt, CarrierTarget::kLgu};
  bool incomplete = r.selected.status != ProbeStatus::kOk || !r.selected.data.ok ||
      r.selected.data.error_status || r.selected.data.error_code ||
      r.list.status != ProbeStatus::kOk || !r.list.data.ok || r.list.data.truncated ||
      r.list.data.error_status || r.list.data.error_code;
  for (size_t i = 0; i < r.info.size(); ++i) {
    const std::string name = i < 4 ? CarrierName(targets[i]) : "synthetic";
    std::printf("%s_observed=%s\n", name.c_str(), r.info_observed[i] ? "true" : "false");
    if (i < 4) PrintInfo(name, targets[i], r.info[i]);
    else PrintExactInfo(name, "DIAGNOSTIC_ONLY_ABSENCE_NOT_ASSUMED",
        PresenceDiagnosticId(), false, r.info[i]);
    std::printf("%s_listed_software_raw=%s\n", name.c_str(), r.listed[i] ? "true" : "false");
    incomplete = incomplete || CarrierPresence(r.info[i]) == ConfigPresence::kUnknown;
  }
  std::printf("mutation_attempted=false\noutcome=%s\n",
      incomplete ? "READ_ONLY_DIAGNOSTICS_WITH_UNKNOWNS" : "READ_ONLY_DIAGNOSTICS_COMPLETE");
  return incomplete ? 3 : 0;
}
void PrintObserved(const CarrierReport& r) {
  std::printf("load_gate=%s\n",r.load_gate.c_str());
  if (r.list_observed) {
    std::printf("pre_list_status=%s\npre_list_ok=%d\npre_list_error_status=%u\npre_list_error_code=%u\npre_list_truncated=%d\npre_list_count=%zu\n",
        ProbeStatusToString(r.list.status), r.list.data.ok, r.list.data.error_status,
        r.list.data.error_code, r.list.data.truncated, r.list.data.configs.size());
    for (auto t : {CarrierTarget::kKt, CarrierTarget::kLgu}) {
      bool listed = false;
      for (const auto& e : r.list.data.configs)
        listed = listed || (e.type == ConfigType::kSoftware && e.id == CarrierId(t));
      std::printf("pre_%s_listed_software_raw=%s\n", CarrierName(t), listed ? "true" : "false");
    }
    bool synthetic_listed = false;
    for (const auto& e : r.list.data.configs)
      synthetic_listed = synthetic_listed || (e.type == ConfigType::kSoftware && e.id == PresenceDiagnosticId());
    std::printf("pre_synthetic_listed_software_raw=%s\n", synthetic_listed ? "true" : "false");
  }
  if(r.synthetic_observed) PrintExactInfo("pre_synthetic","DIAGNOSTIC_ONLY_ABSENCE_NOT_ASSUMED",PresenceDiagnosticId(),false,r.synthetic);
  if (r.pre_observed) PrintSelected("pre", r.pre);
  for (auto t : {CarrierTarget::kCommon, CarrierTarget::kSkt, CarrierTarget::kKt, CarrierTarget::kLgu})
    if (r.queried[static_cast<size_t>(t)]) PrintInfo(CarrierName(t), t, r.info[static_cast<size_t>(t)]);
}
}
static bool ReadSelfContext(std::string* context) {
  char* raw = nullptr;
  const int status = getcon(&raw);
  if (status == 0 && raw) *context = raw;
  const bool ok = status == 0 && raw;
  if (raw) freecon(raw);
  return ok;
}
static int RunCarrierArguments(const std::vector<std::string>& args) {
  using namespace guacamole_pdc;
  if (!args.empty() && args[0] == "--init-report") {
    const auto command = ParseInitReportCommand(args);
    if (!command) return 2;
    return RunWithGuacamoleProbeContext(ReadSelfContext, [&] {
      return RunInitExperimentReport(*command, [&] { return RunCarrierArguments(command->args); });
    });
  }
  if (args == std::vector<std::string>{"self-context-readonly"})
    return ReportGuacamoleSelfContext(static_cast<uint32_t>(geteuid()), ReadSelfContext);
  if (IsAutoSlot0Command(args))
    return RunWithGuacamoleProbeContext(ReadSelfContext, [&] { return RunAutoSlot0(args); });
  if (IsClosedAutoSlot1Command(args)) {
    std::fprintf(stderr,"AUTO_SLOT1_EXPERIMENT_CLOSED\n");
    std::printf("mutation_attempted=false\n");return 4;
  }
  if (IsClosedAutoExperimentCommand(args)) {
    std::fprintf(stderr,"AUTO_EXPERIMENT_CLOSED: send disabled; feature protocol known; original-state restore effect unverified\n");
    std::printf("mutation_attempted=false\n"); return 4;
  }
  if (const auto slot = ParseFeatureReadCommand(args)) return RunFeatureReadOnly(*slot);
  if (IsPresenceDiagnosticsCommand(args)) return RunPresenceDiagnostics();
  const auto req = ParseCarrierArguments(args);
  if (!req) {
    std::fprintf(stderr, "Invalid exact command/confirmation. See MACRO_B3_README. No arbitrary ID/path.\n"); return 2;
  }
  CarrierFile file;
  if (req->operation == CarrierOperation::kLoad || req->operation == CarrierOperation::kH41Load) file = ReadCarrierFile(req->target);
  QcciShim vendor;
  CarrierQcciGuard guard(vendor, *req);
  PdcClient client(guard);
  std::string log;
  if (!client.Init({{1,15,6}}, &log)) {
    std::fprintf(stderr, "outcome=INIT_FAILED\n"); return 5;
  }
  CarrierClientAdapter adapter(client, guard, file);
  const auto result = RunCarrierExperiment(adapter, *req, file, [&](const CarrierReport& pre) {
    std::printf("phase=BEFORE_MUTATION\n"); PrintObserved(pre);
    std::printf("file_verified=%s\nfile_size=%zu\n", file.verified ? "true" : "false", file.bytes.size());
    std::printf("load_gate=%s\n",pre.load_gate.c_str());
    const bool armed = req->operation == CarrierOperation::kH41Load ? guard.ArmH41(file,pre) : guard.Arm(file);
    std::printf("transport_permit=%s\n", armed ? "ARMED" : "BLOCKED");
    std::fflush(stdout);
  });
  std::printf("phase=FINAL_REPORT\n"); PrintObserved(result);
  if (result.post_observed) PrintSelected("post", result.post);
  if (result.post_info_observed) PrintInfo("post_info", req->target, result.post_info);
  std::printf("mutation_attempted=%s\nchunks_attempted=%zu\n", result.mutation_attempted ? "true" : "false", result.chunks_attempted);
  if (result.mutation_attempted && (req->operation == CarrierOperation::kLoad || req->operation == CarrierOperation::kH41Load)) {
    const auto& c = result.last_chunk;
    std::printf("transfer_completed=%s\nload_post_verified=%s\n",
        result.transfer_completed ? "true" : "false", result.load_post_verified ? "true" : "false");
    std::printf("load_status=%s\nload_error_status=%u\nload_error_code=%u\nload_has_received=%d\nload_received=%u\nload_has_remaining=%d\nload_remaining=%u\nload_has_reset=%d\nload_reset=%d\n",
        ProbeStatusToString(c.status), c.data.error_status, c.data.error_code, c.data.has_received, c.data.received,
        c.data.has_remaining_size, c.data.remaining_size, c.data.has_frame_reset, c.data.frame_reset);
  } else if (result.mutation_attempted) {
    const auto& s = result.selection;
    std::printf("setselected_status=%s\nsetselected_error_status=%u\nsetselected_error_code=%u\n",
        ProbeStatusToString(s.status), s.data.error_status, s.data.error_code);
  }
  std::printf("outcome=%s\nreason=%s\n", result.outcome.c_str(), result.reason.c_str());
  if (result.mutation_attempted) return 10;
  return result.outcome == "READ_ONLY_OBSERVED" || result.outcome == "ALREADY_PRESENT_NOOP" || result.outcome == "NOOP_OK" ? 0 : 3;
}
int main(int argc, char** argv) {
  return guacamole_pdc::RunGuacamoleManualExperiment(
      [] { return static_cast<uint32_t>(geteuid()); },
      [](const std::string& key) { return android::base::GetProperty(key, ""); },
      [&] {
        std::vector<std::string> args;
        for (int i=1; i<argc; ++i) args.emplace_back(argv[i]);
        return RunCarrierArguments(args);
      });
}
#else
int main() { std::fprintf(stderr, "EXPERIMENT_BLOCKED: debug build required\n"); return 4; }
#endif
