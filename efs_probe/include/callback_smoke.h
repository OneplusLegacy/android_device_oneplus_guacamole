// guacamole_efs_probe - Phase D0.5 callback-mode smoke test capability.
//
// Runs a single round trip using `diag_loopback_packet.h`'s
// `kH41LoopbackPacket` - the H.41 reference sample's own known-good,
// non-EFS2 7-byte payload - via `DiagTransport::SendSubsysCommand()`.
// This is implemented and host-tested (tests/callback_smoke_test.cpp)
// so the capability is ready ahead of time, but is deliberately NOT
// called from anywhere in main.cpp's CLI dispatch as of this revision -
// `callback-smoke` prints a "not yet authorized" message and refuses to
// run it, pending review of the 2026-09-23 completion report that
// introduced it. Do not wire this into main.cpp without that
// authorization.

#pragma once

#include <chrono>

#include "diag_transport.h"

namespace guacamole_efs {

// `transport` must already be Init()-ialized by the caller; this
// function does not call Init()/Deinit() itself, matching
// SendSubsysCommand's own contract. Sends exactly one request, waits
// once, does not retry - same fail-closed policy as everything else in
// this module.
TransportResult RunCallbackSmoke(DiagTransport* transport,
                                  std::chrono::milliseconds timeout);

}  // namespace guacamole_efs
