# Macro B.4 — AUTO persistence findings / Korea sel_db pre-write gate

## Evidence scope

These are user-reported actual H.41 device findings. Raw captures were not independently collected/re-read by Codex in this round. Machine-readable transcription: evidence/macro_b4/auto_persistence.json. Tests check model behavior and preserve this record; passing a host test does not independently prove a modem observation.

The exact stock-equivalent slot0 enable wire was:
`010400000000001104000300000013010001` (0x2e, slot0, HYBRID3, flex=true).

|Observation|Before|Immediate|After reboot|
|---|---|---|---|
|slot0 feature|mode0 / flex=false|mode3 / flex=true|mode3 / flex=true|
|slot1 feature|mode0 / flex=false|unchanged|unchanged|
|PDC|Common / NONE|unchanged|unchanged|
|main autoselect|0x80|0x87|0x87 retained|
|main setting offset0x04|0x00|0x03|retained|
|main setting offset0x05|0x00|0x01|retained|
|mcfg_setting_1|before value|unchanged|synchronized to main setting|
|Subscription01 family|before values|unchanged|unchanged|
|sel_db / muxd / digest|before values|unchanged|unchanged|

No other snapshot file changed immediately. After reboot only mcfg_setting_1 additionally changed, matching mcfg_setting. This describes observed snapshots, not every unobserved modem EFS file.

The historical reference is now called **pre-SKT stock EFS baseline**: autoselect0x05, setting offset0x04=3/0x08=1. GetFeature was not collected then. That reference MUST NOT be equated with the current HYBRID+flex feature state. Legacy OriginalAutoPreimage/AutoSelectionState identifiers remain source compatible, but mean exact comparison to that historical saved reference, not a byte-to-feature schema.

Current conclusion: slot0 AUTO feature enable persisted, while the pre-SKT stock EFS bytes were NOT restored. The model's live-effect description is AUTO_FEATURE_PERSISTENT_PRE_SKT_EFS_NOT_RESTORED. Common/NONE is neither proof of historical EFS restoration nor sufficient proof of AUTO mode.

## Inferences, explicitly not schema

For the observed H.41 transitions, setting offset0x04 strongly correlates with selection_mode=3 and offset0x05 with flex_mapping=true. This is empirical correlation, not schema-confirmed encoding; no production parser/writer assigns these fields.

mcfg_setting_1 is plausibly a primary mirror/backup, not established as slot1. Subscription01 is strongly associated with a second subscription, but no direct slot1 mutation was performed. No slot-to-file mapping is hardcoded from these hypotheses.

## AUTO experiment permanently closed for this round

`kDeviceAutoSlot0SendOpen=false`. Actual device entry is wrapped in RunAutoSlot0DeviceGate before any journal access/QCCI construction. The Android transport guard additionally refuses to arm when closed. Existing host-only protocol/state simulations remain for regression; their fake sends cannot open the real device gate.

Existing generic and slot1 routes remain closed. The init fixed slot0 service may still exist for compatibility but now returns AUTO_EXPERIMENT_CLOSED without sending. No new command to retry, disable, clear markers or mutate slot1 is provided. **slot0.pending must remain permanently**; do not delete it even after apparently successful feature enable or source rollback. This round never accesses the device or modifies that file.

## Exact sel_db transition under review

Only this full-byte SHA-256 transition is an offline future candidate:

```text
source: 57655 bytes
b81f1d34e8279547c789b6665e82db41a07d53451ea7a15bed7e628eb829f155
→
target: 58941 bytes
b233088abcb1ea75df1c0d127e85508353728d40f180f230823ffeae5bd1a896
```

Target policy remains LGU38/450-06/898206, SKT39/450-05/898205, KT40/450-08/898230; all three volte=true/vowifi=false. Generator ordering remains SKT, KT, LGU before Common. The generator preserves 128 original records byte-for-byte and adds three (131 total). Existing ICE40 remains untouched. No XML/proposal mutation or regeneration was executed in this round.

New pure module: tools/korea_sel_db_prewrite_gate.py. It has no CLI, filesystem, QCCI, DIAG, subprocess or writer integration. It consumes full source/proposal bytes plus typed caller-supplied evidence and always reports device_write_authorized=false. Hash equality uses actual bytes, not caller-reported hash strings. Already-proposed live bytes are not accepted as the original pre-write candidate; unknown/partial source or target fails closed.

Additional prerequisites:
- verified same-invocation live full read by a fresh process with nonce/provenance and completed reap;
- verified PDC active exact Common, pending NONE;
- current slot0 mode3/flextrue and slot1 mode0/flexfalse (drift blocks; evaluator never repairs AUTO);
- exact retained B.3 slot0.pending marker AND reviewed B.4 experiment provenance;
- separate EFS transaction recovery scan clean;
- before-image file fsync, directory fsync, independent backup reread/full bytes/hash matching current source, transaction ID/provenance.

The marker is a required reviewed historical AUTO record, not an unresolved EFS transaction. It stays in its existing AUTO directory. EFS backup/pending/final records use the separate EFS store; no gate reuses/clears the AUTO marker. Marker existence alone is not accepted: malformed/missing/unreviewed records block.

The evaluator cannot prove the truth of supplied durability, freshness, selected or review assertions. A future trusted device orchestrator must obtain and bind them to the actual transaction. Offline candidate=true is NOT a transport permit, W1 FINAL or write approval. No adapter from this module to Binder/worker is added.

## Future transaction design and atomicity limits

Retain existing ApplyProcessPatchPlan/FileTransactionBackend/ProcessEfsExecutor design:
1. Exclusive transaction ownership and fail-closed recovery scan.
2. Fresh-process baseline, full exact source verification, worker exit/reap.
3. Durable before-image using exclusive creation, file+directory fsync, independent reread/hash verification.
4. Durable pending EFS record with identity and exact source/target hashes before any possible mutation.
5. At most one logical write phase in a new process; no blind resend of any ambiguous write. Reap before progressing.
6. Independent fresh-process full readback. Match target → confirmed; match source → unchanged; different fully verified content → proven mismatch; failed/incomplete read → HARD_STOP/recovery pending.
7. Rollback only after proven mismatch AND explicit operator opt-in AND verified durable preimage. Separate fresh rollback-write process, then separate fresh verification process. No automatic rollback on timeout/read failure.
8. Durable final record; torn/unknown/final-record failure stays unresolved and blocks new work. Reboot persistence is a later verification gate.

“Atomic/fail-closed” refers to exclusive host-side transaction authorization/checkpointing and fail-closed recovery decisions. This does **not** establish atomic modem file replacement: a 58KB EFS file may require multiple wire operations and power loss can leave partial content. Existing WriteExclusive leaves torn files visible rather than assuming completion. No unproven rename/atomic-swap semantics are invented. Proven modem write behavior and recovery remain prerequisites before opening a writer.

Numeric PID inequality is not freshness proof. Spawn+exec lifecycle, nonce/identity validation, complete reap ordering and no overlapping worker are required. Current PosixProcessWorkerLauncher rejects non-read operations before spawning. Its write closure is unchanged.

AUTO feature and selected state must not be altered by the future sel_db operation; no PDC SetFeature/SetSelected/Activate dependency is introduced. Observe them before/after rather than attempting correction. Production guacamole_efsd stays BLOCKED_MAPPING; Settings/EFS device write remains closed.

## Tests and user validation

Written, not executed by Codex:
- PDC: two device-gate closure tests prove zero fake operations/journal access even with valid preflight or retained marker; historical wire tests retained.
- EFS C++: synthetic observed-delta fixture demonstrates persistent HYBRID effects and post-reboot setting mirror do not match pre-SKT bytes or declare original restore.
- Python: exact source/target comparator, future-candidate-but-no-write, freshness/reap/provenance, Common/NONE, feature drift, marker presence/review, separate EFS recovery, durable backup/reread, readback classification and explicit rollback review; B.4 evidence transcription checks (12 tests).

User commands (not run):

```bash
m guacamole_pdc_probe_test guacamole_efs_probe_test guacamole_pdc_carrier_probe guacamole_efs_probe guacamole_efsd
out/host/linux-x86/nativetest64/guacamole_pdc_probe_test/guacamole_pdc_probe_test --gtest_output=xml:/tmp/guacamole-b4-pdc.xml
out/host/linux-x86/nativetest64/guacamole_efs_probe_test/guacamole_efs_probe_test --gtest_output=xml:/tmp/guacamole-b4-efs.xml
PYTHONDONTWRITEBYTECODE=1 python3 device/oneplus/guacamole/efs_probe/tests/korea_sel_db_generator_test.py
PYTHONDONTWRITEBYTECODE=1 python3 device/oneplus/guacamole/efs_probe/tests/korea_sel_db_prewrite_gate_test.py
```

This patch changes no SELinux policy. Existing matched system_ext/vendor deployment requirements still apply. Do not retry AUTO or execute any EFS write after tests; tests only assess host logic.
