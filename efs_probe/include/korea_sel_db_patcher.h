// Legacy inspection API retained for existing callers. Never emits XML.
// A kConfirmed flag lacks per-carrier indices/evidence and remains blocked.
// Strict offline generator: tools/korea_sel_db_generator.py.
// Marker presence alone requires manual review, never AlreadyPatched.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace guacamole_efs {
enum class CarrierIndexMappingState { kUnresolved, kConfirmed };
enum class KoreaPatchOutcome {
  kBlockedMapping,
  kMalformedBaseline,
  kAlreadyPatched,  // Reserved; legacy inspector cannot establish this.
  kPartialKoreaStateRejected,
  kOk,             // Reserved; legacy inspector cannot establish this.
};
struct KoreaPatchResult {
  KoreaPatchOutcome outcome = KoreaPatchOutcome::kBlockedMapping;
  std::string message;
  bool common_fallback_found = false;
  size_t common_fallback_insert_offset = 0;
  int korea_markers_found = 0;
  std::vector<uint8_t> proposed_bytes;  // Always empty for this legacy API.
};
KoreaPatchResult GenerateKoreaPatch(const std::vector<uint8_t>& baseline_xml,
                                  CarrierIndexMappingState mapping_state);
}  // namespace guacamole_efs
