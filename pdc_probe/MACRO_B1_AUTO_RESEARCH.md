> Macro B.2 update: the opaque selector is slot_id. PDC1.9 names match local
> IDL; Get Feature with an explicit token is read-only. Set Feature still CLOSED.
> See [current evidence](../efs_probe/MACRO_B2_EVIDENCE.md). The uncertainty
> statements below are historical; original EFS restore effect remains unknown.

# Macro B.1: H.41 AUTO feature evidence (2026-10-04)

This is static analysis, not a device experiment. No AUTO command was sent.
The exact bytes below describe the subset set by the observed vendor helper;
they do not establish subscription semantics or prove original-state restoration.

## Local evidence

See `evidence/macro_b1/idl-evidence.json` for binary SHA-256, descriptor offsets,
C structure sizes and original descriptor bytes, and `auto-callsite-disassembly.txt`
for bounded disassembly. Prior Macro B evidence remains unchanged.

`libril-qc-hal-qmi.so`:

- `pdc_enable_auto_selection(int,int)` at 0x1d42588 zeroes an 80-byte request.
- Second argument is stored at structure offset 0. Its enum name is unknown.
- First argument nonzero sets uint32 at offset 0x10 to 3 and byte at 0x1d to 1;
  zero sets both to zero. Valid flags at 0x0c and 0x1c are set.
- Optional token-valid at offset 4 is not set by this helper.
- `pdc_set_feature` at 0x1d423f4 sends message 0x2e with request C size 80,
  sync response C size 8 and timeout 10000. It then waits for an indication.
- `qmi_pdc_set_feature_ind_hdlr` at 0x1d3fc8c checks decoded error at offset 0,
  signals a feature condition; no token check was found in this handler.
- OTA helper call sites at 0x1d63564/574 pass (0,0)/(0,1); 0x1d63738/748
  pass (1,0)/(1,1). This supports boolean enable/disable as the first argument.
  It does NOT alone prove that the second is subscription, SIM slot or config type.

`libqmiservices.so` PDC service object VA 0x25708 (file 0x24708):
IDL library 6, major 1, minor 15, service 0x24. Message 0x2e entries reference
message indices 41/42/43 (request/response/indication). Request max wire 82,
response max wire 7, indication max wire 12. Message descriptor array at 0x1fdd8.

| Wire field | Width | C offset | Status |
|---|---:|---:|---|
| request mandatory 0x01 | 4 | 0 | opaque observed selector 0 or 1 |
| request optional 0x10 | 4 | 8, valid 4 | not emitted by stock enable helper; token naming remains inferred |
| request optional 0x11 | 4 | 16, valid 12 | observed enable value 3 |
| request optional 0x13 | 1 | 29, valid 28 | observed enable value 1 |
| sync response 0x02 | 4 | common aggregate | result + error, each u16 |
| indication 0x01 | 2 on wire | decoded enum at 0 | error/result |
| indication optional 0x10 | 4 | 8, valid 4 | correlation/token-shaped field; no invented request token |

Observed enable request, selector 0:
`01 04 00 00 00 00 00 11 04 00 03 00 00 00 13 01 00 01`.
Selector 1 changes only the first value byte. Other optional fields are absent.
The pure codec accepts only these two observed enum values. Parsers reject
unknown/duplicate/truncated fields. They record results, never certify restoration.

## Public schema cross-check and limits

Qualcomm's generated-IDL implementation header defines optional flag 0x40,
last flag 0x80, generic-u32 type 2 and two-byte enum type 5, consistent with
these local descriptors. Source:
[Qualcomm qmi_idl_lib_internal.h](https://github.com/bcyj/android_tools_leeco_msm8996/blob/master/qmi-framework/inc/qmi_idl_lib_internal.h).
The [older generated PDC v1.8 header](https://github.com/bcyj/android_tools_leeco_msm8996/blob/master/qmi/services/persistent_device_configuration_v01.h)
ends at message 0x2c; it cannot supply names/semantics for this H.41 0x2e request.
No newer authoritative named schema was established in this review.

Unresolved: selector meaning, full feature semantics, supported correlation,
concurrency, whether reboot is required, and whether all four saved original
EFS files are restored. Thus AUTO_RESTORE_PROTOCOL_UNKNOWN remains appropriate.
No Deactivate/Delete/reset operation is substituted.

## Closed harness and future evidence evaluator

`enable-auto-selection-once --confirm GUACAMOLE_AUTO_RESTORE_EXPERIMENT_V1`
returns 4 before constructing QCCI. Compile-time debug and root/system/vendor
userdebug-or-eng gates still precede the command. There is NO send branch.
Carrier transport rejects 0x2e even when a different permit is armed.

`AssessAutoRestoreExperiment` is a pure evidence function. It requires verified
changed preimage, Common/NONE before and after, protocol acceptance, all four
exact original files and explicit post-reboot evidence before CONFIRMED.
QMI acceptance with different files is ProtocolAcceptedButOriginalNotRestored.
Missing/invalid evidence is ReviewRequired. Exact match without reboot evidence
is AwaitingRebootVerification. Input booleans are supplied evidence, not runtime
authentication; this evaluator is not connected to Binder or an executor.
