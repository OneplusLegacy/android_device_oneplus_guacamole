// guacamole_efs_probe - EFS2-over-DIAG MUTATING wire codec. Cycle 2,
// Section E. Deliberately kept in a PHYSICALLY SEPARATE file from
// efs_wire.h (the read-only codec) - matching this project's own
// established precedent (pdc_wire.h vs pdc_load_wire.h from Phase B/C0:
// read and write kept apart so `grep`-auditing "does efs_wire.h contain
// any mutating command" stays a trivial, permanent, zero-result check).
//
// SCOPE (Cycle 2 Section E, exactly as authorized): OPEN with a single
// FIXED write-truncate flag combination, WRITE, and reuse of the
// already-confirmed read-path CLOSE (see efs_wire.h - CLOSE is not
// inherently read-only, it's shared). RENAME and UNLINK are explicitly
// NOT implemented here: this project has no confirmed EFS2 wire layout
// for either (a 2026-09-24 WebFetch of forth32/qtools' efsio.c found
// `efs_close`/`efs_unlink` present but `efs_rename` absent from that
// reference entirely), and the review that authorized this file is
// explicit that RENAME/UNLINK may only be added "protocol 근거가
// 확보되는 경우에만" (only once real wire-layout evidence exists) - so
// atomic replace-via-temp-file-then-rename is NOT implemented; instead,
// the write path truncates the target itself via OPEN's oflag (see
// kOflagWriteTruncate below) before writing the new content in place.
// PUT/PUT_DEPRECATED are explicitly excluded per the same review (no
// confirmed atomicity/safety advantage over OPEN+WRITE+CLOSE).
//
// SOURCE OF TRUTH: forth32/qtools `efsio.c`/`efsio.h`, fetched directly
// 2026-09-24 (this file's own research pass) via WebFetch of
// https://raw.githubusercontent.com/forth32/qtools/master/efsio.c and
// .../efsio.h - the SAME reference efs_wire.h's read-only layer already
// cites, extended here to its WRITE/OPEN(write-mode) functions:
//
//   efs_write(fd, buf, size, offset):
//     request  = { int32 fd; uint32 offset; char data[size]; }  (8+size bytes)
//     response = { int32 fd; uint32 offset; int32 bytes_written; int32 diag_errno; }
//     command code EFS2_DIAG_WRITE = 5.
//
//   efs_open(filename, oflag) [write-capable form - the read path's own
//   efs_wire.h already covers oflag=O_RDONLY=0]:
//     request  = { int32 oflag; int32 mode; char name[100]; }
//     response = { int32 fd; int32 diag_errno; }  -- IDENTICAL shape to
//     efs_wire.h's already-implemented EfsOpenResult/ParseOpenResponse,
//     so this file does NOT re-declare a parser for it - see
//     BuildOpenWriteRequest's own doc comment below for why it reuses
//     efs_wire.h's ParseOpenResponse directly.
//     command code EFS2_DIAG_OPEN = 2 (same code as the read-only OPEN -
//     oflag is what distinguishes intent, not the command code).
//
//   command codes confirmed alongside (2026-09-24 fetch of efsio.h):
//     EFS2_DIAG_WRITE=5, EFS2_DIAG_UNLINK=8, EFS2_DIAG_RENAME=14,
//     EFS2_DIAG_PUT=38, EFS2_DIAG_TRUNCATE=40 - all PUBLIC_REFERENCE_CONFIRMED
//     as numeric values, but ONLY WRITE's (and the shared OPEN/CLOSE's)
//     full wire layout is used in this codebase - see the SCOPE note
//     above for why the others remain unimplemented.
//
// H41_DEVICE_NOT_YET_CONFIRMED: no real device write has been attempted
// yet (Gate W1 - see the completion report that introduced this file).
// This wire layer is implemented and host-tested to the same standard
// as the read-only layer, but nothing in this codebase calls it against
// a real device until that gate is explicitly cleared.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "efs_wire.h"  // reuses WrapEfsHeader/StripEfsResponseHeader/
                        // kEfsSubsysFs/kEfsHeaderMagic/EfsOpenResult/
                        // ParseOpenResponse/EfsCloseResult/
                        // BuildCloseRequest/ParseCloseResponse - CLOSE
                        // and the OPEN response shape are shared
                        // verbatim with the read-only path; nothing
                        // mutating is added to efs_wire.h itself.

namespace guacamole_efs {

constexpr uint16_t kEfsCmdWrite = 5;
// EFS2_DIAG_OPEN (2) is reused for the write path too - see this file's
// header comment; there is no separate "write open" command code.

// The ONLY oflag combination this codebase will ever send for a write
// OPEN: O_WRONLY(1) | O_CREAT(0100) | O_TRUNC(01000) = 0x241 (577
// decimal) - truncates the target to zero length before the WRITE
// loop begins, so a shorter replacement never leaves stale trailing
// bytes from the old content (this project has no confirmed EFS2
// RENAME wire layout to fall back on a temp-file+atomic-rename
// pattern instead - see this file's header comment). There is
// deliberately NO way to construct a write-mode OPEN request with any
// other oflag anywhere in this file - BuildOpenWriteRequest() takes no
// oflag parameter at all, unlike the read path's
// BuildOpenReadOnlyRequest() (which at least validates a passed-in
// value) - this is a STRUCTURALLY stronger guarantee, not just a
// runtime check.
constexpr int32_t kOflagWriteTruncate = 1 /* O_WRONLY */ | 0100 /* O_CREAT */ |
                                         01000 /* O_TRUNC */;

// Defensive cap on a single WRITE request's payload - this project's
// own trust boundary (no confirmed protocol maximum exists), same
// philosophy and same conservative value as efs_wire.h's
// kMaxReadRequestBytes.
constexpr uint32_t kMaxWriteRequestBytes = 1024;

// Builds an OPEN request for `path` using EXACTLY kOflagWriteTruncate -
// no parameter selects a different flag; this function structurally
// cannot build any other kind of write-mode OPEN. Returns an empty
// vector (refuse to send) if `path` is empty or exceeds the 99-usable-
// byte name field, same bound as the read path's
// BuildOpenReadOnlyRequest. The response to this request is parsed
// with efs_wire.h's own `ParseOpenResponse()`/`EfsOpenResult` -
// deliberately NOT redeclared here, since the wire shape (fd,
// diag_errno) is identical regardless of which oflag was sent; two
// parsers for the same 8-byte struct would only invite them to drift.
std::vector<uint8_t> BuildOpenWriteRequest(const std::string& path);

// Builds a WRITE request for up to kMaxWriteRequestBytes of `data`
// starting at `offset` on already-open descriptor `fd`. Returns an
// empty vector (refuse to send) if `data.size()` is 0 or exceeds
// kMaxWriteRequestBytes - callers chunk larger payloads themselves
// (see efs_writer.h), matching the read path's own chunking
// convention.
std::vector<uint8_t> BuildWriteRequest(int32_t fd, uint32_t offset,
                                        const std::vector<uint8_t>& data);

struct EfsWriteResult {
  bool ok = false;  // structurally parsed - NOT "the write succeeded",
                     // exactly like every other Parse* in this project;
                     // check `diag_errno` and `bytes_written` for that.
  int32_t fd = 0;
  uint32_t offset = 0;
  int32_t bytes_written = 0;
  int32_t diag_errno = 0;
};

// Parses a WRITE response. `expected_fd`/`expected_offset` implement
// the same correlation requirement as efs_wire.h's ParseReadResponse:
// a response for a different fd or a different offset than what THIS
// call asked for is rejected, never silently accepted.
// `requested_nbyte` bounds how many bytes_written this parser will
// ever trust, independent of what the wire claims - a response
// claiming to have written more bytes than were actually sent is
// rejected outright (fail-closed; mirrors ParseReadResponse's
// "bytes_read > requested" rejection, applied here to bytes_written).
EfsWriteResult ParseWriteResponse(const uint8_t* raw, size_t raw_len,
                                   int32_t expected_fd,
                                   uint32_t expected_offset,
                                   uint32_t requested_nbyte);

}  // namespace guacamole_efs
