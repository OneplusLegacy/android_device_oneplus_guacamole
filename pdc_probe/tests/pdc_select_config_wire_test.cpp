#include "pdc_select_config_wire.h"

#include <cstring>

#include <gtest/gtest.h>

using namespace guacamole_pdc;

namespace {

std::vector<uint8_t> BuildSetSelectedIndication(uint32_t token,
                                                  uint16_t err_status,
                                                  bool include_token = true) {
  std::vector<uint8_t> body;
  uint8_t err_le[2] = {static_cast<uint8_t>(err_status & 0xFF),
                        static_cast<uint8_t>((err_status >> 8) & 0xFF)};
  AppendTlv(&body, kTlvIndicationResult, err_le, 2);
  if (include_token) {
    AppendTlvU32(&body, kTlvToken, token);
  }
  return body;
}

}  // namespace

// --- BuildSetSelectedConfigRequest() ---

TEST(PdcSelectConfigWire, BuildRequestContainsTokenAndConfigTypeAndId) {
  std::vector<uint8_t> id = {0xAA, 0xBB, 0xCC};
  auto req = BuildSetSelectedConfigRequest(42, ConfigType::kSoftware, id);
  ASSERT_FALSE(req.empty());

  // Decode it back the same way a real device's request parser would -
  // exercising the exact TLV shape this function produces.
  size_t offset = 0;
  bool found_config_type_and_id = false;
  bool found_token = false;
  while (offset < req.size()) {
    size_t next = 0;
    auto tlv = DecodeOneTlv(req.data(), req.size(), offset, &next);
    ASSERT_TRUE(tlv.has_value());
    if (tlv->type == kTlvConfigTypeAndId) {
      found_config_type_and_id = true;
      // {Config Type: guint32 LE}{Id: guint8-size-prefixed array}
      ASSERT_EQ(tlv->length, 4 + 1 + id.size());
      uint32_t type_le = tlv->value[0] | (tlv->value[1] << 8) |
                          (tlv->value[2] << 16) | (tlv->value[3] << 24);
      EXPECT_EQ(type_le, static_cast<uint32_t>(ConfigType::kSoftware));
      EXPECT_EQ(tlv->value[4], id.size());
      std::vector<uint8_t> decoded_id(tlv->value + 5,
                                       tlv->value + tlv->length);
      EXPECT_EQ(decoded_id, id);
    } else if (tlv->type == kTlvToken) {
      found_token = true;
      auto tok = TlvAsU32(*tlv);
      ASSERT_TRUE(tok.has_value());
      EXPECT_EQ(*tok, 42u);
    }
    offset = next;
  }
  EXPECT_TRUE(found_config_type_and_id);
  EXPECT_TRUE(found_token);
}

TEST(PdcSelectConfigWire, BuildRequestUsesSameEncodingAsGetConfigInfoRequest) {
  // Item K's own header comment claims byte-for-byte identical encoding
  // to BuildGetConfigInfoRequest() (same common-ref TLV) - verified
  // directly here, not just asserted in a comment.
  std::vector<uint8_t> id = {0x01, 0x02, 0x03, 0x04};
  auto select_req = BuildSetSelectedConfigRequest(7, ConfigType::kSoftware, id);
  auto info_req = BuildGetConfigInfoRequest(7, ConfigType::kSoftware, id);
  EXPECT_EQ(select_req, info_req);
}

TEST(PdcSelectConfigWire, BuildRequestRejectsOversizedId) {
  std::vector<uint8_t> id(256, 0xAB);  // exceeds the guint8 length prefix.
  auto req = BuildSetSelectedConfigRequest(1, ConfigType::kSoftware, id);
  EXPECT_TRUE(req.empty());
}

TEST(PdcSelectConfigWire, BuildRequestHandlesPlatformConfigType) {
  std::vector<uint8_t> id = {0x01};
  auto req = BuildSetSelectedConfigRequest(1, ConfigType::kPlatform, id);
  ASSERT_FALSE(req.empty());
  size_t offset = 0;
  size_t next = 0;
  auto tlv = DecodeOneTlv(req.data(), req.size(), offset, &next);
  ASSERT_TRUE(tlv.has_value());
  ASSERT_EQ(tlv->type, kTlvConfigTypeAndId);
  uint32_t type_le = tlv->value[0] | (tlv->value[1] << 8) |
                      (tlv->value[2] << 16) | (tlv->value[3] << 24);
  EXPECT_EQ(type_le, static_cast<uint32_t>(ConfigType::kPlatform));
}

// --- ParseSetSelectedConfigIndication() ---

TEST(PdcSelectConfigWire, ParseIndicationSuccess) {
  auto body = BuildSetSelectedIndication(5, 0);
  auto result = ParseSetSelectedConfigIndication(body.data(), body.size(), 5);
  ASSERT_TRUE(result.ok);
  EXPECT_EQ(result.error_status, 0);
}

TEST(PdcSelectConfigWire, ParseIndicationErrorStatusSurfaced) {
  auto body = BuildSetSelectedIndication(6, 50);  // QMI_PROTOCOL_ERROR_NO_ENTRY-style.
  auto result = ParseSetSelectedConfigIndication(body.data(), body.size(), 6);
  ASSERT_TRUE(result.ok);  // TLV stream itself decoded fine...
  EXPECT_EQ(result.error_status, 50);  // ...but Classify() is what callers
                                        // must use to decide success.
}

TEST(PdcSelectConfigWire, ParseIndicationTokenMismatchRejected) {
  auto body = BuildSetSelectedIndication(7, 0);
  auto result = ParseSetSelectedConfigIndication(body.data(), body.size(), 999);
  EXPECT_FALSE(result.ok);
}

TEST(PdcSelectConfigWire, ParseIndicationMissingTokenAcceptedWhenNoExpectation) {
  auto body = BuildSetSelectedIndication(8, 0, /*include_token=*/false);
  auto result = ParseSetSelectedConfigIndication(body.data(), body.size(),
                                                   std::nullopt);
  EXPECT_TRUE(result.ok);
}

TEST(PdcSelectConfigWire, ParseIndicationMissingIndicationResultRejected) {
  std::vector<uint8_t> body;
  AppendTlvU32(&body, kTlvToken, 9);  // no Indication Result TLV at all.
  auto result = ParseSetSelectedConfigIndication(body.data(), body.size(), 9);
  EXPECT_FALSE(result.ok);
}

TEST(PdcSelectConfigWire, ParseIndicationTruncatedBufferRejected) {
  auto body = BuildSetSelectedIndication(10, 0);
  body.resize(body.size() - 1);  // truncate mid-TLV.
  auto result = ParseSetSelectedConfigIndication(body.data(), body.size(), 10);
  EXPECT_FALSE(result.ok);
}

TEST(PdcSelectConfigWire, ParseIndicationUnknownTlvSkippedNotFatal) {
  std::vector<uint8_t> body;
  AppendTlvU32(&body, kTlvToken, 11);
  uint8_t err_le[2] = {0, 0};
  AppendTlv(&body, kTlvIndicationResult, err_le, 2);
  // A hypothetical future-firmware extra field this parser doesn't know
  // about - must not be fatal, matching every other Parse* function's
  // forward-compatibility policy.
  uint8_t extra[4] = {1, 2, 3, 4};
  AppendTlv(&body, 0x99, extra, sizeof(extra));

  auto result = ParseSetSelectedConfigIndication(body.data(), body.size(), 11);
  EXPECT_TRUE(result.ok);
  EXPECT_EQ(result.error_status, 0);
}

TEST(PdcSelectConfigWire, ParseIndicationNullBufferRejected) {
  auto result = ParseSetSelectedConfigIndication(nullptr, 0, std::nullopt);
  EXPECT_FALSE(result.ok);
}

// --- ClassifySetSelectedConfigResult() ---

TEST(PdcSelectConfigWire, ClassifySuccess) {
  auto body = BuildSetSelectedIndication(20, 0);
  auto result = ParseSetSelectedConfigIndication(body.data(), body.size(), 20);
  EXPECT_EQ(ClassifySetSelectedConfigResult(result), IndicationOutcome::kSuccess);
}

TEST(PdcSelectConfigWire, ClassifyNonzeroErrorStatusIsRejectedNotSuccess) {
  auto body = BuildSetSelectedIndication(21, 1);
  auto result = ParseSetSelectedConfigIndication(body.data(), body.size(), 21);
  EXPECT_EQ(ClassifySetSelectedConfigResult(result), IndicationOutcome::kRejected);
}

TEST(PdcSelectConfigWire, ClassifyMalformedIsNeverSuccess) {
  SetSelectedConfigResult result;  // default: ok=false.
  EXPECT_EQ(ClassifySetSelectedConfigResult(result), IndicationOutcome::kMalformed);
}

TEST(PdcSelectConfigWire, ClassifyWrongTokenIsMalformed) {
  auto body = BuildSetSelectedIndication(22, 0);
  auto result = ParseSetSelectedConfigIndication(body.data(), body.size(), 999);
  EXPECT_EQ(ClassifySetSelectedConfigResult(result), IndicationOutcome::kMalformed);
}

// --- Message id sanity (item K's own research finding) ---

TEST(PdcSelectConfigWire, MessageIdMatchesResearchedValue) {
  EXPECT_EQ(kMsgSetSelectedConfig, 0x0023u);
}
