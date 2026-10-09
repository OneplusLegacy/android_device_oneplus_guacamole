> Historical B.2 findings. Current AUTO persistence/pre-SKT baseline status is documented in MACRO_B4_PREWRITE_GATE.md; B.2 unconfirmed-effect wording is superseded.

# Macro B.2 — generation evidence / read-only feature protocol

## Index correction

Previous `KT=40 REJECTED because ICE=40` and global index uniqueness claims were
incorrect. Exact original XML SHA256 b81f1d34e8279547c789b6665e82db41a07d53451ea7a15bed7e628eb829f155
contains 128 records and repeated indices; e.g.4 nine times,31 eleven,36 seven.
ICE40 is preserved. Korean identity predicate rules are distinct records.

The local 20261004-160938-pdc-index-inventory contains exact GetInfo observations
for LGU/SKT/KT versions08012608/0801270a/08012800. The carrier bytes are38/39/40.
See `evidence/macro_b2/mapping.json` for counts, source hashes and observations.
User additionally reports muxd suffix/version correlation. Only actually read
local files are marked verified in the JSON; no new device query was performed.
Generation-only mapping is LGU38, SKT39, KT40(TTA bucket). This does not validate
SIM/network service, authorize EFS writes, or prove an AUTO restore effect.
`PRE_LOAD_FULL_LIST_NOT_AVAILABLE`: the empty OLD_LIST comparison is invalid;
no profile eviction conclusion is made from list count25.

## Public/local feature agreement

Primary authored document: Telit LM940 QMI Command Reference Guide r1,
80545ST10798A, sections11.2.28–31, pages485–496,
[public mirror](https://pdfcoffee.com/telit-lm940-qmi-command-reference-guide-r1-pdf-free.html).
GET0x2d / SET0x2e address a uint32 slot in TLV01. Get's optional token10 is echoed
in its completion indication. Mode11 values0/1/2/3 are Disable/IIN/IMSI/Hybrid;
Hybrid prefers IIN then IMSI. Carrier12 contains LGU38/SKT39/TTA40. Flex13 is one
byte; refresh14 is a four-byte disabled/enabled enum. Sync acceptance and async
completion are separate. The document's Set indication subsection prints0x2d;
H.41's actual indication service table uses0x2e, so the local table takes priority.

`../pdc_probe/evidence/macro_b2/feature-idl.json` records local request/response/
indication indices38–43, descriptors and blob hash. Get request C-size12/raw-max14;
Get indication C-size80/raw-max80. Mandatory indication result is two wire bytes.
Local H.41 optional15–18 are four bytes,19–1b two bytes: validated as extensions
without assigning undocumented names. Unknown fields, duplicate fields, bad
widths and nonboolean flex are rejected. Unknown enum values are retained as raw
observations and cannot certify enabled state.

The stock helper's second argument is slot_id; true sends Hybrid3 + flex1,
false Disable0 + flex0. Its Set request sends neither carrier, refresh nor token.
That codec remains unsent. Get separately uses the documented optional token,
not an invented Set token. Slot0/1 are the only tool inputs; physical SIM routing
is not inferred solely from the slot number.

## Independent states and gates

AUTO_FEATURE_PROTOCOL_CONFIRMED describes the API semantics, not byte restoration.
AUTO_ORIGINAL_EFS_RESTORE_EFFECT_UNCONFIRMED remains the live-effect status.
Feature classification requires both mode and flex: Disable/false disabled,
known enabled selection mode with true flex enabled, missing/unknown/mixed unknown.
EFS original-state equality still requires all four validated full preimages.
Get Feature performs no EFS read and prints ORIGINAL_AUTO_EFS_UNKNOWN.
Set Feature entry remains blocked before QCCI initialization. Production Binder,
Settings, process write gate and old controlled Load/Select state machines unchanged.

## Offline generator

Exact baseline hash, structure and Common fallback remain mandatory. Indices
may repeat; matching predicates/duplicate Korean names are not accepted in a new
original. Current input must equal the entire exact original or canonical output.
Thus conflicting PLMN/IIN, changed index, duplicate or partial Korean records and
unrelated edits fail closed. AlreadyPatched requires full-byte canonical equality.
Original bytes are preserved by insertion only, SKT/KT/LGU before Common. Country
code410 is retained from existing generator policy. Proposed XML is review data,
not a production payload/allowlist or live write command.
