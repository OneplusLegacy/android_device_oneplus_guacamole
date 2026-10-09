# Phase D1 — EFS2 read-only implementation findings

Companion to `PHASE_D0_EFS2_RESEARCH.md` (the evidence trail). This
document covers what was actually built, how it was tested, and the
completion verdict. As of this revision, TWO things have been executed
on the real device — `transport-info` and `callback-smoke`, both PASS
(see `PHASE_D0_EFS2_RESEARCH.md`'s "Real-device result" and
`LOOPBACK_ROUNDTRIP_DEVICE_RESULT` sections) — and `stat-mcfg`, the
first real EFS2 operation, has been implemented and host-tested but NOT
run on-device; everything else below is either static analysis or
host-side (build-host) compilation/test execution.

**Third same-day update**: `callback-smoke` was run on the real device
and **succeeded** - a genuine callback-mode request/response round trip
with the modem, header-correlated (`header_match=true`). This confirms
the whole transport layer end-to-end for the first time. That same run
found one real bug: `Deinit()`'s `SwitchLogging(0)` call failed on the
real device (H.41 rejects mode 0 as an invalid logging mode - confirmed
both by the real device's own error log and by the public Android diag
kernel driver's mode-validation code, which simply doesn't include 0
among its accepted modes). Fixed by removing that call entirely -
`Diag_LSM_DeInit()` cleans up on its own regardless (confirmed via
disassembly: `pthread_kill -> pthread_join -> close -> free`x2,
internally, independent of any logging-mode switch). Building on the
now-device-confirmed transport, `stat-mcfg` - a single STAT against the
fixed `/nv/item_files/mcfg` target - is now implemented and
host-tested, ready for its own real-device authorization.

(Second same-day update, for context: a review of the first Phase D0.5
fix found `OnCallback()` accepted ANY valid-length callback as a
request's answer, with no check it actually correlated to what was
sent - see `PHASE_D0_EFS2_RESEARCH.md`'s `RESPONSE_CORRELATION`
section. Fixed with a 4-byte header-matching check plus session-level
poisoning after a timeout. While adding regression tests for this,
ThreadSanitizer flagged a lock-order "potential deadlock" - confirmed a
false positive and resolved by replacing the request-reservation mutex
with a plain atomic.)

## New files

| File | Role |
|---|---|
| `include/efs_wire.h` / `src/efs_wire.cpp` | EFS2-over-DIAG wire codec — READ-ONLY command subset only (OPEN, CLOSE, READ, OPENDIR, READDIR, CLOSEDIR, STAT). Pure, transport-independent, host-testable. Contains no encoder for any mutating command at all. |
| `include/efs_client.h` | `IEfsClient` abstract interface (one round trip per EFS2 op) — lets orchestration logic be host-tested against a fake, mirrors `guacamole_pdc::IPdcClient`. No real implementation exists yet (would require wiring `DiagTransport::SendSubsysCommand()` + `efs_wire.h` together, not attempted this phase). |
| `include/efs_reader.h` / `src/efs_reader.cpp` | `ReadWholeFile()` — STAT-first, chunked-READ, size-checked whole-file reader on top of `IEfsClient`. No retries anywhere. |
| `include/diag_transport.h` / `src/diag_transport.cpp` | `Init()`/`Deinit()` (`Diag_LSM_Init`/`Diag_LSM_DeInit`) and `SendSubsysCommand()` (full callback-mode request/response round trip) are both `H41_DEVICE_CONFIRMED` - both real-device runs (`transport-info`, `callback-smoke`) passed. Went through FOUR same-day bugfix rounds total before/around those runs: (1) a dangling callback-context pointer, fixed by registering `this` instead of a local variable's address (`active_instance_` removed entirely); (2) a non-atomic `MAX_OUTSTANDING_REQUESTS=1` reservation, fixed with an atomic `compare_exchange_strong`-based `request_in_flight_` flag; (3) missing response correlation, fixed with a 4-byte expected-header check plus timeout-triggered session poisoning (`SessionState::kDesynchronized`); (4) `Deinit()` calling `SwitchLogging(0)`, which the real device rejected (H.41 has no valid mode 0) - removed entirely. Depends only on the abstract `IDiagCallbackShim` interface, not on libdiag directly - so this whole file is host-testable and lives in BOTH the device binary and the host test target. |
| `include/diag_callback_shim.h` / `src/diag_callback_shim.cpp` | The callback's third parameter is `void* context` (corrected from an original, incorrect `int* proc`), matching `diag_register_callback`'s own disassembled body (stores its 2nd arg by value into a global table, never writes through it - PHASE_D0's `CALLBACK_CONTEXT_SEMANTICS` section). Thin wrapper around the 5 real libdiag.so calls `SendSubsysCommand()` needs. Device-only (links real libdiag). |
| `include/diag_loopback_packet.h` | The exact 7-byte "Modem loopback request to MSM" packet `diag_callback_sample` itself sends - traced byte-for-byte via disassembly, NOT an EFS2 packet, NOT decoded/interpreted further. Used by the now real-device-PASSED `callback-smoke` transport smoke test. |
| `include/callback_smoke.h` / `src/callback_smoke.cpp` | `RunCallbackSmoke()` sends `kH41LoopbackPacket` via `SendSubsysCommand()` and returns the result. **Real-device PASS** via `main.cpp`'s `callback-smoke` command. No libdiag dependency - lives in both the device binary and the host test target. |
| `include/stat_mcfg.h` / `src/stat_mcfg.cpp` | New (this revision). `RunStatMcfg()` - the first real EFS2 operation - sends a STAT for the fixed `/nv/item_files/mcfg` target (`EfsTarget::kMcfgDir`, never a caller-supplied path) via `SendSubsysCommand()` and parses the response with `efs_wire.h`'s already-host-tested `ParseStatResponse()`. Distinguishes transport-level success from EFS-level success (a response can arrive and parse cleanly while still carrying a nonzero `diag_errno`). Implemented, host-tested, wired to `main.cpp`'s `stat-mcfg` command as a real, functional command - **not yet run on an actual device**. No libdiag dependency - lives in both the device binary and the host test target. |
| `include/efs_targets.h` / `src/efs_targets.cpp` | Fixed 8-target EFS path whitelist — the only place in this codebase allowed to know a real EFS path string; maps a CLI target NAME to a fixed `EfsTarget` enum, never accepts an arbitrary path. `stat-mcfg` is the first command to actually use it for a real (prepared) operation. |
| `src/main.cpp` | CLI entry point. `transport-info` and `callback-smoke` are real-device PASS; `stat-mcfg` is a real, functional command not yet run on-device; every other listed subcommand name prints `NOT_IMPLEMENTED_THIS_PHASE`. `stat-mcfg` is the only command that reaches `efs_wire.h`/EFS2 at all, and only through the fixed `/nv/item_files/mcfg` target - no caller-supplied path anywhere in this file. |
| `tests/efs_wire_test.cpp` | 36 tests: wire-encoding exact-bytes, safety/rejection, parser/malformed-response tests. |
| `tests/efs_reader_test.cpp` | 9 tests: whole-file-read orchestration against a fake `IEfsClient`. |
| `tests/efs_targets_test.cpp` | 7 tests: path whitelist resolution and rejection, including a spy-client "zero transport calls" proof. |
| `tests/diag_transport_test.cpp` | **35 tests** (grew from an original 17 across three review-driven rounds): init/deinit, callback-readiness failures, round trip, concurrency/correlation, response correlation (unrelated-callback rejection, wrong-subsys/wrong-cmd/too-short rejection, timeout-measured-from-original-send), session poisoning after timeout, callback-context-lifetime/routing, copy-before-return, shutdown cancellation, and Deinit's logging-mode behavior - `DeinitAfterCallbackSessionCallsLsmDeinit`/`NoInvalidLoggingModeZeroIsRequested` (replacing two tests whose "`SwitchLogging(0)` is valid cleanup" premise the real `callback-smoke` run disproved) - against a fake `IDiagCallbackShim`. |
| `tests/callback_smoke_test.cpp` | **6 tests**: exact-bytes-sent verification against `kH41LoopbackPacket`, a genuinely matching success round trip, unrelated-callback-then-matching-succeeds, unrelated-only-times-out, a static byte-value regression guard, and timeout-does-not-retry. Now backed by a real-device PASS, not just these host tests. |
| `tests/stat_mcfg_test.cpp` | New (this revision). **9 tests**: `StatMcfgRequestStartsWith4B130F00`, `StatMcfgUsesExactWhitelistedPath`, `StatMcfgNeverAcceptsCallerSuppliedPath`, `StatMcfgSuccessfulResponseParsed`, `StatMcfgNonzeroDiagErrnoFails` (transport success ≠ EFS success), `StatMcfgWrongHeaderIgnoredThenTimesOut`, `StatMcfgMalformedResponseFails`, `StatMcfgTimeoutDoesNotRetry`, `StatMcfgDoesNotInvokeAnyMutatingCommand`. |
| `Android.bp` | `guacamole_efs_probe` device binary (vendor:true, debug-only via `device.mk`) + `guacamole_efs_probe_test` vendor-independent host test target. |

### Changed files

- `device/oneplus/guacamole/device.mk`: added `guacamole_efs_probe` to
  `PRODUCT_PACKAGES_DEBUG`, same posture as the existing
  `guacamole_pdc_probe` line (debug-only, this device only).
- `device/oneplus/guacamole/Android.bp`: no change needed — its
  `soong_namespace` already imports `vendor/oneplus/sm8150-common`
  (added for `libqmi_cci`/`libqmiservices` in Phase B), and `libdiag` is
  a `cc_prebuilt_library_shared` module in that same file/namespace
  (confirmed via `grep -n 'name: "libdiag"'`).

## Implemented CLI commands

| Command | Status |
|---|---|
| `transport-info` | **Implemented AND real-device PASS** (2026-09-23) — see `PHASE_D0_EFS2_RESEARCH.md`'s "Real-device result" section. Calls `Diag_LSM_Init(nullptr)`, prints the result, calls `Diag_LSM_DeInit()`. Sends zero EFS2 (or any other subsystem) packets. Should not be re-run. |
| `callback-smoke` | **Implemented AND real-device PASS** (2026-09-23) — see `PHASE_D0_EFS2_RESEARCH.md`'s `LOOPBACK_ROUNDTRIP_DEVICE_RESULT` section. Sends `include/diag_loopback_packet.h`'s byte-exact H.41 loopback packet (not EFS2) via `SendSubsysCommand()`; real result: `transport_status=OK`, `response_len=19`, `header_match=true`. Should not be re-run - the evidence it was meant to gather is already in hand. |
| `stat-mcfg` | **Implemented and host-tested, fully functional - NOT YET RUN on a real device.** The first real EFS2 operation: STAT against the fixed `/nv/item_files/mcfg` target via `SendSubsysCommand()`, parsed with `efs_wire.h`'s codec. Prints `target=`/`request_hex=`/`transport_status=`/`response_len=`/`response_hex=`/`header_match=`, then (only if transport succeeded) `parse_status=`/`diag_errno=`/`mode=`/`size=`/`nlink=`/`atime=`/`mtime=`/`ctime=`. Exit code reflects the EFS-level result (`diag_errno`), not just the transport-level one. One command, no retry. This is the natural next real-device candidate, now that the transport itself is `H41_DEVICE_CONFIRMED` - see "Remaining unresolved items" below for exactly what it would settle. |
| `list-mcfg`, `read-sel-db`, `read-autoselect`, `read-rfs-digest`, `list-cards`, `read-debug-log`, `read-debug-old-log`, `read-efslogging-flag` | Recognized by name (argv parsing only), print `NOT_IMPLEMENTED_THIS_PHASE`, perform no DIAG I/O. Their fixed target paths resolve through `efs_targets.h`'s whitelist, but the underlying real on-device path strings themselves are still not confirmed against a real directory listing — see "Remaining unresolved items" below. |

## Write-path absence audit

Grep across `include/`, `src/`, `tests/` for every public-reference
mutating EFS2 command name (`WRITE`, `UNLINK`, `MKDIR`, `RMDIR`,
`RENAME`, `CHMOD`, `CHOWN`, `PUT`, `TRUNCATE`, `SHRED`) turns up matches
**only** in: (a) doc comments explicitly stating these are NOT
implemented, (b) test names like `OpenWriteOnlyRejectedBeforeTransport`
that assert such opens are rejected, and (c) the unrelated English word
"write" inside comments about the transport's own `write(2)` syscall or
about truncated response payloads. `efs_wire.h`'s command-constant list
(`grep -n '^constexpr uint16_t kEfsCmd'`) shows exactly 7 constants —
`kEfsCmdOpen/Close/Read/OpenDir/ReadDir/CloseDir/Stat` — all from the
public read-only subset, with no `kEfsCmdWrite` (or any other mutating
constant) defined anywhere. `main.cpp` never calls into `efs_wire.h` or
`efs_client.h` at all - `SendSubsysCommand()`/`RunCallbackSmoke()` ARE
now called for real, but only from the `callback-smoke` command, whose
payload is `include/diag_loopback_packet.h`'s fixed, non-EFS2
7-byte array - never anything derived from `efs_wire.h`, and never
anything containing `0x13` (EFS2's subsys id). The only OTHER DIAG call
`main.cpp` makes is `Diag_LSM_Init`/`Diag_LSM_DeInit` via
`diag_callback_shim.cpp`, for `transport-info`. Re-ran this same grep
across every file touched by both same-day reviews
(`diag_callback_shim.h/.cpp`, `diag_transport.h/.cpp`,
`diag_loopback_packet.h`, `callback_smoke.h/.cpp`, `main.cpp`, and all
three test files) specifically: the only matches are (a) the English
word "overwrites"/similar in disassembly-provenance comments and (b)
test names already covered by the earlier list's pattern - no new
mutating-command surface introduced. There is no code path anywhere in
this module that could send a mutating EFS2 packet, or send any EFS2
packet at all, as of this revision.

`/enable_mcfg_efslogging` (the flag the user explicitly required be
read-only and never activated this phase): not referenced anywhere in
this module's code — `read-efslogging-flag` is one of the
not-yet-implemented stub commands above, so no read of it has happened
either, on-device or otherwise.

## Test results

Compiled and run on the build host against real AOSP googletest sources
(`external/googletest/googletest`), the same methodology already
established and proven for `guacamole_pdc_probe`:

```
g++ -std=c++17 -Wall -Wextra -Werror \
  -I googletest/include -I googletest -I efs_probe/include \
  efs_probe/tests/efs_wire_test.cpp efs_probe/tests/efs_reader_test.cpp \
  efs_probe/tests/efs_targets_test.cpp efs_probe/tests/diag_transport_test.cpp \
  efs_probe/tests/callback_smoke_test.cpp efs_probe/tests/stat_mcfg_test.cpp \
  efs_probe/src/efs_wire.cpp efs_probe/src/efs_reader.cpp \
  efs_probe/src/efs_targets.cpp efs_probe/src/diag_transport.cpp \
  efs_probe/src/callback_smoke.cpp efs_probe/src/stat_mcfg.cpp \
  googletest/src/gtest-all.cc googletest/src/gtest_main.cc \
  -lpthread -o efs_all_test
```

Result: **102/102 tests passed**, 0 failures (matching `Android.bp`'s
`-Werror`, not just `-Wall -Wextra`). Re-run 15+ times consecutively
with no flakes; the concurrency-sensitive subset
(`tests/diag_transport_test.cpp` + `tests/callback_smoke_test.cpp` +
`tests/stat_mcfg_test.cpp`) was additionally compiled with
`-fsanitize=thread` and run 18 times total under `setarch -R` (this
sandbox's ASLR layout otherwise trips ThreadSanitizer's own startup
mapping check, unrelated to the code under test) across two reservation
designs:
- Mutex-based reservation (superseded): ThreadSanitizer's data-race
  detector found ZERO races, but its SEPARATE lock-order-inversion
  heuristic reported 1 "potential deadlock" warning consistently. Root
  cause confirmed: two entirely independent `DiagTransport` instances
  in two unrelated tests (`SendFailureFailsClosed`,
  `ShutdownCancelsPendingRequest`), isolated and re-run as JUST those
  two tests, which still reproduced the warning - proving it cannot be
  a real deadlock (these objects never share memory or a thread) and
  is instead a documented false-positive category for that heuristic
  (cross-instance lock-order comparison for a mutex-bearing class).
- Atomic-based reservation (current): **zero warnings of any kind**
  across 5 separate runs (50 tests: 35 `DiagTransport` + 6
  `CallbackSmoke` + 9 `StatMcfg`), passing every time.

- `EfsWireEncoding` (7 tests): `StatRequestExactBytes`,
  `OpenReadOnlyRequestExactBytes`, `ReadRequestExactBytes`,
  `CloseRequestExactBytes`, `OpenDirRequestExactBytes`,
  `ReadDirRequestExactBytes`, `CloseDirRequestExactBytes`.
- `EfsWireSafety` (10 tests): `OpenWriteOnlyRejectedBeforeTransport`,
  `OpenReadWriteRejectedBeforeTransport`,
  `OpenCreateRejectedBeforeTransport`,
  `OpenTruncateRejectedBeforeTransport`, `OpenPathTooLongRejected`,
  `StatPathTooLongRejected`, `EmptyPathRejected`,
  `ReadRequestOverCapRejected`, `ReadRequestZeroBytesRejected`,
  `NoBuildFunctionProducesAMutatingCommandCode` (the automated proxy for
  the "no write-capable code path" requirement, on top of the grep audit
  above).
- `EfsWireParser` (19 tests): success-roundtrip tests for every op, plus
  `StatShortResponseRejected`, `StatWrongCmdEchoRejected`,
  `StatWrongMagicRejected`, `StatWrongSubsysRejected`,
  `ReadFdMismatchRejected`, `ReadOffsetMismatchRejected`,
  `ReadBytesLargerThanRequestedRejected`, `ReadTruncatedPayloadRejected`,
  `ReadNegativeBytesRejected`, `DirectoryEntryMalformedRejected`,
  `DirectoryEntryWrongLengthRejected`, `EmptyResponseRejectedEverywhere`.
- `EfsReader` (9 tests): `EmptyFile`, `Exact1024Boundary`,
  `ShortFinalChunk`, `MultiChunkFile`, `StatSizeAndTotalBytesMustMatch`,
  `StatFailureIsFailClosed`, `StatOnDirectoryIsRejected`,
  `EfsErrorIsNotRetried`, `TimeoutIsNotRetried`.
- `EfsTargets` (7 tests, new — added per the 2026-09-23 review):
  `AllEightFixedTargetsResolve`, `DistinctTargetsHaveDistinctPaths`,
  `NonWhitelistedPathRejectedBeforeTransport` (spy `IEfsClient` proves
  zero transport calls for a rejected name — the exact "call count=0"
  requirement the review asked for),
  `ArbitraryTraversalPathRejectedBeforeTransport`,
  `EmptyNameRejectedBeforeTransport`, `CaseSensitiveNoAccidentalMatch`,
  `RawTargetPathStringNotAcceptedAsAName`.
- `DiagTransport` (35 tests, grown from an original 17 across two
  review-driven rounds):
  - Init/Deinit/readiness: `InitFailureFailsClosed`,
    `InitSuccessAndDeinit`, `CallbackRegistrationFailureFailsClosed`,
    `SwitchLoggingFailureFailsClosed`, `SendFailureFailsClosed`.
  - Round trip/concurrency: `SingleRequestResponseRoundTrip`,
    `SecondConcurrentRequestRejected`,
    `ConcurrentEntryBeforeReservationRejectsSecond` (a real second
    thread races in while the first call is still inside
    `RegisterCallback()`, before `SwitchLogging`/`SendData` have even
    run - the exact window the original unsynchronized reservation
    missed).
  - Callback validation: `NullCallbackRejected`,
    `ZeroLengthCallbackRejected`, `OversizedCallbackRejected`.
  - Timeout: `TimeoutDoesNotRetry`, `NoCallbackCanNeverProduceOk`,
    `LateCallbackAfterTimeoutIgnored`,
    `OneRequestTimeoutCannotCompleteAnotherRequest`,
    `UnsolicitedCallbackDoesNotCompleteRequest`,
    `DuplicateCallbackDoesNotCompleteTwice`.
  - Callback context lifetime/routing: `NoDanglingStackContext`
    (asserts the registered context equals `&t`, the long-lived
    transport instance, never a transient stack address - the direct
    regression test for the dangling-pointer bug),
    `CallbackContextRemainsValidAfterRegisterReturns`,
    `CallbackRoutesToCorrectTransportInstance` (two independent
    `DiagTransport` instances, each with its own context, proving
    per-instance routing works without any process-global state).
  - Copy semantics/shutdown: `ResponseCopiedBeforeCallbackReturns`,
    `ShutdownCancelsPendingRequest`.
  - Response correlation (second review):
    `UnrelatedCallbackDoesNotCompleteLoopbackRequest`,
    `UnrelatedCallbackThenMatchingResponseSucceeds`,
    `OnlyUnrelatedCallbacksEventuallyTimeout`,
    `MatchingSubsystemWrongCommandIsIgnored`,
    `MatchingCommandWrongSubsystemIsIgnored`, `TooShortCallbackIsIgnored`,
    `TimeoutIsMeasuredFromOriginalSendNotLastUnrelatedCallback`.
  - Session poisoning (second review): `TimeoutPoisonsSession`,
    `CommandAfterTimeoutRejectedUntilReinit`,
    `ReinitAfterTimeoutAllowsFreshCommand`,
    `LateSameCommandResponseCannotCompleteNewRequest`.
  - Deinit lifecycle ordering (**revised again, third review**, after
    the real `callback-smoke` run disproved the "`SwitchLogging(0)` is
    valid cleanup" premise): `DeinitAfterCallbackSessionCallsLsmDeinit`
    (`Diag_LSM_DeInit()` still runs after a real callback session),
    `NoInvalidLoggingModeZeroIsRequested` (neither a plain Init/Deinit
    nor a Deinit after a callback session ever requests mode 0 -
    exactly one `CALLBACK_MODE`(6) switch, never more).

  `CallbackProcessorMismatchRejected` (from the original 17) was
  **removed**: it tested a "processor mismatch" concept that turned out
  not to exist in the real API - see `PHASE_D0_EFS2_RESEARCH.md`'s
  `CALLBACK_CONTEXT_SEMANTICS` section for why. Two other original
  tests (`LateCallbackAfterTimeoutIgnored`,
  `OneRequestTimeoutCannotCompleteAnotherRequest`) were updated in
  place to `Deinit()`+`Init()` between their two `SendSubsysCommand()`
  calls, since session poisoning now correctly rejects a second call on
  a timed-out session outright - their original bodies assumed the old
  (pre-poisoning) behavior. `DeinitDisablesCallbackLoggingBeforeLsmDeinit`/
  `DeinitWithoutEverSendingSkipsLoggingCleanup` (from the second review)
  were themselves **replaced** by the two tests above, for the same
  reason described in this section's own heading.
- `StatMcfg` (9 tests, new): `StatMcfgRequestStartsWith4B130F00`,
  `StatMcfgUsesExactWhitelistedPath`,
  `StatMcfgNeverAcceptsCallerSuppliedPath` (`RunStatMcfg()` has no path
  parameter at all - two independent transports produce byte-identical
  requests), `StatMcfgSuccessfulResponseParsed`,
  `StatMcfgNonzeroDiagErrnoFails` (the direct test for "transport
  success ≠ EFS success" - a structurally-valid response with a
  nonzero `diag_errno` must still be surfaced as an error to the
  caller), `StatMcfgWrongHeaderIgnoredThenTimesOut`,
  `StatMcfgMalformedResponseFails`, `StatMcfgTimeoutDoesNotRetry`,
  `StatMcfgDoesNotInvokeAnyMutatingCommand`.
- `CallbackSmoke` (6 tests): `SendsExactH41LoopbackPacketBytes`,
  `ReturnsOkOnMatchingResponse` (an earlier revision's "matching"
  response - `{0x4B,0x99,0x01}` - did not actually share the loopback
  request's header at all, contradicting the test's own name; fixed to
  use a response starting with the request's real 4-byte header),
  `UnrelatedCallbackIgnoredThenMatchingResponseSucceeds`,
  `UnrelatedOnlyResponseTimesOut`, `LoopbackPacketIsExactlySevenBytes`
  (static regression guard on `kH41LoopbackPacket`'s exact byte values),
  `TimeoutDoesNotRetry`.

Every wire-encoding/safety/parser test named in the user's Phase D0
instruction is present and passing; `WriteCommandHasNoCallablePath` from
that list is covered by the grep audit above plus
`NoBuildFunctionProducesAMutatingCommandCode` rather than as a single
named gtest case (a compile-time absence isn't directly expressible as
one runtime test — see that test's own comment for the reasoning). Every
test named in the original Phase D0.5 instruction's list is present in
`DiagTransport` under an identical name (minus
`CallbackProcessorMismatchRejected`, removed as above); every test named
in the two follow-up reviews' requested lists (concurrency/context
regression tests, then response-correlation/session-poisoning/Deinit-order
tests) is present under an identical or directly corresponding name.

Device-side-only sources (`diag_callback_shim.cpp`, `main.cpp`) were
syntax-checked (`g++ -fsyntax-only -Werror`) against this probe's own
headers only — they cannot be linked on this build host since they
require the real `libdiag.so`, matching `guacamole_pdc_probe`'s
established precedent for its own device-only sources.
`diag_transport.cpp`/`callback_smoke.cpp`/`stat_mcfg.cpp` themselves
have NO libdiag dependency (only the abstract `IDiagCallbackShim`, or
`diag_transport.h` respectively) and are therefore compiled and linked
into the host test target directly - the correlation/timeout state
machine, the callback-smoke payload, and the STAT request/parse
orchestration are all genuinely exercised, not merely syntax-checked.

## SELinux/AID note

**Corrected again 2026-09-23** — the previous revision of this note
treated the `oem_2901`/`vendor_qti_diag` name equivalence itself as the
open question. A further review pointed out that this over-stated the
uncertainty: AOSP's generic OEM AID mechanism auto-generates an
`oem_<N>` alias for any AID declared in the 2900–2999 reserved OEM
range (which is exactly what `device/oneplus/sm8150-common/config.fs`'s
`value:2901` declaration does), and the kernel/VFS only ever checks the
numeric id — not which string name was used to look it up. So:

- **`CONFIRMED`**: numeric gid **2901** is a single, real, unambiguous
  ID — referenced as `vendor_qti_diag::2901` in this exact H.41
  firmware's `vendor/etc/group`/`passwd` (`research_firmware_diag_analysis.md`,
  direct extraction) and declared as AID value `2901` in the PixelOS
  source tree's `config.fs` (`research_pixelos_source_analysis.md` §3).
  Both names are aliases of the same numeric id by construction — this
  is no longer treated as an open question.
- The genuinely open questions are the ones AOSP's naming mechanism
  doesn't answer, and this project has not yet resolved either:
  1. **What role gid 2901 actually plays for `/dev/diag` access.**
     Partially known: the character device's DAC mode bits are `0660
     system oem_2901` (`research_kernel_diag_qrtr_analysis.md`), and
     `diagchar_open()` itself has no additional uid/gid/capability
     check beyond that VFS-level DAC gate — so membership in gid 2901
     is DAC-sufficient to open `/dev/diag`, full stop, independent of
     SELinux. Not yet checked: whether any *other* gate (a
     capability check, a separate ioctl-level permission) exists
     beyond the DAC mode bits.
  2. **Which SELinux domains are actually allowed `vendor_diag_device`
     access.** `/dev/diag` is labeled `vendor_diag_device`
     (`research_firmware_diag_analysis.md`), but the compiled `allow`
     rules for that type were not decompiled in any prior pass (tooling
     gap — `sedumpolicy`/`sepolicy-check` were not available), and an
     earlier, separate check of this device's own sepolicy tree found
     zero QMI/diag-specific `.te` rules at the device level
     (`research_pixelos_qmi_pdc_analysis.md` §3) — meaning the actual
     allow rules, if any apply to a plain unprivileged-domain process,
     live in shared/inherited policy this project has not yet traced.
     **Not investigated in Phase D0/D1** — flagged here as the real
     remaining question, not attempted this pass.
- Practical consequence for `guacamole_efs_probe`: **unaffected right
  now** — this tool is not yet wired into any sepolicy domain or init
  `.rc` (manually invoked, not a persistent service, same posture as
  `guacamole_pdc_probe`), so neither open question above blocks
  anything in Phase D0. They matter once a future phase wires this (or
  a successor daemon) into a real domain that needs `/dev/diag` access
  without running as `shell`/`root`.

## PHASE_D0 / D0.5 verdict

**PASS for the explicitly authorized scope, on both counts.**

*D0*: static transport-path analysis complete (with items honestly
marked UNRESOLVED where the tools/access available couldn't settle
them; one prior finding — the original `LIBDIAG_SEND_PATH`'s
direction/role — was corrected per a 2026-09-23 review AND its own
follow-up fork, which confirmed the review and traced the real send
path; see `PHASE_D0_EFS2_RESEARCH.md`), EFS2 read-only wire codec
implemented and fully host-tested, whole-file reader implemented and
tested against a fake client. `transport-info` was authorized, run
once on the real device under SELinux `Enforcing`, and **PASSED**
(`Diag_LSM_Init`/`Diag_LSM_DeInit` both `H41_DEVICE_CONFIRMED` - see
`PHASE_D0_EFS2_RESEARCH.md`'s "Real-device result" section). It should
not be re-run.

*D0.5*: `DiagTransport::SendSubsysCommand()` - a full callback-mode
request/response round trip with a correlation/timeout state machine
enforcing `MAX_OUTSTANDING_REQUESTS=1` - is implemented and host-tested.
**Two same-day follow-up reviews found this class had FOUR real issues
- all fixed, before any device execution:**

- A **dangling callback-context pointer**: the original code registered
  the address of a local stack variable as `diag_register_callback`'s
  context argument. Disassembling that function's own body inside
  `libdiag.so` proved it stores this pointer by value for later,
  unbounded reuse - so the first real callback delivery would have
  dereferenced memory from an already-returned stack frame. Fixed by
  registering `this` (the `DiagTransport` instance) instead; the
  process-global `active_instance_` this project used before
  understanding the real API is gone.
- A **non-atomic `MAX_OUTSTANDING_REQUESTS=1` reservation**: the
  original code checked-then-set `state_` as two separate locked
  sections with a gap in between, so two threads could both pass the
  "is one outstanding?" check before either reserved the slot, both
  reach the shim, and then share the same `state_`/`result_bytes_` -
  letting one request's timeout complete the OTHER request as `kOk`
  with zero matching callbacks. A concrete reproduction confirmed this
  exact failure mode.
- **Missing response correlation**: `OnCallback()` accepted the first
  valid-length callback while `kWaiting` unconditionally - as if it
  were necessarily the outstanding request's own answer. Callback mode
  has no application-level transaction token, so nothing actually ruled
  out unrelated DIAG data completing a request it had nothing to do
  with. Fixed with a 4-byte expected-header match (only for requests
  that look like subsys commands - `>=4` bytes, `0x4B`-prefixed);
  anything else is discarded WITHOUT resetting the deadline. A related
  fix: a request that TIMES OUT now poisons the whole session
  (`kSessionDesynchronized` on every subsequent call, until an explicit
  `Deinit()`+`Init()`) - this protocol has no transaction token, so a
  late response to a timed-out request could otherwise be mistaken for
  a same-header request sent later in the same session.
- A **ThreadSanitizer lock-order-inversion false positive** (not a code
  bug, found while fixing the item above): switching the reservation
  from a mutex to a plain `std::atomic<bool>` resolves it - the
  underlying "check + reserve atomically" guarantee is unchanged, but
  an atomic doesn't participate in ThreadSanitizer's lock-order graph
  at all. Confirmed as a genuine false positive (not a masked real bug)
  by isolating the two unrelated tests it named and re-running JUST
  those two, which reproduced the same warning despite the two tests'
  `DiagTransport` objects never sharing memory or a thread.

A FOURTH issue surfaced by the real `callback-smoke` run described next
(not by static review this time): **`Deinit()` calling `SwitchLogging(0)`**.
The real device rejected mode 0 as invalid
(`diag_switch_logging` logged `"unable to switch logging mode to 0,
err: -1, errno: 22"`; the kernel independently logged `"request to
switch to invalid mode: 0"` - cross-checked against the public Android
diag kernel driver, whose mode-validation code simply has no case for
0). Fixed by removing that call entirely; `Diag_LSM_DeInit()` cleans up
on its own regardless (confirmed via disassembly:
`pthread_kill -> pthread_join -> close -> free`x2, internally).

*D0.5 real-device result*: **`callback-smoke` was run and PASSED**
(2026-09-23) - `transport_status=OK`, `response_len=19`,
`header_match=true`. This is the first real evidence the entire
callback-mode transport - registration, mode switch, send, and this
project's own response-correlation logic - works end-to-end against a
real modem, not just a fake shim in a host test. See
`PHASE_D0_EFS2_RESEARCH.md`'s `LOOPBACK_ROUNDTRIP_DEVICE_RESULT` for
the full record. It should not be re-run - the evidence it existed to
gather is already in hand.

Current state: **102/102 host tests passing** (35 in `DiagTransport`,
6 in `CallbackSmoke`, 9 in a new `StatMcfg` suite), 15+ consecutive
re-runs with no flakes, and 18 ThreadSanitizer runs total across both
reservation designs (1 confirmed-false-positive warning under the
mutex design, 0 warnings of any kind under the atomic design across 5
separate runs, 0 real data races reported at any point).

Building on the now-device-confirmed transport, `stat-mcfg` - the
first real EFS2 operation, a single STAT against the fixed
`/nv/item_files/mcfg` target - is implemented and host-tested as a
real, functional CLI command, but has NOT been run on an actual device
as of this revision.

Zero EFS2 packets, zero mutating DIAG commands of any kind, and zero
writes to `/enable_mcfg_efslogging` were sent or attempted anywhere in
this phase; `STAT` and every other real EFS2 packet remain
unauthorized pending resolution of the items below and separate
go-ahead for `stat-mcfg`'s first real run.

## Remaining unresolved items before real EFS2 (STAT) testing

1. **Resolved, implemented, AND real-device confirmed**: the real
   client-originated outbound command path is `H41_DEVICE_CONFIRMED`
   (`Diag_LSM_Init -> diag_register_callback -> diag_switch_logging(6,NULL)
   -> diag_callback_send_data`, callback signature `(void*, int, void*)`
   — see `research_h41_client_originated_efs2_path.md` and
   `PHASE_D0_EFS2_RESEARCH.md`'s `LOOPBACK_ROUNDTRIP_DEVICE_RESULT`), and
   `DiagTransport::SendSubsysCommand()` implements it correctly
   (response correlation and session poisoning included), validated
   both host-side (fake `IDiagCallbackShim`) AND on the real device
   (`callback-smoke`, `header_match=true`). No longer an open item for
   the transport layer itself. What's NOT done: wiring an EFS2 payload
   through it for real (`stat-mcfg` is implemented and host-tested but
   not yet run - see item 3), which is exactly what remains blocked on
   item 2.
2. `EFS2_SUBSYSTEM_ID` (`0x13`) is **`QUALCOMM_PROTOCOL_CONFIRMED`**
   (three independent public sources: `efsio.c`,
   `bcyj/android_tools_leeco_msm8996`'s `diagcmd.h`, and the public
   Android kernel's `diagchar_core.c` - see
   `PHASE_D0_EFS2_RESEARCH.md`'s `EFS2_SUBSYSTEM_ID` section) but
   **`H41_DEVICE_NOT_YET_CONFIRMED`** — no caller in the four analyzed
   H.41 binaries is an EFS2 caller, so this specific firmware hasn't
   been observed using it yet. **The single most important remaining
   blocker** before EFS2 traffic can be called confirmed - `stat-mcfg`'s
   first real run is exactly what would settle it; everything else
   needed to attempt that run is now confirmed or implemented. The
   `mask_request_validate()` whitelist risk flagged in an earlier
   revision is **no longer treated as a transport-layer blocker**:
   `callback-smoke`'s real run sent `subsys=0x12, cmd=0x0029` - outside
   that whitelist's allowed set for `0x12` - and still reached the
   modem with no `"mask request Invalid"` in dmesg/logcat, proving that
   whitelist (if it exists on this kernel at all) does not gate
   `diag_callback_send_data()`'s actual send path. Whether `0x13`/`cmd=15`
   is itself accepted by the modem's EFS2 service specifically is a
   separate question `stat-mcfg`'s first real run would answer.
3. `stat-mcfg` (implemented, host-tested, ready) is the prepared next
   real-device step - a single STAT against `/nv/item_files/mcfg`, no
   OPEN/READ/OPENDIR wired up yet. It would give the first real-device
   evidence for item 2 above (`0x13` device confirmation) and for the
   `efs_wire.h` codec's own correctness against a real modem response,
   for the first time.
   - see item 3).
4. Exact on-device EFS paths for the fixed, whitelisted read targets
   (`mcfg_sel_db.xml`, `mcfg_autoselect_by_uim`,
   `mcfg_rfs_sw_digest_version`, `/nv/item_files/mcfg/cards`, the debug
   log paths, `/enable_mcfg_efslogging`) — this project's prior research
   (`research_firmware_diag_analysis.md`) confirmed these files/values
   are NOT present in any static AP-side firmware extraction (expected —
   they live only in the modem's runtime EFS) and has not yet obtained
   them from a real EFS directory listing. `list-mcfg`/`OPENDIR`+`READDIR`
   against a known parent directory is the natural way to discover them
   once STAT itself is confirmed and OPENDIR/READDIR get their own
   implementation pass - not attempted this phase.
5. `REMOTE_DEVICE_ROUTING`'s `use_mdm` value for guacamole's primary
   modem path — `UNRESOLVED`, real-device-only question.
6. `HELLO_REQUIRED` — `UNRESOLVED`, no evidence either way from the
   binaries analyzed.
7. Whether gid `vendor_qti_diag`(2901) alone (without root/`su`) is
   sufficient for `/dev/diag` access, and which SELinux domains
   `vendor_diag_device` actually allows — both `NOT_TESTED`/`UNRESOLVED`,
   see the SELinux/AID note above. The real `transport-info` run so far
   only proves the root/`su` case works, which is expected and not
   informative about a future unprivileged daemon domain.

See `DEVICE_EFS_READONLY_PROCEDURE.md` for the record of the
`transport-info` and `callback-smoke` runs this phase's findings
supported and completed. There is currently no phase authorizing any
further real-device command - `stat-mcfg` is implemented and
host-tested as the natural next candidate (item 2's `0x13` device
confirmation is exactly what it would settle), but real-device
execution of it awaits explicit authorization after this update is
reviewed.
