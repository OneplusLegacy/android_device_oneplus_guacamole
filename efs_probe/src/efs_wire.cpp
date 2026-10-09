#include "efs_wire.h"

#include <cstring>

namespace guacamole_efs {

namespace {

void PushU8(std::vector<uint8_t>* out, uint8_t v) { out->push_back(v); }

void PushU16Le(std::vector<uint8_t>* out, uint16_t v) {
  out->push_back(static_cast<uint8_t>(v & 0xFF));
  out->push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

void PushU32Le(std::vector<uint8_t>* out, uint32_t v) {
  out->push_back(static_cast<uint8_t>(v & 0xFF));
  out->push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  out->push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
  out->push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

void PushI32Le(std::vector<uint8_t>* out, int32_t v) {
  PushU32Le(out, static_cast<uint32_t>(v));
}

// Appends `len` bytes copied from `s` (as many as fit, up to `field_len`),
// zero-padding the remainder of a FIXED `field_len`-byte field. Always
// leaves at least one zero byte if `s` is shorter than `field_len` (i.e.
// the field is always NUL-terminated within its own bounds when
// `s.size() < field_len`). Returns false (appends nothing) if `s` would
// not fit with room for its own NUL terminator - caller must treat that
// as "refuse to build this request".
bool PushFixedNulPaddedField(std::vector<uint8_t>* out, const std::string& s,
                              size_t field_len) {
  if (s.size() + 1 > field_len) return false;
  size_t start = out->size();
  out->resize(start + field_len, 0);
  std::memcpy(out->data() + start, s.data(), s.size());
  // Remaining bytes already zero from resize() - guarantees NUL
  // termination within the field.
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

bool ReadI32Le(const uint8_t* buf, size_t len, size_t offset, int32_t* out) {
  uint32_t u;
  if (!ReadU32Le(buf, len, offset, &u)) return false;
  *out = static_cast<int32_t>(u);
  return true;
}

}  // namespace

std::vector<uint8_t> WrapEfsHeader(uint16_t cmd,
                                    const std::vector<uint8_t>& payload) {
  std::vector<uint8_t> out;
  out.reserve(4 + payload.size());
  PushU8(&out, kEfsHeaderMagic);
  PushU8(&out, kEfsSubsysFs);
  PushU16Le(&out, cmd);
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

bool StripEfsResponseHeader(const uint8_t* raw, size_t raw_len,
                             uint16_t expected_cmd, uint8_t expected_subsys,
                             std::vector<uint8_t>* out_payload) {
  if (raw == nullptr || raw_len < 4) return false;
  if (raw[0] != kEfsHeaderMagic) return false;
  if (raw[1] != expected_subsys) return false;
  const uint16_t echoed_cmd =
      static_cast<uint16_t>(raw[2]) | (static_cast<uint16_t>(raw[3]) << 8);
  if (echoed_cmd != expected_cmd) return false;
  out_payload->assign(raw + 4, raw + raw_len);
  return true;
}

std::vector<uint8_t> BuildStatRequest(const std::string& path) {
  if (path.empty() || path.size() > kMaxPathLen) return {};
  std::vector<uint8_t> payload(path.begin(), path.end());
  payload.push_back(0);  // NUL terminator, matching qtools' strlen(path)+1.
  return WrapEfsHeader(kEfsCmdStat, payload);
}

std::vector<uint8_t> BuildOpenDirRequest(const std::string& path) {
  if (path.empty() || path.size() > kMaxPathLen) return {};
  std::vector<uint8_t> payload(path.begin(), path.end());
  payload.push_back(0);
  return WrapEfsHeader(kEfsCmdOpenDir, payload);
}

std::vector<uint8_t> BuildCloseDirRequest(uint32_t dirp) {
  std::vector<uint8_t> payload;
  PushU32Le(&payload, dirp);
  return WrapEfsHeader(kEfsCmdCloseDir, payload);
}

std::vector<uint8_t> BuildReadDirRequest(uint32_t dirp, int32_t seqno) {
  std::vector<uint8_t> payload;
  PushU32Le(&payload, dirp);
  PushI32Le(&payload, seqno);
  return WrapEfsHeader(kEfsCmdReadDir, payload);
}

std::vector<uint8_t> BuildOpenReadOnlyRequest(const std::string& path,
                                               int32_t oflag) {
  // Fail-closed at the request-construction level: this function will
  // NEVER build a request for anything other than the one, hardcoded
  // read-only flag value. This is not a runtime policy check that could
  // be bypassed by a caller passing a different value through some
  // other path - there IS no other path; every caller in this codebase
  // is expected to pass kOflagReadOnly, and this check exists so that
  // remains true even under future modification, not just by
  // convention.
  if (oflag != kOflagReadOnly) return {};
  if (path.empty() || path.size() > kMaxOpenNameLen) return {};

  std::vector<uint8_t> payload;
  PushI32Le(&payload, oflag);
  PushI32Le(&payload, 0);  // mode: ignored unless O_CREAT (qtools' own doc
                            // comment) - we never set O_CREAT, so 0 is
                            // unambiguous and avoids any confusion with
                            // qtools' own (irrelevant-to-us) 0777 default.
  if (!PushFixedNulPaddedField(&payload, path, 100)) return {};
  return WrapEfsHeader(kEfsCmdOpen, payload);
}

std::vector<uint8_t> BuildReadRequest(int32_t fd, uint32_t nbyte,
                                       uint32_t offset) {
  if (nbyte == 0 || nbyte > kMaxReadRequestBytes) return {};
  std::vector<uint8_t> payload;
  PushI32Le(&payload, fd);
  PushU32Le(&payload, nbyte);
  PushU32Le(&payload, offset);
  return WrapEfsHeader(kEfsCmdRead, payload);
}

std::vector<uint8_t> BuildCloseRequest(int32_t fd) {
  std::vector<uint8_t> payload;
  PushI32Le(&payload, fd);
  return WrapEfsHeader(kEfsCmdClose, payload);
}

EfsStatResult ParseStatResponse(const uint8_t* raw, size_t raw_len) {
  EfsStatResult r;
  std::vector<uint8_t> p;
  if (!StripEfsResponseHeader(raw, raw_len, kEfsCmdStat, kEfsSubsysFs, &p)) {
    return r;
  }
  if (p.size() != 28) return r;  // 7 x int32, exactly - efs_filestat.
  int32_t v[7];
  for (int i = 0; i < 7; i++) {
    if (!ReadI32Le(p.data(), p.size(), i * 4, &v[i])) return r;
  }
  r.diag_errno = v[0];
  r.mode = v[1];
  r.size = v[2];
  r.nlink = v[3];
  r.atime = v[4];
  r.mtime = v[5];
  r.ctime = v[6];
  r.ok = true;
  return r;
}

EfsOpenDirResult ParseOpenDirResponse(const uint8_t* raw, size_t raw_len) {
  EfsOpenDirResult r;
  std::vector<uint8_t> p;
  if (!StripEfsResponseHeader(raw, raw_len, kEfsCmdOpenDir, kEfsSubsysFs,
                               &p)) {
    return r;
  }
  if (p.size() != 8) return r;
  uint32_t dirp;
  int32_t err;
  if (!ReadU32Le(p.data(), p.size(), 0, &dirp)) return r;
  if (!ReadI32Le(p.data(), p.size(), 4, &err)) return r;
  r.dirp = dirp;
  r.diag_errno = err;
  r.ok = true;
  return r;
}

EfsReadDirResult ParseReadDirResponse(const uint8_t* raw, size_t raw_len) {
  EfsReadDirResult r;
  std::vector<uint8_t> p;
  if (!StripEfsResponseHeader(raw, raw_len, kEfsCmdReadDir, kEfsSubsysFs,
                               &p)) {
    return r;
  }

  // Common header every READDIR response (success or error) must carry:
  // dirp(4) + seqno(4) + diag_errno(4) = 12 bytes. Nothing can be
  // safely classified below this.
  if (p.size() < 12) return r;
  uint32_t dirp;
  int32_t seqno, errno_v;
  if (!ReadU32Le(p.data(), p.size(), 0, &dirp)) return r;
  if (!ReadI32Le(p.data(), p.size(), 4, &seqno)) return r;
  if (!ReadI32Le(p.data(), p.size(), 8, &errno_v)) return r;

  if (errno_v != 0) {
    // Error response: accept the minimal 12-byte header alone - an
    // error carries no meaningful entry metadata/name, and requiring
    // the full 36-byte+name layout here risked rejecting a genuine
    // protocol error as "malformed" purely because it omitted fields
    // that don't apply to an error. `ok=true` means "structurally
    // parsed", NOT "operation success" - see this function's own
    // declaration comment; the caller (efs_dir_lister.cpp) decides
    // what a given diag_errno value means.
    r.dirp = dirp;
    r.seqno = seqno;
    r.diag_errno = errno_v;
    r.ok = true;
    return r;
  }

  // Success response: the 6 remaining fixed int32 fields (24 bytes,
  // total fixed metadata 36 bytes) must be fully present, followed by
  // a NUL-terminated name - accepted in EITHER of two public reference
  // layouts (see this function's declaration comment): a fixed
  // 100-byte field (qtools) or a variable-length tail (OpenPST). Both
  // require a NUL within the name bytes actually present, and any
  // bytes after that NUL must themselves be zero padding, never
  // arbitrary trailing data.
  if (p.size() < 36) return r;  // truncated metadata.
  int32_t entry_type, mode, size_v, atime, mtime, ctime;
  if (!ReadI32Le(p.data(), p.size(), 12, &entry_type)) return r;
  if (!ReadI32Le(p.data(), p.size(), 16, &mode)) return r;
  if (!ReadI32Le(p.data(), p.size(), 20, &size_v)) return r;
  if (!ReadI32Le(p.data(), p.size(), 24, &atime)) return r;
  if (!ReadI32Le(p.data(), p.size(), 28, &mtime)) return r;
  if (!ReadI32Le(p.data(), p.size(), 32, &ctime)) return r;

  const uint8_t* name_start = p.data() + 36;
  const size_t name_capacity = p.size() - 36;
  if (name_capacity == 0) return r;  // no name field at all - reject.
  // Bounded at the fixed-layout's own field size - anything longer is
  // not a name field either reference layout defines, it's unexpected
  // trailing data.
  constexpr size_t kMaxNameFieldBytes = 100;
  if (name_capacity > kMaxNameFieldBytes) return r;

  size_t nul_pos = name_capacity;
  for (size_t i = 0; i < name_capacity; i++) {
    if (name_start[i] == 0) {
      nul_pos = i;
      break;
    }
  }
  if (nul_pos == name_capacity) return r;  // no NUL anywhere - reject.
  if (nul_pos > kMaxDirNameLen) return r;  // usable name too long.
  for (size_t i = nul_pos + 1; i < name_capacity; i++) {
    // Trailing padded zero bytes are fine (the fixed-100 layout always
    // has these); anything nonzero after the NUL is unrecognized
    // garbage, not a name this probe trusts.
    if (name_start[i] != 0) return r;
  }

  r.dirp = dirp;
  r.seqno = seqno;
  r.diag_errno = errno_v;  // == 0 here.
  r.entry_type = entry_type;
  r.mode = mode;
  r.size = size_v;
  r.atime = atime;
  r.mtime = mtime;
  r.ctime = ctime;
  r.name.assign(reinterpret_cast<const char*>(name_start), nul_pos);
  r.ok = true;
  return r;
}

EfsOpenResult ParseOpenResponse(const uint8_t* raw, size_t raw_len) {
  EfsOpenResult r;
  std::vector<uint8_t> p;
  if (!StripEfsResponseHeader(raw, raw_len, kEfsCmdOpen, kEfsSubsysFs, &p)) {
    return r;
  }
  if (p.size() != 8) return r;
  int32_t fd, err;
  if (!ReadI32Le(p.data(), p.size(), 0, &fd)) return r;
  if (!ReadI32Le(p.data(), p.size(), 4, &err)) return r;
  r.fd = fd;
  r.diag_errno = err;
  r.ok = true;
  return r;
}

EfsReadResult ParseReadResponse(const uint8_t* raw, size_t raw_len,
                                 int32_t expected_fd, uint32_t expected_offset,
                                 uint32_t requested_nbyte) {
  EfsReadResult r;
  std::vector<uint8_t> p;
  if (!StripEfsResponseHeader(raw, raw_len, kEfsCmdRead, kEfsSubsysFs, &p)) {
    return r;
  }
  if (p.size() < 16) return r;  // fixed header: fd+offset+bytes_read+errno.
  int32_t fd, bytes_read, errno_v;
  uint32_t offset;
  if (!ReadI32Le(p.data(), p.size(), 0, &fd)) return r;
  if (!ReadU32Le(p.data(), p.size(), 4, &offset)) return r;
  if (!ReadI32Le(p.data(), p.size(), 8, &bytes_read)) return r;
  if (!ReadI32Le(p.data(), p.size(), 12, &errno_v)) return r;

  if (fd != expected_fd) return r;
  if (offset != expected_offset) return r;
  if (bytes_read < 0) return r;
  if (static_cast<uint32_t>(bytes_read) > requested_nbyte) return r;
  const size_t declared_end = 16 + static_cast<size_t>(bytes_read);
  if (p.size() < declared_end) return r;  // truncated payload.

  r.fd = fd;
  r.offset = offset;
  r.bytes_read = bytes_read;
  r.diag_errno = errno_v;
  r.data.assign(p.begin() + 16, p.begin() + declared_end);
  r.ok = true;
  return r;
}

EfsCloseResult ParseCloseResponse(const uint8_t* raw, size_t raw_len) {
  EfsCloseResult r;
  std::vector<uint8_t> p;
  if (!StripEfsResponseHeader(raw, raw_len, kEfsCmdClose, kEfsSubsysFs, &p)) {
    return r;
  }
  if (p.size() != 4) return r;
  int32_t err;
  if (!ReadI32Le(p.data(), p.size(), 0, &err)) return r;
  r.errno_value = err;
  r.ok = true;
  return r;
}

EfsCloseDirResult ParseCloseDirResponse(const uint8_t* raw, size_t raw_len) {
  EfsCloseDirResult r;
  std::vector<uint8_t> p;
  if (!StripEfsResponseHeader(raw, raw_len, kEfsCmdCloseDir, kEfsSubsysFs,
                               &p)) {
    return r;
  }
  if (p.size() != 4) return r;
  int32_t err;
  if (!ReadI32Le(p.data(), p.size(), 0, &err)) return r;
  r.errno_value = err;
  r.ok = true;
  return r;
}

}  // namespace guacamole_efs
