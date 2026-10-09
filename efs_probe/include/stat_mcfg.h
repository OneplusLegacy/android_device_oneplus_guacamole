// guacamole_efs_probe - the first real EFS2 operation this project
// prepares: a single STAT against the fixed, whitelisted
// `/nv/item_files/mcfg` target (`EfsTarget::kMcfgDir`, see
// efs_targets.h - never a caller-supplied path). Built entirely from
// already-confirmed pieces: `efs_wire.h`'s `BuildStatRequest()`/
// `ParseStatResponse()` (PUBLIC_REFERENCE_CONFIRMED wire format) and
// `DiagTransport::SendSubsysCommand()` (now `H41_DEVICE_CONFIRMED` for
// the transport round trip itself, via the real `callback-smoke` run -
// see PHASE_D0_EFS2_RESEARCH.md's `LOOPBACK_ROUNDTRIP_DEVICE_RESULT`).
//
// What is STILL unconfirmed and what this function does NOT paper
// over: `EFS2_SUBSYSTEM_ID` (`0x13`) itself is `QUALCOMM_PROTOCOL_CONFIRMED`
// but `H41_DEVICE_NOT_YET_CONFIRMED` - this is exactly the call that
// would confirm or refute it for real, which is why it is implemented
// and host-tested but NOT yet run on-device (see main.cpp's
// `stat-mcfg` command and the completion report that introduced this
// file).
//
// Transport success (`TransportStatus::kOk`, a correctly-correlated
// response arrived) is NOT the same as EFS success - a response can
// arrive and still carry a nonzero `diag_errno` (the modem's own EFS2
// layer reporting an error, e.g. "no such subsystem" if `0x13` turns
// out to be wrong, or a real filesystem error). Callers MUST check
// `StatMcfgResult::parsed` AND `.stat.diag_errno`, never just the
// transport status - see `RunStatMcfg()`'s own contract below.

#pragma once

#include <chrono>
#include <cstdint>
#include <vector>

#include "diag_transport.h"
#include "efs_wire.h"

namespace guacamole_efs {

struct StatMcfgResult {
  std::vector<uint8_t> request;  // exact bytes sent - always populated,
                                  // even if the transport never got to
                                  // send them (empty in that case).
  TransportResult transport;     // raw transport-layer result.
  bool parsed = false;           // did ParseStatResponse() succeed
                                  // structurally? Only meaningful when
                                  // transport.status == kOk. false does
                                  // NOT imply an EFS-level error - it
                                  // means the response couldn't even be
                                  // decoded as a STAT response.
  EfsStatResult stat;             // only meaningful when parsed == true;
                                   // check stat.diag_errno for the
                                   // modem's own EFS-level result.
};

// `transport` must already be Init()-ialized by the caller; this
// function does not call Init()/Deinit() itself, matching
// SendSubsysCommand()'s own contract. Sends exactly one STAT request
// for the fixed `/nv/item_files/mcfg` target - never a caller-supplied
// path (there is no path parameter to this function at all). No
// retries.
StatMcfgResult RunStatMcfg(DiagTransport* transport,
                            std::chrono::milliseconds timeout);

}  // namespace guacamole_efs
