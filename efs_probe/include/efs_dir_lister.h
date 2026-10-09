// guacamole_efs_probe - EFS2 directory enumerator: OPENDIR -> repeated
// READDIR -> CLOSEDIR, built on the already-confirmed efs_wire.h codec
// and IEfsClient. No retries anywhere - matching this project's
// standing policy.
//
// SEQUENCE NUMBERING (corrected 2026-09-23 preflight review): the
// first READDIR call uses sequence number `kFirstReadDirSequence = 1`,
// NOT 0. bkerler/edl's real, working EFS2 directory enumeration
// (`for seqno in range(1, 0xFFFFFFFF)` in its qc_diag.py) is
// PUBLIC_IMPLEMENTATION_CONFIRMED evidence this firmware family starts
// at 1 - an earlier revision of this file started at 0 and would very
// likely have received `kEfsErrInvalidSequence` on its very first call
// against a real device.
//
// EOF DETECTION, ERRNO PATH (corrected 2026-09-23 preflight review):
// the confirmed wire layout (efsio.c, PUBLIC_REFERENCE_CONFIRMED - see
// PHASE_D0_EFS2_RESEARCH.md) has no separate "more entries follow"
// flag, so a nonzero `diag_errno` was the original guess for a
// termination signal - but "any nonzero errno" was wrong: OpenPST's
// dm_efs.h documents at least 9 distinct EFS2 protocol error codes
// (efs_wire.h's kEfsErr*), of which only `kEfsErrDirEntryNotFound`
// (0x40000004) is a plausible "no more entries" signal -
// PUBLIC_REFERENCE_SUPPORTED. Every OTHER nonzero errno (invalid
// sequence, directory not open, invalid directory entry, etc.) is a
// genuine protocol error and MUST fail the whole listing closed, never
// be silently treated as a successful empty/completed directory.
//
// EOF DETECTION, ZERO-SENTINEL PATH (H41_DEVICE_CONFIRMED, third real
// snapshot-mcfg-readonly attempt, 2026-09-23): a real enumeration of
// `/nv/item_files/mcfg` returned 34 real entries (seq 1-34), then at
// seq 35 a STRUCTURALLY VALID, CRC-correct response with `diag_errno
// == 0` but EVERY entry field zeroed (`entry_type=0, mode=0, size=0,
// atime=0, mtime=0, ctime=0`) and an EMPTY name - a "zero sentinel",
// not a real zero-length-named entry. `kEfsErrDirEntryNotFound` was
// NEVER observed on this device - this firmware's real EOD signal is
// the zero sentinel, not that errno. Without recognizing it, the
// original code kept incrementing seqno and re-requesting, receiving
// the same zero sentinel over and over until `kMaxDirectoryEntries`
// (4096) correctly tripped the safety cap - the cap did its job
// exactly as designed, but the listing should have stopped at seq 35,
// not 4096. `IsZeroSentinelEndOfDirectory()` below detects this and
// stops the loop WITHOUT sending a seq-36 request. The
// `kEfsErrDirEntryNotFound` path is KEPT (in case a future firmware
// path or an edge case on this same firmware ever uses it - it remains
// PUBLIC_REFERENCE_SUPPORTED, just H41_NOT_OBSERVED on the real paths
// exercised so far) - both EOD signals are checked, whichever a given
// real response actually uses.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "efs_client.h"

namespace guacamole_efs {

enum class ListDirectoryState { kDone, kFailed };

struct EfsDirEntry {
  int32_t entry_type = 0;  // 0=file, 1=dir, 2=symlink (qtools convention).
  int32_t mode = 0;
  int32_t size = 0;
  int32_t atime = 0;
  int32_t mtime = 0;
  int32_t ctime = 0;
  std::string name;
};

struct ListDirectoryResult {
  ListDirectoryState state = ListDirectoryState::kFailed;
  std::string message;
  std::vector<EfsDirEntry> entries;
};

// Defensive cap on entry count - this project's own trust boundary (no
// confirmed protocol maximum exists), same philosophy as efs_wire.h's
// kMaxReadRequestBytes. Exceeding it is treated as a hard failure, not
// a silent truncation.
constexpr int32_t kMaxDirectoryEntries = 4096;

// The first READDIR sequence number ListDirectory() ever sends - see
// this file's header comment for why this is 1, not 0.
constexpr int32_t kFirstReadDirSequence = 1;

// True iff `r` is the H.41 zero-metadata end-of-directory sentinel -
// see this file's header comment for the real-device evidence. Checks
// ONLY the entry's own shape (diag_errno/entry fields/name) - by the
// time ListDirectory() calls this, it has ALREADY validated `r.dirp`
// against the opened directory handle and `r.seqno` against the
// requested sequence number (fail-closed on either mismatch, before
// this function is ever reached), so this function does not re-check
// them. Exposed here (not file-local) for direct unit testing - see
// tests/efs_dir_lister_test.cpp.
bool IsZeroSentinelEndOfDirectory(const EfsReadDirResult& r);

// `path` must come from the fixed whitelist (efs_targets.h) - this
// function itself has no opinion on that; callers enforce it (see
// efs_snapshot.h, which is the only real caller).
ListDirectoryResult ListDirectory(IEfsClient* client, const std::string& path);

}  // namespace guacamole_efs
