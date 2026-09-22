# guacamole_efsd Phase B Findings — read-only PDC probe

Scope: implement and document a 100% read-only QMI PDC observation tool
(`guacamole_pdc_probe`: `list`, `info <id>`) for OnePlus 7 Pro
(`guacamole`), per the Phase B instructions. No modem/EFS/PDC write of
any kind was implemented or executed. Source lives in this directory;
this file records what was found, what was decided, what is still
unverified, and the go/no-go call for Phase C.

Evidence this builds on (all in `/home/ccc/바탕화면/claude/`):
`research_firmware_diag_analysis.md`, `research_pixelos_source_analysis.md`,
`guacamole_efsd_design_report.md`, `research_firmware_qmi_pdc_analysis.md`,
`research_pixelos_qmi_pdc_analysis.md`, `research_kernel_diag_qrtr_analysis.md`,
`guacamole_efsd_stock_modem_design.md`, `research_iqtioemhook_pdc_mapping.md`,
`research_pdc_service_object_version.md`, and Codex's `REVIEW.md`.

## 1. IQtiOemHook ↔ qcril_qmi_pdc_* mapping — final verdict: **UNRESOLVED**

A dedicated investigation pass (radare2/objdump/baksmali, no jadx/Ghidra
GUI automation attempted for this specific question) traced the call
chain in both directions:

- `qcrilhook.jar`'s `IQcRilHook.smali` has real integer constants
  (`QCRIL_EVT_HOOK_GET_AVAILABLE_CONFIGS = 0x80017`, etc.) whose names
  correspond 1:1 to the `qcril_qmi_pdc_*` function names - but this
  Java-level API is sent via `IQcrilMsgTunnel.sendOemRilRequestRaw`, a
  **different, legacy AIDL/Binder channel** than the HIDL `IQtiOemHook`
  the user originally pointed at.
- `libril-qc-hal-qmi.so` does contain a real `OemHookImpl` HIDL server
  implementation (`sendRequestRaw`/`sendRequestStrings`), confirmed by
  disassembly to funnel into a generic `dispatchRaw()` request-table
  dispatcher used by ALL RIL requests, not a PDC-specific path.
- `MbnModule::handleGetAvailableMbnConfig` is confirmed (by direct
  `bl qcril_qmi_pdc_get_available_configs@plt` call) to be the one and
  only caller of the real List Configs logic, triggered by a
  `QcRilRequestGetAvlMbnConfigMessage` C++ message object.
- **The missing link:** how a `0x80017`-tagged OEM hook payload (via
  either transport) turns into a `QcRilRequestGetAvlMbnConfigMessage`
  could not be traced further with the tools used in that pass - no
  literal `0x80017` reference was found anywhere in code or rodata that
  wasn't provably noise (hash tables, unrelated string data).

**Decision:** per the original instructions ("OEM Hook PDC mapping이 없으면
raw QCCI fallback"), and because this is UNRESOLVED rather than
NOT MAPPED or CONFIRMED, this probe uses the **raw QCCI fallback**, not
the OEM hook. This was also the pragmatically better choice regardless
of the mapping question: raw QCCI is a completely independent QRTR
client connection that never touches rild's OEM-hook/request-dispatch
state at all (confirmed multi-client-safe by kernel analysis - see §7),
whereas experimenting with rild's live OEM-hook path risks perturbing
rild's own request/serial/event-queue state for uncertain benefit.

## 2. Transport actually used: raw QCCI, not OEM hook

`libqmi_cci.so` + `libqmiservices.so`, **linked normally via Android.bp
`shared_libs`** (not dlopen - see §"Code-review addendum" below for why
this changed from the original `libqcci_legacy.so` + dlopen design).
Both are real `cc_prebuilt_library_shared` modules already in
`vendor/oneplus/sm8150-common/Android.bp` (`libqmi_cci` at line 2051,
`libqmiservices` at line 2362 as of this writing); this probe declares
its own `extern "C"` prototypes for the handful of functions it needs
(Qualcomm doesn't publish the headers) and links against the real
modules, so a missing/renamed symbol is a build-time link error, not a
runtime `dlsym()` failure. Message encoding/decoding is done by hand
against the raw TLV wire format (`include/qmi_tlv.h`,
`include/pdc_wire.h`), NOT via Qualcomm's typed IDL C-struct API
(`qmi_client_send_msg_sync`) - that API requires matching Qualcomm's
proprietary, unpublished struct-ABI layout for PDC, which we do not
have; the raw API only requires matching the public,
independently-documented QMI wire protocol, which we do have (see §4).

## 3. Symbols used, and their evidence class

| Symbol | Library | Evidence |
|---|---|---|
| `pdc_get_service_object_internal_v01` | libqmiservices.so | **Confirmed export** (readelf -Ws), this exact firmware |
| `qmi_client_init_instance` | libqmi_cci.so | **Confirmed export** (readelf -Ws, 19 real defined symbols including full discovery API); parameter shape taken from `missing_qmi_client_api.h`'s already-working declaration of this exact function from this exact library (used there for the DMS service) - NOT byte-verified for the PDC call specifically, see addendum item 1 |
| `qmi_client_release` | libqmi_cci.so | **Confirmed export** |
| `qmi_client_send_raw_msg_sync` | libqmi_cci.so | **Confirmed export**; parameter shape cross-checked against `qmi_cci_send_raw_msg_sync` in Qualcomm's public `qmi_cci.h` |
| PDC version triple `(1, 15, 6)` | libril-qc-hal-qmi.so callers | **Confirmed** via radare2 disassembly of two independent call sites (`pdc_init()`, `PdcModemEndPointModule::getServiceObject()`), both loading identical immediates - see `research_pdc_service_object_version.md` |

`libqcci_legacy.so` (the original target) is no longer used at all: it
was found to have no service-discovery function whatsoever, and
`libqmi_cci.so` is a strict superset that also happens to already have
a real Android.bp module - see the code-review addendum's item 1 for the
full story.

## 4/5. List Configs / Get Config Info message definitions

Taken from `linux-mobile-broadband/libqmi`'s public, actively-maintained
QMI PDC service definition (`data/qmi-service-pdc.json`,
`data/qmi-common.json`, `src/libqmi-glib/qmi-enums-pdc.h`) - the same
class of evidence Codex's review used for EFS2 (qtools/EfsTools) rather
than a single-binary disassembly guess:

- **List Configs** = message ID `0x0024`. Request: Token TLV `0x10`
  (u32) + Config Type TLV `0x11` (u32, mandatory, `1`=SOFTWARE for the
  Korea MBNs). Sync ack: Operation Result TLV `0x02` only. Indication
  (same ID, async): Token `0x10`, Indication Result `0x01` (u16), Configs
  array `0x11` (u8-count-prefixed array of {Config Type u32, Id
  u8-length-prefixed bytes}).
- **Get Config Info** = message ID `0x0028`. Request: "Config Type And
  Id" TLV `0x01` ({Config Type u32}{Id u8-prefixed bytes}) + Token `0x10`.
  Sync ack: Operation Result only. Indication: Token, Indication Result,
  Total Size `0x11` (u32), Description `0x12` (u8-prefixed string, **not
  NUL-terminated on the wire** - this probe never assumes NUL
  termination), Version `0x13` (u32).
- Every parse path (`src/pdc_wire.cpp`) rejects malformed/truncated/
  oversized input rather than best-effort decoding it - see
  `tests/pdc_wire_test.cpp` for the explicit malformed-length,
  truncated-buffer, oversized-id, and unknown-TLV-is-skipped-not-fatal
  cases, matching the Phase B safety requirements. `kMaxIdLen = 124` is
  cross-checked against `QcRilHook.smali`'s own `MAX_PDC_ID_LEN = 0x7c`
  constant (§1's investigation), not invented.
- **Not independently confirmed for this exact firmware's PDC build:**
  the "since 1.18"/"since 1.32" versioning notes in libqmi's schema imply
  Qualcomm has revised this service over time; a version mismatch would
  show up as a non-zero Operation Result or a decode rejection, both of
  which this probe reports distinctly (see README's exit codes) rather
  than silently misparsing.

## 6. Expected output on this device

`list` is expected to show at least the pre-existing "Common/Commercial"
baseline software config(s) already on the modem. Per the user's own
prior PC-PDC testing history, if a Korea SKT MBN was ever manually
`Load`ed via PC PDC and never explicitly deleted, it may also show up
here - if so, record its config ID, type, and (via `info`) its version/
description/total_size **before** any further work, as partial
real-device evidence toward item 11 in `guacamole_efsd_stock_modem_
design.md` (persistence across reboot) - a surviving prior manual Load
is suggestive, not conclusive, of QCCI Loads persisting the same way.

## 7. Remaining blockers before Phase C (write path)

None of these are resolved by more static analysis - all require an
actual run of this probe on the device:

1. **`qmi_client_init_instance`'s exact parameter layout for the PDC
   call specifically (not eliminated, but now well-supported by
   evidence):** the function name, library, and 7-argument shape are
   confirmed real (real export in `libqmi_cci.so`; same function, same
   library, real disassembled call site in `imsqmidaemon` for a
   different service, DMS). What's NOT disassembly-verified is this
   exact call for the PDC service object specifically - see the
   code-review addendum's item 1 for the full evidence chain. The very
   first `list` invocation is the real test of this last remaining gap.
2. **SELinux label assumption on the two linked libraries** (`same_process
   _hal_file`, borrowed from libdiag.so's confirmed label) - flagged
   explicitly in `sepolicy/vendor/guacamole_pdc_probe.te`, first
   real-device step is `ls -Z` on both libraries.
3. **Whether `qmi_client_send_raw_msg_sync`'s exact parameter layout
   matches** what's cross-checked against Qualcomm's public
   `qmi_cci_send_raw_msg_sync` analogue - not disassembly-verified
   byte-for-byte against `libqmi_cci.so` itself.
4. **Whether rild's own permanently-open PDC/QMI session (if any)
   conflicts with a second independent QCCI client** - kernel-level QRTR
   is confirmed multi-client-by-design (`research_kernel_diag_qrtr_
   analysis.md` item d), and SELinux already grants 10+ non-rild vendor
   domains the same `qmux_socket` access in production policy - but
   whether the MODEM's PDC *service* itself (not the transport) accepts
   a second concurrent client is firmware-internal and only observable
   live. A `TIMEOUT` result on first run is itself informative here, not
   just a bug to fix blindly.
5. **Whether a prior manually-Loaded Korea config still exists** (§6) -
   directly informs item 11 of `guacamole_efsd_stock_modem_design.md`.
6. **Whether the PDC message "since 1.32" (Token-on-Config-Type-And-Id-v2,
   Refresh indication) vs. older wire shapes actually matches this
   device's PDC service version** - a version mismatch is the most likely
   cause if `info` fails while `list` succeeds (their message shapes
   differ in how much history/versioning is layered on).

## Code-review addendum (post-implementation, pre-device-test)

An independent code review of the implemented probe (not just the design)
found four issues, all fixed in this pass before any device execution:

1. **`qmi_client_init(NULL service_info, ...)` — investigated further,
   then the design changed as a result, not just patched. RESOLVED.**
   The reviewer correctly flagged this as the highest-risk unverified
   assumption, citing Qualcomm's modern public `qmi-framework`
   (`qmi_cci.h`), where the equivalent call documents `service_info` as
   required, sourced from `qmi_client_get_service_list()`. Re-checking
   this exact firmware directly (`readelf -Ws` on both
   `vendor/lib{,64}/libqcci_legacy.so`) found the reviewer's suggested
   fix wasn't actually available on THAT library: **no
   `qmi_client_get_service_list`, `qmi_client_get_any_service`,
   `qmi_client_get_service_instance`, `qmi_client_init_instance`, or
   `qmi_client_notifier_init` is exported anywhere in either build of
   `libqcci_legacy.so`** - only `qmi_client_init` itself. A follow-up
   radare2 pass then found a real, already-working call site of
   `qmi_client_init` in `imsqmidaemon` (a real, independent, already-
   shipping QMI client on this exact firmware) - and it showed two
   things at once: (a) **NULL is confirmed NOT how this device does it**
   - the real call passes a buffer that `qmi_client_get_service_list()`
   populated one instruction sequence earlier - and (b) **that evidence
   came from `libqmi_cci.so`, a separate, physically distinct library
   `imsqmidaemon` links instead of `libqcci_legacy.so`**. Re-checking
   `libqmi_cci.so`'s exports directly confirmed it has the full
   discovery API (`qmi_client_get_service_list`, `qmi_client_init_instance`,
   etc. - 19 real exported functions total) that `libqcci_legacy.so`
   completely lacks, AND that `libqmi_cci.so`/`libqmiservices.so` both
   already have real `cc_prebuilt_library_shared` Android.bp modules in
   this tree (unlike `libqcci_legacy.so`, which has none).
   **Resulting design change:** the transport was switched entirely from
   `libqcci_legacy.so` (dlopen) to `libqmi_cci.so` (linked normally via
   `shared_libs`) using `qmi_client_init_instance()` - a one-shot
   resolve-and-connect convenience function with a proven precedent in
   this exact tree (`missing_qmi_client_api.h` calls this exact function
   from this exact library for the DMS service). This avoids needing to
   decode `qmi_client_get_service_list()`'s output struct layout (not
   reverse engineered in this research) while still being evidence-based
   rather than a guess. See `research_qmi_client_init_callsite.md` for
   the full disassembly evidence, and §2/§3 above for the resulting
   design. **Residual risk (honestly still open):** `qmi_client_init_instance`
   specifically (as opposed to `qmi_client_init` or DMS's use of
   `_instance`) was not itself disassembled for the PDC call - the
   evidence is strong-by-analogy (same library, same function, proven
   pattern) but not byte-exact for this specific call. This is exactly
   the kind of thing only an on-device run can finish confirming, and is
   listed as such in §7 below.
2. **Sync-ack malformed/rejected conflation - FIXED.** `ListConfigs()`/
   `GetConfigInfo()` originally wrote
   `if (ParseOperationResultOnly(...) && est != 0)`, which meant a
   `false` return (malformed/missing Operation Result TLV) fell through
   the `&&` and proceeded to `WaitForIndication()` as if the request had
   been accepted - a real bug, confirmed by inspection, not a
   hypothetical. Fixed to check the decode result and the error status as
   two separate, sequential conditions, with the decode failure now
   short-circuiting to `kMalformedResponse` immediately. Covered by the
   new `OperationResult.EmptySyncAckIsMalformedNotRejected` test.
3. **Indication `error_status` never actually checked by the caller -
   FIXED.** `ParseListConfigsIndication`/`ParseGetConfigInfoIndication`'s
   `.ok` field only ever meant "the TLV stream decoded safely" - it was
   never meant to also mean "the modem said yes", and the unit tests
   written alongside those parsers said so explicitly in a comment
   ("caller must check this field"). But `PdcClient::ListConfigs()`/
   `GetConfigInfo()` mapped `.ok` straight to `ProbeStatus::kOk` without
   ever reading `.error_status` - so a real modem-level rejection
   (Indication Result != 0) that still decoded cleanly would have been
   reported as success with empty/zero data. Fixed by adding
   `IndicationOutcome ClassifyListConfigsResult()`/
   `ClassifyGetConfigInfoResult()` (pure, host-testable functions in
   `pdc_wire.h`/`.cpp`) and a new `ProbeStatus::kIndicationRejected`
   (exit code 7), so "decoded but rejected" is now a distinct, correctly
   classified outcome from both "malformed" and "real success". Six new
   tests added (`PdcWire.Classify*`).
4. **Version-triple fallback list removed - FIXED.** `main.cpp` tried
   `(1,15,6)` then `(1,0,1)` then `(1,0,0)`. Since `(1,15,6)` is
   CONFIRMED (not inferred) via two independent disassembled call sites
   in this exact firmware (§3), silently accepting a different version
   that happens to also return a non-null service object risks getting a
   real but wire-incompatible object (this probe's parser is built
   against the libqmi-derived TLV shapes, which may not match whatever a
   different, un-verified PDC IDL version actually emits). Now fails
   closed with only `(1, 15, 6)`.

All 32 host unit tests (11 `QmiTlv` + 17 `PdcWire` + 4 `OperationResult`,
up from 25 before this pass) compile and pass against real AOSP
googletest sources - see `VERIFICATION.md` for the exact commands and
output. `pdc_client.cpp`/`main.cpp`/`qcci_shim.cpp` remain syntax-clean
under `g++ -Wall -Wextra -fsyntax-only` after these changes.

## Phase C readiness verdict: **GO WITH CONDITIONS**

Rationale: every structural prerequisite for the raw-QCCI path is either
CONFIRMED-by-disassembly (service object symbol, version triple, QCCI
export names, wire-level message/TLV definitions from a public,
cross-device-validated reference) or is a narrowly-scoped, cheaply
observable unknown that this exact probe was built to resolve (§7) -
none of the remaining unknowns require more reverse engineering, only
running this tool once, safely (it cannot write anything).

**Conditions before Phase C (write path) may start:**
- This probe must actually be run on the device and return `OK` for
  `list` (and ideally `info`) at least once. A `TIMEOUT`/`INIT_FAILED`
  result means Phase C's design (which reuses this exact transport and
  wire codec for Load Config) needs to be revisited before writing any
  code that touches modem state.
- Item 6/7's persistence and versioning observations should be recorded,
  since Phase C's Load Config implementation will need the SAME
  version-triple/transport validation this file documents, plus its own
  (currently unwritten) TLV encode for the Load Config message, which
  has a materially more complex wire shape (chunked transfer, a `Total
  Size` + `Chunk` sequence TLV) that Phase B intentionally does not
  implement.
- Codex REVIEW.md's EFS2/backup/rollback/lock/SELinux findings remain
  fully in force for Phase C's EFS2 half of the work (unrelated to PDC,
  unaffected by anything in this file).
