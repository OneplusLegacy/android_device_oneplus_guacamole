# Phase D3 / Cycle 2 — daemon/UI architecture + carrier-index mapping experiment plan

2026-09-24. This document is DESIGN-LEVEL, not built-and-tested C++
like everything under `include/`/`src/`/`tests/` in this directory.
Sections J (daemon), most of the SELinux/init wiring, and L (Settings
UI) require an AOSP build (AIDL compiler, Java/Kotlin toolchain,
SELinux policy compiler) this sandbox does not have - none of it has
been compiled or run. What follows is concrete enough to implement
directly, cross-checked against this project's own already-reviewed
`guacamole_pdc_probe.te` sepolicy precedent and the real, tested C++
layers (`efs_transaction.h`, `korea_sel_db_patcher.h`,
`known_profiles.h`) it wraps.

**What IS real and tested in this pass**: the AIDL interface
(`aidl/com/guacamole/efsd/IGuacamoleEfsd.aidl`) as a syntactically
complete definition; `sepolicy/vendor/guacamole_efsd.te` + its
`file_contexts`/`service_contexts` entries, modeled closely on
`guacamole_pdc_probe.te`'s own real-device-informed policy; a `.rc`
service skeleton. None of these three are wired into an active build
target yet (see each file's own header comment).

## J — `guacamole_efsd` daemon architecture

### Process structure

A single persistent vendor binary (`/vendor/bin/guacamole_efsd`),
started by init (see `init/guacamole_efsd.rc`), running under its own
SELinux domain (`guacamole_efsd`, see the `.te` file) and a dedicated
uid/gid — never `root`/`su`. It links the SAME `libdiag.so`/
`libqmi_cci.so`/`libqmiservices.so` the two probe CLIs already use, and
depends internally on:

- `efs_probe/src/{efs_wire,efs_reader,efs_targets,diag_transport,diag_frame,efs_real_client,efs_dir_lister,efs_snapshot,efs_write_wire,efs_write_targets,efs_writer,efs_real_write_client,efs_transaction,korea_sel_db_patcher}.cpp`
  (compiled as a static library target, `libguacamole_efs_core`, so
  both `guacamole_efs_probe` (CLI) and `guacamole_efsd` (daemon) share
  ONE implementation - no forked/duplicated logic between debug tool
  and production daemon).
- `pdc_probe/src/{pdc_wire,pdc_load_wire,pdc_client,qcci_shim,hash,known_profiles,skt_load}.cpp`
  likewise as `libguacamole_pdc_core`.

### Global transaction serialization

A single `std::mutex transaction_mutex_` (or equivalent) around EVERY
call that touches `ApplyPatchPlan()` or `RunLoadCarrier()` - the AIDL
binder thread pool means concurrent `applyCarrier()` calls are
otherwise possible; the daemon must reject (not queue, not block
indefinitely) a second call while one is in flight -
`GuacamoleEfsdResult::ALREADY_IN_PROGRESS`. This mirrors
`DiagTransport`'s own `MAX_OUTSTANDING_REQUESTS=1` pattern (see
`diag_transport.h`) one layer up.

### Daemon-level state machine

Directly the `GuacamoleEfsdState` enum from the AIDL file, driven by:

```
IDLE
  -> MAPPING_BLOCKED     (at startup, once korea_sel_db_patcher.h's
                           CarrierIndexMappingState is checked - always
                           kUnresolved today, see Section "38/39/40
                           experiment plan" below for how this actually
                           changes)
  -> READY_FOR_PREPARE   (once mapping IS confirmed by a future update -
                           unreachable today)
READY_FOR_PREPARE -> TRANSACTION_IN_PROGRESS   (on prepareKoreanSupport()/applyCarrier())
TRANSACTION_IN_PROGRESS -> IDLE | READY_FOR_PREPARE   (TransactionState::kSucceeded[/AfterAmbiguousResponse])
TRANSACTION_IN_PROGRESS -> REBOOT_REQUIRED             (post-Load, matching Phase C0's own real-device finding that a loaded PDC config may need a reboot to take effect - carried over, not re-derived)
TRANSACTION_IN_PROGRESS -> MUTATION_UNKNOWN             (TransactionState::kMutationOutcomeUnknown)
TRANSACTION_IN_PROGRESS -> ROLLBACK_REQUIRED            (kMutationOutcomeUnknown persisted across a restart with no recovery attempted yet - see persistent backend below)
TRANSACTION_IN_PROGRESS -> HARD_STOP                    (TransactionState::kRollbackFailedHardStop)
MUTATION_UNKNOWN | ROLLBACK_REQUIRED -> IDLE | HARD_STOP  (via an explicit, operator-triggered recovery attempt on a FRESH session - see efs_transaction.h's IWriteSessionFactory)
HARD_STOP -> (terminal; only cleared by a build/config change acknowledging manual operator intervention happened out-of-band)
```

### Persistent transaction backend (Section F/7)

`ITransactionLog`'s real implementation (`FileTransactionLog`, not yet
written) persists each `TransactionRecord` as an append-only, one-
JSON-object-per-line log at a fixed internal path
(`/data/vendor/guacamole_efsd/transactions.log` - see the `.te` file's
`guacamole_efsd_data_file` type), written and `fsync()`'d BEFORE the
WRITE OPEN(truncate) call ever reaches the modem (durable backup
first, matching item 6/7's explicit ordering requirement), then
`fsync()`'d again after each subsequent state transition
(`kSucceeded`/`kRolledBackSuccessfully`/`kMutationOutcomeUnknown`/etc).
Preimage full bytes are stored alongside the record (a
`preimage_<transaction_id>.bin` sibling file, hash-verified on write
AND on every read-back), not just their SHA-256 - a crash recovery path
needs the ACTUAL bytes to attempt a rollback, not just a digest to
verify against.

**Startup crash recovery**: on daemon start, scan the transaction log
for the last record whose `state` is one of `TRANSACTION_IN_PROGRESS`
(the daemon crashed mid-transaction - the log's last line for that
transaction_id was never updated to a terminal state),
`MUTATION_UNKNOWN`, or `ROLLBACK_REQUIRED`. For each: re-open a FRESH
`IEfsClient`/`IEfsWriteClient` pair (never trust in-memory state that
died with the crashed process), read the target's current bytes, and
classify:
- Matches the stored `preimage` exactly -> `IDLE` (nothing to do,
  device already at/back-to original state).
- Matches the stored `proposed` exactly -> resume as
  `kSucceededAfterAmbiguousResponse`-equivalent, transition to `IDLE`/
  `REBOOT_REQUIRED`.
- Matches neither -> `ROLLBACK_REQUIRED`, surfaced via `getStatus()`
  for the UI/operator, NEVER auto-attempted without an explicit
  `restoreOriginal()` call (which itself goes through
  `ApplyPatchPlan()`'s own fresh-session recovery path).

## SELinux/init (Section 12)

Covered by the 4 real files this pass added
(`sepolicy/vendor/guacamole_efsd.te`, that dir's `file_contexts`/
`service_contexts` additions, `efs_probe/init/guacamole_efsd.rc`) -
see each file's own header comment for exact grants and the honest
"not yet policy-compiler-verified" caveat. Remaining before Gate W1
can extend to a real daemon run:
1. A real `m selinux_policy` (or `checkpolicy`) pass against the full
   tree once the daemon module itself exists in an `Android.bp`
   `cc_binary` target (currently only source files/policy exist, no
   `Android.bp` entry for `guacamole_efsd` itself yet - deliberately,
   since a `cc_binary` with no real `main()` logic behind it would be
   actively misleading to add).
2. `neverallow` verification (does this domain's grant set violate any
   existing platform `neverallow` rule - e.g. the standard
   "coredomain may not X" rules) - requires the same real build.
3. Confirming `vendor_qti_diag`'s real gid value on THIS device (the
   real-device evidence already has it: 2901, per
   `DEVICE_EFS_READONLY_PROCEDURE.md`) is what `group guacamole_efsd
   vendor_qti_diag` in the `.rc` file resolves to at the AOSP-build
   level (should be automatic via the platform's own group definition,
   not something this project defines, but worth confirming on first
   real build).

## L — Settings UI design

LineageOS Settings, a new preference screen
(`EfsKoreanCarrierSettingsFragment`, not yet written - Kotlin/Java,
`packages/apps/Settings`-style, out of this repo's own tree) bound to
`IGuacamoleEfsd` via a thin repository class
(`GuacamoleEfsdRepository`) that:
- Calls `getStatus()` on fragment resume, maps `GuacamoleEfsdState` to
  UI copy (never raw enum names) - `MAPPING_BLOCKED` shows "Korean
  carrier support: investigation in progress" with Apply DISABLED,
  never a raw error string.
- Exposes SIM1/SIM2 x {Auto, SKT, KT, LG U+} as two independent
  `ListPreference`s, each backed by `Slot`/`Carrier` enum values only -
  no free text, no path/ID ever reaches the UI layer (matches every
  other layer's "no raw path" discipline).
- `Apply` requires an `AlertDialog` confirmation before calling
  `applyCarrier()`, exactly matching item L's "apply 전 confirmation"
  requirement.
- `restoreOriginal()` behind its own separate, equally-confirmed
  button.
- Surfaces `rebootRequired` as a persistent banner + a "Reboot now"
  action, never an automatic silent reboot.
- On `HARD_STOP`/`MUTATION_UNKNOWN`, shows a distinct, non-dismissable
  (until acknowledged) warning state directing the user to further
  support - never presented as an ordinary "apply failed, try again"
  (there is no "try again" from this daemon's own design - see
  `restoreOriginal()`'s fresh-session-only recovery path).

None of this Kotlin/Java/XML has been written yet - it depends on a
real Settings app module tree this repository doesn't contain, and
writing untested UI code here would carry a very high risk of subtle
build/behavior errors this session cannot catch.

## 38/39/40 carrier-index mapping — experiment plan (not yet run)

Per explicit instruction, this mapping is NOT to be determined by
guessing, and no runtime experiment has been executed this pass. Plan,
for when Gate W1 (or a narrower "PDC-only, no EFS write" sub-gate)
authorizes it:

1. **Precondition**: `known_profiles.h`'s KT/LGU profiles (added this
   pass) let `RunLoadCarrier()` load either onto the real device -
   itself still gated (KT/LGU real loads explicitly prohibited this
   pass, host/fake-client tests only).
2. **Step A (PDC-only, no EFS write)**: `RunLoadCarrier(client,
   Carrier::kSkt, ...)` (already real-device-proven, Phase C0) followed
   by `RunLoadCarrier(client, Carrier::kKt, ...)` and `Carrier::kLgu` -
   load ALL THREE Korean PDC configs (load-if-absent semantics already
   proven), confirming via `GetConfigInfo` (never `ListConfigs`) that
   all 3 config IDs (the real SHA-1 values in
   `PHASE_D2_EFS2_WRITE_RESEARCH.md`'s Section C) are now present. This
   step touches ONLY PDC, never EFS2/mcfg_sel_db.xml - lower risk than
   any EFS write, and does not require the index mapping to be known
   at all.
3. **Step B (read-only EFS, already-proven infrastructure)**: run
   `snapshot-mcfg-readonly` again (the SAME command already
   real-device-PASSed 3 times this project) and diff
   `mcfg_sel_db.xml`'s SHA-256 against the frozen baseline
   (`evidence/mcfg_sel_db_2026-09-24_live_baseline.xml`) - if the
   modem's OWN internal MCFG selection logic writes anything to
   `mcfg_sel_db.xml` or a sibling file as a SIDE EFFECT of a PDC Load
   (unconfirmed either way today), this step would reveal it, again
   without any write from this project's own tooling.
4. **Step C (the actual mapping observation, still design-only)**:
   with all 3 Korean PDC configs present, use PDC's `Set Selected
   Config` (NOT YET IMPLEMENTED anywhere in this codebase - Section H
   explicitly scoped OUT `Select`/`Activate`/`Deactivate`/`Delete`, per
   Phase C0's own original constraint, carried forward) to select KT's
   config specifically, then re-run `snapshot-mcfg-readonly` and
   inspect whether `mcfg_autoselect_by_uim`/`mcfg_setting*`/any
   `mcfg_sw_muxd_version_*` file changed in a way that names a specific
   index (e.g. a new `mcfg_sw_muxd_version_N` file appearing where `N`
   is the just-selected carrier's real index). Cross-reference against
   the ALREADY-OBSERVED `mcfg_sw_muxd_version_39` /
   `mcfg_sw_muxd_version_Subscription01_38` real evidence.
5. **Decision rule**: the mapping is considered CONFIRMED only once a
   `Set Selected Config` for a SPECIFIC, uniquely-identified carrier
   (SKT/KT/LGU, by its own real config ID from step A) is followed by a
   NEW or CHANGED `mcfg_sw_muxd_version_N` file whose `N` is
   observable, read-only, with no ambiguity about which carrier caused
   it (e.g. run SKT alone first, observe, THEN KT alone, observe the
   DELTA). This is an observational, read-only-on-the-EFS-side
   experiment - `Set Selected Config` is the only new WRITE-adjacent
   PDC primitive it needs, and it needs its OWN wire-layout research
   and host tests before ANY real use, exactly like every other
   primitive in this project.
6. Until this experiment actually runs and produces a real, cited
   result, `CarrierIndexMappingState::kConfirmed` remains unreachable
   in this codebase, and `korea_sel_db_patcher.h`'s
   `GenerateKoreaPatch()` continues to return `kBlockedMapping`
   unconditionally.

This experiment is NOT scheduled or authorized by this document - it
is a plan for the next explicitly-authorized real-device round, per
the same one-continuous-session, no-individual-command-authorization
pattern this whole project has used since Cycle 1.
