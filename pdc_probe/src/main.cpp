// guacamole_pdc_probe - Phase B read-only PDC observation tool.
//
//   guacamole_pdc_probe list
//   guacamole_pdc_probe info <hex-config-id>
//
// This binary NEVER calls Load Config / Set Selected Config / Activate
// Config / Deactivate Config / Delete Config, and never touches EFS2 in
// any way. It only issues PDC List Configs and Get Config Info, both
// pure read operations against the modem's existing, already-persisted
// PDC config database. See PHASE_B_FINDINGS.md for what this is for.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "pdc_client.h"

namespace {

using guacamole_pdc::ConfigType;
using guacamole_pdc::GetConfigInfoOutcome;
using guacamole_pdc::ListConfigsOutcome;
using guacamole_pdc::PdcClient;
using guacamole_pdc::ProbeStatus;
using guacamole_pdc::ProbeStatusToString;
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
          "  %s info <hex-config-id>\n",
          argv0, argv0);
}

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
  if (cmd != "list" && cmd != "info") {
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
  return RunInfo(&client, argv[2]);
}
