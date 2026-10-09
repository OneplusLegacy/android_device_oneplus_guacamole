#include "pdc_select_config_wire.h"

#include "qmi_tlv.h"

namespace guacamole_pdc {

std::vector<uint8_t> BuildSetSelectedConfigRequest(
    uint32_t token, ConfigType type, const std::vector<uint8_t>& id) {
  std::vector<uint8_t> body;
  // "Config Type And Id" (common-ref, TLV 0x01) - byte-for-byte the
  // same encoding as BuildGetConfigInfoRequest() in pdc_wire.cpp (see
  // this file's header comment for why they must stay identical).
  std::vector<uint8_t> value;
  value.push_back(static_cast<uint8_t>(static_cast<uint32_t>(type) & 0xFF));
  value.push_back(static_cast<uint8_t>((static_cast<uint32_t>(type) >> 8) & 0xFF));
  value.push_back(static_cast<uint8_t>((static_cast<uint32_t>(type) >> 16) & 0xFF));
  value.push_back(static_cast<uint8_t>((static_cast<uint32_t>(type) >> 24) & 0xFF));
  if (id.size() > 0xFF) {
    // Caller must have already validated against kMaxIdLen; refuse to
    // build a request that can't be represented on the wire at all.
    return {};
  }
  value.push_back(static_cast<uint8_t>(id.size()));
  value.insert(value.end(), id.begin(), id.end());
  AppendTlv(&body, kTlvConfigTypeAndId, value.data(), value.size());
  AppendTlvU32(&body, kTlvToken, token);
  return body;
}

SetSelectedConfigResult ParseSetSelectedConfigIndication(
    const uint8_t* buf, size_t len, std::optional<uint32_t> expected_token) {
  SetSelectedConfigResult result;
  if (buf == nullptr) return result;

  bool got_result = false;
  bool got_token = false;
  bool token_ok = !expected_token.has_value();
  size_t offset = 0;
  while (offset < len) {
    size_t next = 0;
    auto tlv = DecodeOneTlv(buf, len, offset, &next);
    if (!tlv) {
      result.ok = false;
      return result;
    }
    offset = next;

    if (tlv->type == kTlvToken) {
      auto tok = TlvAsU32(*tlv);
      if (!tok || got_token) return result;
      got_token = true;
      token_ok = !expected_token || *tok == *expected_token;
      continue;
    }
    if (tlv->type == kTlvIndicationResult) {
      // Same convention as every other ParseXxxIndication() in this
      // project (see pdc_wire.cpp's ParseGetSelectedConfigIndication/
      // ParseGetConfigInfoIndication): Indication Result carries only
      // error_status on the wire - error_code stays at its default (0).
      auto rc = TlvAsU16(*tlv);
      if (!rc || got_result) return result;
      result.error_status = *rc;
      got_result = true;
      continue;
    }
    // Any other TLV: ignored, not an error - forward-compatible with a
    // future firmware revision that adds fields this parser does not
    // yet know about, matching every other Parse* function's policy.
  }

  result.ok = got_result && token_ok;
  return result;
}

IndicationOutcome ClassifySetSelectedConfigResult(
    const SetSelectedConfigResult& result) {
  if (!result.ok) return IndicationOutcome::kMalformed;
  if (result.error_status != 0) return IndicationOutcome::kRejected;
  return IndicationOutcome::kSuccess;
}

}  // namespace guacamole_pdc
