#include "efs_write_wire.h"

#include <cstring>

namespace guacamole_efs {

namespace {

void PushI32Le(std::vector<uint8_t>* out, int32_t v) {
  uint32_t u = static_cast<uint32_t>(v);
  out->push_back(static_cast<uint8_t>(u & 0xFF));
  out->push_back(static_cast<uint8_t>((u >> 8) & 0xFF));
  out->push_back(static_cast<uint8_t>((u >> 16) & 0xFF));
  out->push_back(static_cast<uint8_t>((u >> 24) & 0xFF));
}

void PushU32Le(std::vector<uint8_t>* out, uint32_t v) {
  out->push_back(static_cast<uint8_t>(v & 0xFF));
  out->push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  out->push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
  out->push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

// Same fixed-100-byte, always-NUL-terminated-within-bounds convention
// as efs_wire.cpp's PushFixedNulPaddedField - duplicated locally rather
// than shared across the read/write file boundary (matching this
// project's established pdc_wire.h/pdc_load_wire.h precedent of never
// letting the write file silently depend on read-file internals, or
// vice versa).
bool PushFixedNulPaddedField(std::vector<uint8_t>* out, const std::string& s,
                              size_t field_len) {
  if (s.size() + 1 > field_len) return false;
  size_t start = out->size();
  out->resize(start + field_len, 0);
  std::memcpy(out->data() + start, s.data(), s.size());
  return true;
}

bool ReadI32Le(const uint8_t* buf, size_t len, size_t offset, int32_t* out) {
  if (offset + 4 > len) return false;
  uint32_t u = static_cast<uint32_t>(buf[offset]) |
               (static_cast<uint32_t>(buf[offset + 1]) << 8) |
               (static_cast<uint32_t>(buf[offset + 2]) << 16) |
               (static_cast<uint32_t>(buf[offset + 3]) << 24);
  *out = static_cast<int32_t>(u);
  return true;
}

bool ReadU32Le(const uint8_t* buf, size_t len, size_t offset, uint32_t* out) {
  if (offset + 4 > len) return false;
  *out = static_cast<uint32_t>(buf[offset]) |
         (static_cast<uint32_t>(buf[offset + 1]) << 8) |
         (static_cast<uint32_t>(buf[offset + 2]) << 16) |
         (static_cast<uint32_t>(buf[offset + 3]) << 24);
  return true;
}

constexpr size_t kOpenNameFieldLen = 100;  // matches efsio.c's char name[100].

}  // namespace

std::vector<uint8_t> BuildOpenWriteRequest(const std::string& path) {
  // Reuses efs_wire.h's own kMaxOpenNameLen (99 usable + NUL) - the
  // name field's fixed 100-byte size is a wire-level fact shared by
  // both the read-only and write-mode OPEN requests, not something
  // that should have two independent definitions.
  if (path.empty() || path.size() > kMaxOpenNameLen) return {};

  std::vector<uint8_t> payload;
  PushI32Le(&payload, kOflagWriteTruncate);
  PushI32Le(&payload, 0777);  // mode - matches efsio.c's efs_open() own
                               // hardcoded 0777 default; EFS2 OPEN mode
                               // is irrelevant unless O_CREAT is set,
                               // and this project's O_CREAT usage never
                               // varies the mode.
  if (!PushFixedNulPaddedField(&payload, path, kOpenNameFieldLen)) return {};
  return WrapEfsHeader(kEfsCmdOpen, payload);
}

std::vector<uint8_t> BuildWriteRequest(int32_t fd, uint32_t offset,
                                        const std::vector<uint8_t>& data) {
  if (data.empty() || data.size() > kMaxWriteRequestBytes) return {};

  std::vector<uint8_t> payload;
  PushI32Le(&payload, fd);
  PushU32Le(&payload, offset);
  payload.insert(payload.end(), data.begin(), data.end());
  return WrapEfsHeader(kEfsCmdWrite, payload);
}

EfsWriteResult ParseWriteResponse(const uint8_t* raw, size_t raw_len,
                                   int32_t expected_fd,
                                   uint32_t expected_offset,
                                   uint32_t requested_nbyte) {
  EfsWriteResult r;
  std::vector<uint8_t> p;
  if (!StripEfsResponseHeader(raw, raw_len, kEfsCmdWrite, kEfsSubsysFs, &p)) {
    return r;
  }
  if (p.size() != 16) return r;  // fd + offset + bytes_written + errno.

  int32_t fd, bytes_written, errno_v;
  uint32_t offset;
  if (!ReadI32Le(p.data(), p.size(), 0, &fd)) return r;
  if (!ReadU32Le(p.data(), p.size(), 4, &offset)) return r;
  if (!ReadI32Le(p.data(), p.size(), 8, &bytes_written)) return r;
  if (!ReadI32Le(p.data(), p.size(), 12, &errno_v)) return r;

  if (fd != expected_fd) return r;
  if (offset != expected_offset) return r;
  if (bytes_written < 0) return r;
  if (static_cast<uint32_t>(bytes_written) > requested_nbyte) return r;

  r.fd = fd;
  r.offset = offset;
  r.bytes_written = bytes_written;
  r.diag_errno = errno_v;
  r.ok = true;
  return r;
}

}  // namespace guacamole_efs
