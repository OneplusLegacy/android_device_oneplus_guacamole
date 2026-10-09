> Macro B.2 correction: index reuse is valid, including ICE=40 + KT=40.
> The prior uniqueness/collision conclusion below is superseded by
> [MACRO_B2_EVIDENCE.md](MACRO_B2_EVIDENCE.md). Slot/feature protocol is now
> confirmed; exact original EFS restoration remains unconfirmed. Historical
> evidence below records what was known during Macro B, not current gate policy.

# Macro B: local evidence and closed gates (2026-09-29)

This is static analysis, not device validation. No build, host test, adb, QMI
command, reboot, or EFS mutation was executed during this work.

## AUTO is not Common

User-supplied device captures show Common/NONE restored after Common selection
and reboot, while two autoselect files and mcfg_setting/_1 retain changed bytes.
Another clean reboot preserves the differences. These are device observations
reported by the user; no new capture was taken here. PDC selection, exact original
AUTO-state match, and complete original restoration must be tracked separately.
The four-file set is an observation/preimage set, NOT a write whitelist.
No production meaning is assigned to 05/80 or the internal 41-byte field offsets.

## Stock AUTO candidate: confirmed API exists, restoration remains unknown

Local extraction root:
`/home/ccc/dumpyara/working/oneplus7profirmware`.
`vendor/lib64/libril-qc-hal-qmi.so` SHA256:
`492cf327e70532122e0fcb1a16fd26798dd4dbb396de1efd47082112de109fbe`.

`readelf -Ws` confirms these exported AArch64 functions:

- `_Z25pdc_enable_auto_selectionii`, address `0x1d42588`, size 244.
- `_Z15pdc_set_featureP27pdc_set_feature_req_msg_v01`, `0x1d423f4`, size 404.
- `_Z21pdc_deactivate_configj24pdc_config_type_enum_v01`, `0x1d417ec`.

Disassembly (saved under evidence/macro_b) confirms enable_auto_selection calls
set_feature. The latter calls `qmi_client_send_msg_sync` with message ID `0x2e`
and a decoded C request size `0x50`. This is NOT raw TLV length/layout. The helper
zeros that structure; second integer argument is stored at request offset 0;
first argument controls stores of 3/0 at +0x10 and 1/0 at +0x1d, with valid-looking
bytes at +0x0c and +0x1c. Field names, subscription identity, TLV encodings and
firmware side effects are not established by those stores alone. No request
codec, command or API invocation for 0x2e was added.

This is stronger evidence than claiming there is no AUTO API. It still does not
prove this function restores saved original bytes, both subscriptions, or the
observed four files. Required next research: H.41-matched IDL feature schema,
caller/subscription semantics, response/indication routing and an independently
reviewed device experiment. Gate remains `AUTO_RESTORE_PROTOCOL_UNKNOWN`.
Do not invoke reset/deactivate/delete as an assumed AUTO restore.

## Four-file references

A literal binary search in extracted vendor, odm, modem found relevant names in
`modem/image/modem.b19`; no corresponding literal path match in vendor/odm.
`strings -t x` provides:

| String | File offset in modem.b19 |
|---|---|
| /nv/item_files/mcfg/mcfg_autoselect_by_uim | 0x1816cf3 |
| /nv/item_files/mcfg/mcfg_autoselect_by_uim_Subscription01 | 0x1816d1e |
| /nv/item_files/mcfg/mcfg_setting | 0x1816d92 |
| mcfg_setting_1 literal | not found |

MCFG diagnostics reference failure to write settings stores and setting init.
These strings suggest modem-side ownership; they do not prove a caller or the
conditions that reconstruct the original state at boot. A suffix such as `_1`
may be constructed dynamically; absence of a literal does not establish absence
of a code path. No field decoding or four-file restore write is implemented.
`vendor-references.json` deliberately records a narrower NUL-suffix search;
`auto-modem-strings.txt` records strings-tool matches including other delimiters.

## PDC GetInfo 41/0

Primary public protocol source:
https://raw.githubusercontent.com/linux-mobile-broadband/libqmi/main/data/qmi-service-pdc.json

Get Config Info 0x28 uses Type+ID and token; this schema contains no subscription
input for that message. Local BuildGetConfigInfoRequest encodes SOFTWARE with
length-prefixed exact ID and token. No speculative subscription TLV was added.
This public schema is not an exhaustive H.41 vendor IDL (notably the local 0x2e
feature API above is absent), so absence from it alone cannot disprove vendor
extensions.

Local ParseGetConfigInfoIndication reads one uint16 indication result into
`error_status`. Its `error_code=0` is a default, not a second success field sent
by the modem. Sync Operation Result is a distinct status/error tuple. Presence
classification must use the response phase as well as the numeric value.
Only decoded, correlated **async indication** NO_ENTRY=50 counts as ABSENT;
sync 50 and async 41 remain UNKNOWN. Advisory List omission never means absence.

Primary generic error enum:
https://raw.githubusercontent.com/linux-mobile-broadband/libqmi/main/src/libqmi-glib/qmi-errors.h

That generic enum names 41 INVALID_QOS_ID and 50 NO_ENTRY. The generic name for
41 does not explain its H.41 PDC usage and is not grounds to classify absence.
A fixed synthetic ID read is an observation control, not a guaranteed absent ID.
The diagnostics do not grant any Load/Select authorization.

## Offline MBN evidence

Local KT/LGU sizes, full SHA1 and SHA256 match the user-provided identities;
see `evidence/macro_b/mbn-evidence.json`. Printable strings include Commercial-KT
and Commercial-LGU next to MCFG_TRL. These are heuristic evidence only. No
H.41-matched metadata parser/schema was established, so no version parsing was
guessed and no offline field replaces exact runtime GetConfigInfo success.

## Caller authorization deferred to Macro C

Actual Settings AndroidManifest.xml uses sharedUserId android.uid.system.
Existing daemon SELinux grants service find + binder_call to system_app.
Checking UID 1000, a claimed package string, or platform signature alone cannot
uniquely identify Settings among shared-UID clients. No such pseudo-auth check
was added and no extra broad permission was granted.

Production remains unconditionally BLOCKED. Before enabling mutation, choose
and validate a dedicated-identity mediator/service with explicit Binder caller
policy and guacamole product scoping; the Settings front end must route through
that reviewed boundary. A mediator alone does not solve shared-UID caller
identity: its front door needs a separately reviewed identity, not merely
UID1000/package-name assertions. A new signature permission alone is insufficient to
separate packages sharing UID/signature. Cross-Treble service/SELinux rules,
unauthorized-before-backend tests and real authorized-path smoke are Macro C
gates. No 0018 authorization patch is claimed in Macro B.

## Mapping/generator caveat

The exact 57655-byte baseline already has carrier index 40 for ICE. A proposed
KT=40 mapping conflicts with existing records. Do not waive index collision
validation because 38/39/40 were previous hypotheses. Runtime mapping proof and
schema-compatible collision resolution are separate requirements.
