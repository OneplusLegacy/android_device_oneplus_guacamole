// guacamole_pdc_probe - Phase B/B2 (read-only) + Phase C0 (one gated
// write) PDC observation/provisioning tool.
//
//   guacamole_pdc_probe list
//   guacamole_pdc_probe info <hex-config-id>
//   guacamole_pdc_probe selected
//   guacamole_pdc_probe load-skt      <-- THE ONLY COMMAND THAT WRITES ANYTHING
//
// list/info/selected NEVER call Load Config / Set Selected Config /
// Activate Config / Deactivate Config / Delete Config, and never touch
// EFS2 in any way. load-skt calls Load Config ONLY, against exactly one
// internally-known stock SKT MBN (see known_profiles.h - no external
// path input is possible), and NEVER calls Set Selected/Activate/
// Deactivate/Delete either. See PHASE_B_FINDINGS.md and
// PHASE_C0_FINDINGS.md for what this is for and its current status.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "pdc_client.h"
#include "skt_load.h"

namespace {

using guacamole_pdc::ConfigPresenceToString;
using guacamole_pdc::ConfigType;
using guacamole_pdc::GetConfigInfoOutcome;
using guacamole_pdc::GetSelectedConfigOutcome;
using guacamole_pdc::ListConfigsOutcome;
using guacamole_pdc::LoadState;
using guacamole_pdc::LoadStateToString;
using guacamole_pdc::PdcClient;
using guacamole_pdc::ProbeStatus;
using guacamole_pdc::ProbeStatusToString;
using guacamole_pdc::RunLoadSkt;
using guacamole_pdc::SelectedSnapshot;
using guacamole_pdc::SelectionCheckResultToString;
using guacamole_pdc::SktLoadReport;
using guacamole_pdc::VersionTriple;

constexpr unsigned int kTimeoutMs = 5000;

enum ExitCode {
  kExitOk = 0,
  kExitUsage = 1,
  kExitInitFailed = 2,
  kExitSendFailed = 3,
  kExitRejected = 4,
  kExitTimeout = 5,
  kExitMalformed = 6,
  kExitIndicationRejected = 7,
  kExitLoadUnknownResult = 8,
  kExitLoadFailed = 9,
};

const char* ConfigTypeName(ConfigType t) {
  switch (t) {
    case ConfigType::kPlatform: return "PLATFORM";
    case ConfigType::kSoftware: return "SOFTWARE";
  }
  return "UNKNOWN";
}

std::string ToHex(const std::vector<uint8_t>& bytes) {
  static const char* kHex = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (uint8_t b : bytes) {
    out.push_back(kHex[b >> 4]);
    out.push_back(kHex[b & 0xF]);
  }
  return out;
}

std::vector<uint8_t> FromHex(const std::string& hex, bool* ok) {
  std::vector<uint8_t> out;
  *ok = (hex.size() % 2 == 0);
  if (!*ok) return out;
  for (size_t i = 0; i < hex.size(); i += 2) {
    auto nibble = [&](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    };
    int hi = nibble(hex[i]);
    int lo = nibble(hex[i + 1]);
    if (hi < 0 || lo < 0) {
      *ok = false;
      return {};
    }
    out.push_back(static_cast<uint8_t>((hi << 4) | lo));
  }
  return out;
}

int ExitCodeFor(ProbeStatus status) {
  switch (status) {
    case ProbeStatus::kOk: return kExitOk;
    case ProbeStatus::kInitFailed: return kExitInitFailed;
    case ProbeStatus::kRequestSendFailed: return kExitSendFailed;
    case ProbeStatus::kRequestRejected: return kExitRejected;
    case ProbeStatus::kTimeout: return kExitTimeout;
    case ProbeStatus::kMalformedResponse: return kExitMalformed;
    case ProbeStatus::kIndicationRejected: return kExitIndicationRejected;
  }
  return kExitMalformed;
}

void PrintUsage(const char* argv0) {
  fprintf(stderr,
          "usage:\n"
          "  %s list\n"
          "  %s info <hex-config-id>\n"
          "  %s selected\n"
          "  %s load-skt   (Phase C0 - the only write command)\n",
          argv0, argv0, argv0, argv0);
}

// IMPORTANT (found on a real Phase C0 device test - see
// PHASE_C0_FINDINGS.md "real-device test #2"): `count` below is the
// number of entries THIS List Configs call happened to return. It is
// NOT proven to be the total number of configurations actually stored
// in the modem's PDC database, and a specific config's absence from
// this list is NOT proof that config doesn't exist - a real config
// (CZ-VoLTE-Vodafone) vanished from one List Configs call and
// reappeared in a later one, while `info <its-exact-id>` succeeded
// throughout. Use `info <id>` for an authoritative existence/detail
// check on any specific config ID; treat `list` as a survey, not a
// ground truth enumeration.
int RunList(PdcClient* client) {
  ListConfigsOutcome outcome = client->ListConfigs(kTimeoutMs);
  if (outcome.status != ProbeStatus::kOk) {
    fprintf(stderr, "list failed: %s (error_status=%u error_code=%u)\n",
            ProbeStatusToString(outcome.status), outcome.data.error_status,
            outcome.data.error_code);
    return ExitCodeFor(outcome.status);
  }
  printf("count=%zu%s\n", outcome.data.configs.size(),
         outcome.data.truncated ? " (truncated at cap)" : "");
  for (const auto& cfg : outcome.data.configs) {
    printf("type=%s id=%s\n", ConfigTypeName(cfg.type), ToHex(cfg.id).c_str());
  }
  return kExitOk;
}

int RunInfo(PdcClient* client, const std::string& hex_id) {
  bool ok = false;
  std::vector<uint8_t> id = FromHex(hex_id, &ok);
  if (!ok) {
    fprintf(stderr, "invalid hex config id: %s\n", hex_id.c_str());
    return kExitUsage;
  }
  if (id.size() > guacamole_pdc::kMaxIdLen) {
    fprintf(stderr, "config id too long (%zu > %zu)\n", id.size(),
            guacamole_pdc::kMaxIdLen);
    return kExitUsage;
  }

  GetConfigInfoOutcome outcome =
      client->GetConfigInfo(ConfigType::kSoftware, id, kTimeoutMs);
  if (outcome.status != ProbeStatus::kOk) {
    fprintf(stderr, "info failed: %s (error_status=%u error_code=%u)\n",
            ProbeStatusToString(outcome.status), outcome.data.error_status,
            outcome.data.error_code);
    return ExitCodeFor(outcome.status);
  }
  printf("id=%s\n", hex_id.c_str());
  if (outcome.data.has_total_size) {
    printf("total_size=%u\n", outcome.data.total_size);
  }
  if (outcome.data.has_description) {
    printf("description=%s\n", outcome.data.description.c_str());
  }
  if (outcome.data.has_version) {
    printf("version=%u\n", outcome.data.version);
  }
  return kExitOk;
}

int RunSelected(PdcClient* client) {
  GetSelectedConfigOutcome outcome =
      client->GetSelectedConfig(ConfigType::kSoftware, kTimeoutMs);
  if (outcome.status != ProbeStatus::kOk) {
    fprintf(stderr, "selected failed: %s (error_status=%u error_code=%u)\n",
            ProbeStatusToString(outcome.status), outcome.data.error_status,
            outcome.data.error_code);
    return ExitCodeFor(outcome.status);
  }
  printf("type=%s\n", ConfigTypeName(ConfigType::kSoftware));
  printf("active_id=%s\n", outcome.data.active_id
                                ? ToHex(*outcome.data.active_id).c_str()
                                : "NONE");
  printf("pending_id=%s\n", outcome.data.pending_id
                                 ? ToHex(*outcome.data.pending_id).c_str()
                                 : "NONE");
  return kExitOk;
}

void PrintOptId(const char* label,
                 const std::optional<std::vector<uint8_t>>& id) {
  printf("%s=%s\n", label, id ? ToHex(*id).c_str() : "NONE");
}

// Prints a before/after selected-config snapshot, being explicit about
// the difference between "we queried and the modem said NONE" and "we
// never successfully queried at all" - conflating those two was a real
// bug found on the first Phase C0 real-device test (see
// PHASE_C0_FINDINGS.md).
void PrintSelectedSnapshot(const char* prefix, const SelectedSnapshot& snap) {
  if (!snap.queried) {
    printf("selected_active_%s=NOT_QUERIED\n", prefix);
    printf("selected_pending_%s=NOT_QUERIED\n", prefix);
    return;
  }
  PrintOptId((std::string("selected_active_") + prefix).c_str(),
             snap.active_id);
  PrintOptId((std::string("selected_pending_") + prefix).c_str(),
             snap.pending_id);
}

int RunLoadSktCommand(PdcClient* client) {
  fprintf(stderr,
          "load-skt: this WILL send a PDC Load Config write to the modem "
          "for the stock SKT MBN if all safety checks pass. No Select/"
          "Activate/Delete/EFS write will be performed.\n");

  SktLoadReport report = RunLoadSkt(*client, kTimeoutMs);

  printf("state=%s\n", LoadStateToString(report.final_state));
  printf("message=%s\n", report.message.c_str());
  printf("file_size=%u\n", report.file_size);
  printf("config_id=%s\n", ToHex(report.config_id).c_str());
  printf("chunks_sent=%u\n", report.chunks_sent);
  printf("bytes_sent=%u\n", report.bytes_sent);
  printf("config_presence_before=%s\n",
         ConfigPresenceToString(report.config_presence_before));
  printf("config_presence_after=%s\n",
         ConfigPresenceToString(report.config_presence_after));
  printf("already_existed_before_load=%s\n",
         report.already_existed_before_load ? "yes" : "no");
  PrintSelectedSnapshot("before", report.selected_before);
  PrintSelectedSnapshot("after", report.selected_after);
  printf("selection_check=%s\n",
         SelectionCheckResultToString(report.selection_check));
  // DIAGNOSTIC ONLY - a real Phase C0 device test found List Configs can
  // silently omit an entry that Get Config Info (config_presence_after
  // above) still resolves successfully. `count` in `list`'s own output
  // is the number of entries THAT List Configs call happened to return,
  // not a proven total of everything in the modem's PDC database. Never
  // treat found_in_list_after=no as proof of absence - config_presence_*
  // above is authoritative.
  printf("found_in_list_after=%s (diagnostic only, not authoritative)\n",
         report.found_in_list_after ? "yes" : "no");
  if (report.info_after.status == ProbeStatus::kOk) {
    printf("info_total_size=%u\n", report.info_after.data.total_size);
    if (report.info_after.data.has_description) {
      printf("info_description=%s\n",
             report.info_after.data.description.c_str());
    }
    if (report.info_after.data.has_version) {
      printf("info_version=%u\n", report.info_after.data.version);
    }
  }

  if (report.final_state == LoadState::kDone) {
    printf(
        "\n"
        "SKT load step complete. Manual verification sequence (do NOT "
        "automate reboot):\n"
        "  1. guacamole_pdc_probe list\n"
        "  2. guacamole_pdc_probe info %s\n"
        "  3. guacamole_pdc_probe selected\n"
        "  4. manually reboot the device\n"
        "  5. guacamole_pdc_probe list        (after reboot)\n"
        "  6. guacamole_pdc_probe info %s     (after reboot)\n"
        "  7. guacamole_pdc_probe selected    (after reboot)\n"
        "If the config is still present and selected/active is unchanged "
        "after reboot, that is direct real-device evidence for "
        "PDC_LOAD_CONFIG_PERSISTENT_AFTER_REBOOT and "
        "LOAD_DOES_NOT_IMPLICITLY_SELECT.\n",
        ToHex(report.config_id).c_str(), ToHex(report.config_id).c_str());
    return kExitOk;
  }
  if (report.final_state == LoadState::kUnknownResult) {
    return kExitLoadUnknownResult;
  }
  return kExitLoadFailed;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    PrintUsage(argv[0]);
    return kExitUsage;
  }
  const std::string cmd = argv[1];
  if (cmd == "info" && argc != 3) {
    PrintUsage(argv[0]);
    return kExitUsage;
  }
  if (cmd != "list" && cmd != "info" && cmd != "selected" &&
      cmd != "load-skt") {
    PrintUsage(argv[0]);
    return kExitUsage;
  }

  PdcClient client;
  // (major, minor, tool) for pdc_get_service_object_internal_v01.
  // CONFIRMED via radare2 cross-reference against this exact H.41
  // libril-qc-hal-qmi.so: two independent call sites (`pdc_init()` at
  // 0x1d40a5c and `PdcModemEndPointModule::getServiceObject()` at
  // 0x119b1a0) both load (1, 15, 6) - see
  // research_pdc_service_object_version.md for the full disassembly
  // evidence.
  //
  // Deliberately fail-closed with ONLY this one, confirmed value - no
  // fallback triples. This tool targets one specific device/firmware
  // (H.41 guacamole); silently accepting whatever OTHER version happens
  // to return a non-null service object risks getting a real object
  // whose wire format doesn't actually match the libqmi-derived TLV
  // parser in pdc_wire.cpp, which would be a much more confusing failure
  // mode than a clean, loud "the confirmed version didn't work" error
  // (see PHASE_B_FINDINGS.md's code-review addendum).
  std::vector<VersionTriple> version_candidates = {
      {1, 15, 6},
  };

  std::string init_log;
  if (!client.Init(version_candidates, &init_log)) {
    fprintf(stderr, "PDC client init failed:\n%s\n", init_log.c_str());
    return kExitInitFailed;
  }
  fprintf(stderr, "PDC client init log:\n%s", init_log.c_str());

  if (cmd == "list") return RunList(&client);
  if (cmd == "selected") return RunSelected(&client);
  if (cmd == "load-skt") return RunLoadSktCommand(&client);
  return RunInfo(&client, argv[2]);
}
