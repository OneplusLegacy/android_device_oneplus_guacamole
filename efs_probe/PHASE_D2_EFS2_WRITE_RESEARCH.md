# Phase D2 / Cycle 2 (Sections A, C, E, F, G) — EFS2 write layer + live baseline freeze

2026-09-24. Builds on Cycle 1 (`PHASE_D1_EFS2_READONLY_FINDINGS.md`,
`DEVICE_EFS_READONLY_PROCEDURE.md`), now CLOSED with three real-device
`snapshot-mcfg-readonly` attempts culminating in a full pass
(`outcome=DONE_WITH_WARNINGS`, `EXIT=0`). This document covers Cycle
2's foundation work: freezing the real Cycle 1 output as the live
baseline (Section A), researching the 3 Korean MBN files (Section C),
and implementing the EFS2 MUTATING wire layer + transaction backend
(Sections E/F/G) — all BEFORE any real device write (Gate W1, not yet
cleared).

## Section A — Cycle 1 live baseline, frozen from the ACTUAL raw log

Source: `~/android/pixelos/efs-snapshot-mcfg-readonly-final.txt` (2664
lines, the real, complete stdout of the third and final
`snapshot-mcfg-readonly` real-device run). Every value below was
decoded directly from that file's own `data_hex=`/`sha256=` lines and
independently re-verified by recomputing SHA-256 from the decoded
bytes — not transcribed from a summary.

```
mcfg_sel_db.xml
  size=57655
  sha256=b81f1d34e8279547c789b6665e82db41a07d53451ea7a15bed7e628eb829f155
  (independently recomputed - MATCH. Full bytes preserved at
  efs_probe/evidence/mcfg_sel_db_2026-09-24_live_baseline.xml)

mcfg_autoselect_by_uim
  size=1, data=05
  sha256=e77b9a9ae9e30b0dbdb6f510a264ef9de781501d7b6b92ae89eb059c5ab743db

mcfg_autoselect_by_uim_Subscription01
  size=1, data=05  (BYTE-IDENTICAL to the base slot)
  sha256=e77b9a9ae9e30b0dbdb6f510a264ef9de781501d7b6b92ae89eb059c5ab743db

mcfg_setting / mcfg_setting_1
  size=41 each, byte-identical to each other
  data=020000000300000001000000000000000001010000000001009a010000000000000000000000000000
  sha256=5a1537866e1fb468c4c6718cbfdad0ad2994ce421a6ba0a97135f9361d3e9621

mcfg_setting_Subscription01 / mcfg_setting_Subscription01_1
  size=41 each, byte-identical to each other, DIFFERENT from the base pair
  data=02000000030000000100000000000000000000000000000300fa000000000000000000000000000000
  sha256=0b3d54bbbcf37ffb70e12348abc1b1d8e173e8a75ac351e04a58c64156ccb4e2

mcfg_rfs_sw_digest_version
  size=32
  data=403971b0c1ab7d3910b4f47a580585041ac53a0ed9cd0e0461d4e4a0aa9484cf

cards/, /mcfg_debug.log, /mcfg_debug_old.log, /enable_mcfg_efslogging
  all diag_errno=2, presence=UNKNOWN (STAT error, absence NOT proven -
  see efs_snapshot.h's presence semantics)
```

**Policy reminder, restated from the review that froze this data**:
`mcfg_setting*`'s binary layout is UNKNOWN. The two raw `0x05` bytes in
the autoselect files are NOT to be interpreted as a boolean without
source-backed evidence. Nothing in this codebase parses or interprets
these bytes — only raw preservation + SHA-256.

Directory enumeration (STEP 2, real device): exactly 34 entries, seq 1
through 34, then the zero-metadata EOD sentinel at seq 35 (see
`efs_dir_lister.h` for that finding). Full real entry list (order =
real seqno):

```
1  mcfg_autoselect_by_uim
2  mcfg_autoselect_by_uim_Subscription01
3  mcfg_def_config_hw_version
4  mcfg_def_config_sw_version
5  mcfg_def_config_sw_version_Subscription01
6  mcfg_default_hw_digest_version
7  mcfg_default_sw_digest_version
8  mcfg_default_sw_digest_version_Subscription01
9  mcfg_hw_config_digest_version
10 mcfg_rfs_hw_digest_version
11 mcfg_rfs_sw_digest_version
12 mcfg_sel_db.xml
13 mcfg_setting
14 mcfg_setting_1
15 mcfg_setting_Subscription01
16 mcfg_setting_Subscription01_1
17 mcfg_sw_config_digest_version
18 mcfg_sw_muxd_version
19 mcfg_sw_muxd_version_12
20 mcfg_sw_muxd_version_151
21 mcfg_sw_muxd_version_154
22 mcfg_sw_muxd_version_241
23 mcfg_sw_muxd_version_242
24 mcfg_sw_muxd_version_39
25 mcfg_sw_muxd_version_65
26 mcfg_sw_muxd_version_8
27 mcfg_sw_muxd_version_Subscription01_12
28 mcfg_sw_muxd_version_Subscription01_151
29 mcfg_sw_muxd_version_Subscription01_154
30 mcfg_sw_muxd_version_Subscription01_241
31 mcfg_sw_muxd_version_Subscription01_242
32 mcfg_sw_muxd_version_Subscription01_38
33 mcfg_sw_muxd_version_Subscription01_65
34 mcfg_sw_muxd_version_Subscription01_8
```

Note `mcfg_sw_muxd_version_39` (entry 24) and
`mcfg_sw_muxd_version_Subscription01_38` (entry 32) - real, on-device
evidence relevant to (but NOT proof of) the LGU=38/SKT=39/KT=40
hypothesis - see Section C below.

## Section B — live `mcfg_sel_db.xml` structure (independently re-verified)

Parsed directly from the frozen evidence file, not from the earlier
chat summary:

```
mcfg_db_ver = "1.2.3"
SelRecord count = 128
MCC 450 rules = 0 (only coincidental substring: an unrelated IIN
  EXAMPLE value "89450201 8945002" inside the policy's own <initial>
  documentation block - NOT a real 450 rule)
IIN 898205 = 0 occurrences
IIN 898206 = 0 occurrences
IIN 898230 = 0 occurrences
mcfg_carrier_index="38" records = 0
mcfg_carrier_index="39" records = 0
mcfg_carrier_index="40" records = 1:
  <SelRecord carrier_name="ICE" country_code="578" iin="iin"
    imsi_3gpp="3gpp_imsi" mcfg_carrier_index="40" volte="true"
    vowifi="true"/>
Fallback (last real SelRecord): <SelRecord carrier_name="Common"
  iin="iin" imsi_3gpp="3gpp_imsi" mcfg_carrier_index="8" />
```

Confirms the prior chat analysis exactly: a Korean SIM on this device
would fall through to Common (index 8) today - not a guess, a directly
re-derived fact from the frozen baseline file.

## Section C — Korean MBN files: FOUND on disk, index-binding: UNRESOLVED

Located at (this environment, NOT the real device -
`~/dumpyara/working/oneplus7profirmware/` is a prior full firmware
extraction for this exact device model, H.41):

```
modem/image/modem_pr/mcfg/configs/mcfg_sw/generic/korea/skt/commerci/mcfg_sw.mbn
  size=41172  sha1=ca9c9dcb8d6d2be6e4f6b49ca3e4b5420769a7d1
  sha256=9ee13411bd223576b0bdd14823929ace3544f1f1c2eab57a23a30038bc33805e

modem/image/modem_pr/mcfg/configs/mcfg_sw/generic/korea/kt/commerci/mcfg_sw.mbn
  size=39544  sha1=79bd2e28f61310fd538c0a5b9bcbd19f143da762
  sha256=0305b51ca7446db9bb16e78964f6c6520739281c228ff596987d23484b9cd101

modem/image/modem_pr/mcfg/configs/mcfg_sw/generic/korea/lgu/commerci/mcfg_sw.mbn
  size=41088  sha1=3a87a2ce830826e7de1f1a8cb2faf5012b4b71bc
  sha256=c84bcf76cb626debee291d6ee9b87e83b7c686070efe541c28d6ef46b84819b2

modem/image/modem_pr/mcfg/configs/mcfg_sw/generic/korea/tta/commerci/mcfg_sw.mbn
  (present, not in the original 3-carrier list - generic Korea/TTA
  reference config; size=39048, sha1=261d9efbc6a8ebeaebad0b1b08e9ecc9f1a4f021)
```

**Cross-check against already-proven Phase C0 work**: the SKT file's
SHA-256 here (`9ee13411...`) is BYTE-IDENTICAL to
`pdc_probe/include/known_profiles.h`'s `GetSktProfile()` - the exact
same file already confirmed to PDC-Load successfully on the real
device in Phase C0. This is not a new/different file - it's the same
one, now cross-verified from a second, independent extraction of the
same firmware.

**PDC config ID scheme (already CONFIRMED in
`research_pdc_load_config_id_scheme.md`, reused here unchanged)**:
config ID = SHA-1 of the file's full bytes. The three SHA-1 values
above ARE therefore the expected PDC config IDs for SKT/KT/LGU, usable
as a POST-LOAD cross-check (never a hardcoded success condition - real
`ListConfigs`/`GetConfigInfo` output remains authoritative, matching
the existing Phase C0 policy).

Each MBN embeds a readable `carrier_policy.xml` fragment confirming
carrier identity textually (`"Carrier Policy with SKT requirements"`,
`policyman/configurations/Carrier/SKT/subsidized/carrier_policy.xml`,
etc. - SKT/KT/LGU each independently self-identify).

**UNRESOLVED, genuinely gating (not just under-researched)**: nothing
found anywhere in this firmware extraction maps a `mcfg_carrier_index`
number (38/39/40) to a specific `mcfg_sw.mbn` file. Checked:
- `mcfg_sw/mbn_sw.txt` (the firmware's own 243-entry manifest of every
  `mcfg_sw.mbn` path, incl. all 4 Korea variants) - paths only, no
  index numbers.
- `mcfg_sw/mbn_sw.dig` - binary digest file, no readable mapping.
- `strings` output of all 4 Korean MBNs - no numeric carrier-index
  string anywhere.
- No other manifest/registry file exists near `mcfg_sw/` or
  `mcfg_hw/`.

**Conclusion: the LGU=38/SKT=39/KT=40 hypothesis remains PLAUSIBLE
(supported by the real on-device `mcfg_sw_muxd_version_39`/
`_Subscription01_38` evidence) but NOT CONFIRMED**, and per explicit
review instruction, Section D (writing a real Korea `SelRecord` block
into `mcfg_sel_db.xml`) is BLOCKED until this is resolved — most likely
requiring either real-device runtime observation (load a profile,
observe which index activates) or further modem-side binary reverse
engineering, both out of scope for this pass. **Nothing in this
codebase writes to `mcfg_sel_db.xml`'s content yet.**

## Sections E/F/G — EFS2 write wire layer, transaction backend, write policy

**Wire layout** (`efs_write_wire.h`/`.cpp`), cross-checked via a fresh
2026-09-24 WebFetch of `forth32/qtools`' `efsio.c`/`efsio.h` (verbatim
function bodies, not a paraphrase):

```
EFS2_DIAG_WRITE = 5
  request:  { int32 fd; uint32 offset; char data[size]; }        (8+size bytes)
  response: { int32 fd; uint32 offset; int32 bytes_written; int32 diag_errno; }

EFS2_DIAG_OPEN = 2 (SAME code as the read-only OPEN - oflag distinguishes intent)
  request:  { int32 oflag; int32 mode; char name[100]; }
  response: { int32 fd; int32 diag_errno; }  -- IDENTICAL shape to the
    read path's own EfsOpenResult/ParseOpenResponse, reused verbatim.

EFS2_DIAG_CLOSE = 3 - reused verbatim from the read path (not
  inherently read-only).
```

Scope, exactly as authorized: OPEN (one FIXED oflag,
`O_WRONLY|O_CREAT|O_TRUNC` = 0x241 - truncates before writing, so a
shorter replacement never leaves stale trailing bytes), WRITE, CLOSE.
**RENAME and UNLINK are NOT implemented** - the same qtools fetch found
`efs_close`/`efs_unlink` present but `efs_rename` absent from that
reference entirely, so there is no confirmed wire layout for RENAME to
build a temp-file+atomic-rename pattern on. **PUT/PUT_DEPRECATED are
NOT implemented** - no confirmed atomicity/safety advantage over
OPEN+WRITE+CLOSE. `efs_wire.h` (the read-only layer) is completely
untouched.

**Transaction backend** (`efs_transaction.h`'s new `ApplyPatchPlan()`):
read current → verify against `PatchPlan.preimage` (fail closed, NO
write if stale) → write proposed → **unconditional** read-back (never
trusts the write response as the success signal) → exact byte+SHA-256
match required → on any mismatch, exactly one rollback write +
its own read-back verification → `TransactionState::kRollbackFailedHardStop`
if that also fails (caller MUST NOT retry automatically). An optional
`ITransactionLog` sink records `{target, preimage_sha256,
proposed_sha256, timestamp, transaction_id, state,
verification_result, rollback_state}` per transaction -
`InMemoryTransactionLog` is the host-testable implementation available
today; a persistent (file-backed) implementation is production-daemon
territory (Section J, not yet started).

**Write-target whitelist** (`efs_write_targets.h`): a SEPARATE
`EfsWriteTarget` enum from the read path's `EfsTarget`, exactly ONE
member today (`kMcfgSelDb`), matching the explicit "first live
mutation target is `mcfg_sel_db.xml` alone" scope. `ApplyPatchPlan()`
resolves ALL of its read/write/read-back/rollback paths through this
one write-target parameter — a plan built against a different
`EfsTarget` cannot redirect where the write actually lands (verified
directly by `ApplyPatchPlan.FixedWriteTargetOverridesPlanTarget`).

**No CLI command reaches any of this yet** - `main.cpp` was
deliberately NOT given a new subcommand this pass, so there is zero way
to trigger a real write even by running the existing binary. This is
stricter than what was strictly required, kept as an extra safety
margin until Gate W1 clears.
