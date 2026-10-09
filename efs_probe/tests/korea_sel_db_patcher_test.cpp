#include "korea_sel_db_patcher.h"

#include <gtest/gtest.h>

using namespace guacamole_efs;

namespace {
std::vector<uint8_t> Bytes(const std::string& s) {
  return std::vector<uint8_t>(s.begin(), s.end());
}

// Synthetic baseline matching the REAL live mcfg_sel_db.xml's exact
// structural shape (see PHASE_D2_EFS2_WRITE_RESEARCH.md's Section A/B
// for the real file this was modeled on: mcfg_db_ver="1.2.3", ordinary
// carrier rules using <any_of>/<all_of>, a single Common fallback using
// bare <tristate_reset_all return="true" />, as the LAST rule before
// </policy>). Deliberately NOT the real 57,655-byte file itself -
// matching this project's established host-test convention of
// synthetic fixtures, not embedded real device data.
std::string CleanBaseline() {
  return
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<policy mcfg_db_ver=\"1.2.3\" name=\"generic\">\n"
      "  <initial>\n"
      "    <tristate_define name=\"iin\" value=\"undef\" />\n"
      "  </initial>\n"
      "  <if>\n"
      "    <any_of evaluate_all=\"true\">\n"
      "      <tristate_reset_all return=\"false\" />\n"
      "      <iin_in not_present=\"false\" store_in=\"iin\">898600</iin_in>\n"
      "    </any_of>\n"
      "    <then>\n"
      "      <SelRecord carrier_name=\"CMCC\" country_code=\"156\" "
      "iin=\"iin\" imsi_3gpp=\"3gpp_imsi\" mcfg_carrier_index=\"32\" "
      "volte=\"true\" vowifi=\"false\" />\n"
      "    </then>\n"
      "  </if>\n"
      "  <if>\n"
      "    <all_of>\n"
      "      <tristate_reset_all return=\"true\" />\n"
      "      <iin_in not_present=\"false\" store_in=\"iin\">898109"
      "</iin_in>\n"
      "    </all_of>\n"
      "    <then>\n"
      "      <SelRecord carrier_name=\"ICE\" country_code=\"578\" "
      "iin=\"iin\" imsi_3gpp=\"3gpp_imsi\" mcfg_carrier_index=\"40\" "
      "volte=\"true\" vowifi=\"true\"/>\n"
      "    </then>\n"
      "  </if>\n"
      "  <if>\n"
      "    <tristate_reset_all return=\"true\" />\n"
      "    <then>\n"
      "       <SelRecord carrier_name=\"Common\" iin=\"iin\" "
      "imsi_3gpp=\"3gpp_imsi\" mcfg_carrier_index=\"8\" />\n"
      "    </then>\n"
      "  </if>\n"
      "</policy>";
}
}  // namespace

TEST(KoreaSelDbPatcher, CleanBaselineBlockedByUnresolvedMapping) {
  auto result = GenerateKoreaPatch(Bytes(CleanBaseline()),
                                    CarrierIndexMappingState::kUnresolved);
  EXPECT_EQ(result.outcome, KoreaPatchOutcome::kBlockedMapping);
  EXPECT_TRUE(result.common_fallback_found);
  EXPECT_EQ(result.korea_markers_found, 0);
  EXPECT_TRUE(result.proposed_bytes.empty());  // never built while blocked.
}

TEST(KoreaSelDbPatcher, CommonFallbackInsertOffsetPrecedesCommonRecord) {
  std::string baseline = CleanBaseline();
  auto result =
      GenerateKoreaPatch(Bytes(baseline), CarrierIndexMappingState::kUnresolved);
  ASSERT_TRUE(result.common_fallback_found);
  // The computed insertion offset must point at the <if> that encloses
  // the Common record, i.e. strictly before "Common" appears in the text.
  size_t common_text_pos = baseline.find("carrier_name=\"Common\"");
  ASSERT_NE(common_text_pos, std::string::npos);
  EXPECT_LT(result.common_fallback_insert_offset, common_text_pos);
  // And it must land exactly on an "<if>" tag.
  EXPECT_EQ(baseline.substr(result.common_fallback_insert_offset, 4), "<if>");
}

TEST(KoreaSelDbPatcher, MissingPolicyRootRejectedAsMalformed) {
  auto result = GenerateKoreaPatch(Bytes("not xml at all"),
                                    CarrierIndexMappingState::kUnresolved);
  EXPECT_EQ(result.outcome, KoreaPatchOutcome::kMalformedBaseline);
}

TEST(KoreaSelDbPatcher, MissingCommonFallbackRejectedAsMalformed) {
  std::string no_common =
      "<policy mcfg_db_ver=\"1.2.3\" name=\"generic\">\n"
      "  <if><then><SelRecord carrier_name=\"X\" mcfg_carrier_index=\"1\" "
      "/></then></if>\n"
      "</policy>";
  auto result =
      GenerateKoreaPatch(Bytes(no_common), CarrierIndexMappingState::kUnresolved);
  EXPECT_EQ(result.outcome, KoreaPatchOutcome::kMalformedBaseline);
  EXPECT_FALSE(result.common_fallback_found);
}

TEST(KoreaSelDbPatcher, MultipleCommonRecordsRejectedAsMalformed) {
  std::string two_commons =
      "<policy mcfg_db_ver=\"1.2.3\" name=\"generic\">\n"
      "  <if><then><SelRecord carrier_name=\"Common\" "
      "mcfg_carrier_index=\"8\" /></then></if>\n"
      "  <if><then><SelRecord carrier_name=\"Common\" "
      "mcfg_carrier_index=\"8\" /></then></if>\n"
      "</policy>";
  auto result = GenerateKoreaPatch(Bytes(two_commons),
                                    CarrierIndexMappingState::kUnresolved);
  EXPECT_EQ(result.outcome, KoreaPatchOutcome::kMalformedBaseline);
}

TEST(KoreaSelDbPatcher, ThreeMarkersCannotProveAlreadyPatched) {
  std::string patched = CleanBaseline();
  // Insert all 3 Korean carrier identity markers (IIN form is enough
  // per the detector's own OR-of-IIN-or-PLMN logic).
  size_t insert_at = patched.find("<if>\n    <tristate_reset_all "
                                   "return=\"true\" />\n    <then>\n       "
                                   "<SelRecord carrier_name=\"Common\"");
  ASSERT_NE(insert_at, std::string::npos);
  std::string korea_block =
      "  <if><then><SelRecord carrier_name=\"SKT\" "
      "mcfg_carrier_index=\"39\"><!-- 898205 --></SelRecord></then></if>\n"
      "  <if><then><SelRecord carrier_name=\"KT\" "
      "mcfg_carrier_index=\"40\"><!-- 898230 --></SelRecord></then></if>\n"
      "  <if><then><SelRecord carrier_name=\"LGU\" "
      "mcfg_carrier_index=\"38\"><!-- 898206 --></SelRecord></then></if>\n";
  patched.insert(insert_at, korea_block);

  auto result =
      GenerateKoreaPatch(Bytes(patched), CarrierIndexMappingState::kUnresolved);
  EXPECT_EQ(result.outcome, KoreaPatchOutcome::kPartialKoreaStateRejected);
  EXPECT_EQ(result.korea_markers_found, 3);
}

TEST(KoreaSelDbPatcher, PartialStateWithOneMarkerRejected) {
  std::string partial = CleanBaseline();
  size_t insert_at = partial.find("<if>\n    <tristate_reset_all "
                                   "return=\"true\" />\n    <then>\n       "
                                   "<SelRecord carrier_name=\"Common\"");
  ASSERT_NE(insert_at, std::string::npos);
  partial.insert(insert_at,
                  "  <if><then><SelRecord carrier_name=\"SKT\" "
                  "mcfg_carrier_index=\"39\"><!-- 898205 -->"
                  "</SelRecord></then></if>\n");

  auto result =
      GenerateKoreaPatch(Bytes(partial), CarrierIndexMappingState::kUnresolved);
  EXPECT_EQ(result.outcome, KoreaPatchOutcome::kPartialKoreaStateRejected);
  EXPECT_EQ(result.korea_markers_found, 1);
}

TEST(KoreaSelDbPatcher, PartialStateWithTwoMarkersRejected) {
  std::string partial = CleanBaseline();
  size_t insert_at = partial.find("<if>\n    <tristate_reset_all "
                                   "return=\"true\" />\n    <then>\n       "
                                   "<SelRecord carrier_name=\"Common\"");
  ASSERT_NE(insert_at, std::string::npos);
  partial.insert(
      insert_at,
      "  <if><then><SelRecord carrier_name=\"SKT\" "
      "mcfg_carrier_index=\"39\"><!-- 450-05 --></SelRecord></then></if>\n"
      "  <if><then><SelRecord carrier_name=\"KT\" "
      "mcfg_carrier_index=\"40\"><!-- 450-08 --></SelRecord></then></if>\n");

  auto result =
      GenerateKoreaPatch(Bytes(partial), CarrierIndexMappingState::kUnresolved);
  EXPECT_EQ(result.outcome, KoreaPatchOutcome::kPartialKoreaStateRejected);
  EXPECT_EQ(result.korea_markers_found, 2);
}

TEST(KoreaSelDbPatcher, ConfirmedMappingStillDoesNotProduceRealPatch) {
  // Even if a FUTURE caller somehow constructs kConfirmed, this
  // function's own implementation deliberately has no real SelRecord-
  // building logic behind it yet (see the .cpp file's own comment) -
  // it must never silently return a plausible-looking but fabricated
  // proposed image.
  auto result = GenerateKoreaPatch(Bytes(CleanBaseline()),
                                    CarrierIndexMappingState::kConfirmed);
  EXPECT_EQ(result.outcome, KoreaPatchOutcome::kBlockedMapping);
  // Whatever it returns, it must NOT be a populated real patch with
  // guessed content.
  if (result.outcome == KoreaPatchOutcome::kOk) {
    EXPECT_TRUE(result.proposed_bytes.empty());
  }
}
