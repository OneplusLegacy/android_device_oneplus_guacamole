# Phase C0 findings — SKT-only PDC Load Config

Builds on Phase A/B/B2 (see PHASE_B_FINDINGS.md) and the real device
results already obtained there: `list` succeeded (25 SOFTWARE configs
enumerated), `info` succeeded, and the tool built on PixelOS ran
correctly on the real LineageOS 23.2 guacamole device (byte-identical
`libqmi_cci.so`/`libqmiservices.so` confirmed via SHA-256 against the
H.41 blobs this whole project's research is based on).

## 1. Config ID generation scheme — the spec's stated blocking condition

Per the Phase C0 instructions: *"config ID 생성 규칙이 불확실하면 C0 write
구현을 중단한다"* (if the config ID generation rule is uncertain, halt
the C0 write implementation).

**Verdict: CONFIRMED, not uncertain.** Two independent lines of
evidence converge exactly on the same answer (full detail in
`research_pdc_load_config_id_scheme.md`):

1. libqmi's own public, production reference CLI client
   (`qmicli-pdc.c`) computes `SHA-1` over the whole mapped file and
   sends that digest as the Config Chunk's `Id` field, identically on
   every chunk.
2. **Empirically verified against this exact real device**: the real
   Common-Commercial config's actual reported id from a real `info` run
   (`0bd71c00fc30d2ff8af86a8b16d1e01f69e6b493`) is the exact,
   byte-for-byte SHA-1 of a real 24508-byte `.mbn` file found in the
   H.41 extraction (`generic/oppo_exp/common/commerci/mcfg_sw.mbn`) -
   not a coincidence, not an inference, a direct match against live
   device output.

H.41 disassembly of `qcril_qmi_pdc_load_config_file` could NOT
independently confirm the algorithm internally (no crypto library
dependency was found in `libril-qc-hal-qmi.so`, and logging boilerplate
obscured the relevant loop) - this is recorded honestly as UNRESOLVED
in the research file, but does not weaken the headline verdict, since
the empirical real-device match is a stronger evidence class than a
disassembly that was never completed.

**This blocking condition is satisfied. Phase C0 write code was NOT
halted, and this is the correct call given the evidence.**

## 2. Chunk protocol - PLAUSIBLE, not fully disassembly-confirmed

- Chunk size 1024 bytes: taken from libqmi's public reference
  (`LOAD_CONFIG_CHUNK_SIZE`), not independently confirmed against H.41's
  own qcrild chunking loop. This is a conservative, safe default (well
  within the wire format's own guint16 chunk-length ceiling and any
  realistic QRTR/QMUX transport MTU), not a guess invented for this
  project.
- `Total Size` resent on every chunk, no per-chunk offset field on the
  wire (offset is inferred from cumulative bytes sent and cross-checked
  against the modem's own `Received`/`Remaining Size` fields) - both
  confirmed from the public reference implementation's actual behavior.

## 3. Safety design compliance with the Phase C0 spec

Every explicit prohibition in the spec is enforced by the absence of
the corresponding code path, not by a runtime flag:

| Prohibition | Status |
|---|---|
| Set Selected Config | No such function exists anywhere in this codebase. |
| Activate/Deactivate Config | Same. |
| Delete Config | Same. |
| EFS2 WRITE of any kind | Same - this codebase has no EFS2 code at all, only PDC. |
| `mcfg_sel_db.xml` / `mcfg_autoselect_by_uim` / `mcfg_rfs_sw_digest_version` / `/nv/item_files/mcfg/cards` | Untouched - not even referenced. |
| Modem restart / auto reboot | No such call anywhere. `load-skt`'s success output explicitly instructs a MANUAL reboot. |
| Deleting an existing config on "failure" | No rollback-via-delete path exists; a failure just reports FAILED and leaves all existing PDC state untouched. |
| Arbitrary external file path input | `load-skt` takes no path argument at all - `known_profiles.h` is the only source of the file path, hardcoded. |

## 4. Test coverage

**Current total: 70 host tests, all passing** (`atest guacamole_pdc_probe_test`
/ direct g++ + real AOSP googletest run - see `VERIFICATION.md`). Broken
down:

- 46 pre-existing Phase A/B/B2 host tests (`QmiTlv`, `PdcWire`,
  `PdcWireSelected`, `OperationResult`) - unaffected by Phase C0.
- 6 `Sha1`/`Sha256` tests against FIPS 180-4 standard vectors (generated
  via this host's own `sha1sum`/`sha256sum`, not hand-transcribed - an
  earlier hand-transcription attempt had two typos, caught and fixed by
  regenerating programmatically).
- 18 `SktLoad` state-machine tests against a fake `IPdcClient`. Two
  generations, both reflected in the current 18:
  - **Received/Remaining-Size generation** (from the fix after
    real-device test #1, §5): one chunk, multiple chunks, the exact
    real 41172-byte/41-chunk case
    (`RealStockSktSizeAllFortyOneChunksSucceed`), exact boundary chunk
    size, short final chunk, `Received` absent-but-valid-Remaining
    succeeds, `Received` wrong-for-current-chunk fails, `Remaining Size`
    wrong fails, final-chunk-nonzero-remaining fails, modem reject at
    first/middle chunk, timeout (not retried; resolved via
    authoritative Get Config Info re-check, both "exists" and "absent"
    outcomes), malformed indication, Frame-Reset-true-is-failure,
    selection-changed-is-failure, selected-after-not-queried-shows-
    NOT_QUERIED, stock-file-identity-mismatch-fails-closed-zero-calls.
  - **GetConfigInfo-authoritative-presence generation** (from the
    hardening after real-device test #2, §6-§7 below), REPLACING two
    tests whose premise real-device test #2 proved wrong:
    `PreexistingConfigFoundByGetInfoEvenWhenListOmitsItSkipsLoad`
    (replaces the old `AlreadyExistsSkipsLoadEntirely`, now explicitly
    with List Configs omitting the target while Get Config Info still
    finds it), `PreloadGetInfoInvalidIdAllowsLoad`,
    `PreloadGetInfoUnexpectedFailureFailsClosed`,
    `PostLoadListOmissionButGetInfoSuccessStillVerifies`,
    `PostLoadListPresentButGetInfoFailureDoesNotVerify` (**this
    directly REPLACES and INVERTS the old, now-known-incorrect
    `ListVerifySucceedsButInfoFailsIsStillDone` test** - the old test's
    premise, that List Configs presence plus a failed Get Config Info
    should still be `DONE`, was disproven by real-device test #2 and
    would have been actively dangerous to keep as a passing
    "specification"), `PostMortemGetInfoRunsEvenWhenListOmitsTarget`
    (replaces `PostMortemVerificationRunsAfterMidTransferFailure`, now
    asserting the post-mortem step calls Get Config Info directly, not
    just List Configs). The old `ListVerifyDoesNotFindNewIdFails` test
    was removed outright - its premise (a List Configs miss alone
    should fail the whole load) is exactly what real-device test #2
    disproved.
- **Not independently testable at this level** (noted honestly, not
  glossed over): true "duplicate indication" and "late indication" race
  conditions are properties of `PdcClient::OnIndication`/
  `WaitForIndication`'s map-based, single-shot-wait design (established
  in Phase B, unchanged here) - simulating a genuine concurrent
  double-delivery race requires either a live transport or a
  lower-level threading test harness beyond a synchronous call-based
  fake's reach. The design itself (copy-then-notify in the callback,
  last-value-wins in the pending map, no work done inside the callback)
  is documented and was reviewed in Phase B, but this specific race is
  not covered by a dedicated automated test.

Verified linkable and runnable with ZERO dependency on the real vendor
`.so` files (matches the real `cc_test_host` Android.bp target's actual
constraint - a build host has no `libqmi_cci.so`/`libqmiservices.so` to
link against at all).

## 5. Real-device test #1 — `qmi_client_init_instance` and Load Config both work; a state-machine bug aborted the transfer

The user ran `load-skt` for real. Result: init succeeded, DISCOVER
succeeded (List Configs returned the expected baseline), chunk #1 (1024
bytes) was accepted cleanly by the modem, and chunk #2 (offset 1024,
1024 bytes) was also accepted by the modem - but this implementation
reported `LOAD_CHUNKS failed: ... modem-reported Received (1024) does
not match expected cumulative (2048)` and aborted.

**Root cause: a bug in this codebase's interpretation of the `Received`
TLV, not a modem/protocol problem.** The original implementation
treated `Received` as a required, cumulative running total. Two
independent pieces of evidence, gathered after the fact, show this was
wrong:

1. **The real modem's actual behavior**: it reported `Received=1024` on
   chunk #2 as well - i.e. *this chunk's own size*, not a cumulative
   count.
2. **libqmi's public reference client** (`qmicli-pdc.c`,
   `load_config_ready_indication()`, fetched and read directly -
   `https://raw.githubusercontent.com/linux-mobile-broadband/libqmi/main/src/qmicli/qmicli-pdc.c`):
   **never reads "Received" at all.** Progress control is driven
   exclusively by `Remaining Size` (loop until it reaches 0) and
   `Frame Reset` (abort immediately, no auto-resend). The function's
   full logic, byte for byte: check `Indication Result` != 0 → error;
   check `Frame Reset` == true → error; read `Remaining Size` (required
   - error if absent); if 0 → done; else build and send the next chunk.

**Fix applied** (see `src/skt_load.cpp`): `Received`, when present, is
now checked only against *this chunk's own length* (advisory - its
absence is no longer a failure at all, matching that the public schema
never marks it mandatory and the reference client ignores it
entirely). `Remaining Size` is now the sole authoritative,
required-every-chunk progress field, checked against
`total_size - cumulative_bytes_sent_so_far`, exactly matching the
reference implementation's own logic. `Frame Reset` handling is
unchanged (already correct).

**Real-device evidence that the aborted transfer left the modem in a
safe state** (gathered manually by the user after the failure, exactly
the kind of check `RunPostMortemVerification` now performs
automatically going forward):
```
List Configs: count=25 (SKT id NOT present)
Get Config Info(SKT id): INVALID_ID
Get Selected Config: active=Common-Commercial (unchanged), pending=NONE
```
No partial/corrupt config was committed. The abort happened purely at
this tool's own verification layer, one round-trip after the modem had
already done exactly what it was asked.

**Separate bug found and fixed in the same pass:** the report's
`selected_active_after`/`selected_pending_after` printed `NONE` even
though the post-load `Get Selected Config` call was never actually
made (the chunk loop aborted before reaching that step) - `NONE` looked
identical to "the modem was asked and confirmed no active config",
which is a materially different, more alarming statement. Fixed by
introducing an explicit `SelectedSnapshot{queried, active_id,
pending_id}` (see `include/skt_load.h`) and a three-way
`SelectionCheckResult{kUnchanged, kChanged, kUnknown}`, so
"not queried" (`NOT_QUERIED`), "queried, modem said none"
(`NONE`), and "queried, got a value" are always visually distinct in
`load-skt`'s output. Also added: a best-effort, read-only post-mortem
re-check (`RunPostMortemVerification`) that now runs automatically
after any mid-transfer chunk failure, populating
`found_in_list_after`/`info_after`/`selected_after`/`selection_check`
without ever overturning the failure itself - this is exactly the
manual `list`/`info`/`selected` follow-up the user ran by hand after
the first failure, now captured automatically in the tool's own output.

**Summary verdicts from this real-device pass:**
```
H.41 PDC Load accepts 1024-byte chunks (at least the first two): CONFIRMED
Load indication Received is per-current-chunk, not cumulative: CONFIRMED
Previous C0 cumulative-Received check: BUG (fixed)
No completed SKT config committed after the aborted transfer: CONFIRMED
Existing active Common-Commercial config remained selected throughout: CONFIRMED
```

Everything else observed in this pass (init succeeding, DISCOVER
succeeding, chunk acceptance, no side effects from a partial/aborted
transfer) is a positive signal for the overall design, not just "no
new information" - it's direct confirmation that the transport,
service-object version triple, and DISCOVER/safety-baseline logic all
work correctly against real hardware. The only thing that needed fixing
was this state machine's interpretation of one already-correctly-
transmitted indication field.

## 6. Real-device test #2 — full success, plus a new finding about List Configs

After the §5 fix, the user retried `load-skt` for real (after restoring
SELinux enforcing mode and rebooting once, per the interim
`READY_FOR_C0_RETRY_AFTER_FIX` guidance this file previously ended
with). Result: **complete success.**

```
config_id       = ca9c9dcb8d6d2be6e4f6b49ca3e4b5420769a7d1
file size       = 41172 bytes
chunks          = 41 (40 × 1024 + 1 × 212) - all accepted
state           = DONE
info: total_size=41172, description=Commercial-SKT

pre-load  selected: active=0bd71c00fc30d2ff8af86a8b16d1e01f69e6b493 (Common-Commercial), pending=NONE
post-load selected: active=0bd71c00fc30d2ff8af86a8b16d1e01f69e6b493 (unchanged),          pending=NONE

after a MANUAL reboot:
  info ca9c9dcb...  -> EXIT=0, total_size=41172 (still present, still correct size)
  selected          -> active=0bd71c00... (unchanged), pending=NONE (unchanged)
```

```
PDC_LOAD_FULL_41172_BYTES = CONFIRMED
PDC_LOAD_41_CHUNKS = CONFIRMED
PDC_LOAD_CONFIG_PERSISTENT_AFTER_REBOOT = CONFIRMED
LOAD_DOES_NOT_IMPLICITLY_SELECT = CONFIRMED
```

This closes the two biggest open questions
`guacamole_efsd_stock_modem_design.md` had listed as unresolved (items
11/12) - item 11 is now updated to CONFIRMED there directly, citing
this test.

**A separate, important new finding surfaced in the same test: List
Configs is not a reliable enumeration.**

```
baseline (before SKT load): List Configs count=25, includes
  3d39a9558fc940c09ea9b26d29ecaf03587ce771 (CZ-VoLTE-Vodafone)

after SKT load:              List Configs count=25 (still 25!), SKT id
  now appears, but 3d39a955... is MISSING from this list

after a MANUAL reboot:       info 3d39a9558fc940c09ea9b26d29ecaf03587ce771
  -> EXIT=0, total_size=48880, description=CZ-VoLTE-Vodafone
  (i.e. it was there all along - List Configs just didn't report it in
  that particular call)
```

```
PDC_LIST_NEGATIVE_IS_AUTHORITATIVE = FALSE
PDC_LIST_COMPLETE_ENUMERATION = NOT_CONFIRMED
```

A config's absence from one `List Configs` response is not proof it
doesn't exist, and `count=N` is only "how many entries this specific
call returned", never a proven total. (This is also consistent with
how upstream QMI/PDC clients treat these as two logically separate
operations - e.g. ModemManager's `mm-shared-qmi.c` requests `List
Configs` and then issues a SEPARATE `Get Config Info` per listed config
id to get authoritative detail; this device goes one step further by
showing `List Configs` itself can be an incomplete/unstable
enumeration, which `Get Config Info` on an exact id is not.)

## 7. Post-test-#2 hardening: Get Config Info is now the sole authoritative presence check

In direct response to §6's finding, every existence/success decision in
`skt_load.cpp` that used to read `List Configs`' contents (DISCOVER's
already-exists check, VERIFY_LIST/VERIFY_INFO's post-load success
check, and `RunPostMortemVerification`'s post-failure diagnostics) was
changed to use **`Get Config Info` on the exact config ID exclusively**,
classified through a new three-way `ConfigPresence{kExists, kAbsent,
kUnknown}` (see `pdc_client.h`/`pdc_wire.cpp`):

- `kExists`: Get Config Info succeeded.
- `kAbsent`: Get Config Info failed at the indication stage with the
  specific, publicly-documented `QMI_PROTOCOL_ERROR_NO_ENTRY` (= 50,
  from libqmi's public `src/libqmi-glib/qmi-errors.h` - PDC has no
  error codes of its own, confirmed by checking
  `qmi-enums-pdc.h`, which defines none). This exact numeric mapping is
  the best publicly-evidenced candidate, not byte-confirmed against
  this device's own raw `error_status` value for this specific
  scenario - flagged honestly, and deliberately safe either way: the
  classifier fails closed (see next bullet) rather than risk
  misclassifying `kUnknown` as `kAbsent`.
- `kUnknown`: anything else at all - timeout, malformed response,
  request-stage rejection, or an indication-stage rejection with any
  OTHER error code. Never treated as `kAbsent`.

`List Configs` remains in the code (`found_in_list_after`,
`ListContainsId`) purely as a diagnostic/advisory data point printed
alongside the authoritative result - it gates nothing. `main.cpp`'s
`list` command output and `README.md`/`DEVICE_TEST_PROCEDURE.md` were
updated to state this explicitly: `count=` is "entries this call
returned", not a proven total; a miss is not proof of absence.

New/replaced tests for this hardening are itemized in §4 above. All 70
host tests pass (`atest guacamole_pdc_probe_test`).

**Explicitly NOT changed by this hardening pass** (per the scope given):
Config ID = full file SHA-1 (§1), SKT known-SHA-256/size fail-closed
gate, chunk size 1024, Load Config message 0x0026, `Remaining Size`
authoritative / `Received` optional-advisory semantics (§5), `Frame
Reset` fail-closed handling, token correlation, no automatic retry of
any kind, and every write-path prohibition in §3's table. No new write
path was added - this pass is entirely about how EXISTING read
operations (`Get Config Info`, `List Configs`) are interpreted.

## 8. Final Verdict: **PHASE_C0 = PASS**

Both real-device tests are now accounted for: test #1 surfaced and led
to fixing the `Received`/`Remaining Size` bug; test #2, run against the
fix, succeeded completely (41/41 chunks, `DONE`, confirmed persistent
after a real reboot, confirmed no implicit selection change) and
additionally surfaced the List Configs reliability finding, which has
now been hardened against in the code itself (§7) and is covered by
regression tests (§4).

No further PDC write work is needed on the device used for this
testing - it already has the SKT config loaded and persistent. Per
instruction, **`load-skt` was NOT re-executed against any real device
to "confirm" this hardening pass** - re-loading an already-loaded,
already-verified-persistent config would be an unnecessary additional
write. The hardening was verified via host tests only (§4, §7); the
user separately confirmed, via read-only commands
(`list`/`info <skt-id>`/`info <cz-volte-id>`/`selected`), that the
device's actual PDC state matches what this document describes.

**What remains genuinely out of scope for Phase C0** (not blockers -
explicitly the next phase, per the user's own direction): KT/LGU MBN
loading (not attempted - SKT-only was always this phase's scope), EFS2
implementation (`mcfg_sel_db.xml`, `mcfg_autoselect_by_uim` - a
completely separate layer per `guacamole_efsd_stock_modem_design.md`
item 12's own caution against over-generalizing), and any
Settings/AIDL/daemon productionization work. The recommended next step
is a **read-only EFS2 investigation probe** (mirroring this project's
own Phase A→B methodology: read first, understand the real H.41 EFS2
state and access path, design a write only after that), not further PDC
write work.

Per the spec: **no `load-skt` command was executed against any real
device from within this session at any point** - both real-device runs
referenced in §5 and §6 were performed by the user, manually, exactly
as the process intends; this session only received and analyzed their
output, found and fixed root causes, and re-verified via host tests.
