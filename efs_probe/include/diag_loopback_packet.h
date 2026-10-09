// guacamole_efs_probe - the exact "Modem loopback request to MSM" packet
// `diag_callback_sample` itself sends on this exact H.41 firmware.
// Deliberately NOT an EFS2 packet, and NOT an inference/guess - every
// byte's origin instruction was traced directly in
// `vendor/bin/diag_callback_sample`:
//
//   0x00002270  movz w11, 0x100          ; w11 = 0x00000100
//   0x00002284  movk w11, 0x302, lsl 16  ; w11 = 0x03020100
//   0x00002288  movz w9, 0x124b          ; w9  = 0x0000124b
//   0x00002290  movk w9, 0x29, lsl 16    ; w9  = 0x0029124b
//   0x00002294  stur w11, [sp, 0x1b]     ; sp+0x1b..0x1e = 00 01 02 03
//   0x000022b4  str  w9,  [sp, 0x18]     ; sp+0x18..0x1b = 4b 12 29 00
//                                        ; (overwrites sp+0x1b, executes
//                                        ;  later in program order)
//   0x00002520  add x1, sp, 0x18         ; buffer pointer
//   0x00002524  movz w2, 0x7             ; length = 7
//   0x00002528  mov  w0, wzr             ; proc = 0 (local MSM)
//   0x0000252c  bl diag_callback_send_data
//
// Final 7 bytes (sp+0x18 through sp+0x1e), byte-for-byte:
//   4B 12 29 00 01 02 03
//
// `H41_STATICALLY_CONFIRMED`. This is NOT an EFS2 packet, not a
// `[0x4B][0x13]...` EFS2 subsys command, and not decoded further here -
// its semantic meaning (whatever "Modem loopback" does at the modem
// side) is irrelevant to its purpose in this codebase: it is a
// byte-exact, independently-firmware-confirmed payload this project can
// use to smoke-test `DiagTransport::SendSubsysCommand()` against a real
// device WITHOUT needing to know or guess anything about EFS2's own
// wire format or subsystem id - see `callback-smoke` in main.cpp.
//
// Per the 2026-09-23 review that requested this: this packet has NOT
// been sent to a real device by this codebase as of this revision -
// `callback-smoke` is implemented and host-tested only, pending
// separate authorization for its first real-device run.

#pragma once

#include <array>
#include <cstdint>

namespace guacamole_efs {

constexpr std::array<uint8_t, 7> kH41LoopbackPacket = {0x4B, 0x12, 0x29,
                                                        0x00, 0x01, 0x02,
                                                        0x03};

}  // namespace guacamole_efs
