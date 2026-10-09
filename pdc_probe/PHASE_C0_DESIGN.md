# Phase C0 design — SKT-only PDC Load Config

Scope: extend `guacamole_pdc_probe` with exactly one write capability
(`load-skt`), gated by extensive pre-flight checks, targeting exactly
one file (the stock H.41 Korea SKT `mcfg_sw.mbn`), never touching
selection/activation/EFS2/reboot. See PHASE_C0_FINDINGS.md for the
evidence trail and go/no-go verdict; this file is the "how it works".

## New files

| File | Role |
|---|---|
| `include/hash.h` / `src/hash.cpp` | Self-contained SHA-1 (config ID) + SHA-256 (file identity check). Verified against FIPS 180-4 test vectors and cross-checked against the real SKT MBN via system `sha1sum`/`sha256sum`. |
| `include/known_profiles.h` / `src/known_profiles.cpp` | The ONLY source of the SKT file's runtime path, expected size, and expected SHA-256 - hardcoded, never externally parameterizable. |
| `include/pdc_load_wire.h` / `src/pdc_load_wire.cpp` | Load Config (0x26) wire codec - request builder, indication parser, classifier. Pure, transport-independent, host-testable (mirrors pdc_wire.h's design). THE WRITE PATH's wire layer, kept structurally separate from pdc_wire.h (read-only messages). |
| `include/skt_load.h` / `src/skt_load.cpp` | The state machine: PRECHECK → DISCOVER → OPEN_FILE → LOAD_BEGIN → LOAD_CHUNKS → WAIT_COMPLETION → VERIFY_LIST → VERIFY_INFO → DONE / UNKNOWN_RESULT / FAILED. |
| `tests/hash_test.cpp` | FIPS 180-4 test vectors for Sha1/Sha256. |
| `tests/skt_load_test.cpp` | Full state-machine tests against a fake `IPdcClient`, synthetic buffers only. |

## Changed files

- `include/pdc_client.h` / `src/pdc_client.cpp`: added `IPdcClient`
  (abstract interface `PdcClient` now implements, purely so
  `skt_load.cpp` is host-testable against a fake); added
  `LoadConfigChunk()` - one chunk, one round trip, same shape as every
  other PdcClient method. `ProbeStatusToString()` moved to
  `pdc_wire.cpp` so it links into the vendor-independent host test
  target.
- `src/main.cpp`: added the `load-skt` command and exit codes 8
  (`UNKNOWN_RESULT`) / 9 (`FAILED`).
- `Android.bp`: added the new sources to both the device binary and
  the (vendor-independent) host test target.

## Config ID scheme (the single most safety-critical design decision)

**CONFIRMED: `Id = SHA-1(entire file bytes)`.** See
`research_pdc_load_config_id_scheme.md` for the full evidence chain:
libqmi's public reference client computes exactly this and sends it as
the Config Chunk's Id field on every chunk (the modem never generates
or returns a new id anywhere in the Load Config indication - only
Received/Remaining Size/Frame Reset). Empirically double-checked against
this real device: the real Common-Commercial config's actual reported
id (`0bd71c00fc30d2ff8af86a8b16d1e01f69e6b493`) is the exact SHA-1 of a
real 24508-byte `.mbn` found in this firmware
(`generic/oppo_exp/common/commerci/mcfg_sw.mbn`).

The predicted SKT config id (computed fresh at runtime from the actual
file, NOT hardcoded as a pass/fail condition anywhere in the code) is
printed by `load-skt` for the operator's own cross-reference. For the
record (documentation only): `sha1sum` of the stock SKT MBN in the H.41
extraction is `ca9c9dcb8d6d2be6e4f6b49ca3e4b5420769a7d1`.

## Chunk protocol

- `kLoadChunkSize = 1024` bytes - matches libqmi's public reference
  implementation (`LOAD_CONFIG_CHUNK_SIZE`). H.41 disassembly could not
  independently confirm qcrild's own chunk size (tracked as UNRESOLVED
  in `research_pdc_load_config_id_scheme.md` - logging boilerplate
  obscured the loop). 1024 is the conservative, publicly-precedented
  choice.
- `Total Size` (the whole file's size) is sent identically on every
  chunk, not just the first - matches the reference client's
  `load_config_input_create_chunk()`, which re-reads the file length on
  every call.
- No explicit per-chunk offset field exists on the wire. **`Remaining
  Size` is the sole authoritative progress field** - required on every
  chunk, checked against `total_size - cumulative_bytes_sent_so_far`,
  looped until it reaches 0. **`Received` is optional/advisory only**,
  and when present describes *this chunk's own size*, not a cumulative
  total - CONFIRMED against real H.41 hardware (see
  PHASE_C0_FINDINGS.md §5: the first real device test wrongly treated
  `Received` as a required cumulative count and aborted at chunk #2
  when the real modem correctly reported `Received=1024` for that
  chunk's own 1024 bytes) and independently matches libqmi's public
  reference client (`qmicli-pdc.c`'s `load_config_ready_indication()`,
  which never reads `Received` at all - only `Remaining Size` and
  `Frame Reset`). Any `Remaining Size` mismatch, or a `Received`
  mismatch against the current chunk's own length when the TLV is
  present, is treated as FAILED, not silently accepted.
- `Frame Reset == true` is treated as a rejection/abort signal, not a
  benign event.
- Chunks are sent strictly sequentially - request, wait for its
  indication, THEN send the next chunk. No pipelining anywhere.
- A chunk timeout is NEVER retried. The state machine moves to
  `UNKNOWN_RESULT` and re-queries List Configs to determine what
  actually happened, per the explicit Phase C0 policy.

## Safety mechanisms, mapped to the state machine

1. **Fail-closed file identity check (PRECHECK):** the on-device file at
   the known SKT path must match the known-good SHA-256 exactly, or the
   whole operation aborts before any modem communication at all (0 List
   Configs calls, 0 Load Config calls - verified by
   `SktLoad.StockFileIdentityMismatchIsFailClosed`).
2. **Duplicate-load avoidance (DISCOVER), GetConfigInfo-authoritative
   (post-real-device-test-#2 hardening):** Get Config Info on the exact
   SHA-1 config id is queried first (NOT List Configs - a real device
   test found List Configs can silently omit an entry Get Config Info
   still resolves, see PHASE_C0_FINDINGS.md §6). Classified via
   `ConfigPresence{kExists, kAbsent, kUnknown}` (pdc_client.h): `kExists`
   skips the load entirely and reports `DONE` with
   `already_existed_before_load = true`; `kAbsent` (the specific,
   publicly-documented `QMI_PROTOCOL_ERROR_NO_ENTRY`) proceeds to load;
   `kUnknown` (anything else - timeout, malformed, any other error code)
   fails closed, refusing to load without a reliable answer. List
   Configs is still queried for `found_in_list_after` but is diagnostic
   only from here on - it never gates this decision.
3. **Selected-config safety baseline (DISCOVER):** Get Selected Config
   is queried and stored BEFORE any write. If this itself fails, the
   whole operation aborts (no baseline, no safe way to detect a
   violation later).
4. **Per-chunk consistency checks (LOAD_CHUNKS):** `Remaining Size` (the
   sole authoritative progress field, required every chunk) and `Frame
   Reset` are independently verified every chunk; `Received`, when
   present, is checked only against the current chunk's own size
   (optional/advisory - see the file-header note on real-device test #1)
   - not just a generic "success" status.
5. **No blind trust in a "clean" transfer, GetConfigInfo-authoritative
   (VERIFY_INFO):** even after every chunk reports success, Get Config
   Info on the exact config id is the SOLE basis for declaring success -
   it must return `kExists` with a matching `total_size`, or the result
   is FAILED despite the clean chunk transfer. List Configs
   (`VERIFY_LIST`) is queried too but is diagnostic only
   (`found_in_list_after`) - it never gates this decision either way (a
   miss doesn't fail it, a hit doesn't save it - see
   `PostLoadListOmissionButGetInfoSuccessStillVerifies` and
   `PostLoadListPresentButGetInfoFailureDoesNotVerify` in
   `skt_load_test.cpp`).
6. **Selected-config safety check, again (post-load):** Get Selected
   Config is queried once more and compared against the baseline from
   step 3 via an explicit three-way `SelectionCheckResult{kUnchanged,
   kChanged, kUnknown}` (see `SelectedSnapshot` in `skt_load.h`).
   `kUnknown` is a real, visually-distinct outcome (`NOT_QUERIED` in the
   printed report) - it is NOT collapsed into "NONE" the way an earlier
   version of this code did (a real bug found and fixed on the first
   device test - see PHASE_C0_FINDINGS.md §5). ANY result other than
   `kUnchanged` demotes the final result to `FAILED`, even though the
   transfer itself succeeded - this is the single most important safety
   property Phase C0 exists to verify.
7. **No deletes, ever:** there is no code path anywhere in this
   codebase that calls Delete Config, including on failure/rollback.
   The existing PDC config database is only ever added to, never
   subtracted from, by this tool.
8. **Automatic post-mortem verification on mid-transfer failure,
   GetConfigInfo-authoritative:** if a chunk fails outright (rejected,
   malformed, or a consistency check trips), `RunPostMortemVerification`
   still runs a best-effort, read-only re-check before returning -
   `ConfigPresence` via Get Config Info (authoritative), plus List
   Configs and Get Selected Config (diagnostic/safety) - capturing
   exactly the manual follow-up an operator would otherwise have to run
   by hand, without ever overturning the FAILED verdict that triggered
   it.

## What `load-skt` prints on success

Full report fields (state, message, file_size, config_id, chunks_sent,
bytes_sent, before/after selected, found_in_list_after, info fields),
followed by the exact manual verification sequence the operator should
run (list/info/selected, then a MANUAL reboot - never automated -, then
list/info/selected again) to obtain direct real-device evidence for the
two open questions `guacamole_efsd_stock_modem_design.md` identified as
the last real unknowns: does a PDC Load persist across reboot, and does
Load avoid implicitly changing the selected config.
