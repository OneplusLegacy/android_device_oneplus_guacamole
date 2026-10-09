// guacamole_efs_probe - abstract EFS2 WRITE round-trip interface.
// Cycle 2, Section E/F. Mirrors IEfsClient's role exactly (see
// efs_client.h) - lets higher-level logic (WriteWholeFile, the
// transaction backend) be host-tested against a fake, without linking
// libdiag.so or a real DIAG transport. Deliberately a SEPARATE
// interface from IEfsClient, not an extension of it - a caller holding
// only an IEfsClient& structurally cannot write anything; only code
// that was explicitly handed an IEfsWriteClient& can, and there is no
// implicit conversion between the two.
//
// NO RAW PATH (hardened 2026-09-24, Gate W1 preflight): `OpenWrite()`
// takes an `EfsWriteTarget` enum value, never a `std::string` - the
// mutation layer's public API surface structurally cannot accept a
// caller-supplied path anywhere, matching efs_write_targets.h's own
// whitelist enforcement all the way up through this interface. The
// path string only ever exists internally, inside a concrete
// implementation's own OpenWrite() body (see efs_real_write_client.cpp),
// resolved via `TargetPath(target)` - never a parameter anyone outside
// that one call site can influence.
//
// TRANSPORT STATUS PRESERVED (hardened 2026-09-24, Gate W1 preflight):
// every method returns a `Mutation*Result` that bundles the underlying
// `TransportStatus` alongside the parsed EFS2 result - an earlier
// revision collapsed TIMEOUT/FRAME_INTEGRITY_FAILED/
// SESSION_DESYNCHRONIZED/etc. into a single `ok=false`, which made it
// impossible for the transaction layer (efs_transaction.h) to tell "the
// modem explicitly rejected this" apart from "no confirmed response
// ever arrived, outcome unknown" - a distinction that matters enormously
// for a MUTATING request (see diag_transport.h's
// IsTransportOutcomeAmbiguous()).

#pragma once

#include <cstdint>
#include <vector>

#include "diag_transport.h"   // TransportStatus.
#include "efs_wire.h"         // EfsOpenResult, EfsCloseResult.
#include "efs_write_targets.h"
#include "efs_write_wire.h"   // EfsWriteResult.

namespace guacamole_efs {

struct MutationOpenResult {
  TransportStatus transport_status = TransportStatus::kInitFailed;
  // Only meaningful when transport_status == kOk - a non-kOk status
  // means no confirmed response arrived, so `parsed` is left at its
  // default (ok=false) and MUST NOT be trusted.
  EfsOpenResult parsed;
};

struct MutationWriteResult {
  TransportStatus transport_status = TransportStatus::kInitFailed;
  EfsWriteResult parsed;
};

struct MutationCloseResult {
  TransportStatus transport_status = TransportStatus::kInitFailed;
  EfsCloseResult parsed;
};

class IEfsWriteClient {
 public:
  virtual ~IEfsWriteClient() = default;

  // Opens `target`'s fixed path for writing, ALWAYS truncating it
  // first (see efs_write_wire.h's kOflagWriteTruncate) - there is no
  // way to open for append or for read+write anywhere in this
  // interface, and no way to open anything other than one of
  // efs_write_targets.h's fixed enum members.
  virtual MutationOpenResult OpenWrite(EfsWriteTarget target) = 0;
  virtual MutationWriteResult Write(int32_t fd, uint32_t offset,
                                     const std::vector<uint8_t>& data) = 0;
  virtual MutationCloseResult Close(int32_t fd) = 0;
};

}  // namespace guacamole_efs
