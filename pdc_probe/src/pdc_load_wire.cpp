#include "pdc_load_wire.h"

namespace guacamole_pdc {

std::vector<uint8_t> BuildLoadConfigChunkRequest(
    uint32_t token, ConfigType type, const std::vector<uint8_t>& id,
    uint32_t total_size, const uint8_t* chunk_bytes, size_t chunk_len) {
  if (id.size() > 0xFF) return {};        // guint8 size prefix limit.
  if (chunk_len > 0xFFFF) return {};      // guint16 size prefix limit.

  // Config Chunk (TLV 0x01): sequence of
  //   {Type: guint32}{Id: guint8-prefixed bytes}
  //   {Total Size: guint32}{Chunk: guint16-prefixed bytes}
  std::vector<uint8_t> chunk_value;
  const uint32_t type_val = static_cast<uint32_t>(type);
  chunk_value.push_back(static_cast<uint8_t>(type_val & 0xFF));
  chunk_value.push_back(static_cast<uint8_t>((type_val >> 8) & 0xFF));
  chunk_value.push_back(static_cast<uint8_t>((type_val >> 16) & 0xFF));
  chunk_value.push_back(static_cast<uint8_t>((type_val >> 24) & 0xFF));

  chunk_value.push_back(static_cast<uint8_t>(id.size()));
  chunk_value.insert(chunk_value.end(), id.begin(), id.end());

  chunk_value.push_back(static_cast<uint8_t>(total_size & 0xFF));
  chunk_value.push_back(static_cast<uint8_t>((total_size >> 8) & 0xFF));
  chunk_value.push_back(static_cast<uint8_t>((total_size >> 16) & 0xFF));
  chunk_value.push_back(static_cast<uint8_t>((total_size >> 24) & 0xFF));

  chunk_value.push_back(static_cast<uint8_t>(chunk_len & 0xFF));
  chunk_value.push_back(static_cast<uint8_t>((chunk_len >> 8) & 0xFF));
  if (chunk_len > 0 && chunk_bytes != nullptr) {
    chunk_value.insert(chunk_value.end(), chunk_bytes, chunk_bytes + chunk_len);
  }

  std::vector<uint8_t> body;
  AppendTlv(&body, kTlvLoadConfigReqChunk, chunk_value.data(),
            chunk_value.size());
  AppendTlvU32(&body, kTlvToken, token);
  return body;
}

LoadConfigChunkResult ParseLoadConfigIndication(
    const uint8_t* buf, size_t len, std::optional<uint32_t> expected_token) {
  LoadConfigChunkResult result;
  if (buf == nullptr) return result;

  bool got_result = false;
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
      if (tok && expected_token && *tok == *expected_token) token_ok = true;
      continue;
    }
    if (tlv->type == kTlvIndicationResult) {
      auto rc = TlvAsU16(*tlv);
      if (!rc) return result;
      result.error_status = *rc;
      got_result = true;
      continue;
    }
    if (tlv->type == kTlvLoadConfigIndReceived) {
      auto v = TlvAsU32(*tlv);
      if (!v) return result;
      result.received = *v;
      result.has_received = true;
      continue;
    }
    if (tlv->type == kTlvLoadConfigIndRemainingSize) {
      auto v = TlvAsU32(*tlv);
      if (!v) return result;
      result.remaining_size = *v;
      result.has_remaining_size = true;
      continue;
    }
    if (tlv->type == kTlvLoadConfigIndFrameReset) {
      if (tlv->length != 1 || tlv->value == nullptr) return result;
      result.frame_reset = (tlv->value[0] != 0);
      result.has_frame_reset = true;
      continue;
    }
    // Unknown TLV: skip, forward-compatible.
  }

  result.ok = got_result && token_ok;
  return result;
}

IndicationOutcome ClassifyLoadConfigChunkResult(
    const LoadConfigChunkResult& result) {
  if (!result.ok) return IndicationOutcome::kMalformed;
  if (result.error_status != 0) return IndicationOutcome::kRejected;
  return IndicationOutcome::kSuccess;
}

}  // namespace guacamole_pdc
