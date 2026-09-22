# guacamole_pdc_probe (Phase B)

Read-only QMI PDC (Persistent Device Configuration) observation tool for
OnePlus 7 Pro (`guacamole`). Part of the `guacamole_efsd` research project.
See `PHASE_B_FINDINGS.md` for the full writeup, evidence, and the GO /
GO WITH CONDITIONS / NO-GO verdict on proceeding to Phase C.

## What this does

Two commands, both pure reads against the modem's already-persisted PDC
config database:

```
guacamole_pdc_probe list
guacamole_pdc_probe info <hex-config-id>
```

`list` issues QMI PDC "List Configs" and prints every config's type and
ID. `info` issues "Get Config Info" for one config ID (as printed by
`list`) and prints its total size, description, and version.

## What this absolutely does not do

No Load Config, Set Selected Config, Activate/Deactivate Config, Delete
Config, EFS2 WRITE/UNLINK, `mcfg_sel_db.xml` edits, `mcfg_autoselect_by_uim`
edits, `mcfg_rfs_sw_digest_version` edits, `/nv/item_files/mcfg/cards/*`
deletion, modem restart, or automatic reboot. This binary has no code
path that can do any of those things - it isn't a matter of a flag being
off.

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

Host-side unit tests (transport-independent TLV/PDC codec only - see
`PHASE_B_FINDINGS.md` for what is and isn't covered):

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
```

First-run checklist (see `PHASE_B_FINDINGS.md` "남은 blocker" for why each
of these matters):

1. `adb shell ls -Z /vendor/lib64/libqmiservices.so /vendor/lib64/libqmi_cci.so`
   - confirm their real SELinux label and prune `guacamole_pdc_probe.te`'s
     `same_process_hal_file` allow if it turns out to be wrong.
2. Watch `adb logcat` / `dmesg` while running `list` for the first time -
   this is the first live call to `qmi_client_init_instance()`/
   `pdc_get_service_object_internal_v01(1, 15, 6)` against real hardware.
3. If `list` returns `INIT_FAILED`, check stderr - it prints exactly which
   (major, minor, tool) triple was tried and why it failed (now always
   just `(1, 15, 6)` - see `PHASE_B_FINDINGS.md`'s code-review addendum
   for why the fallback list was removed).
4. If `list` returns `TIMEOUT`, that alone is diagnostic information (see
   PHASE_B_FINDINGS.md item on rild/PDC concurrency) - it does not mean
   the tool crashed or something is broken.

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

Codes 4 and 7 are deliberately distinct: 4 means the modem rejected the
request immediately (see `pdc_client.cpp`'s `ParseOperationResultOnly`
check), 7 means it initially accepted the request but the actual async
answer carried a failure. See `PHASE_B_FINDINGS.md`'s code-review
addendum for why this distinction was added.
