# Device EFS2 read-only test procedure (Phase D0/D1/Cycle 1)

**`transport-info`: DONE, PASS (2026-09-23). Do not re-run.** Real
result: SELinux `Enforcing`, `u:r:su:s0`/uid=0, `/dev/diag` type
`vendor_diag_device` group `vendor_qti_diag`(2901), `Diag_LSM_Init`/
`Diag_LSM_DeInit` both succeeded with a clean read-thread
create/signal/join lifecycle, `EXIT=0`, zero EFS2 packets sent.

**`callback-smoke`: DONE, PASS (2026-09-23). Do not re-run.** Real
result: `transport_status=OK`, `response_len=19`, `header_match=true`,
`EXIT=0` - a genuine callback-mode request/response round trip with the
real modem, confirming the whole transport layer end-to-end for the
first time. That run also found a real bug (`Deinit()`'s
`SwitchLogging(0)` call failed with EINVAL - H.41 has no valid mode 0),
now fixed. See `PHASE_D0_EFS2_RESEARCH.md`'s "Real-device result" and
`LOOPBACK_ROUNDTRIP_DEVICE_RESULT` sections for full write-ups and
confirmation-level tables. Steps 1-4 below are kept as a record of what
was run and its exact expected output, not as something to repeat.

**`snapshot-mcfg-readonly` (Cycle 1): FIRST REAL ATTEMPT RUN
(2026-09-23) - a partial success that CONFIRMED the transport and
STAT command both work, but surfaced a real parser bug, now fixed.**
The real STAT response for `/nv/item_files/mcfg` arrived correctly
(`transport_status=OK`, `header_match=true`) but was 35 raw bytes, not
the 32 the parser expected - `ParseStatResponse` correctly rejected it
as malformed, because it WAS malformed relative to the bare 32-byte
STAT contract. Root cause: H.41's real callback responses carry a
trailing HDLC-style `CRC-16/X-25 + 0x7e` frame terminator
(`H41_DEVICE_CONFIRMED` - independently reproduced against BOTH the
real STAT response and the earlier real `callback-smoke` response; see
`diag_frame.h` for the full evidence) that nothing was stripping before
handing bytes to the EFS2 parsers. This is now fixed CENTRALLY in
`DiagTransport::SendSubsysCommand()` (see `diag_frame.h`/`.cpp`) - no
per-parser workaround. The real, now-confirmed logical STAT result:
`diag_errno=0`, `mode=0x000041ff` (directory, 0777), `size=36`,
`nlink=2` - `/nv/item_files/mcfg` is a real, existing directory on this
device. Status:
```
EFS2_SUBSYSTEM_0x13 = H41_DEVICE_CONFIRMED
EFS2_STAT           = H41_DEVICE_CONFIRMED
HDLC_TRAILER_FRAMING = H41_DEVICE_CONFIRMED (2 independent responses)
```
This is now implemented + host-tested (174 tests, normal/ASAN/UBSAN/TSAN
clean - see `PHASE_D1_EFS2_READONLY_FINDINGS.md`) with the framing fix,
but NOT YET re-run on the real device. This remains the ONLY further
real-device command this document authorizes requesting authorization
for, and it is still a SINGLE integrated run - see Step 5. There is
still no separate "run `stat-mcfg` first, then come back for the rest"
procedure: `snapshot-mcfg-readonly`'s own STEP 1 performs exactly that
STAT, and every later step in the same process only runs if STEP 1
succeeded.

## What this phase actually authorizes running

**`transport-info` and `callback-smoke`** (both already done, PASS, do
not re-run). **`snapshot-mcfg-readonly`** is implemented and host-tested
with the framing fix but requires separate, explicit go-ahead before
Step 5 below is RE-run - that authorization is what this document's
companion completion report is requesting. `stat-mcfg` remains
implemented and host-tested but is superseded by `snapshot-mcfg-readonly`
(whose STEP 1 performs the exact
same STAT) - there is no reason to run `stat-mcfg` on its own anymore.
Every `NOT_IMPLEMENTED_THIS_PHASE` CLI subcommand (`list-mcfg`,
`read-sel-db`, `read-autoselect`, `read-rfs-digest`, `list-cards`,
`read-debug-log`, `read-debug-old-log`, `read-efslogging-flag`) prints
that message and performs no DIAG I/O - running them accomplishes
nothing beyond confirming it; `snapshot-mcfg-readonly` is the fixed,
integrated equivalent of all of them combined, and is the only one of
this whole group that actually sends anything.

## Step 1 — build

```
cd ~/android/pixelos
source build/envsetup.sh
lunch lineage_guacamole-userdebug
m guacamole_efs_probe
```

## Step 2 — push to `/vendor/bin`, not `/data/local/tmp`

`guacamole_efs_probe` is `vendor: true` and links directly against the
private vendor library `libdiag.so`. Per the Android linker namespace
model (https://source.android.com/docs/core/architecture/vndk/linker-namespace),
namespace configuration is determined by the executable's own path —
only a binary actually running from `/vendor/bin` gets the vendor
namespace needed to resolve `libdiag.so`. Running it from
`/data/local/tmp` instead risks an unrelated library-resolution failure
that has nothing to do with the DIAG transport itself, muddying any
result. Use:

```bash
adb root
adb remount

adb push \
  out/target/product/guacamole/vendor/bin/guacamole_efs_probe \
  /vendor/bin/guacamole_efs_probe

adb shell chmod 0755 /vendor/bin/guacamole_efs_probe
adb shell restorecon /vendor/bin/guacamole_efs_probe
```

Confirm the pushed binary is byte-identical to what was built:

```bash
sha256sum out/target/product/guacamole/vendor/bin/guacamole_efs_probe
adb shell sha256sum /vendor/bin/guacamole_efs_probe
```

Confirm SELinux is still `Enforcing` before running anything — this
probe is not running under any dedicated domain yet (see
`PHASE_D1_EFS2_READONLY_FINDINGS.md`'s SELinux/AID note), so testing
under `Enforcing` is what actually tells you whether a real product
build would need one:

```bash
adb shell getenforce
```

## Step 3 — run `transport-info` (historical - DONE, PASS, do not re-run)

```bash
adb shell '/vendor/bin/guacamole_efs_probe transport-info; echo EXIT=$?' \
  |& tee efs-transport-info.txt
```

Expected output on success:

```
guacamole_efs_probe transport-info
Calling Diag_LSM_Init(nullptr)...
RESULT: Diag_LSM_Init succeeded.
Calling Diag_LSM_DeInit()...
RESULT: OK - init/deinit round trip completed. No EFS2 or any other subsystem packets were sent.
```

Exit code 0 on success, 1 if `Diag_LSM_Init` itself fails (prints the
failure reason instead), 2 for an unrecognized/missing command.

## Step 4 — `callback-smoke` (historical - DONE, PASS, do not re-run)

```bash
adb shell '/vendor/bin/guacamole_efs_probe callback-smoke; echo EXIT=$?' \
  |& tee efs-callback-smoke.txt
```

Actual real-device output (2026-09-23):

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

No `"mask request Invalid"` appeared in dmesg/logcat - the
kernel-level whitelist risk flagged in an earlier revision does not
block this send path (see `PHASE_D1_EFS2_READONLY_FINDINGS.md`).
Terminal `Deinit()` originally also called `SwitchLogging(0)`, which
FAILED on the real device (`"unable to switch logging mode to 0, err:
-1, errno: 22"`, kernel: `"request to switch to invalid mode: 0"`) -
this did not prevent `Diag_LSM_DeInit()` from completing cleanly, and
the code no longer makes that call at all (see
`PHASE_D0_EFS2_RESEARCH.md`'s `LOOPBACK_ROUNDTRIP_DEVICE_RESULT`
section for the full record).

## Step 4.5 — first real `snapshot-mcfg-readonly` attempt (historical, 2026-09-23, partial - DO NOT treat as a template to repeat, see Step 5)

This ran BEFORE the framing fix below existed. STEP 1's actual captured
output (fields as the pre-fix binary printed them - `response_hex` here
is what later became `raw_response_hex`):

```
=== STEP: STAT /nv/item_files/mcfg ===
target=/nv/item_files/mcfg
request_hex=4b130f002f6e762f6974656d5f66696c65732f6d63666700
transport_status=OK
response_len=35
response_hex=4b130f0000000000ff4100002400000002000000803dd512803dd512803dd51256677e
header_match=true
efs_status=MALFORMED
```

`transport_status=OK` and `header_match=true` PROVED the transport and
STAT command both work end-to-end for the first time
(`H41_DEVICE_CONFIRMED` for both `EFS2_SUBSYSTEM_0x13` and
`EFS2_STAT`). `efs_status=MALFORMED` was the parser correctly rejecting
35 bytes against its 32-byte STAT contract - not a parser bug, a
FRAMING bug: the real response carries a trailing HDLC-style
`CRC-16/X-25 (little-endian) + 0x7e` terminator
(`H41_DEVICE_CONFIRMED`, cross-checked against the earlier real
`callback-smoke` response's own trailing `0c 30 7e` = CRC(payload) =
0x300c - two independent real responses, same framing). The first 32
bytes decode as a fully valid STAT response: `diag_errno=0`,
`mode=0x000041ff` (directory, 0777), `size=36`, `nlink=2` -
`/nv/item_files/mcfg` is confirmed to really exist. See `diag_frame.h`
for the full evidence and the fix, now applied centrally in
`DiagTransport::SendSubsysCommand()`.

## Step 4.6 — second real `snapshot-mcfg-readonly` attempt (historical, 2026-09-23, further progress)

With the trailing-CRC fix from Step 4.5 applied, this run went further:
STAT succeeded fully, OPENDIR succeeded, and READDIR successfully
decoded TWO real directory entries before hitting a second, DIFFERENT
framing bug:

```
READDIR seq=1: mcfg_autoselect_by_uim
READDIR seq=2: mcfg_autoselect_by_uim_Subscription01
READDIR seq=3: (decode failed - see below)
```

**Finding worth its own record**: `mcfg_autoselect_by_uim_Subscription01`
existing as a REAL live EFS entry on this device is directly relevant
to Cycle 2's dual-SIM design - a second-subscription autoselect file
already exists on-device, not merely theorized.

The seq=3 decode failure was NOT a corrupted packet - it was a SECOND
framing bug, now also fixed: real H.41 responses use classic HDLC
byte-stuffing (`0x7d 0x5d` -> literal `0x7d`; `0x7d 0x5e` -> literal
`0x7e`) across the ENTIRE stuffed region (payload AND its trailing
CRC), and the Step 4.5 fix incorrectly split the raw trailing 3 bytes
into crc_lo/crc_hi/terminator BEFORE unescaping - which happened to
work for the first two real responses (neither needed any byte
escaped) but broke on this one, whose CRC's own low byte was `0x7d`
and needed unescaping first. The real raw seq=3 response (71 bytes,
trailing `7d 5d 35 7e`) unescapes to a 69-byte body whose real
little-endian CRC is `0x357d` - which matches CRC-16/X-25 of the
67-byte logical payload EXACTLY. Decoded: `dirp=1`, `seqno=3`,
`diag_errno=0`, entry name `mcfg_def_config_hw_version`. See
`diag_frame.h` for the full evidence and the corrected decode order
(unescape THEN split CRC, never the reverse) - now applied the same
way, still centrally, still in `DiagTransport::SendSubsysCommand()`.
Status now confirmed:

```
EFS2_STAT                    = H41_DEVICE_CONFIRMED
EFS2_OPENDIR                 = H41_DEVICE_CONFIRMED
EFS2_READDIR                 = H41_DEVICE_CONFIRMED
HDLC_TRAILER_FRAMING         = H41_DEVICE_CONFIRMED
HDLC_BYTE_STUFFING (7d5d/7d5e) = H41_DEVICE_CONFIRMED
```

No SELinux denial or `"mask request Invalid"` evidence was found in
dmesg/logcat from either real attempt.

Every wire call from here on reports BOTH `raw_response_hex`
(unchanged, still framed/stuffed) and `logical_response_hex`
(de-framed AND unescaped - what the parser actually consumed), plus
`hdlc_escape_present`/`hdlc_escape_count`/`unescaped_frame_len`.

## Step 4.7 — third real `snapshot-mcfg-readonly` attempt (historical, 2026-09-23, Cycle 1 nearly closed)

With both framing fixes from Steps 4.5/4.6 applied, this run went all
the way through STAT, OPENDIR, and a FULL real directory enumeration:
34 real entries (seq 1-34), then correctly hit a THIRD, different bug -
not a framing bug this time, a semantic one. The real device's READDIR
end-of-directory signal for `/nv/item_files/mcfg` turned out to be a
**zero-metadata sentinel entry** (`diag_errno=0`, every entry field
zeroed, empty name) - NOT the `kEfsErrDirEntryNotFound` errno this
codebase had assumed. Real seq=35 response (decoded): `dirp=2,
seqno=35, diag_errno=0, entry_type=0, mode=0, size=0, atime=0,
mtime=0, ctime=0, name=""`, with a valid CRC. Without recognizing this
shape, the old code kept incrementing seqno and re-requesting,
receiving the same zero sentinel repeatedly until the `kMaxDirectoryEntries`
(4096) safety cap correctly tripped - the cap worked exactly as
designed (no infinite loop, `CLOSEDIR` still succeeded afterward), but
the listing should have stopped at seq 35, not 4096. Fixed with
`IsZeroSentinelEndOfDirectory()` in `efs_dir_lister.cpp` - see that
file's header comment for the full evidence. Status now confirmed:

```
EFS2 STAT/OPENDIR/READDIR/CLOSEDIR = H41_DEVICE_CONFIRMED
HDLC CRC-16/X-25 + byte-stuffing (7d5d, 7d5e), incl. within
  payload bytes (seqno values 0x7d/0x7e observed escaped
  and correctly restored) = H41_DEVICE_CONFIRMED
READDIR first seq=1                = H41_DEVICE_CONFIRMED
READDIR zero-metadata EOD sentinel = H41_DEVICE_CONFIRMED
READDIR kEfsErrDirEntryNotFound EOD = PUBLIC_REFERENCE_SUPPORTED,
  H41_NOT_OBSERVED (kept as a secondary check, never relied upon)
```

**The real `/nv/item_files/mcfg` directory baseline** (34 entries,
non-exhaustive names below; full list in the completion report):
`mcfg_sel_db.xml` (REAL DEVICE CONFIRMED - present as a live entry),
`mcfg_autoselect_by_uim` and `mcfg_autoselect_by_uim_Subscription01`
(REAL DEVICE CONFIRMED - a second-subscription autoselect file already
exists on this device, directly relevant to Cycle 2's dual-SIM
design), plus multiple `mcfg_def_config_*`, `mcfg_default_*_digest_version`,
`mcfg_setting*`, and `mcfg_sw_muxd_version*` entries, several with
their own `_Subscription01` variants. STAT on the directory itself
reported `mode=0x41ff, size=36, nlink=2` - any relationship between
`size=36` and "34 real entries + `.` + `..`" is an interesting
observation, not a confirmed protocol fact, and is not relied upon
anywhere in this codebase.

No SELinux denial or `"mask request Invalid"` evidence was found in
dmesg/logcat from any of the three real attempts. This run's process
entered callback logging and completed `Diag_LSM_DeInit()` cleanly
roughly 6 seconds later.

## Step 5 (READY, awaiting the operator's own go-ahead) — `snapshot-mcfg-readonly`, the ONE integrated run (RE-run after the EOD-sentinel fix + the scope expansion below)

**Do not run this until you're ready - it needs no further ChatGPT
review round after this document, since it is a fixed-path read-only
scope expansion, not a new transport/protocol primitive (see the
completion report's item 7 reasoning); it still needs YOUR own
go-ahead to actually execute, same as every real-device step in this
project.** Recorded here so the exact steps are already written down.
This is intentionally ONE command, run ONCE, covering everything Cycle
1 built - there is no "run STAT first, evaluate, come back for the
directory listing" pattern here; the command performs its own fixed
14-step sequence internally (expanded 2026-09-23 from 9 steps, once
the third real attempt's real directory enumeration confirmed 5
additional dual-SIM/state file names - see efs_targets.h):
STAT+list `/nv/item_files/mcfg` → read `mcfg_sel_db.xml`,
`mcfg_autoselect_by_uim`, `mcfg_autoselect_by_uim_Subscription01`,
`mcfg_setting`, `mcfg_setting_1`, `mcfg_setting_Subscription01`,
`mcfg_setting_Subscription01_1`, `mcfg_rfs_sw_digest_version` → list
`cards/` → read `/mcfg_debug.log`, `/mcfg_debug_old.log`,
`/enable_mcfg_efslogging` - and stops on its own at the first
fail-fast condition (see `include/efs_snapshot.h`) - there is nothing
for the operator to decide or re-invoke between steps. This cycle
reads ONLY raw bytes/size/SHA-256 for the 5 newly-added targets - no
interpretation of `mcfg_setting*`'s content/schema happens anywhere in
this codebase; that is explicitly deferred to Cycle 2.

```bash
adb shell '/vendor/bin/guacamole_efs_probe snapshot-mcfg-readonly; echo EXIT=$?' \
  |& tee efs-snapshot-mcfg-readonly.txt
```

Expected output shape (one block per step actually attempted - fewer
than 14 blocks if a fail-fast condition stopped the sequence early;
`EXIT=0` if all 14 steps completed, whether `outcome=DONE` or
`outcome=DONE_WITH_WARNINGS`):

```
guacamole_efs_probe snapshot-mcfg-readonly
=== STEP: STAT /nv/item_files/mcfg ===
target=/nv/item_files/mcfg
--- wire: STAT ---
request_hex=<hex>
transport_status=OK
raw_response_len=<N>
raw_response_hex=<hex, still carries the trailing HDLC-stuffed CRC+0x7e>
hdlc_trailer_present=true
hdlc_escape_present=<true if any 0x7d 0x5d/0x5e pair was decoded>
hdlc_escape_count=<N, 0 if hdlc_escape_present=false>
unescaped_frame_len=<len after unescape, before splitting off the CRC>
crc_expected=0x<hex>
crc_received=0x<hex>
crc_valid=true
logical_response_len=<unescaped_frame_len - 2>
logical_response_hex=<hex, de-framed AND unescaped - what efs_status/diag_errno below reflect>
header_match=true
efs_status=OK
diag_errno=0
presence=EXISTS
directory_entry_seen=false
mode=<value>
size=<value>
nlink=<value>
atime=<value>
mtime=<value>
ctime=<value>

=== STEP: OPENDIR/READDIR /nv/item_files/mcfg ===
target=/nv/item_files/mcfg
--- wire: OPENDIR ---
...
--- wire: READDIR ---
...
--- wire: CLOSEDIR ---
...
presence=EXISTS
directory_entry_seen=false
entry_index=0
entry_name=mcfg_sel_db.xml
entry_type=<value>
entry_size=<value>
...

=== STEP: STAT+READ mcfg_sel_db.xml ===
target=/nv/item_files/mcfg/mcfg_sel_db.xml
--- wire: STAT ---
...
--- wire: OPEN ---
...
--- wire: READ ---
...
--- wire: CLOSE ---
...
presence=EXISTS
directory_entry_seen=true
mode=<value>
size=<value>
nlink=<value>
atime=<value>
mtime=<value>
ctime=<value>
expected_size=<value>
bytes_read=<value>
sha256=<hex>
data_hex_truncated=false
data_hex=<hex>

... (STEPS 4-9 in the same shape - mcfg_autoselect_by_uim,
mcfg_rfs_sw_digest_version, the cards/ directory, /mcfg_debug.log,
/mcfg_debug_old.log, /enable_mcfg_efslogging - the 2 debug logs may
instead show `data_hex_truncated=true` with a bounded preview if their
content exceeds 256 bytes; see `src/main.cpp`'s
`kDebugLogHexPreviewBytes` - the full raw bytes are always preserved
internally regardless, only the printed preview is bounded)

outcome=DONE
EXIT=0
```

**Read this carefully before judging the result** (this section was
corrected 2026-09-23, in a preflight review that ran BEFORE this
command's first real-device use - see the Cycle 1 preflight correction
report for the full reasoning; the corrections below are current, not
speculative):

- `header_match=true` for a wire call means a correctly-correlated
  response arrived — it does NOT by itself mean that particular EFS2
  operation succeeded, or even that its payload was parsed at all.
  Every wire call now reports BOTH `raw_response_hex` (exactly what the
  callback delivered - still carries the trailing HDLC CRC+terminator
  if `hdlc_trailer_present=true`) and `logical_response_hex` (de-framed
  - what `efs_status`/`diag_errno` actually reflect); see `diag_frame.h`
  for why the split exists (2026-09-23 real-device finding). Check that
  call's `efs_status`:
  - `efs_status=NOT_PARSED` - no correlated response ever arrived at
    all (timeout, cancellation, session desync, OR a CRC mismatch -
    `transport_status=FRAME_INTEGRITY_FAILED`, see below) - the payload
    parser was never invoked; `logical_response_hex` will be empty.
    `raw_response_hex` may still be populated (a CRC mismatch DOES
    deliver raw bytes, they just fail integrity) - report it for
    analysis.
  - `efs_status=MALFORMED` - a response DID arrive, de-framed cleanly
    (`crc_valid=true` if `hdlc_trailer_present=true`), but the payload
    parser rejected the LOGICAL bytes (wrong length, no NUL in a name
    field, etc.) - report `logical_response_hex` for analysis.
  - `efs_status=EFS_ERROR` with a nonzero `diag_errno` - the response
    decoded fine, but the modem's EFS2 layer reported an error for
    THAT specific call. For a READDIR call specifically,
    `diag_errno=0x40000004` (`kEfsErrDirEntryNotFound`) is currently
    the only value this code treats as normal end-of-directory - any
    other nonzero READDIR errno fails the whole directory listing
    closed (see `include/efs_wire.h`'s `kEfsErr*` constants and
    `efs_dir_lister.cpp`).
  - `efs_status=OK` - full success for that specific wire call.
- `transport_status=FRAME_INTEGRITY_FAILED` (`crc_valid=false`) means a
  correlated callback arrived but its trailing CRC-16/X-25 didn't match
  its own payload - a genuine transport integrity failure, not an
  application-level EFS2 error. This poisons the session exactly like a
  timeout (no automatic retry) and stops the whole snapshot - report
  both `raw_response_hex` and the `crc_expected`/`crc_received` values
  for analysis; this has never been observed on this device (both real
  responses so far had valid CRCs), so seeing it at all would itself be
  a notable finding.
- `presence=UNKNOWN` on ANY target - mandatory or optional - means
  STAT returned a structurally valid but nonzero `diag_errno`. This is
  **NOT** a confirmed "file doesn't exist" - this project has no
  confirmed EFS2 "no such entry" errno value yet (qtools' own
  `efs_stat()` doesn't distinguish "not found" from "some other error"
  either). For an OPTIONAL target this continues the snapshot as a
  WARNING (`outcome=DONE_WITH_WARNINGS`); for the MANDATORY
  `/nv/item_files/mcfg` target (STEP 1) it still aborts the entire
  snapshot (`outcome=FAILED`), worded as "mandatory STAT EFS error,
  diag_errno=N" - never "mandatory target is absent". `presence=ABSENT`
  is reserved for a future cycle, once a specific "no such entry" errno
  is confirmed on this real device - nothing in this command sets it
  today, so seeing it at all on a real run would itself be worth
  reporting as a surprise.
- `directory_entry_seen=true` alongside `presence=UNKNOWN` for one of
  the 4 targets under `/nv/item_files/mcfg` (sel_db.xml, autoselect,
  rfs_sw_digest_version, cards - NOT the 2 debug logs or the
  efslogging flag, which are outside that directory and always print
  `directory_entry_seen=false`) is a genuine, DELIBERATELY PRESERVED
  contradiction: STEP 2's own directory listing saw that name, but that
  target's own STAT call independently reported an error. Do not
  resolve this contradiction by hand when reading the log - report it
  exactly as printed; Cycle 2 needs both signals intact.
- `outcome=FAILED` with a `fail_reason=` line means a fail-fast
  condition stopped the sequence: a transport failure, a timeout, a
  session-desynchronization, a malformed response, an invalid handle, a
  non-progressing directory enumeration, a genuine EFS2 protocol error
  on a READDIR call, or a STAT error on the MANDATORY target. There is
  no automatic retry — a single `outcome=FAILED` result is the complete
  result; do not re-run speculatively hoping for a different outcome on
  the same invocation.
- `outcome=DONE_WITH_WARNINGS` means all 14 steps completed but at least
  one optional target's STAT returned an EFS error (`presence=UNKNOWN`
  somewhere in the log) - this is a successful run for exit-code
  purposes (exit 0), just one whose per-step detail is worth reading
  before treating it as a clean baseline.
- `transport_status=TIMEOUT` on any individual wire call does NOT
  distinguish "no service at that command" from "wrong subsys id" from
  "kernel/driver rejected it silently" - capture dmesg/logcat per below
  regardless.

Whether it finishes with `outcome=DONE`, `outcome=DONE_WITH_WARNINGS`,
or `outcome=FAILED`, also capture:

```bash
adb shell dmesg | grep -iE "avc|diag|mask request Invalid"
adb logcat -b all -d | grep -iE "avc|diag|mask request Invalid"
```

No automatic retry anywhere in this command — it performs its fixed
14-step sequence exactly once and reports exactly what happened. Do not
re-run it speculatively; one clean result (`outcome=DONE` or a single,
fully-captured `outcome=FAILED`) is the goal, matching this whole
project's fail-closed, no-retry design philosophy.
