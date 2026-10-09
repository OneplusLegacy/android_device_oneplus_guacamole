#include "pdc_wire.h"

// Only for the ProbeStatus enum / ProbeStatusToString() below - pdc_wire.h
// itself stays independent of pdc_client.h (no circularity: pdc_client.h
// includes pdc_wire.h, not the other way around). ProbeStatusToString is
// implemented here, not in pdc_client.cpp, specifically so it links
// cleanly into the host test target, which cannot link pdc_client.cpp
// (that pulls in QcciShim, which needs the real on-device
// libqmi_cci.so/libqmiservices.so - unavailable on a build host).
#include "pdc_client.h"

namespace guacamole_pdc {

const char* ProbeStatusToString(ProbeStatus status) {
  switch (status) {
    case ProbeStatus::kOk: return "OK";
    case ProbeStatus::kInitFailed: return "INIT_FAILED";
    case ProbeStatus::kRequestSendFailed: return "REQUEST_SEND_FAILED";
    case ProbeStatus::kRequestRejected: return "REQUEST_REJECTED";
    case ProbeStatus::kTimeout: return "TIMEOUT";
    case ProbeStatus::kMalformedResponse: return "MALFORMED_RESPONSE";
    case ProbeStatus::kIndicationRejected: return "INDICATION_REJECTED";
  }
  return "UNKNOWN";
}

const char* ConfigPresenceToString(ConfigPresence presence) {
  switch (presence) {
    case ConfigPresence::kExists: return "EXISTS";
    case ConfigPresence::kAbsent: return "ABSENT";
    case ConfigPresence::kUnknown: return "UNKNOWN";
  }
  return "UNKNOWN";
}

ConfigPresence ClassifyConfigPresence(const GetConfigInfoOutcome& outcome) {
  if (outcome.status == ProbeStatus::kOk) return ConfigPresence::kExists;
  if (outcome.status == ProbeStatus::kIndicationRejected &&
      outcome.data.error_status == kQmiProtocolErrorNoEntry) {
    return ConfigPresence::kAbsent;
  }
  // Every other case - timeout, malformed, request-stage rejection, or
  // an indication-stage rejection with any OTHER error code - is
  // deliberately kUnknown, not kAbsent. See ConfigPresence's own
  // comment in pdc_client.h for why this must fail closed.
  return ConfigPresence::kUnknown;
}

std::vector<uint8_t> BuildGetSelectedConfigRequest(uint32_t token,
                                                    ConfigType type) {
  std::vector<uint8_t> body;
  AppendTlvU32(&body, kTlvGetSelectedConfigReqConfigType,
               static_cast<uint32_t>(type));
  AppendTlvU32(&body, kTlvToken, token);
  return body;
}

std::vector<uint8_t> BuildListConfigsRequest(uint32_t token,
                                              ConfigType type) {
  std::vector<uint8_t> body;
  // Token is technically optional per the public schema, but we always
  // send it so the indication can be matched unambiguously.
  AppendTlvU32(&body, kTlvToken, token);
  AppendTlvU32(&body, kTlvListConfigsReqConfigType,
               static_cast<uint32_t>(type));
  return body;
}

std::vector<uint8_t> BuildGetConfigInfoRequest(
    uint32_t token, ConfigType type, const std::vector<uint8_t>& id) {
  std::vector<uint8_t> body;
  // "Config Type And Id" (common-ref, TLV 0x01): sequence of
  // {Config Type: guint32}{Id: guint8-size-prefixed array}, sent as ONE
  // TLV whose value is the concatenation of both fields.
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

bool ParseOperationResultOnly(const uint8_t* buf, size_t len,
                               uint16_t* error_status, uint16_t* error_code) {
  if (buf == nullptr) return false;
  size_t offset = 0;
  bool found = false;
  while (offset < len) {
    size_t next = 0;
    auto tlv = DecodeOneTlv(buf, len, offset, &next);
    if (!tlv) return false;  // malformed stream - do not partially trust it.
    if (tlv->type == kTlvOperationResult) {
      if (tlv->length != 4) return false;  // Error Status(u16)+Error Code(u16)
      *error_status = static_cast<uint16_t>(tlv->value[0]) |
                       (static_cast<uint16_t>(tlv->value[1]) << 8);
      *error_code = static_cast<uint16_t>(tlv->value[2]) |
                    (static_cast<uint16_t>(tlv->value[3]) << 8);
      found = true;
    }
    offset = next;
  }
  return found;
}

std::optional<uint32_t> PeekIndicationToken(const uint8_t* buf, size_t len) {
  if (buf == nullptr) return std::nullopt;
  size_t offset = 0;
  while (offset < len) {
    size_t next = 0;
    auto tlv = DecodeOneTlv(buf, len, offset, &next);
    if (!tlv) return std::nullopt;
    if (tlv->type == kTlvToken) {
      return TlvAsU32(*tlv);
    }
    offset = next;
  }
  return std::nullopt;
}

ListConfigsResult ParseListConfigsIndication(
    const uint8_t* buf, size_t len, std::optional<uint32_t> expected_token) {
  ListConfigsResult result;
  if (buf == nullptr) return result;

  bool got_result = false;
  bool token_ok = !expected_token.has_value();
  size_t offset = 0;
  while (offset < len) {
    size_t next = 0;
    auto tlv = DecodeOneTlv(buf, len, offset, &next);
    if (!tlv) {
      result.ok = false;
      return result;  // truncated/malformed - never return partial success.
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
    if (tlv->type == kTlvListConfigsIndConfigs) {
      if (tlv->length < 1) return result;
      const uint8_t count = tlv->value[0];
      size_t pos = 1;
      const size_t declared = count;
      size_t parsed = 0;
      while (parsed < declared) {
        if (pos + 4 + 1 > tlv->length) {
          result.ok = false;
          return result;  // element header runs past the TLV - reject.
        }
        uint32_t cfg_type = static_cast<uint32_t>(tlv->value[pos]) |
                             (static_cast<uint32_t>(tlv->value[pos + 1]) << 8) |
                             (static_cast<uint32_t>(tlv->value[pos + 2]) << 16) |
                             (static_cast<uint32_t>(tlv->value[pos + 3]) << 24);
        pos += 4;
        const uint8_t id_len = tlv->value[pos];
        pos += 1;
        if (pos + id_len > tlv->length) {
          result.ok = false;
          return result;
        }
        if (id_len > kMaxIdLen) {
          // Wire-valid but outside what we trust; stop parsing further
          // rather than accept a suspicious oversized id silently.
          result.ok = false;
          return result;
        }
        if (result.configs.size() < kMaxConfigs) {
          ConfigEntry entry;
          entry.type = static_cast<ConfigType>(cfg_type);
          entry.id.assign(tlv->value + pos, tlv->value + pos + id_len);
          result.configs.push_back(std::move(entry));
        } else {
          result.truncated = true;
        }
        pos += id_len;
        parsed++;
      }
      continue;
    }
    // Unknown TLV: per the QMI convention, skip it - forward compatible
    // fields must not abort parsing of the fields we do understand.
  }

  result.ok = got_result && token_ok;
  return result;
}

GetConfigInfoResult ParseGetConfigInfoIndication(
    const uint8_t* buf, size_t len, std::optional<uint32_t> expected_token) {
  GetConfigInfoResult result;
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
    if (tlv->type == kTlvGetConfigInfoIndTotalSize) {
      auto v = TlvAsU32(*tlv);
      if (!v) return result;
      result.total_size = *v;
      result.has_total_size = true;
      continue;
    }
    if (tlv->type == kTlvGetConfigInfoIndDescription) {
      auto s = TlvAsU8PrefixedString(*tlv, kMaxDescriptionLen);
      if (!s) return result;
      result.description = *s;
      result.has_description = true;
      continue;
    }
    if (tlv->type == kTlvGetConfigInfoIndVersion) {
      auto v = TlvAsU32(*tlv);
      if (!v) return result;
      result.version = *v;
      result.has_version = true;
      continue;
    }
    // Unknown TLV: skip, forward-compatible.
  }

  result.ok = got_result && token_ok;
  return result;
}

GetSelectedConfigResult ParseGetSelectedConfigIndication(
    const uint8_t* buf, size_t len, std::optional<uint32_t> expected_token) {
  GetSelectedConfigResult result;
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
    if (tlv->type == kTlvGetSelectedConfigIndActiveId) {
      auto id = TlvAsU8PrefixedBytes(*tlv, kMaxIdLen);
      if (!id) return result;
      result.active_id = *id;
      continue;
    }
    if (tlv->type == kTlvGetSelectedConfigIndPendingId) {
      auto id = TlvAsU8PrefixedBytes(*tlv, kMaxIdLen);
      if (!id) return result;
      result.pending_id = *id;
      continue;
    }
    // Unknown TLV: skip, forward-compatible.
  }

  result.ok = got_result && token_ok;
  return result;
}

IndicationOutcome ClassifyListConfigsResult(const ListConfigsResult& result) {
  if (!result.ok) return IndicationOutcome::kMalformed;
  if (result.error_status != 0) return IndicationOutcome::kRejected;
  return IndicationOutcome::kSuccess;
}

IndicationOutcome ClassifyGetSelectedConfigResult(
    const GetSelectedConfigResult& result) {
  if (!result.ok) return IndicationOutcome::kMalformed;
  if (result.error_status != 0) return IndicationOutcome::kRejected;
  return IndicationOutcome::kSuccess;
}

IndicationOutcome ClassifyGetConfigInfoResult(
    const GetConfigInfoResult& result) {
  if (!result.ok) return IndicationOutcome::kMalformed;
  if (result.error_status != 0) return IndicationOutcome::kRejected;
  return IndicationOutcome::kSuccess;
}

}  // namespace guacamole_pdc
