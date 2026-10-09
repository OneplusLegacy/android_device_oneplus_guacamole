#include "diag_frame.h"

namespace guacamole_efs {

uint16_t Crc16X25(const uint8_t* data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; bit++) {
      if (crc & 1) {
        crc = static_cast<uint16_t>((crc >> 1) ^ 0x8408);
      } else {
        crc = static_cast<uint16_t>(crc >> 1);
      }
    }
  }
  return static_cast<uint16_t>(crc ^ 0xFFFF);
}

namespace {
constexpr uint8_t kHdlcTerminator = 0x7E;
constexpr uint8_t kHdlcEscape = 0x7D;
constexpr uint8_t kHdlcEscaped7d = 0x5D;  // 7d 5d -> literal 0x7d.
constexpr uint8_t kHdlcEscaped7e = 0x5E;  // 7d 5e -> literal 0x7e.
}  // namespace

DiagFrameResult NormalizeDiagFrame(const std::vector<uint8_t>& raw, DiagFrameMode mode) {
  DiagFrameResult r;
  r.raw = raw;

  if (raw.empty() || raw.back() != kHdlcTerminator) {
    // Never accept an unframed H.41 production reply.
    r.had_hdlc_trailer = false;
    r.ok = mode == DiagFrameMode::kTestAllowUnframed;
    if (r.ok) r.logical = raw;
    return r;
  }
  r.had_hdlc_trailer = true;

  // STEP 1: strip the terminator - it is never itself stuffed.
  const size_t stuffed_len = raw.size() - 1;

  // STEP 2: HDLC-unescape the remaining bytes BEFORE anything else -
  // the CRC bytes are stuffed exactly like payload bytes (see this
  // file's header comment on why the earlier, now-corrected revision
  // that split off the CRC before unescaping was wrong).
  std::vector<uint8_t> unescaped;
  unescaped.reserve(stuffed_len);
  for (size_t i = 0; i < stuffed_len; i++) {
    uint8_t b = raw[i];
    if (b != kHdlcEscape) {
      unescaped.push_back(b);
      continue;
    }
    r.escape_present = true;
    r.escape_count++;
    if (i + 1 >= stuffed_len) {
      // Dangling escape byte with nothing left to pair it with - not
      // a sequence this project has any evidence for. Fail closed.
      r.ok = false;
      return r;
    }
    uint8_t next = raw[i + 1];
    if (next == kHdlcEscaped7d) {
      unescaped.push_back(0x7D);
    } else if (next == kHdlcEscaped7e) {
      unescaped.push_back(0x7E);
    } else {
      // Unrecognized escape sequence - never guess, fail closed.
      r.ok = false;
      return r;
    }
    i++;  // consumed the escape pair's second byte too.
  }
  r.unescaped_len = unescaped.size();

  // STEP 3: only NOW split off the trailing 2-byte little-endian CRC
  // from the UNESCAPED body, and verify it against the UNESCAPED
  // payload preceding it.
  if (unescaped.size() < 2) {
    r.ok = false;
    return r;
  }
  const size_t payload_len = unescaped.size() - 2;
  r.crc_expected = Crc16X25(unescaped.data(), payload_len);
  r.crc_received = static_cast<uint16_t>(
      unescaped[payload_len] |
      (static_cast<uint16_t>(unescaped[payload_len + 1]) << 8));

  if (r.crc_expected != r.crc_received) {
    r.crc_valid = false;
    r.ok = false;
    return r;
  }

  r.crc_valid = true;
  r.ok = true;
  r.logical.assign(unescaped.begin(), unescaped.begin() + payload_len);
  return r;
}

}  // namespace guacamole_efs
