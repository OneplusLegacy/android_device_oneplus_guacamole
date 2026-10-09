# guacamole_pdc_probe (Phase B / B2 / C0)

QMI PDC (Persistent Device Configuration) tool for OnePlus 7 Pro
(`guacamole`). Part of the `guacamole_efsd` research project.
`list`/`info`/`selected` are read-only (Phase B/B2 - see
`PHASE_B_FINDINGS.md`). `load-skt` is the one gated write command
(Phase C0 - see `PHASE_C0_DESIGN.md` / `PHASE_C0_FINDINGS.md` for the
full design, evidence, and READY_FOR_C0_DEVICE_TEST verdict).

## What this does

```
guacamole_pdc_probe list
guacamole_pdc_probe info <hex-config-id>
guacamole_pdc_probe selected
guacamole_pdc_probe load-skt
```

`list` issues QMI PDC "List Configs" and prints every config's type and
ID. `info` issues "Get Config Info" for one config ID (as printed by
`list`) and prints its total size, description, and version. `selected`
issues "Get Selected Config" and prints the active/pending config IDs
(or `NONE`). `load-skt` runs the full Phase C0 state machine: verifies
the on-device stock SKT MBN's identity (fail-closed on any mismatch),
checks it isn't already loaded, records the current selected config,
transfers it via PDC Load Config in 1024-byte chunks with per-chunk
consistency checks, verifies it now appears in List Configs, and
re-checks that the selected config did NOT change as a side effect.

## What this absolutely does not do

No Set Selected Config, Activate/Deactivate Config, Delete Config, EFS2
WRITE/UNLINK, `mcfg_sel_db.xml` edits, `mcfg_autoselect_by_uim` edits,
`mcfg_rfs_sw_digest_version` edits, `/nv/item_files/mcfg/cards/*`
deletion, modem restart, or automatic reboot - not even `load-skt`. This
binary has no code path that can do any of those things - it isn't a
matter of a flag being off. `load-skt` also cannot be pointed at an
arbitrary file: the SKT MBN's path is a hardcoded constant in
`known_profiles.h`, not a CLI argument.

## Build

This is only wired into `guacamole`'s own `device.mk`
(`PRODUCT_PACKAGES_DEBUG += guacamole_pdc_probe`), not into
`sm8150-common`, so sibling devices on this platform don't pick it up.
It's also gated to userdebug/eng in sepolicy (`sepolicy/vendor/
guacamole_pdc_probe.te`, wrapped in `userdebug_or_eng()`).

```
source build/envsetup.sh
lunch lineage_guacamole-userdebug   # or your PixelOS equivalent target
m guacamole_pdc_probe
```

Host-side unit tests (transport-independent TLV/PDC codec + the full
Phase C0 state machine against a fake client - see
`PHASE_C0_FINDINGS.md` for exactly what is and isn't covered):

```
atest guacamole_pdc_probe_test
# or: m guacamole_pdc_probe_test && \
#     out/host/linux-x86/nativetest64/guacamole_pdc_probe_test/guacamole_pdc_probe_test
```

## Run

```
adb root
adb push out/target/product/guacamole/vendor/bin/guacamole_pdc_probe /vendor/bin/  # if not already flashed via a full build
adb shell /vendor/bin/guacamole_pdc_probe list
adb shell /vendor/bin/guacamole_pdc_probe info <hex-id-from-list>
adb shell /vendor/bin/guacamole_pdc_probe selected
```

Real device results already obtained (Phase B/B2): `list` returned 25
SOFTWARE configs; `info` succeeded (Common-Commercial:
`0bd71c00fc30d2ff8af86a8b16d1e01f69e6b493`, size 24508, version
134285321); one special entry (`30353035`) is rejected by the modem on
`info` (`INDICATION_REJECTED`, exit 7 - not a bug, the modem itself said no).

**Recommended order before ever running `load-skt`:**

```
1. guacamole_pdc_probe selected        <- read-only, confirms Phase B2 works first
2. (only if 1 looks sane) guacamole_pdc_probe load-skt
3. guacamole_pdc_probe list / info <new-id> / selected   <- record ALL output
4. manually reboot                     <- never automated by this tool
5. guacamole_pdc_probe list / info <new-id> / selected   <- again, after reboot
```

`selected-before` and `selected-after` (printed by `load-skt` itself,
steps 1 and the post-load check) must match, or `load-skt` will already
report `FAILED` even though the transfer succeeded - see
`PHASE_C0_DESIGN.md` §"Safety mechanisms" item 7.

First-run checklist (see `PHASE_B_FINDINGS.md`/`PHASE_C0_FINDINGS.md`
"남은 blocker"/"§5" for why each of these matters):

1. `adb shell ls -Z /vendor/lib64/libqmiservices.so /vendor/lib64/libqmi_cci.so`
   - confirm their real SELinux label and prune `guacamole_pdc_probe.te`'s
     `same_process_hal_file` allow if it turns out to be wrong.
2. Watch `adb logcat` / `dmesg` while running `list` for the first time -
   this is the first live call to `qmi_client_init_instance()`/
   `pdc_get_service_object_internal_v01(1, 15, 6)` against real hardware.
3. If a command returns `INIT_FAILED`, check stderr - it prints exactly
   which (major, minor, tool) triple was tried and why it failed (now
   always just `(1, 15, 6)` - see `PHASE_B_FINDINGS.md`'s code-review
   addendum for why the fallback list was removed).
4. If `list` returns `TIMEOUT`, that alone is diagnostic information (see
   PHASE_B_FINDINGS.md item on rild/PDC concurrency) - it does not mean
   the tool crashed or something is broken.
5. If `load-skt` returns `UNKNOWN_RESULT` (exit 8), read the printed
   `message` field carefully - it already tells you whether a
   post-timeout List Configs re-check found the config or not. Do not
   re-run `load-skt` reflexively; check `list`/`selected` by hand first.

## Exit codes

| Code | Meaning |
|---|---|
| 0 | success |
| 1 | usage error (bad args) |
| 2 | PDC client init failed (service object / `qmi_client_init_instance`) |
| 3 | request send failed (`qmi_client_send_raw_msg_sync` itself errored) |
| 4 | request rejected at the synchronous-ack stage (modem returned a non-zero Operation Result before any indication arrived) |
| 5 | timeout waiting for the indication |
| 6 | malformed/untrusted response - either the sync ack or the indication itself failed to decode safely (see stderr) |
| 7 | indication decoded fine, but its own Indication Result was non-zero (modem rejected the request after accepting it synchronously) |
| 8 | (`load-skt` only) `UNKNOWN_RESULT` - a chunk timed out and a post-timeout List Configs re-check was inconclusive either way; see the printed message |
| 9 | (`load-skt` only) `FAILED` - any Phase C0 safety check, consistency check, or verification step failed; see the printed message for exactly which one |

Codes 4 and 7 are deliberately distinct: 4 means the modem rejected the
request immediately (see `pdc_client.cpp`'s `ParseOperationResultOnly`
check), 7 means it initially accepted the request but the actual async
answer carried a failure. See `PHASE_B_FINDINGS.md`'s code-review
addendum for why this distinction was added.
