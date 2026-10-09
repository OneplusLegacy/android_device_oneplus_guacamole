// guacamole_efs_probe - EFS2-over-DIAG wire codec. Phase D0/D1: READ-ONLY
// ONLY. This file intentionally has NO encoder for WRITE, UNLINK, MKDIR,
// RMDIR, RENAME, CHMOD, CHOWN, PUT, TRUNCATE, or any other mutating EFS2
// command - not disabled by a flag, simply never implemented here. If a
// future phase needs them, they belong in a new file reviewed under that
// phase's own explicit scope, matching this project's Phase B/Phase C0
// precedent (pdc_wire.h vs pdc_load_wire.h: read vs write kept in
// physically separate files).
//
// SOURCE OF TRUTH: forth32/qtools `efsio.c`/`efsio.h`
// (https://github.com/forth32/qtools), fetched and read directly for
// this project - not recalled from memory. This is a real, working,
// published EFS2-over-DIAG client used against real Qualcomm modems.
// Every request/response byte layout below is transcribed directly from
// `send_efs_cmd()` and the `efs_stat/efs_opendir/efs_readdir/efs_open/
// efs_read/efs_close` functions in that file - this is
// PUBLIC_REFERENCE_CONFIRMED per PHASE_D0_EFS2_RESEARCH.md's own
// evidence-grading scheme; whether it is byte-identical on THIS H.41
// device (H41_STATICALLY_CONFIRMED / H41_DEVICE_CONFIRMED) is tracked
// separately in that file, not asserted here.
//
// WIRE FORMAT (confirmed directly from `send_efs_cmd()`):
//   Request:  [0x4B][subsys][cmd_lo][cmd_hi][payload...]
//     - subsys is 0x13 normally (0x3E for qtools' "alternate EFS" flag -
//       NOT used by this probe; guacamole is a single-EFS device as far
//       as this phase's research goes, see PHASE_D0_EFS2_RESEARCH.md).
//     - cmd_lo/cmd_hi is the EFS2 command code as a little-endian
//       guint16 (cmd_hi is 0 for every command code used in this file -
//       all are well under 256).
//   Response: [0x4B][subsys][cmd_lo][cmd_hi][payload...] - same 4-byte
//     echo header, then the command-specific response payload. qtools
//     validates `iobuf[0]==0x4b`; THIS file goes further per the Phase
//     D0 fail-closed requirement and also validates subsys and the
//     echoed cmd code match what was sent - see StripEfsResponseHeader.
//     This response-header check is also this probe's answer to "how do
//     we correlate a response to our request" (Phase D0 finding: DIAG's
//     `diag_register_callback` is NOT itself keyed by cmd/subsys - see
//     PHASE_D0_EFS2_RESEARCH.md item 5 - so the ECHOED header inside the
//     response body is the correlation mechanism, not anything at the
//     transport/callback-registration layer).
//
// All multi-byte integers are little-endian, matching every other wire
// format this project has implemented (pdc_wire.h/pdc_load_wire.h) and
// matching qtools' own struct layouts on a little-endian target (ARM).
// Never reinterpret_cast a C struct onto/from the wire - every Build*/
// Parse* function below does explicit byte-at-a-time (de)serialization.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace guacamole_efs {

// --- EFS2 command codes (forth32/qtools efsio.h) - READ-ONLY subset only.
// The full public enum has ~57 commands (including WRITE=5, UNLINK=8,
// MKDIR=9, RMDIR=10, RENAME=14, CHMOD=18, PUT=38, TRUNCATE=40, etc.) -
// deliberately NOT transcribed here at all, so there is no numeric
// constant in this codebase a future change could accidentally wire up
// to a mutating command by mistake. ---
constexpr uint16_t kEfsCmdOpen = 2;
constexpr uint16_t kEfsCmdClose = 3;
constexpr uint16_t kEfsCmdRead = 4;
constexpr uint16_t kEfsCmdOpenDir = 11;
constexpr uint16_t kEfsCmdReadDir = 12;
constexpr uint16_t kEfsCmdCloseDir = 13;
constexpr uint16_t kEfsCmdStat = 15;

// --- Wire header constants ---
constexpr uint8_t kEfsHeaderMagic = 0x4B;   // DIAG_SUBSYS_CMD_F-style framing byte (qtools).
constexpr uint8_t kEfsSubsysFs = 0x13;      // DIAG_SUBSYS_FS / EFS2 (19 decimal).

// --- Defensive caps (this probe's own trust boundary, not part of the
// wire spec - same philosophy as pdc_wire.h's kMaxConfigs/kMaxIdLen). ---
constexpr size_t kMaxPathLen = 199;      // qtools uses a 200-byte cmdbuf for
                                          // variable-length paths (STAT/
                                          // OPENDIR); leave room for the NUL.
constexpr size_t kMaxOpenNameLen = 99;   // OPEN's name field is a FIXED
                                          // 100-byte buffer (efsio.c's
                                          // `char name[100]`) - 99 usable
                                          // chars + NUL.
constexpr uint32_t kMaxReadRequestBytes = 1024;  // Phase D0 explicit
                                                   // instruction: cap the
                                                   // first real chunks at
                                                   // <=1024 bytes.
constexpr size_t kMaxDirNameLen = 99;    // efs_dirent.name[100] (qtools
                                          // efsio.h) - 99 usable + NUL.

// --- EFS2 protocol error codes - PUBLIC_REFERENCE_SUPPORTED (OpenPST
// libopenpst's dm_efs.h), NOT YET H41_DEVICE_CONFIRMED (no directory
// has been successfully enumerated, nor any protocol error observed,
// on the real device yet - the first snapshot-mcfg-readonly run is
// what would confirm or refute these). Added per the 2026-09-23
// preflight review, which found the previous "any nonzero READDIR
// errno == EOF" interpretation could misclassify a genuine protocol
// error (e.g. INVALID_SEQUENCE from a wrong starting sequence number)
// as a normal, successful empty-directory listing - a false success.
// Only kEfsErrDirEntryNotFound is currently treated as a plausible
// normal end-of-directory signal anywhere in this codebase (see
// efs_dir_lister.cpp) - every other value here is a genuine protocol
// error and must fail closed, never be silently treated as EOF. ---
constexpr int32_t kEfsErrInconsistentState = 0x40000001;
constexpr int32_t kEfsErrInvalidSequence = 0x40000002;
constexpr int32_t kEfsErrDirNotOpen = 0x40000003;
constexpr int32_t kEfsErrDirEntryNotFound = 0x40000004;
constexpr int32_t kEfsErrInvalidPath = 0x40000005;
constexpr int32_t kEfsErrPathTooLong = 0x40000006;
constexpr int32_t kEfsErrTooManyOpenDirs = 0x40000007;
constexpr int32_t kEfsErrInvalidDirEntry = 0x40000008;
constexpr int32_t kEfsErrTooManyOpenFiles = 0x40000009;

// Wraps a command-specific payload (already built by one of the Build*
// functions below) with the 4-byte [0x4B][subsys][cmd_lo][cmd_hi]
// header. `payload` may be empty (e.g. none of our read-only commands
// actually need an empty payload, but this keeps the function general).
std::vector<uint8_t> WrapEfsHeader(uint16_t cmd, const std::vector<uint8_t>& payload);

// Validates the 4-byte echo header at the START of a raw EFS2 response
// and, on success, returns the bytes AFTER it (the actual response
// payload) via `out_payload`. Returns false - and does NOT touch
// `out_payload` - if the response is too short for even the header, or
// the magic byte doesn't match, or (Phase D0 hardening beyond qtools'
// own check) the subsystem or echoed command code don't match what this
// probe actually sent. This is the "cmd/subsys correlation" mechanism -
// see this file's header comment.
bool StripEfsResponseHeader(const uint8_t* raw, size_t raw_len,
                             uint16_t expected_cmd, uint8_t expected_subsys,
                             std::vector<uint8_t>* out_payload);

// --- Request builders. Every one returns an EMPTY vector (never a
// throw, never a partially-built buffer) if given input that would
// violate this probe's own safety rules (oversized path, non-read-only
// open flags) - callers MUST treat an empty return as "refuse to send",
// not as "empty payload is fine". ---

std::vector<uint8_t> BuildStatRequest(const std::string& path);
std::vector<uint8_t> BuildOpenDirRequest(const std::string& path);
std::vector<uint8_t> BuildCloseDirRequest(uint32_t dirp);
std::vector<uint8_t> BuildReadDirRequest(uint32_t dirp, int32_t seqno);

// `oflag` MUST be exactly kOflagReadOnly (0, i.e. POSIX O_RDONLY, which
// is universally 0 on every libc this project has ever needed to
// consider) - anything else returns an empty vector. This is the
// request-construction-level enforcement of "no write path" Phase D0
// requires; see also the CLI layer, which never even offers a caller a
// way to pass a different oflag in the first place.
constexpr int32_t kOflagReadOnly = 0;
std::vector<uint8_t> BuildOpenReadOnlyRequest(const std::string& path,
                                               int32_t oflag);

std::vector<uint8_t> BuildReadRequest(int32_t fd, uint32_t nbyte,
                                       uint32_t offset);
std::vector<uint8_t> BuildCloseRequest(int32_t fd);

// --- Parsed response types. `ok` means "decoded safely, structurally
// valid" - it does NOT mean "the file exists"/"the read succeeded";
// check `diag_errno`/`bytes_read` etc. for that, exactly like this
// project's PDC code separates decode-success from modem-level
// rejection (see pdc_wire.h's ParseOperationResultOnly /
// IndicationOutcome precedent). ---

struct EfsStatResult {
  bool ok = false;
  int32_t diag_errno = 0;
  int32_t mode = 0;
  int32_t size = 0;
  int32_t nlink = 0;
  int32_t atime = 0;
  int32_t mtime = 0;
  int32_t ctime = 0;
};

struct EfsOpenDirResult {
  bool ok = false;
  uint32_t dirp = 0;
  int32_t diag_errno = 0;
};

struct EfsReadDirResult {
  bool ok = false;
  uint32_t dirp = 0;
  int32_t seqno = 0;
  int32_t diag_errno = 0;
  int32_t entry_type = 0;  // 0=file, 1=dir, 2=symlink (qtools convention).
  int32_t mode = 0;
  int32_t size = 0;
  int32_t atime = 0;
  int32_t mtime = 0;
  int32_t ctime = 0;
  std::string name;
};

struct EfsOpenResult {
  bool ok = false;
  int32_t fd = 0;
  int32_t diag_errno = 0;
};

struct EfsReadResult {
  bool ok = false;
  int32_t fd = 0;
  uint32_t offset = 0;
  int32_t bytes_read = 0;
  int32_t diag_errno = 0;
  std::vector<uint8_t> data;
};

struct EfsCloseResult {
  bool ok = false;
  int32_t errno_value = 0;
};

struct EfsCloseDirResult {
  bool ok = false;
  int32_t errno_value = 0;
};

// Every Parse* function takes the RAW response (header still attached)
// plus the token/cmd this response must match, and internally calls
// StripEfsResponseHeader first - callers never have to remember to
// strip the header themselves, and can never accidentally parse an
// unrelated/mismatched response as if it were the answer to their
// request.

EfsStatResult ParseStatResponse(const uint8_t* raw, size_t raw_len);
EfsOpenDirResult ParseOpenDirResponse(const uint8_t* raw, size_t raw_len);
// Hardened (2026-09-23 preflight review) to accept whichever of two
// public reference layouts this firmware actually uses, since which
// one applies has NOT been H41-confirmed yet: forth32/qtools' fixed
// `name[100]` field (PUBLIC_REFERENCE_CONFIRMED), or OpenPST's
// dm_efs.h variable-length name tail (PUBLIC_REFERENCE_SUPPORTED). A
// nonzero `diag_errno` response is accepted with only the minimal
// 12-byte dirp/seqno/diag_errno header present - error responses need
// nothing else to be classified, and requiring the full 36-byte
// metadata + name field for an error packet risked rejecting a
// genuine protocol error as "malformed" purely because it omitted
// fields meaningless to an error. `ok == true` here still means
// "structurally parsed", exactly as elsewhere in this file - a
// nonzero `diag_errno` is NOT operation success; see
// efs_dir_lister.cpp for how a caller must classify it (only
// kEfsErrDirEntryNotFound is currently treated as normal EOF).
EfsReadDirResult ParseReadDirResponse(const uint8_t* raw, size_t raw_len);
EfsOpenResult ParseOpenResponse(const uint8_t* raw, size_t raw_len);
// `requested_nbyte` bounds how many data bytes this parser will ever
// trust, independent of what the wire claims - a response claiming more
// bytes than we asked for is rejected outright (fail-closed; see Phase
// D0's explicit "bytes_read > requested size" rejection rule).
// `expected_fd`/`expected_offset` implement Phase D0's explicit
// correlation requirement ("response fd == request fd", "response
// offset == request offset") - a response for a different fd or a
// different offset than what THIS call asked for is rejected, not
// silently accepted as if it were the right answer.
EfsReadResult ParseReadResponse(const uint8_t* raw, size_t raw_len,
                                 int32_t expected_fd, uint32_t expected_offset,
                                 uint32_t requested_nbyte);
EfsCloseResult ParseCloseResponse(const uint8_t* raw, size_t raw_len);
EfsCloseDirResult ParseCloseDirResponse(const uint8_t* raw, size_t raw_len);

}  // namespace guacamole_efs
