# Phase D0 — EFS2-over-DIAG research findings

> **CORRECTED (2026-09-23):** a review flagged that the original
> `LIBDIAG_SEND_PATH` finding misidentified which DIRECTION of traffic
> `diagpkt_subsys_alloc`/`diagpkt_commit` belong to. A follow-up fork
> confirmed the review outright: that pair is the DIAG Packet Service
> RESPONSE-builder API used by a registered inbound-command HANDLER
> (`test_diag` imports `diagpkt_tbl_reg` + the whole alloc/commit
> family; `diag_callback_sample` — the actual client-that-originates-
> commands sample — imports NEITHER symbol, at all). The real
> client-originated send path was traced instead:
> `Diag_LSM_Init(NULL) -> diag_register_callback -> diag_switch_logging(6,NULL)
> -> diag_callback_send_data(proc, buf, len)` — full detail in
> `research_h41_client_originated_efs2_path.md`, which supersedes this
> document's original `LIBDIAG_SEND_PATH`/`LIBDIAG_RESPONSE_PATH`
> items (renamed/corrected below; the underlying disassembly evidence in
> those items was not wrong, only its role).
>
> **CORRECTED AGAIN, same day, before any real EFS2/callback execution:**
> a second code review + this project's own re-disassembly found the
> registered callback's third parameter is NOT `int *proc` as first
> written above - it is a caller-supplied, opaque `void *context`.
> Disassembling `diag_register_callback`'s OWN BODY inside `libdiag.so`
> (not just the sample's call site) proves it: `stp x0, x1, [x9, 8]`
> copies the callback pointer AND the second argument BY VALUE into a
> persistent global table, with no write ever made THROUGH that second
> argument. The original implementation had passed the address of a
> local stack variable as this argument - a dangling-pointer bug that
> would have caused undefined behavior on first real callback delivery.
> A SEPARATE, independently-confirmed concurrency bug (`MAX_OUTSTANDING_REQUESTS=1`
> was checked-then-set non-atomically) was also found and fixed. See
> `CLIENT_ORIGINATED_SEND_PATH` and `CALLBACK_CONTEXT_SEMANTICS` below,
> and `diag_transport.h`'s header comment for the full fix. Neither bug
> ever reached a real device - both were caught by code review before
> any authorized execution.

Scope: static analysis only (no code executed on any target binary),
covering the real DIAG→EFS2 transport path on H.41 and the EFS2
read-only wire format. This document is the evidence trail behind

Scope: static analysis only (no code executed on any target binary),
covering the real DIAG→EFS2 transport path on H.41 and the EFS2
read-only wire format. This document is the evidence trail behind
`efs_wire.h`/`efs_wire.cpp` and `diag_transport.h`/`diag_transport.cpp`;
see `PHASE_D1_EFS2_READONLY_FINDINGS.md` for what was actually built and
tested on top of it.

Confirmation levels (same scheme as every prior phase in this project):
- `PUBLIC_REFERENCE_CONFIRMED` — matches a public, independently
  published reference implementation exactly; not yet re-verified
  against this specific H.41 binary.
- `H41_STATICALLY_CONFIRMED` — independently verified by disassembling
  the actual H.41 binary (radare2 + readelf), no code executed.
- `H41_DEVICE_CONFIRMED` — verified by running code on the real device.
  **None of the items below reach this level in Phase D0** — no code
  has been executed on the device yet, per the explicit Phase D0
  restriction.
- `UNRESOLVED` — genuinely undetermined with the tools/access used so
  far; stated honestly rather than guessed.

Sources:
- `forth32/qtools`' `efsio.c`/`efsio.h`
  (https://github.com/forth32/qtools), fetched and read directly for
  this project (`/tmp/efsio.c`, 387 lines, read in full) — a real,
  published, working EFS2-over-DIAG client used against real Qualcomm
  modems.
- `research_h41_libdiag_efs2_transport.md` (this project,
  `/home/ccc/바탕화면/claude/`), the static-analysis pass over this exact
  H.41 firmware's `diag_callback_sample`, `diag_dci_sample`, `test_diag`,
  and `libdiag.so` (radare2 + readelf, all four binaries confirmed
  present and stripped, AArch64).
- `research_h41_client_originated_efs2_path.md` (this project,
  `/home/ccc/바탕화면/claude/`), the 2026-09-23 follow-up pass that
  confirmed the review's reinterpretation and traced the real
  client-originated send path, callback signature, and DCI path.
- `research_firmware_diag_analysis.md`, `research_kernel_diag_qrtr_analysis.md`
  (this project) — earlier phases' symbol-table and kernel-source
  cross-references.

## Real-device result: `transport-info` (2026-09-23) — PASS

The FIRST real-device execution of any code in this module. Result:
**H41_DEVICE_CONFIRMED PASS.**

Conditions: SELinux `Enforcing`, process context `u:r:su:s0`, uid=0.
`/dev/diag`: owner `system`, group `vendor_qti_diag` (gid 2901), type
`vendor_diag_device`. Host/device binary SHA-256 matched exactly
(confirms the pushed binary was byte-identical to what was built - see
`DEVICE_EFS_READONLY_PROCEDURE.md`'s verification step).

Observed: `Diag_LSM_Init(nullptr)` succeeded; libdiag's internal read
thread was created; `Diag_LSM_DeInit()` cleanly signaled it
(`"CreateWaitThread, Read DEINIT_TYPE packet"`, `"sending signal to
unblock read thread"`), joined it (`"CreateWaitThread, Exiting"` /
`"Joining the read thread"`), and completed (`"Diag_LSM_DeInit done"`)
— this is the NORMAL libdiag shutdown lifecycle, not an error, despite
the alarming-looking "DEINIT_TYPE packet" wording. Final `EXIT=0`. No
`vendor_diag_device`-related SELinux denial for this process's PID in
the captured avc log (the denials present belonged to unrelated
processes, e.g. `system_suspend`).

| Item | Level |
|---|---|
| `Diag_LSM_Init` runtime | `H41_DEVICE_CONFIRMED` |
| `Diag_LSM_DeInit` runtime | `H41_DEVICE_CONFIRMED` |
| libdiag read-thread create/signal/join lifecycle | `H41_DEVICE_CONFIRMED` |
| `transport-info` under SELinux Enforcing | `H41_DEVICE_CONFIRMED` |
| `/dev/diag` accessible via gid 2901 for a `uid=0`/`su` process | `H41_DEVICE_CONFIRMED` (trivially - root bypasses DAC group checks entirely) |
| `/dev/diag` access from a future **unprivileged production daemon domain** | `NOT_TESTED` - this run proves nothing about that case; see `PHASE_D1_EFS2_READONLY_FINDINGS.md`'s SELinux/AID note |
| gid `vendor_qti_diag` (2901) **alone** being sufficient for a non-root process | `NOT_TESTED` |
| `diag_register_callback` runtime | `NOT_TESTED` - `transport-info` never calls it; only exercised host-side (`tests/diag_transport_test.cpp`) as of this revision |
| `diag_switch_logging(CALLBACK_MODE)` runtime | `NOT_TESTED` (same reason) |
| `diag_callback_send_data` round trip | `NOT_TESTED` (same reason) |
| `EFS2_SUBSYSTEM_ID` (`0x13`) on H.41 | `PUBLIC_REFERENCE_CONFIRMED` / `H41_DEVICE_NOT_CONFIRMED` - unchanged, no EFS2 packet has ever been sent |
| EFS2 `STAT` | `NOT_TESTED` - not implemented, not attempted |

## Item-by-item findings

### EFS2_DIAG_HEADER

`[0x4B][subsys_id][cmd_lo][cmd_hi]`, a 4-byte header preceding every
request/response payload.

- **`PUBLIC_REFERENCE_CONFIRMED`**: `efsio.c`'s `send_efs_cmd()` builds
  exactly this — `char cmdbuf[200]={0x4b, 0x13,0,0}; cmdbuf[2]=cmd;` —
  and validates only `iobuf[0]==0x4b` on the response.
- **`H41_STATICALLY_CONFIRMED`** (structure, not the EFS2-specific
  value): `diagpkt_subsys_alloc` in this exact `libdiag.so`
  (offset `0x21284`) hardcodes the literal byte `0x4B` at the start of
  the buffer it returns, then writes the caller-supplied subsys-id byte
  and a little-endian `u16` subsys-cmd immediately after it — the same
  4-byte layout, confirmed by disassembly, not by name matching alone.

### EFS2_SUBSYSTEM_ID

`0x13` (19 decimal, `DIAG_SUBSYS_FS`/EFS2).

**Verdict, revised 2026-09-23: `QUALCOMM_PROTOCOL_CONFIRMED` /
`H41_DEVICE_NOT_YET_CONFIRMED`** — not blanket `UNRESOLVED` as an
earlier revision of this document stated. That earlier wording
conflated "not confirmed as an H.41-specific call-site value" with "not
established as a real protocol constant at all"; a review correctly
pointed out these are different claims.

- **`QUALCOMM_PROTOCOL_CONFIRMED`**: `efsio.c` hardcodes `0x13` as the
  second header byte for all EFS2 traffic (with a documented alternate
  `0x3E` for a second/alternate EFS instance not used here). Directly
  re-verified against a SEPARATE, independent public source
  (`bcyj/android_tools_leeco_msm8996`'s `diag/include/diagcmd.h`,
  fetched 2026-09-23): `DIAG_SUBSYS_FS = 19, /* File System - EFS2 */`
  — an exact, unambiguous protocol-level definition, not this project's
  inference. The review additionally cites the same constant appearing
  in the public Android Qualcomm DIAG kernel driver
  (`diagchar_core.c`) and in ModemManager/libqcdm's command headers -
  independently converging public sources, not a single citation this
  project is taking on faith.
- **`H41_DEVICE_NOT_YET_CONFIRMED`** (renamed from `UNRESOLVED`): no
  caller in the four analyzed H.41 binaries actually invokes an EFS2
  request with `0x13` specifically - none of them is an EFS2 caller at
  all, so this project has no H.41-specific disassembly evidence for
  this exact value on THIS firmware. This is expected (a userspace
  DIAG sample program has no reason to call EFS2) and does NOT cast any
  doubt on the protocol constant itself - it only means this project
  hasn't yet independently observed THIS firmware using it. Still the
  single most important open item before any real EFS2 packet is sent
  to this device: the first real `STAT` attempt is what would actually
  confirm it end-to-end (a response the codec can validate via
  `StripEfsResponseHeader`'s echoed-subsys check is itself the
  confirmation) - not attempted this phase.
- **Independently re-verified 2026-09-23**, directly fetching the
  public Android kernel source the review cited
  (`android.googlesource.com/kernel/msm`, `drivers/char/diag/diagchar_core.c`):
  `mask_request_validate()` parses exactly the header shape this
  project's codec already assumes -
  `subsys_id = mask_buf[1]; ss_cmd = *(uint16_t*)(mask_buf + 2);` for a
  `packet_id == 0x4B` packet - and its switch statement has
  `case 0x13: /* DIAG_SUBSYS_FS */ if ((ss_cmd == 0) || (ss_cmd == 0x1)) return 1;`
  (and separately `case 0x12: /* DIAG_SUBSYS_DIAG_SERV */`, matching
  `0x12`'s role in H.41's own loopback packet - see
  `include/diag_loopback_packet.h`). This further corroborates
  `QUALCOMM_PROTOCOL_CONFIRMED` for `0x13`/`DIAG_SUBSYS_FS` from a
  THIRD independent public source (after `efsio.c` and `diagcmd.h`).
- **New finding, `UNRESOLVED`, flagged honestly rather than chased
  further this pass** (out of scope for this revision's actual
  request, but directly relevant to real-device risk and found while
  verifying the citation above, so recorded here rather than silently
  dropped): `mask_request_validate()` is only reached when
  `driver->mask_check` is enabled, gating `diag_user_process_raw_data()`
  - the kernel's raw-write-from-userspace path - and for subsys `0x13`
  it only allows `ss_cmd` (the 16-bit command code) to be `0` or `1`.
  Neither H.41's own loopback packet (`subsys=0x12, cmd=0x0029=41` -
  not in `{0,6,7}`) nor a future EFS2 `STAT` (`subsys=0x13, cmd=15` -
  not in `{0,1}`) would pass this specific whitelist IF it applies to
  the send path this project's `diag_callback_send_data()` actually
  uses. Whether it does apply is itself unconfirmed: (a) this
  validation gates a raw `write()`-style path in this particular
  kernel tree, and whether `diag_callback_send_data()` routes through
  that same path (vs. a different ioctl/mechanism) was not determined
  in this pass; (b) `driver->mask_check`'s default/actual state on
  guacamole's SM8150 kernel is unknown; (c) this exact kernel source
  (a generic `kernel/msm` tree) may not match guacamole's own kernel
  fork byte-for-byte. If `callback-smoke` is run and fails, checking
  `dmesg`/`logcat` for the exact string `"mask request Invalid"` is the
  fastest way to confirm or rule this out - added to
  `DEVICE_EFS_READONLY_PROCEDURE.md`'s failure-diagnosis checklist.

### DIAG_PACKET_RESPONSE_PATH (formerly misfiled as `LIBDIAG_SEND_PATH`)

`diagpkt_subsys_alloc()` → caller fills payload → `diagpkt_commit()` →
`diag_lsm_comm_write(fd, buf, len)` → a plain checked `write(2)`
syscall.

**`H41_STATICALLY_CONFIRMED`, role corrected 2026-09-23**: this is the
DIAG Packet Service's response-builder path, used by a registered
inbound-command HANDLER (see `test_diag`'s import list below) to
construct and transmit a response to a request it has already
received — **not** the path `guacamole_efs_probe` would use to
originate an EFS2 request. The byte-level disassembly is unchanged from
the original pass and remains accurate for what it actually is:

- Every hop resolved via `readelf -r` against this `libdiag.so`'s
  relocation table (not r2's own symbol auto-labels, which were
  independently found to be wrong for two of these PLT stubs — see
  `research_h41_libdiag_efs2_transport.md`'s detail sections).
  `diag_lsm_comm_write` (`0x2e320`) sign-extends `len` and calls
  `__write_chk` directly — no HDLC escaping, no CRC16 computation
  anywhere in this userspace chain.
- **Role confirmed via import-list separation** (`research_h41_client_originated_efs2_path.md`
  §1): `test_diag` imports `diagpkt_tbl_reg`, `diagpkt_subsys_alloc`,
  `diagpkt_commit`, `diagpkt_err_rsp`, plus `event_report*`/`log_alloc`/
  `msg_send*` — the complete standard "register myself as a DIAG
  command/log/event SOURCE" symbol family, with `diagpkt_tbl_reg`
  unambiguously registering (subsys, cmd) → handler mappings for
  INBOUND requests. `diag_callback_sample` — the actual "originate a
  command, get a reply" reference sample — imports `diagpkt_subsys_alloc`/
  `diagpkt_commit` from **neither** binary, at all.
- Framing possibly happening inside the kernel `/dev/diag` driver itself
  is still `UNRESOLVED` — out of scope for userspace-only static
  analysis.

### CLIENT_ORIGINATED_SEND_PATH (the real path this probe needs)

**`H41_STATICALLY_CONFIRMED`** — full linear disassembly of
`diag_callback_sample`'s setup/send flow and its registered callback
body (`research_h41_client_originated_efs2_path.md` §2-3):

```
Diag_LSM_Init(NULL)
  -> diag_register_callback(cb_fn, context)
  -> diag_has_remote_device(&mask) / diag_register_remote_callback(...) [only if remote devices present]
  -> diag_switch_logging(6 /* CALLBACK_MODE */, NULL)
  -> [construct raw request bytes directly into a stack buffer, starting with 0x4B]
  -> diag_callback_send_data(proc, buf, len)      // one call per request, sleep() between calls
  -> diag_switch_logging(0, NULL)
  -> Diag_LSM_DeInit()
```

- `diag_callback_send_data` signature, consistent across all 6 call
  sites in the sample: `int diag_callback_send_data(int proc, void *buf,
  int len)`, 0 = success (`cbz w0, <success>`).
- No `diagpkt_subsys_alloc`/`diagpkt_commit` call anywhere in this flow
  — the client builds its raw request bytes directly, no alloc/fill/
  commit sequence.
- The sample's own first request buffer (a canned "modem loopback"
  command, not EFS2) starts with the literal byte `0x4B` — the same
  DIAG_SUBSYS_CMD_F framing byte `efs_wire.h` already uses — independent
  confirmation that raw client-originated subsystem-style commands in
  this exact firmware really do start with `0x4B` when sent this way.
  The FULL 7-byte packet was traced byte-for-byte
  (`include/diag_loopback_packet.h`) for use as a known-good
  callback-transport smoke test, independent of any EFS2-specific
  assumption.
- **Registered callback's real signature — CORRECTED 2026-09-23**:
  `void callback(void *buf, int len, void *context)`, not `int *proc`
  as first written. See `CALLBACK_CONTEXT_SEMANTICS` immediately below
  for why, and for what this means for correlation/routing.
- `MAX_OUTSTANDING_REQUESTS = 1` is not merely a conservative choice but
  a REQUIREMENT this project confirmed by reproduction: the original
  implementation's non-atomic reservation allowed two concurrent
  `SendSubsysCommand()` calls to both reach the shim, after which one
  request's timeout could complete the OTHER request as if it had
  succeeded, with zero matching callbacks. Fixed with an atomic
  `try_lock`-based reservation held for the whole call - see
  `diag_transport.h`.

### CALLBACK_CONTEXT_SEMANTICS (correction, 2026-09-23)

**`H41_STATICALLY_CONFIRMED`**, and the most safety-critical finding
of this revision. The registered callback's third parameter was
originally read as `int *proc` - a value libdiag itself supposedly
reports, indicating which processor a response came from. This is
wrong on two counts, both settled by disassembling `diag_register_callback`'s
OWN BODY inside `libdiag.so` (address `0x188ac`, 268 bytes) - not just
the sample's call site, which only shows the caller's side and cannot
by itself distinguish "output parameter" from "context pointer":

```
0x188cc  cbz  x0, <error>              ; if (cb_func == NULL) goto error
0x188d0  ...                          ; load a global registration-table
0x188dc  add  x9, x9, 0x250            ; base pointer
0x188e4  str  w8, [x9]                 ; mark this table slot "in use"
0x188ec  stp  x0, x1, [x9, 8]          ; ***copy (cb_func, arg2) BY VALUE***
                                       ; into the persistent global table
```

There is no store THROUGH `x1` (the second argument) anywhere in this
function - only a store OF `x1`'s value into a global. This proves:

1. **The third callback parameter is a caller-supplied, OPAQUE context
   pointer**, not something libdiag computes or reports. Whatever the
   registering caller passes as `diag_register_callback`'s second
   argument is exactly what comes back, unexamined, as the callback's
   third argument. `diag_callback_sample`'s callback body happens to
   dereference it as an `int*` and interpret `0` as "MSM/local" purely
   by ITS OWN convention (it registers a zeroed local for this purpose)
   - not because libdiag defines any processor-reporting semantics
   here. `diag_register_remote_callback`'s OWN body (immediately
   following in the same disassembly, `0x189b8`) independently confirms
   `proc` and `context` are separate, distinct arguments in this API
   family (`mov w19, w1` captures a PROC argument distinct from the
   context pointer) - further evidence against conflating the two.
2. **The original implementation's `RegisterCallback()` passed the
   address of one of its OWN LOCAL variables as this context** -
   `diag_register_callback(cb, &out_handle)`, with `out_handle` a stack
   variable inside a short helper function. Since libdiag stores this
   pointer VALUE for later, unbounded reuse, and the helper function
   returns almost immediately, this was a **dangling-pointer bug**:
   the first real callback delivery would have dereferenced memory
   belonging to a stack frame that no longer existed. The reference
   sample avoids this failure mode only because its equivalent slot
   lives in `main`'s own frame, which never returns for the life of the
   process - not because the API is safe to use this way in general.

**Fix**: `DiagTransport` now registers `this` (the transport instance,
whose lifetime the caller controls and which outlives every possible
callback) as the context, and the trampoline casts it directly back -
`active_instance_`, the process-global this project used before
understanding the real API, is no longer needed at all. See
`diag_callback_shim.h`/`diag_transport.h` for the corrected
implementation, and `tests/diag_transport_test.cpp`'s
`NoDanglingStackContext`/`CallbackContextRemainsValidAfterRegisterReturns`/
`CallbackRoutesToCorrectTransportInstance` for the regression coverage.
Neither this bug nor the concurrency bug above ever reached a real
device - both were caught by code review before any authorized
execution of `SendSubsysCommand()`.

### RESPONSE_CORRELATION (third bug, fixed same day)

A further review found a THIRD real gap, again before any real-device
callback execution: `OnCallback()` accepted the first valid-length
callback while a request was `kWaiting` UNCONDITIONALLY - as if it were
necessarily that request's own answer. Callback mode has no
application-level transaction token (confirmed in
`CLIENT_ORIGINATED_SEND_PATH` above), so nothing actually ruled out an
unrelated piece of DIAG data completing a request it has nothing to do
with - a real risk once more than one kind of DIAG traffic can reach
this process. **Fixed**: whenever a request looks like a subsys command
(`>=4` bytes, `request[0]==0x4B`), its first 4 bytes are remembered as
an expected header; only a callback whose own first 4 bytes match
exactly completes the request. Anything else is treated as unrelated
DIAG data and discarded WITHOUT resetting the timeout - a request's
deadline is measured from its own original `SendData()` call, never
extended by unrelated traffic arriving in the meantime
(`TimeoutIsMeasuredFromOriginalSendNotLastUnrelatedCallback`).

A related, structural consequence: because this transport has no
transaction token, a request that TIMES OUT poisons the whole session
(`SessionState::kDesynchronized`) - a late response to that exact
subsys/cmd header could otherwise be mistaken for a NEW request sharing
the same header. Once poisoned, every further `SendSubsysCommand()`
call is rejected until the caller explicitly `Deinit()`s and `Init()`s
again - no automatic recovery, matching this project's standing
no-auto-retry policy at a session level, not just a per-request one.

Also fixed the same pass, THEN REVERTED after real-device evidence
(see `LOOPBACK_ROUNDTRIP_DEVICE_RESULT` below): a since-removed version
of `Deinit()` called `diag_switch_logging(0, NULL)` before
`Diag_LSM_DeInit()`, on the assumption that mode 0 is a valid "logging
off" cleanup mode matching the H.41 sample's own textual call sequence.
The real device rejected it. `Deinit()` no longer calls
`SwitchLogging(0)` at all - see that section for the full evidence and
`diag_transport.cpp`'s current comment.

**A fourth issue, this time a tooling false positive rather than a code
bug**: after implementing the response-correlation fix, a
ThreadSanitizer run reported a "lock-order-inversion (potential
deadlock)" warning between `mutex_` and the mutex-based reservation
this project's second bugfix had introduced. Manually auditing every
lock-acquisition path confirmed no single `DiagTransport` instance ever
acquires these two locks in reverse order (Deinit()/OnCallback() never
touch the reservation lock at all), and isolating the exact two flagged
test cases (`SendFailureFailsClosed`, `ShutdownCancelsPendingRequest` -
each constructing its own independent `DiagTransport` object, never
sharing memory or a thread with the other) and re-running ONLY those
two under ThreadSanitizer reproduced the warning - proving it cannot be
a real deadlock (these two objects never interact) and is instead
ThreadSanitizer's lock-order heuristic conflating separate instances of
the same mutex-bearing class, a documented false-positive category for
that specific checker. Resolved by replacing the reservation's
`std::mutex` with a plain `std::atomic<bool>` reserved via
`compare_exchange_strong` - functionally identical (a single indivisible
check-and-set) but not a mutex at all, so it never enters
ThreadSanitizer's lock-order graph. Re-ran the full concurrency-relevant
suite under ThreadSanitizer 13 times total after this change: **zero
warnings of any kind**, including the actual data-race detector (which
never flagged anything in either revision - only the separate
deadlock-heuristic did, and only against the mutex-based version).

### LOOPBACK_ROUNDTRIP_DEVICE_RESULT (2026-09-23) — real-device PASS

`callback-smoke` was run on the real device and **succeeded**: the
callback-mode transport completed a genuine request/response round trip
with the modem for the first time.

```
request=4b122900010203
proc=0
timeout_ms=2000
transport_status=OK
response_len=19
response_hex=4b12290090be47e98a961201010203000c307e
header_match=true
EXIT=0
```

The response's first 4 bytes (`4b122900`) exactly echo the request's
own header, byte-for-byte - confirming `RESPONSE_CORRELATION`'s design
against a real callback, not just a fake shim, for the first time.

| Item | Level |
|---|---|
| `diag_register_callback` runtime (real call, real registration) | `H41_DEVICE_CONFIRMED` |
| `diag_switch_logging(CALLBACK_MODE=6, NULL)` runtime | `H41_DEVICE_CONFIRMED` |
| `diag_callback_send_data(proc, buf, len)` runtime, real send | `H41_DEVICE_CONFIRMED` |
| Full client-originated request/response round trip | `H41_DEVICE_CONFIRMED` |
| Application-layer response-header correlation (this project's own `has_expected_header_` check) | `H41_DEVICE_CONFIRMED` - validated against a REAL response, not a fake one |
| `mask_request_validate()`'s per-subsystem whitelist blocking this send path | **Ruled out for this specific request.** `subsys=0x12, cmd=0x0029` is outside that whitelist's allowed set (`{0,6,7}` for `0x12`) yet the request reached the modem and returned a real response; no `"mask request Invalid"` string appeared in dmesg/logcat. This does not prove the whitelist doesn't exist or never applies anywhere - only that it did not block callback-mode's `diag_callback_send_data()` path for this request. Whether it could still affect a DIFFERENT path (e.g. a raw ioctl write) remains unconfirmed, but it is no longer treated as a blocker for `SendSubsysCommand()` specifically. |

**A genuine bug found in this same run, not a probe failure**: `Deinit()`'s
(now-removed) `SwitchLogging(0)` call failed on the real device -
`diag_switch_logging` logged `"unable to switch logging mode to 0, err:
-1, errno: 22"` (EINVAL) and the kernel independently logged `"request
to switch to invalid mode: 0"`. Cross-checked directly against the
public Android diag kernel driver
(`android.googlesource.com/kernel/msm`, `diagchar_core.c`'s
`diag_switch_logging()`): its mode-validation `switch` statement only
accepts `CALLBACK_MODE`/`UART_MODE`/`SOCKET_MODE`/`MEMORY_DEVICE_MODE`/
`USB_MODE` - `0` is not among them and falls through to
`default: ... return -EINVAL`, logging that exact string.
`Diag_LSM_DeInit()` itself completed cleanly regardless - confirmed via
disassembly that it internally performs its own
`pthread_kill -> pthread_join -> close -> free`x2 sequence, independent
of any prior logging-mode switch (matches the real-device log lines
already seen for `transport-info`: "sending signal to unblock read
thread" / "Joining the read thread"). **Fix**: `Deinit()` no longer
calls `SwitchLogging(0)` at all - the correct H.41 lifecycle is simply
`Init -> RegisterCallback -> SwitchLogging(6) -> requests ->
Diag_LSM_DeInit`, confirmed end-to-end on the real device.

### DCI_PATH (evaluated as an alternative, not recommended)

**`H41_STATICALLY_CONFIRMED`** (structurally heavier than callback
mode): `diag_dci_sample` requires `sigaction` setup on a realtime signal
(`SIGRTMIN+n`) and spawns a dedicated worker thread
(`pthread_create`) before any DCI request can be sent — a materially
more complex initialization than callback mode's linear
`Init -> register_callback -> switch_logging -> send`. The actual
`diag_register_dci_client`/`diag_send_dci_async_req` call sites live
inside that spawned thread's own function and were not reached in this
pass — `UNRESOLVED` at the argument level.

**Recommendation for a single synchronous EFS2 STAT: callback mode**,
based on what was actually found — DCI's threading/signal overhead buys
nothing for a one-shot CLI tool, and callback mode's raw-buffer-in/
raw-buffer-out shape maps directly onto the existing `efs_wire.h` codec
with no adaptation needed.

### CALLBACK_API_RETURN_VALUE_SEMANTICS (Phase D0.5 addition)

Re-verified 2026-09-23, directly relevant to how `diag_callback_shim.h`
declares these two functions:

- **`diag_register_callback`'s return value**: re-disassembled its
  call site (`0x2360` in `diag_callback_sample`) specifically for
  return-value handling. `H41_STATICALLY_CONFIRMED`: the very next
  instruction (`0x2364: add x0, sp, 0xc`, setting up the following
  `diag_has_remote_device` call) overwrites `w0`/`x0` before the
  reference sample ever reads it. **This call's return value is never
  checked anywhere in the sample.**
- **`diag_switch_logging`'s return value**: same re-check at its call
  site (`0x23b4`). `H41_STATICALLY_CONFIRMED`: the following
  instruction (`0x23b8: ldrb w8, [x23, 0x318]`) reads an unrelated
  global/struct byte field, not `w0`. Also never checked.
- **Why**: `PUBLIC_REFERENCE_CONFIRMED` — fetched and read
  `mobile-insight/mobileinsight-mobile`'s `diag_revealer.c` (a real,
  independent, working public libdiag client), which declares
  `diag_switch_logging` via `dlsym` as: `void (*diag_switch_logging)(int,
  const char *);` — **void-returning**. This directly explains the
  disassembly finding (there is nothing to check) rather than merely
  being consistent with it. `diag_register_callback`'s own public
  signature was not found via an equally reliable direct quote in this
  pass (a web search surfaced a plausible-looking but unverifiable
  signature string, not sourced from an inspectable file — treated as
  unreliable, not cited); declared `void` in `diag_callback_shim.h` on
  the strength of the disassembly finding plus this project's own
  judgment that an adjacent registration-style function in the same
  library following the identical "call it, never check it" pattern is
  the more likely explanation than "the sample happens to ignore a
  real error code by coincidence, twice" — graded `PLAUSIBLE`, not
  `PUBLIC_REFERENCE_CONFIRMED`.
- **Consequence**: `IDiagCallbackShim::RegisterCallback`/`SwitchLogging`
  are declared to return `bool` for host-test fault-injection purposes,
  but the REAL `DiagCallbackShim` implementation can only ever report
  success for these two calls specifically (documented in
  `diag_callback_shim.h`) — there is no confirmed way to detect their
  failure on this firmware. `diag_callback_send_data` is unaffected:
  its `0=success` convention IS checked at all 6 call sites in the
  reference sample (`cbz w0, <success>`), independently confirmed twice
  now (original fork pass + this revision).

### REMOTE_DEVICE_ROUTING

Whether an EFS2 request might need to be routed to a remote/secondary
modem processor rather than the local one.

- **`H41_STATICALLY_CONFIRMED`**: a real, distinct local-vs-remote code
  path exists in this `libdiag.so` (`diag_has_remote_device`,
  `diag_register_remote_callback`, the string `"diag: wrote remote dev
  pkt"`, remote-processor-mask error strings) and is referenced by
  `diag_callback_sample`. It is reached only via explicit opt-in API
  calls — nothing in the plain `diagpkt_subsys_alloc`/`diagpkt_commit`
  path automatically routes to it.
- Whether guacamole's primary modem path ever needs this (i.e. whether
  `DIAG_IOCTL_REMOTE_DEV`'s `use_mdm` value is always 0 for this
  device) remains **`UNRESOLVED`** — noted already in
  `guacamole_efsd_design_report.md` as a real-device-only question.

### HELLO_REQUIRED

Whether a DIAG HELLO/feature-negotiation packet must precede EFS2
traffic.

- **`UNRESOLVED`**: no HELLO string or obvious HELLO-packet constant was
  found in any of the four analyzed binaries' string tables. This is
  not evidence that no HELLO exists anywhere in the diag stack — only
  that these specific sample programs' visible strings don't reference
  one. Left honestly unresolved rather than assumed either way.

### STAT_WIRE_LAYOUT

Request: NUL-terminated path string (variable length, cap 199 usable
chars in this probe). Response: 7×`int32` LE —
`{diag_errno, mode, size, nlink, atime, mtime, ctime}` (28 bytes).

- **`PUBLIC_REFERENCE_CONFIRMED`**: transcribed directly from
  `efsio.c`'s `efs_stat()`/`struct efs_filestat` and `EFS2_DIAG_STAT`
  usage (`strcpy(cmdbuf,filename); send_efs_cmd(EFS2_DIAG_STAT,...)`).
  Implemented in `efs_wire.h::BuildStatRequest`/`ParseStatResponse`,
  covered by `EfsWireEncoding.StatRequestExactBytes` and five
  `EfsWireParser.Stat*` tests (all passing, see
  `PHASE_D1_EFS2_READONLY_FINDINGS.md`).

### OPEN_WIRE_LAYOUT

Request: `{int32 oflag; int32 mode; char name[100];}` (112 bytes,
FIXED-size name field, NUL-padded). Response:
`{int32 fd; int32 diag_errno;}` (8 bytes).

- **`PUBLIC_REFERENCE_CONFIRMED`**: transcribed directly from
  `efsio.c`'s `efs_open()`. This probe additionally hard-rejects any
  `oflag != O_RDONLY(0)` at the request-construction layer — a Phase D0
  safety rule, not part of the public wire spec itself. Implemented in
  `BuildOpenReadOnlyRequest`/`ParseOpenResponse`, covered by
  `EfsWireEncoding.OpenReadOnlyRequestExactBytes` and four
  `EfsWireSafety.Open*RejectedBeforeTransport` tests.

### READ_WIRE_LAYOUT

Request: `{int32 fd; uint32 nbyte; uint32 offset;}` (12 bytes). Response:
`{int32 fd; uint32 offset; int32 bytes_read; int32 diag_errno; char
data[...];}` (16-byte fixed header + variable trailing data, capped at
2048 in the public reference; this probe additionally caps requests at
1024 bytes per the explicit Phase D0 instruction).

- **`PUBLIC_REFERENCE_CONFIRMED`**: transcribed directly from
  `efsio.c`'s `efs_read()`. Implemented in `BuildReadRequest`/
  `ParseReadResponse`, with Phase-D0-specific fail-closed additions
  beyond the reference client (which trusts `bytes_read` unconditionally
  and only refuses `bytes_read<=0`): this probe additionally rejects any
  response whose `fd`/`offset` don't match what the specific call
  asked for, any `bytes_read` exceeding the requested byte count, and
  any response whose claimed `bytes_read` exceeds the payload bytes
  actually present. Covered by `EfsWireParser.Read*` (9 tests) and
  `EfsReader.EfsErrorIsNotRetried`/`TimeoutIsNotRetried`.

### DIRECTORY_WIRE_LAYOUT

OPENDIR request: NUL-terminated path (variable). OPENDIR response:
`{uint32 dirp; int32 diag_errno;}` (8 bytes). READDIR request:
`{uint32 dirp; int32 seqno;}` (8 bytes). READDIR response: `{uint32
dirp; int32 seqno; int32 diag_errno; int32 entry_type; int32 mode; int32
size; int32 atime; int32 mtime; int32 ctime; char name[100];}` (136
bytes). CLOSEDIR request/response: a bare `int32 dirp` / `int32
diag_errno`.

- **`PUBLIC_REFERENCE_CONFIRMED`**: transcribed directly from
  `efsio.c`'s `efs_opendir()`/`efs_readdir()`/`efs_closedir()` and the
  public `efs_dirent` struct shape referenced there. This probe rejects
  a READDIR response whose fixed 100-byte name field has no NUL
  anywhere within it (fail-closed — the reference client trusts it
  unconditionally). Covered by `EfsWireEncoding.OpenDirRequestExactBytes`/
  `ReadDirRequestExactBytes`/`CloseDirRequestExactBytes` and
  `EfsWireParser.DirectoryEntry*`/`OpenDirSuccessRoundtrip`/
  `CloseDirSuccessRoundtrip`.

## What this means for Phase D0's implementation scope

The wire codec (`efs_wire.h/.cpp`) is complete and fully tested against
synthetic data for every read-only EFS2 command — it needed only the
`PUBLIC_REFERENCE_CONFIRMED` layer plus this project's own fail-closed
additions, and does not depend on any of the still-`UNRESOLVED` items
above.

The transport layer's `Init()`/`Deinit()` (`Diag_LSM_Init`/`Diag_LSM_DeInit`)
are now `H41_DEVICE_CONFIRMED` via the real `transport-info` run above.
Phase D0.5 implemented and host-tested `DiagTransport::SendSubsysCommand()`
— the full callback-mode request/response round trip, built on
`CLIENT_ORIGINATED_SEND_PATH`. Two same-day follow-up reviews found and
this project fixed FOUR real issues before any device execution of it:
a dangling callback-context pointer, a non-atomic
`MAX_OUTSTANDING_REQUESTS=1` reservation, a missing response-correlation
check (`RESPONSE_CORRELATION` above - `OnCallback()` originally accepted
ANY valid-length callback as a request's answer), and - discovered only
after fixing the second issue - a ThreadSanitizer lock-order-inversion
FALSE POSITIVE (not a code bug) resolved by switching the reservation
from a mutex to a plain atomic. Current state: **102/102 host tests
passing** (35 in `DiagTransport`, 6 in `CallbackSmoke`, 9 in a new
`StatMcfg` suite), plus 18 clean ThreadSanitizer runs total (zero
warnings, including zero real data races reported at any point) across
the concurrency-sensitive subset.

`callback-smoke` was RUN ON THE REAL DEVICE and **PASSED**
(`H41_DEVICE_CONFIRMED` - see `LOOPBACK_ROUNDTRIP_DEVICE_RESULT` above)
- the full callback-mode request/response transport is now confirmed
end-to-end against a real modem, not just a fake shim. That same run
found and this project fixed a real bug: `Deinit()` no longer calls
`SwitchLogging(0)` (H.41 rejects mode 0 as invalid - confirmed both on
the real device and via the public kernel source).

`stat-mcfg` - the first real EFS2 operation, a single STAT against the
fixed `/nv/item_files/mcfg` target - is now implemented and host-tested
(`StatMcfg` suite) on top of the now-device-confirmed transport, but has
NOT been run on an actual device as of this revision - that remains a
separate, explicit go-ahead this document's companion completion report
is requesting. `EFS2_SUBSYSTEM_ID` (`0x13`) is
`QUALCOMM_PROTOCOL_CONFIRMED`/`H41_DEVICE_NOT_YET_CONFIRMED` (see that
section above) - running `stat-mcfg` for real is exactly what would
settle the device-confirmation half. The `mask_request_validate()` risk
flagged earlier is no longer treated as a blocker for the transport
layer itself: the real `callback-smoke` run sent `subsys=0x12,
cmd=0x0029` - outside that whitelist's allowed set for `0x12` - and
still reached the modem and got a real response, with no `"mask
request Invalid"` in dmesg/logcat. Whether `0x13`/`cmd=15` (STAT) is
itself accepted by the modem's EFS2 service is precisely what
`stat-mcfg`'s first real run would determine - not further guessable
from static analysis. `diag_register_callback`'s and
`diag_switch_logging`'s REAL on-device runtime behavior is now
`H41_DEVICE_CONFIRMED` via that same `callback-smoke` run - no longer
`NOT_TESTED`.
