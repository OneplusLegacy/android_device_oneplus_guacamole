#include "korea_sel_db_patcher.h"

#include <array>

namespace guacamole_efs {

namespace {

std::string ToStr(const std::vector<uint8_t>& bytes) {
  return std::string(bytes.begin(), bytes.end());
}

size_t CountOccurrences(const std::string& haystack, const std::string& needle) {
  size_t count = 0;
  size_t pos = 0;
  while ((pos = haystack.find(needle, pos)) != std::string::npos) {
    count++;
    pos += needle.size();
  }
  return count;
}

// The 3 Korean carriers' known identity markers - SKT/KT/LGU, per this
// project's long-standing research (guacamole_efsd_design_report.md /
// PHASE_C0_FINDINGS.md). Presence of EITHER the IIN or the PLMN marker
// counts as "this carrier's identity appears somewhere in the
// baseline" - deliberately permissive for idempotency/partial-state
// DETECTION (we want to notice even a half-applied patch), never used
// to construct a real patch.
struct KoreaMarker {
  const char* iin;
  const char* plmn;
};
constexpr std::array<KoreaMarker, 3> kKoreaMarkers = {{
    {"898205", "450-05"},  // SKT
    {"898230", "450-08"},  // KT
    {"898206", "450-06"},  // LGU
}};

}  // namespace

KoreaPatchResult GenerateKoreaPatch(const std::vector<uint8_t>& baseline_xml,
                                     CarrierIndexMappingState mapping_state) {
  KoreaPatchResult result;
  const std::string text = ToStr(baseline_xml);

  // --- Baseline structural validation ---
  if (text.find("<policy") == std::string::npos ||
      text.find("</policy>") == std::string::npos) {
    result.outcome = KoreaPatchOutcome::kMalformedBaseline;
    result.message = "baseline does not contain a <policy>...</policy> root";
    return result;
  }

  const std::string kCommonMarker = "carrier_name=\"Common\"";
  size_t common_count = CountOccurrences(text, kCommonMarker);
  if (common_count != 1) {
    result.outcome = KoreaPatchOutcome::kMalformedBaseline;
    result.message = "expected exactly one carrier_name=\"Common\" fallback "
                      "record, found " +
                      std::to_string(common_count);
    return result;
  }

  size_t common_pos = text.find(kCommonMarker);
  size_t if_tag_pos = text.rfind("<if>", common_pos);
  if (if_tag_pos == std::string::npos) {
    result.outcome = KoreaPatchOutcome::kMalformedBaseline;
    result.message = "could not find the enclosing <if> block for the "
                      "Common fallback record";
    return result;
  }
  result.common_fallback_found = true;
  result.common_fallback_insert_offset = if_tag_pos;

  // --- Idempotency / partial-state detection ---
  int found = 0;
  for (const auto& m : kKoreaMarkers) {
    if (text.find(m.iin) != std::string::npos ||
        text.find(m.plmn) != std::string::npos) {
      found++;
    }
  }
  result.korea_markers_found = found;

  if (found == 3) {
    result.outcome = KoreaPatchOutcome::kPartialKoreaStateRejected;
    result.message = "all 3 identity markers present without structural proof "
                      "in baseline - HARD_STOP/manual review";
    return result;
  }
  if (found == 1 || found == 2) {
    result.outcome = KoreaPatchOutcome::kPartialKoreaStateRejected;
    result.message = std::to_string(found) +
                      " of 3 Korean carrier identity markers found - "
                      "ambiguous partial state, refuse to touch";
    return result;
  }

  // found == 0: a clean, structurally patchable baseline - but the
  // carrier-index mapping is still the final, currently-unconditional
  // gate (see this file's header comment).
  if (mapping_state != CarrierIndexMappingState::kConfirmed) {
    result.outcome = KoreaPatchOutcome::kBlockedMapping;
    result.message =
        "carrier-index mapping (LGU/SKT/KT <-> mcfg_carrier_index) is not "
        "confirmed - refuse to generate a real Korea XML patch; see "
        "PHASE_D2_EFS2_WRITE_RESEARCH.md Section C";
    return result;
  }

  // Legacy bool-like confirmation carries neither per-carrier indices nor
  // evidence. It can never authorize generation or claim idempotence.
  // Offline tools/korea_sel_db_generator.py owns strict proposal generation.
  result.outcome = KoreaPatchOutcome::kBlockedMapping;
  result.message = "GENERATE_BLOCKED_UNCONFIRMED_MAPPING: use typed offline evidence";
  return result;
}

}  // namespace guacamole_efs
