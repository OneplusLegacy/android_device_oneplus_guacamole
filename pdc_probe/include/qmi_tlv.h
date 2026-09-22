// guacamole_pdc_probe - generic QMI TLV codec (transport-agnostic, no I/O here).
//
// Wire format (per Qualcomm QCCI "raw" API contract and libqmi-glib's public,
// actively-maintained implementation of the same wire protocol):
//   qmi_client_send_raw_msg_sync()/qmi_client_ind_cb() hand us the message
//   body with the service_header (flags/transaction/message/tlv_length)
//   already stripped by QCCI - msg_id is passed as a separate argument.
//   What is left is a flat list of TLVs:
//     u8  type
//     u16 length   (little-endian, length of value[] that follows)
//     u8  value[length]
//   repeated until the buffer is exhausted.
//
// Reference: linux-mobile-broadband/libqmi src/libqmi-glib/qmi-message.c
// (`struct tlv { guint8 type; guint16 length; guint8 value[]; } PACKED;`),
// cross-checked against the public Qualcomm qmi-framework qmi_cci.h
// (`qmi_cci_send_raw_msg_sync`/`qmi_client_ind_cb` doc comments: "raw/
// undecoded" buffers are TLV streams, msg_id is out-of-band).
//
// This header is intentionally transport- and PDC-independent so it can be
// unit-tested on the host without touching any vendor .so.

#pragma once

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace guacamole_pdc {

// One decoded TLV, value bytes owned by the caller-provided buffer (view,
// not copied) - callers that need to keep data past the decode call must
// copy it out explicitly. This avoids hidden allocations in the hot path
// and makes "did we copy or are we pointing into a freed buffer" explicit
// at every call site (Codex's DCI buffer-lifetime finding applies equally
// here: never hold a raw pointer into a QCCI-owned response buffer past
// the point where that buffer could be reused/freed).
struct TlvView {
  uint8_t type = 0;
  const uint8_t* value = nullptr;
  uint16_t length = 0;
};

// Appends one TLV (type + LE16 length + value) to `out`. Rejects values
// that would overflow the 16-bit length field rather than silently
// truncating.
inline bool AppendTlv(std::vector<uint8_t>* out, uint8_t type,
                       const uint8_t* value, size_t length) {
  if (length > 0xFFFFu) return false;
  out->push_back(type);
  out->push_back(static_cast<uint8_t>(length & 0xFF));
  out->push_back(static_cast<uint8_t>((length >> 8) & 0xFF));
  if (length > 0) {
    out->insert(out->end(), value, value + length);
  }
  return true;
}

inline bool AppendTlvU8(std::vector<uint8_t>* out, uint8_t type,
                         uint8_t v) {
  return AppendTlv(out, type, &v, 1);
}

inline bool AppendTlvU32(std::vector<uint8_t>* out, uint8_t type,
                          uint32_t v) {
  uint8_t le[4] = {
      static_cast<uint8_t>(v & 0xFF),
      static_cast<uint8_t>((v >> 8) & 0xFF),
      static_cast<uint8_t>((v >> 16) & 0xFF),
      static_cast<uint8_t>((v >> 24) & 0xFF),
  };
  return AppendTlv(out, type, le, sizeof(le));
}

// A byte array TLV with a 1-byte length prefix INSIDE the TLV value, used
// by PDC's "Id" field ("size-prefix-format": "guint8" per the public PDC
// service definition - see pdc_wire.h). This is distinct from the TLV's
// own outer 16-bit length.
inline bool AppendTlvU8PrefixedBytes(std::vector<uint8_t>* out, uint8_t type,
                                      const uint8_t* data, size_t len) {
  if (len > 0xFF) return false;  // guint8 size prefix: 0-255 only.
  std::vector<uint8_t> value;
  value.reserve(len + 1);
  value.push_back(static_cast<uint8_t>(len));
  value.insert(value.end(), data, data + len);
  return AppendTlv(out, type, value.data(), value.size());
}

// Decodes exactly one TLV starting at `buf[offset]`. Returns std::nullopt
// (and leaves *next_offset untouched) if there isn't enough data for even
// the 3-byte type+length header, or if the declared length would run past
// the end of `buf` - never trusts the declared length blindly. This is the
// single choke point that must reject a malformed/truncated response
// instead of letting garbage propagate into a config list or config id.
inline std::optional<TlvView> DecodeOneTlv(const uint8_t* buf, size_t buf_len,
                                            size_t offset,
                                            size_t* next_offset) {
  if (buf == nullptr || offset + 3 > buf_len) return std::nullopt;
  TlvView tlv;
  tlv.type = buf[offset];
  tlv.length = static_cast<uint16_t>(buf[offset + 1]) |
               (static_cast<uint16_t>(buf[offset + 2]) << 8);
  const size_t value_start = offset + 3;
  if (value_start + tlv.length > buf_len) {
    // Declared length overruns the buffer we actually received - reject,
    // do not clamp and continue (a clamp would silently accept a
    // corrupted/hostile response as if it were valid).
    return std::nullopt;
  }
  tlv.value = buf + value_start;
  *next_offset = value_start + tlv.length;
  return tlv;
}

// Reads a little-endian u32 out of a TLV value, refusing anything whose
// length doesn't match exactly (no partial reads, no reading past `length`).
inline std::optional<uint32_t> TlvAsU32(const TlvView& tlv) {
  if (tlv.length != 4 || tlv.value == nullptr) return std::nullopt;
  return static_cast<uint32_t>(tlv.value[0]) |
         (static_cast<uint32_t>(tlv.value[1]) << 8) |
         (static_cast<uint32_t>(tlv.value[2]) << 16) |
         (static_cast<uint32_t>(tlv.value[3]) << 24);
}

inline std::optional<uint16_t> TlvAsU16(const TlvView& tlv) {
  if (tlv.length != 2 || tlv.value == nullptr) return std::nullopt;
  return static_cast<uint16_t>(tlv.value[0]) |
         (static_cast<uint16_t>(tlv.value[1]) << 8);
}

// Reads a guint8-size-prefixed byte array out of a TLV value (used for
// PDC's "Id" field and similar). `max_len` bounds how many bytes we are
// willing to accept even if the wire claims more, closing off the
// "oversized config ID" failure mode called out explicitly for this probe.
inline std::optional<std::vector<uint8_t>> TlvAsU8PrefixedBytes(
    const TlvView& tlv, size_t max_len) {
  if (tlv.value == nullptr || tlv.length < 1) return std::nullopt;
  const uint8_t declared_len = tlv.value[0];
  if (static_cast<size_t>(1) + declared_len > tlv.length) {
    // The 1-byte prefix claims more payload than the TLV actually carries.
    return std::nullopt;
  }
  if (declared_len > max_len) {
    // Not a wire-format violation per se, but outside what this probe is
    // willing to trust/print - treat the same as malformed.
    return std::nullopt;
  }
  return std::vector<uint8_t>(tlv.value + 1, tlv.value + 1 + declared_len);
}

// Reads a guint8-size-prefixed string (PDC "Description") without ever
// relying on NUL termination from the wire - we take exactly the declared
// number of bytes and the caller decides how to display them.
inline std::optional<std::string> TlvAsU8PrefixedString(const TlvView& tlv,
                                                          size_t max_len) {
  auto bytes = TlvAsU8PrefixedBytes(tlv, max_len);
  if (!bytes) return std::nullopt;
  return std::string(reinterpret_cast<const char*>(bytes->data()),
                      bytes->size());
}

}  // namespace guacamole_pdc
