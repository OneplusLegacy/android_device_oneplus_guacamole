# Phase K - Set Selected Config research and future-experiment readiness

2026-09-24, W1 Finalization Round, item K. Everything in this document
is **host-test-only preparation** for a future, separately-gated
real-device experiment (the "38/39/40" carrier-index mapping question).
**No code in this codebase calls `PdcClient::SetSelectedConfig()`
against a real device anywhere** - see `pdc_client.h`'s own comment on
that method and `pdc_select_config_wire.h`'s header comment. This round
performed **zero** real PDC mutation, zero `SetSelectedConfig` calls,
zero Activate calls, on any real device.

## 1. Wire format findings (implemented in code, summarized here)

Source: `data/qmi-service-pdc.json` from
`linux-mobile-broadband/libqmi` (same public, cross-device-validated
reference this whole project's PDC wire layer is built from - see
`pdc_wire.h`'s own provenance comment), fetched fresh 2026-09-24.

| Fact | Value | Evidence |
|---|---|---|
| Message ID | `0x0023` | Fetched directly from the source JSON's message table, not extrapolated from neighboring IDs (0x22/0x24/0x26/0x28 are NOT a strict arithmetic sequence - 0x25 is Delete Config, 0x27 is Activate Config, 0x29 is Get Config Limits - so guessing by pattern would have been wrong). |
| Request TLVs | "Config Type And Id" (common-ref, id `0x01`) + "Token" (common-ref, id `0x10`) | Cross-checked against this codebase's own already-proven `kTlvConfigTypeAndId`/`kTlvToken` constants - the JSON's "Common" section defines both ids identically. Byte-for-byte the same encoding as the already-implemented `BuildGetConfigInfoRequest()` - verified by `PdcSelectConfigWire.BuildRequestUsesSameEncodingAsGetConfigInfoRequest`. |
| Sync response TLVs | "Operation Result" + "Token" | Same shape as every other PDC message in this codebase - `ParseOperationResultOnly()` reused verbatim, no new code needed. |
| Indication TLVs | "Indication Result" (common-ref, id `0x01`) + "Token" (common-ref, id `0x10`) **only** | This is the simplest indication shape in the whole PDC API - even simpler than Load Config's (which also carries Received/RemainingSize/FrameReset). |

## 2. Answers to item K's explicit research questions

- **Subscription selector semantics**: the request carries *only*
  `{Config Type, Config Id}` - there is no separate SIM-slot/
  subscription-index TLV anywhere in this message. This matches Get
  Selected Config's own request shape (also selector-free beyond
  Config Type). If this device has real per-SIM-slot PDC selection
  semantics, they are **not exposed through this message** - any such
  mechanism would have to live at a different layer (e.g. QMI_UIM or a
  subscription-manager API), and nothing in this research found
  evidence such a mechanism exists for this specific call.
- **Config type**: `kPlatform` (0) or `kSoftware` (1) - same
  `ConfigType` enum already used everywhere else in this codebase. The
  Korea MBNs are `kSoftware` (see `known_profiles.h`).
- **Pending vs. active semantics**: **NOT resolvable from this
  message's own wire shape.** The indication reports only accept/reject
  - it never echoes back "now active" vs. "now pending until reboot".
  The *only* way to observe that distinction is a **separate** Get
  Selected Config call (`ParseGetSelectedConfigIndication`'s
  `active_id`/`pending_id` fields, already implemented) issued *after*
  Set Selected Config's own indication succeeds - see the procedure in
  section 3.
- **Indication semantics**: success/failure only (`error_status`, via
  `ClassifySetSelectedConfigResult()` returning `kSuccess`/`kRejected`/
  `kMalformed` - the same three-way classification every other PDC call
  in this codebase already uses).

## 3. Required procedure for the future real-device experiment

**Not implemented as code this round** - this is the *procedure* a
future, explicitly-authorized experiment must follow, using the
already-implemented (host-tested only) primitives:

1. **Pre-snapshot**: call `GetSelectedConfig(ConfigType::kSoftware)`.
   Record both `active_id` and `pending_id` (each independently may be
   absent - record that fact too, not just the bytes). This is the
   *only* record of "what to recover to" if anything goes wrong - it
   MUST be captured and durably logged (e.g. via
   `FileTransactionBackend`, see `efs_transaction_backend.h`, or an
   equivalent PDC-side durable log not yet built) before step 2.
2. **Operation**: call `SetSelectedConfig(ConfigType::kSoftware, <target
   id>)`. Record the raw `SetSelectedConfigOutcome` (status +
   `error_status`) verbatim - do not discard it even on apparent
   success.
3. **Post-snapshot**: call `GetSelectedConfig(ConfigType::kSoftware)`
   again. Compare against the pre-snapshot:
   - If `active_id` changed to the target id: the change took effect
     immediately (no reboot pending).
   - If `pending_id` changed to the target id but `active_id` did not:
     the change is staged, pending some other event (reboot, most
     likely, though this is not confirmed by this message alone -
     Activate Config, `0x0027`, is a distinct, still-unimplemented
     message that may be what actually commits a pending selection;
     this codebase implements neither Activate nor any reboot-triggered
     observation of that transition).
   - If neither changed despite a `kSuccess` classification: the
     indication's own report and the observable selected-config state
     disagree - treat this as **`kMutationOutcomeUnknown`**-equivalent
     (see `efs_transaction.h`'s own state of that name for the parallel
     EFS2 case) and do NOT proceed to any further step until resolved
     out-of-band.
4. **Recovery to baseline**: if the post-snapshot shows anything other
   than the originally intended state, or if the experiment is simply
   being concluded, call `SetSelectedConfig(ConfigType::kSoftware,
   <baseline id>)` where **baseline id =
   `0bd71c00fc30d2ff8af86a8b16d1e01f69e6b493`** (the real, confirmed
   Common-Commercial/oppo_exp config ID - see
   `research_pdc_load_config_id_scheme.md`'s real-device cross-check:
   this exact SHA-1 matched a real on-device Get Config Info response
   AND independently matched a real `.mbn` file's own SHA-1 in this
   firmware image).
5. **Post-recovery snapshot**: call `GetSelectedConfig` one final time
   and confirm `active_id` (and `pending_id`, if it was set) reads back
   as the baseline id before considering the device restored.

Every step above uses primitives that are implemented and host-tested
(`GetSelectedConfig` since Phase B2, `SetSelectedConfig` as of this
round) but **none of this sequence has ever been executed, nor may it
be executed, against a real device by this session or without explicit
separate authorization for that specific future round.**

## 4. What this round explicitly did NOT do

- Did not call `SetSelectedConfig` against any real device.
- Did not implement or call Activate Config (`0x0027`), Deactivate
  Config (`0x002B`), or Delete Config (`0x0025`) - out of scope for
  this round, not researched.
- Did not resolve the LGU/SKT/KT `mcfg_carrier_index` mapping (still
  `korea_sel_db_patcher.h`'s own unconditional `kBlockedMapping`) - this
  document prepares the PDC-side primitive the eventual mapping
  experiment would use, it does not itself attempt that experiment.
