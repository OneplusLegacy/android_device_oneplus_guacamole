// guacamole_efs_probe - whole-file chunked EFS2 writer. Cycle 2,
// Section E/F. Mirrors efs_reader.h's ReadWholeFile() shape exactly:
// OPEN(write-truncate) -> repeated WRITE (each capped at
// kMaxWriteRequestBytes) -> CLOSE, with EXACT bytes-written accounting
// as the sole success criterion. No retries anywhere - a single
// failed/short/timed-out chunk aborts the write as kFailed.
//
// NO RAW PATH (hardened 2026-09-24, Gate W1 preflight): takes an
// `EfsWriteTarget`, never a `std::string` - see efs_write_client.h's
// header comment for why.
//
// SHORT WRITE POLICY, read this before changing anything: efs_write_wire.h's
// ParseWriteResponse() itself accepts a short write as STRUCTURALLY
// valid (the wire format allows it - a modem could legitimately write
// fewer bytes than requested, e.g. under memory pressure). THIS layer
// is what turns "the server wrote fewer bytes than we asked for" into
// a hard failure - a partially-applied chunk with no way to safely
// resume mid-chunk (no confirmed retry/resume semantics exist for this
// protocol) is exactly the kind of ambiguous, partially-mutated state
// this project's whole design philosophy refuses to leave unresolved.
// A short write is therefore treated identically to a fully-failed
// write: abort the whole operation, best-effort CLOSE, report kFailed.
//
// TRANSPORT AMBIGUITY (hardened 2026-09-24, Gate W1 preflight; widened
// 2026-09-24 W1 Finalization Round): every OPEN(write)/WRITE/CLOSE
// call's `TransportStatus` is inspected via diag_transport.h's
// `IsMutationOutcomeAmbiguous()` - the MUTATION-specific classifier,
// not the shared read-path one, since it also treats `kSendFailed` as
// ambiguous here (see that function's own doc comment). ADDITIONALLY,
// a call whose `TransportStatus` is `kOk` but whose parsed response
// failed to parse structurally (`parsed.ok == false`) is ALSO now
// treated as ambiguous - a confirmed round trip does not mean the
// request's MUTATING side effect (a truncate on OPEN, bytes landing on
// WRITE) didn't happen; it only means we can't read what the modem
// actually reported about it. If ANY call comes back ambiguous by
// either rule, `EfsWriteFileResult.mutation_outcome_ambiguous` is set
// true, REGARDLESS of whether this function's own `state` ends up
// kDone or kFailed. The caller (efs_transaction.h's ApplyPatchPlan())
// is responsible for treating an ambiguous outcome specially - never
// assuming the on-device content is safe, and never blindly reusing the
// same (possibly poisoned) write client for an automatic rollback.
//
// NO SAME-SESSION CLOSE AFTER AN AMBIGUOUS WRITE (2026-09-24 W1
// Finalization Round): if a WRITE call itself comes back ambiguous,
// this function does NOT call Close() on that session - an ambiguous
// WRITE means the session's own health is unconfirmed, and a second
// unconfirmed mutating call (CLOSE) on top of the first would only
// compound the uncertainty. The function returns immediately, leaving
// resolution to the caller's independent read-back (never this same
// client). Close() is only ever called after a WRITE outcome that is
// CONFIRMED non-ambiguous (a clean diag_errno rejection or a confirmed
// short write) - in those cases the transport round trip was clean, so
// the session is known healthy and a best-effort Close() is safe.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "efs_write_client.h"

namespace guacamole_efs {

enum class EfsWriteFileState {
  kDone,
  kFailed,
};

struct EfsWriteFileResult {
  EfsWriteFileState state = EfsWriteFileState::kFailed;
  std::string message;
  uint32_t total_bytes_written = 0;
  // True iff ANY OPEN(write)/WRITE/CLOSE call this operation performed
  // had an ambiguous mutation outcome - see this file's header comment
  // (renamed from `transport_ambiguous` in the 2026-09-24 W1
  // Finalization Round: this now also covers "transport confirmed OK,
  // but the response failed to parse", which is not purely a transport-
  // level signal, so the old name was misleading). Independent of
  // `state`: a `kFailed` result can ALSO be unambiguous (e.g. a clean
  // transport round trip carrying a nonzero diag_errno - we know for
  // certain the modem processed and rejected the request), and a
  // `kDone` result can technically still have this set if, say, WRITE
  // succeeded cleanly but the final CLOSE timed out (the bytes are
  // almost certainly written, but not 100% confirmed flushed/closed -
  // conservatively still flagged).
  bool mutation_outcome_ambiguous = false;
};

// Same defensive hard cap as efs_reader.h's kMaxFileSizeBytes, applied
// to the PROPOSED content this writer will ever attempt to send - this
// project's own trust boundary, not a confirmed protocol maximum.
constexpr size_t kMaxWriteFileSizeBytes = 16 * 1024 * 1024;

// Writes `data` to `target`'s fixed path in full: OPEN (always
// truncating first - see efs_write_wire.h's kOflagWriteTruncate), a
// sequential WRITE loop with exact offset accounting, then CLOSE.
// `data.empty()` is handled by still performing the OPEN (which
// truncates the target to zero length) and CLOSE, with zero WRITE
// calls - an explicit, valid way to truncate a target to empty, not a
// no-op.
EfsWriteFileResult WriteWholeFile(IEfsWriteClient* client,
                                   EfsWriteTarget target,
                                   const std::vector<uint8_t>& data);

}  // namespace guacamole_efs
