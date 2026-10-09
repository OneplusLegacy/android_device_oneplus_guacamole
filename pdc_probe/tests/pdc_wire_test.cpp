#include "pdc_wire.h"

#include <cstring>

#include <gtest/gtest.h>

using namespace guacamole_pdc;

namespace {

// Hand-builds a raw List Configs indication body matching the on-wire
// shape documented in pdc_wire.h, so these tests exercise the exact same
// decode path the real device's indication would go through, without any
// dependency on a live modem or the vendor .so.
std::vector<uint8_t> BuildListIndication(uint32_t token, uint16_t err_status,
                                          const std::vector<ConfigEntry>& configs) {
  std::vector<uint8_t> body;
  AppendTlvU32(&body, kTlvToken, token);
  uint8_t err_le[2] = {static_cast<uint8_t>(err_status & 0xFF),
                        static_cast<uint8_t>((err_status >> 8) & 0xFF)};
  AppendTlv(&body, kTlvIndicationResult, err_le, 2);

  std::vector<uint8_t> configs_value;
  configs_value.push_back(static_cast<uint8_t>(configs.size()));
  for (const auto& c : configs) {
    uint32_t t = static_cast<uint32_t>(c.type);
    configs_value.push_back(static_cast<uint8_t>(t & 0xFF));
    configs_value.push_back(static_cast<uint8_t>((t >> 8) & 0xFF));
    configs_value.push_back(static_cast<uint8_t>((t >> 16) & 0xFF));
    configs_value.push_back(static_cast<uint8_t>((t >> 24) & 0xFF));
    configs_value.push_back(static_cast<uint8_t>(c.id.size()));
    configs_value.insert(configs_value.end(), c.id.begin(), c.id.end());
  }
  AppendTlv(&body, kTlvListConfigsIndConfigs, configs_value.data(),
            configs_value.size());
  return body;
}

}  // namespace

TEST(PdcWire, ListConfigsEmptyList) {
  auto body = BuildListIndication(7, 0, {});
  auto result = ParseListConfigsIndication(body.data(), body.size(), 7);
  ASSERT_TRUE(result.ok);
  EXPECT_EQ(result.error_status, 0);
  EXPECT_TRUE(result.configs.empty());
  EXPECT_FALSE(result.truncated);
}

TEST(PdcWire, ListConfigsOneEntry) {
  ConfigEntry e;
  e.type = ConfigType::kSoftware;
  e.id = {0x01, 0x02, 0x03};
  auto body = BuildListIndication(1, 0, {e});
  auto result = ParseListConfigsIndication(body.data(), body.size(), 1);
  ASSERT_TRUE(result.ok);
  ASSERT_EQ(result.configs.size(), 1u);
  EXPECT_EQ(result.configs[0].type, ConfigType::kSoftware);
  EXPECT_EQ(result.configs[0].id, (std::vector<uint8_t>{0x01, 0x02, 0x03}));
}

TEST(PdcWire, ListConfigsMultipleEntries) {
  ConfigEntry a, b, c;
  a.type = ConfigType::kSoftware; a.id = {0xAA};
  b.type = ConfigType::kSoftware; b.id = {0xBB, 0xBB};
  c.type = ConfigType::kPlatform; c.id = {};
  auto body = BuildListIndication(2, 0, {a, b, c});
  auto result = ParseListConfigsIndication(body.data(), body.size(), 2);
  ASSERT_TRUE(result.ok);
  ASSERT_EQ(result.configs.size(), 3u);
  EXPECT_EQ(result.configs[2].type, ConfigType::kPlatform);
  EXPECT_TRUE(result.configs[2].id.empty());
}

TEST(PdcWire, ListConfigsTokenMismatchRejected) {
  auto body = BuildListIndication(5, 0, {});
  auto result = ParseListConfigsIndication(body.data(), body.size(),
                                            /*expected_token=*/6);
  EXPECT_FALSE(result.ok) << "an indication for a different, stale/foreign "
                              "request must never be accepted as ours";
}

TEST(PdcWire, ListConfigsOversizedConfigIdRejected) {
  // Build a config-list TLV by hand where the declared id length exceeds
  // kMaxIdLen (124), even though the TLV framing itself is well-formed.
  std::vector<uint8_t> body;
  AppendTlvU32(&body, kTlvToken, 9);
  uint8_t err_le[2] = {0, 0};
  AppendTlv(&body, kTlvIndicationResult, err_le, 2);

  std::vector<uint8_t> configs_value;
  configs_value.push_back(1);  // one element
  configs_value.push_back(1); configs_value.push_back(0);
  configs_value.push_back(0); configs_value.push_back(0);  // Config Type = 1
  configs_value.push_back(200);  // id length byte says 200 (> kMaxIdLen)
  std::vector<uint8_t> big_id(200, 0x41);
  configs_value.insert(configs_value.end(), big_id.begin(), big_id.end());
  AppendTlv(&body, kTlvListConfigsIndConfigs, configs_value.data(),
            configs_value.size());

  auto result = ParseListConfigsIndication(body.data(), body.size(), 9);
  EXPECT_FALSE(result.ok);
}

TEST(PdcWire, ListConfigsMalformedElementCountRejected) {
  // Config array claims 3 elements but only carries data for one.
  std::vector<uint8_t> body;
  AppendTlvU32(&body, kTlvToken, 3);
  uint8_t err_le[2] = {0, 0};
  AppendTlv(&body, kTlvIndicationResult, err_le, 2);
  std::vector<uint8_t> configs_value = {3, 1, 0, 0, 0, 0};  // count=3, 1 short entry
  AppendTlv(&body, kTlvListConfigsIndConfigs, configs_value.data(),
            configs_value.size());

  auto result = ParseListConfigsIndication(body.data(), body.size(), 3);
  EXPECT_FALSE(result.ok);
}

TEST(PdcWire, ListConfigsTruncatedBufferRejected) {
  ConfigEntry e;
  e.type = ConfigType::kSoftware;
  e.id = {1, 2, 3, 4};
  auto body = BuildListIndication(4, 0, {e});
  body.resize(body.size() - 3);  // cut off the tail mid-TLV
  auto result = ParseListConfigsIndication(body.data(), body.size(), 4);
  EXPECT_FALSE(result.ok);
}

TEST(PdcWire, ListConfigsUnknownTlvSkippedNotFatal) {
  auto body = BuildListIndication(8, 0, {});
  // Append an unrecognized TLV (type 0x99) after the well-known ones -
  // forward-compatible fields must not break parsing of what we do know.
  uint8_t junk[3] = {0xDE, 0xAD, 0xBE};
  AppendTlv(&body, 0x99, junk, sizeof(junk));
  auto result = ParseListConfigsIndication(body.data(), body.size(), 8);
  EXPECT_TRUE(result.ok);
}

TEST(PdcWire, ListConfigsCapEnforcedAndTruncatedFlagSet) {
  std::vector<ConfigEntry> many;
  for (size_t i = 0; i < kMaxConfigs + 5; i++) {
    ConfigEntry e;
    e.type = ConfigType::kSoftware;
    e.id = {static_cast<uint8_t>(i & 0xFF)};
    many.push_back(e);
  }
  auto body = BuildListIndication(10, 0, many);
  auto result = ParseListConfigsIndication(body.data(), body.size(), 10);
  ASSERT_TRUE(result.ok);
  EXPECT_EQ(result.configs.size(), kMaxConfigs);
  EXPECT_TRUE(result.truncated);
}

TEST(PdcWire, GetConfigInfoFullResponse) {
  std::vector<uint8_t> body;
  AppendTlvU32(&body, kTlvToken, 11);
  uint8_t err_le[2] = {0, 0};
  AppendTlv(&body, kTlvIndicationResult, err_le, 2);
  AppendTlvU32(&body, kTlvGetConfigInfoIndTotalSize, 4096);
  const char* desc = "OnePlus Korea SW";
  AppendTlvU8PrefixedBytes(&body, kTlvGetConfigInfoIndDescription,
                            reinterpret_cast<const uint8_t*>(desc),
                            strlen(desc));
  AppendTlvU32(&body, kTlvGetConfigInfoIndVersion, 7);

  auto result = ParseGetConfigInfoIndication(body.data(), body.size(), 11);
  ASSERT_TRUE(result.ok);
  EXPECT_TRUE(result.has_total_size);
  EXPECT_EQ(result.total_size, 4096u);
  EXPECT_TRUE(result.has_description);
  EXPECT_EQ(result.description, "OnePlus Korea SW");
  EXPECT_TRUE(result.has_version);
  EXPECT_EQ(result.version, 7u);
}

TEST(PdcWire, GetConfigInfoErrorStatusSurfaced) {
  std::vector<uint8_t> body;
  AppendTlvU32(&body, kTlvToken, 12);
  uint8_t err_le[2] = {1, 0};  // non-zero: modem-reported failure
  AppendTlv(&body, kTlvIndicationResult, err_le, 2);
  auto result = ParseGetConfigInfoIndication(body.data(), body.size(), 12);
  ASSERT_TRUE(result.ok);  // TLV stream itself decoded fine...
  EXPECT_EQ(result.error_status, 1);  // ...but ClassifyGetConfigInfoResult
                                       // (not `.ok`) is what callers must
                                       // use to decide success - see below.
}

// --- IndicationOutcome classification: a decoded-but-modem-rejected
// indication must never be treated the same as a real success. This is
// the regression test for the code-review finding that PdcClient
// originally mapped `.ok == true` straight to ProbeStatus::kOk without
// checking `.error_status` at all. ---

TEST(PdcWire, ClassifyListConfigsSuccess) {
  auto body = BuildListIndication(20, 0, {});
  auto result = ParseListConfigsIndication(body.data(), body.size(), 20);
  EXPECT_EQ(ClassifyListConfigsResult(result), IndicationOutcome::kSuccess);
}

TEST(PdcWire, ClassifyListConfigsNonzeroErrorStatusIsRejectedNotSuccess) {
  auto body = BuildListIndication(21, /*err_status=*/7, {});
  auto result = ParseListConfigsIndication(body.data(), body.size(), 21);
  ASSERT_TRUE(result.ok) << "TLV stream decodes fine even when the modem "
                            "reports an error - that's a separate axis.";
  EXPECT_EQ(ClassifyListConfigsResult(result), IndicationOutcome::kRejected)
      << "a correct token with a nonzero Indication Result must NEVER "
         "classify as kSuccess";
}

TEST(PdcWire, ClassifyListConfigsWrongTokenIsMalformed) {
  auto body = BuildListIndication(22, 0, {});
  auto result = ParseListConfigsIndication(body.data(), body.size(),
                                            /*expected_token=*/23);
  EXPECT_EQ(ClassifyListConfigsResult(result), IndicationOutcome::kMalformed);
}

TEST(PdcWire, ClassifyGetConfigInfoSuccess) {
  std::vector<uint8_t> body;
  AppendTlvU32(&body, kTlvToken, 24);
  uint8_t ok_status[2] = {0, 0};
  AppendTlv(&body, kTlvIndicationResult, ok_status, 2);
  auto result = ParseGetConfigInfoIndication(body.data(), body.size(), 24);
  EXPECT_EQ(ClassifyGetConfigInfoResult(result), IndicationOutcome::kSuccess);
}

TEST(PdcWire, ClassifyGetConfigInfoNonzeroErrorStatusIsRejectedNotSuccess) {
  std::vector<uint8_t> body;
  AppendTlvU32(&body, kTlvToken, 25);
  uint8_t err_status[2] = {3, 0};
  AppendTlv(&body, kTlvIndicationResult, err_status, 2);
  auto result = ParseGetConfigInfoIndication(body.data(), body.size(), 25);
  ASSERT_TRUE(result.ok);
  EXPECT_EQ(ClassifyGetConfigInfoResult(result), IndicationOutcome::kRejected);
}

TEST(PdcWire, ClassifyGetConfigInfoMalformedIsNeverSuccess) {
  std::vector<uint8_t> body;  // empty: no Indication Result TLV at all.
  auto result = ParseGetConfigInfoIndication(body.data(), body.size(), 26);
  EXPECT_EQ(ClassifyGetConfigInfoResult(result), IndicationOutcome::kMalformed);
}

TEST(OperationResult, ParsesZeroStatus) {
  std::vector<uint8_t> body;
  uint8_t value[4] = {0, 0, 0, 0};
  AppendTlv(&body, kTlvOperationResult, value, 4);
  uint16_t est = 99, ecode = 99;
  ASSERT_TRUE(ParseOperationResultOnly(body.data(), body.size(), &est, &ecode));
  EXPECT_EQ(est, 0);
  EXPECT_EQ(ecode, 0);
}

TEST(OperationResult, MissingTlvFails) {
  std::vector<uint8_t> body;
  AppendTlvU32(&body, kTlvToken, 1);  // no Operation Result TLV at all
  uint16_t est = 0, ecode = 0;
  EXPECT_FALSE(ParseOperationResultOnly(body.data(), body.size(), &est, &ecode));
}

TEST(OperationResult, WrongLengthFails) {
  std::vector<uint8_t> body;
  uint8_t value[2] = {0, 0};  // Operation Result must be exactly 4 bytes
  AppendTlv(&body, kTlvOperationResult, value, 2);
  uint16_t est = 0, ecode = 0;
  EXPECT_FALSE(ParseOperationResultOnly(body.data(), body.size(), &est, &ecode));
}

TEST(OperationResult, EmptySyncAckIsMalformedNotRejected) {
  // Regression test for the code-review finding that PdcClient used to
  // fall through to WaitForIndication() whenever ParseOperationResultOnly
  // returned false (malformed ack), instead of failing fast with
  // kMalformedResponse. This test pins the wire-level contract that
  // PdcClient's fix depends on: a malformed/empty sync ack must report
  // failure to decode, not a false "everything's fine, est==0" default.
  uint16_t est = 0, ecode = 0;
  EXPECT_FALSE(ParseOperationResultOnly(nullptr, 0, &est, &ecode));
}

// --- Phase B2: Get Selected Config ---

namespace {
std::vector<uint8_t> BuildSelectedIndication(
    uint32_t token, uint16_t err_status,
    const std::optional<std::vector<uint8_t>>& active_id,
    const std::optional<std::vector<uint8_t>>& pending_id) {
  std::vector<uint8_t> body;
  AppendTlvU32(&body, kTlvToken, token);
  uint8_t err_le[2] = {static_cast<uint8_t>(err_status & 0xFF),
                        static_cast<uint8_t>((err_status >> 8) & 0xFF)};
  AppendTlv(&body, kTlvIndicationResult, err_le, 2);
  if (active_id) {
    AppendTlvU8PrefixedBytes(&body, kTlvGetSelectedConfigIndActiveId,
                              active_id->data(), active_id->size());
  }
  if (pending_id) {
    AppendTlvU8PrefixedBytes(&body, kTlvGetSelectedConfigIndPendingId,
                              pending_id->data(), pending_id->size());
  }
  return body;
}
}  // namespace

TEST(PdcWireSelected, ActiveOnly) {
  std::vector<uint8_t> active = {0x0b, 0xd7, 0x1c};
  auto body = BuildSelectedIndication(30, 0, active, std::nullopt);
  auto result = ParseGetSelectedConfigIndication(body.data(), body.size(), 30);
  ASSERT_TRUE(result.ok);
  EXPECT_EQ(ClassifyGetSelectedConfigResult(result), IndicationOutcome::kSuccess);
  ASSERT_TRUE(result.active_id.has_value());
  EXPECT_EQ(*result.active_id, active);
  EXPECT_FALSE(result.pending_id.has_value());
}

TEST(PdcWireSelected, PendingOnly) {
  std::vector<uint8_t> pending = {0xaa, 0xbb};
  auto body = BuildSelectedIndication(31, 0, std::nullopt, pending);
  auto result = ParseGetSelectedConfigIndication(body.data(), body.size(), 31);
  ASSERT_TRUE(result.ok);
  EXPECT_FALSE(result.active_id.has_value());
  ASSERT_TRUE(result.pending_id.has_value());
  EXPECT_EQ(*result.pending_id, pending);
}

TEST(PdcWireSelected, ActiveAndPending) {
  std::vector<uint8_t> active = {0x01};
  std::vector<uint8_t> pending = {0x02, 0x03};
  auto body = BuildSelectedIndication(32, 0, active, pending);
  auto result = ParseGetSelectedConfigIndication(body.data(), body.size(), 32);
  ASSERT_TRUE(result.ok);
  ASSERT_TRUE(result.active_id.has_value());
  ASSERT_TRUE(result.pending_id.has_value());
  EXPECT_EQ(*result.active_id, active);
  EXPECT_EQ(*result.pending_id, pending);
}

TEST(PdcWireSelected, NeitherPresentIsStillValidSuccess) {
  // A device with no config selected yet at all is a legitimate state,
  // not an error - both fields absent means NONE/NONE, not malformed.
  auto body = BuildSelectedIndication(33, 0, std::nullopt, std::nullopt);
  auto result = ParseGetSelectedConfigIndication(body.data(), body.size(), 33);
  ASSERT_TRUE(result.ok);
  EXPECT_EQ(ClassifyGetSelectedConfigResult(result), IndicationOutcome::kSuccess);
  EXPECT_FALSE(result.active_id.has_value());
  EXPECT_FALSE(result.pending_id.has_value());
}

TEST(PdcWireSelected, VariableLengthIdNotAssumed20Bytes) {
  // Regression guard: do not assume a fixed 20-byte id just because the
  // real device's Common-Commercial config happened to have one.
  std::vector<uint8_t> long_active(kMaxIdLen, 0x7A);  // right at the cap
  auto body = BuildSelectedIndication(34, 0, long_active, std::nullopt);
  auto result = ParseGetSelectedConfigIndication(body.data(), body.size(), 34);
  ASSERT_TRUE(result.ok);
  ASSERT_TRUE(result.active_id.has_value());
  EXPECT_EQ(result.active_id->size(), kMaxIdLen);

  std::vector<uint8_t> short_active = {0x01};  // 1 byte, also valid
  auto body2 = BuildSelectedIndication(35, 0, short_active, std::nullopt);
  auto result2 = ParseGetSelectedConfigIndication(body2.data(), body2.size(), 35);
  ASSERT_TRUE(result2.ok);
  ASSERT_TRUE(result2.active_id.has_value());
  EXPECT_EQ(result2.active_id->size(), 1u);
}

TEST(PdcWireSelected, MalformedActiveIdTlvRejected) {
  std::vector<uint8_t> body;
  AppendTlvU32(&body, kTlvToken, 36);
  uint8_t err_le[2] = {0, 0};
  AppendTlv(&body, kTlvIndicationResult, err_le, 2);
  // Active Id TLV whose 1-byte size prefix (claims 10) exceeds the TLV's
  // own outer length (only 3 bytes of value follow the prefix byte).
  uint8_t bad_active[4] = {10, 1, 2, 3};
  AppendTlv(&body, kTlvGetSelectedConfigIndActiveId, bad_active, sizeof(bad_active));
  auto result = ParseGetSelectedConfigIndication(body.data(), body.size(), 36);
  EXPECT_FALSE(result.ok);
  EXPECT_EQ(ClassifyGetSelectedConfigResult(result), IndicationOutcome::kMalformed);
}

TEST(PdcWireSelected, IndicationRejectedNotSuccess) {
  auto body = BuildSelectedIndication(37, /*err_status=*/5, std::nullopt,
                                       std::nullopt);
  auto result = ParseGetSelectedConfigIndication(body.data(), body.size(), 37);
  ASSERT_TRUE(result.ok);  // decoded fine...
  EXPECT_EQ(ClassifyGetSelectedConfigResult(result),
            IndicationOutcome::kRejected);  // ...but must not be kSuccess.
}

TEST(PdcWireSelected, WrongTokenRejected) {
  auto body = BuildSelectedIndication(38, 0, std::nullopt, std::nullopt);
  auto result = ParseGetSelectedConfigIndication(body.data(), body.size(),
                                                  /*expected_token=*/39);
  EXPECT_FALSE(result.ok);
  EXPECT_EQ(ClassifyGetSelectedConfigResult(result), IndicationOutcome::kMalformed);
}
