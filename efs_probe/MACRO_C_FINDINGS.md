# Macro C / W1 integrated experiment boundary

Device evidence supplied by the operator (not re-executed by this source round):
Common/NONE, all four exact PDC profiles verified, slot0 HYBRID(3)/flex=true
persistent after reboot, slot1 DISABLE(0)/flex=false unchanged. The exact immutable
AUTO marker is `slot0.pending`, SHA-256
`e1b98ba2352d83550fcb64c5e27a808fbd6c40eb0d73dd30366b3d4c7f2e0b64`.
The code contains no operation that updates/removes this file.

AUTO experiment is CLOSED. The slot0 experiment's existing source gate remains
false. No AUTO disable, slot1 write, retry, automatic recovery or reboot is added.

Main autoselect changed 0x80 -> 0x87. Setting offsets 0x04 and 0x05 changed
00 -> 03 and 00 -> 01. Changes persisted; setting_1 subsequently mirrored the
primary. Subscription01 files, sel_db, muxd and digest did not change. This is
empirical correlation, not a decoded persistent schema. setting_1 is not called
a slot1 file. The pre-SKT stock EFS baseline (autoselect 0x05) did not have a
GetFeature observation and is not equated with current HYBRID/flexible mapping.
Common selection is not evidence of restoring that original EFS state.

The reviewed proposal is embedded verbatim after build-time validation. It has
58941 bytes, 131 records and SHA-256
`b233088abcb1ea75df1c0d127e85508353728d40f180f230823ffeae5bd1a896`.
Removing the contiguous SKT/KT/LGU insertion must yield exactly 57655 bytes,
128 records and SHA-256
`b81f1d34e8279547c789b6665e82db41a07d53451ea7a15bed7e628eb829f155`.
Repeated carrier indices are allowed; ICE=40 is preserved. This mapping evidence
is not KT/LGU physical-SIM network validation.

## Runtime boundary

Only the debug-packaged guacamole_efs_experiment has a device write entry.
Its user-build main is a disabled stub. Init is the only domain transition source.
Each public init service has fixed argv. Internal worker commands additionally
require the exact execution context, a private inherited nonce pipe, inherited
journal lock inode, exact retained checkpoint and a one-use worker entry marker.
There is no payload/path input. The writer target is EfsWriteTarget::kMcfgSelDb.

Apply: exact environment/evidence/radio/artifact/journal checks -> fresh baseline
worker (reaped) -> durable before-image/reread -> durable pending -> fresh write
worker (reaped) -> fresh independent readback -> retained verified record.
Unknown/unreadable state blocks. The pending file never disappears. A read-only
inspect performs two independent reads; it never arms the writer. Inspect can
observe a radio preflight mismatch without granting a write.

A logical write consists of OPEN(write-truncate), multiple WRITE packets, CLOSE.
**It is not an atomic modem filesystem replacement.** Power loss/truncate/short
write can leave a partial XML. Local checkpoint creation is durable/fail-closed;
it does not make the modem update atomic. There is no demonstrated atomic EFS
rename/replace primitive in this implementation. Any write launch consumes the
attempt permanently. Ambiguous outcome does not imply unchanged content. No retry,
no automatic rollback; an unreaped child prevents readback/overlap. Explicit
rollback uses another checkpoint, another write process, another read process.

The lock is inherited by workers, so coordinator death cannot release the lock
while a worker remains alive. It serializes this experiment only: the operator
must not run other DIAG/PDC tools concurrently. A torn local file blocks further
mutation; no cleanup command is provided.

## Production/UI

Frozen V1 AIDL is unchanged. getStatus reads only a root-owned observation cache;
getLastError exposes its versioned diagnostic text while production remains
BLOCKED_MAPPING. Cache entries are boot-bound, checksummed, and expire in 300 s.
This is an observed snapshot, not automatic live polling. A successful experiment
cannot unlock prepare/apply/restore. Settings has disabled SIM1/SIM2 choices and
disabled prepare/apply/restore controls, with no mutation Binder call in its
repository. Legacy selectedCarrierSim* fields are not used to infer actual SIM
selection or AUTO restoration.

Future production integration requires explicit source change, authorization,
post-reboot/network validation and another final build. The debug writer library
is not connected to daemon or Settings. Existing PosixProcessWorkerLauncher
continues rejecting device writes.

## C.1 recovery hardening

Apply still requires Common/NONE. Explicit sel_db-only rollback accepts an exact
known Common/SKT/KT/LGU active profile with pending NONE; all other AUTO, metadata,
context, artifact, backup and one-shot gates remain mandatory. It never changes
PDC selection or AUTO. Unknown active or any pending entry blocks recovery.

An orphan exact before-image without pending may be reused after immutable
metadata/label/full-byte validation and renewed file/directory fsync. It is never
overwritten/renamed/deleted. A retained pending still blocks every later apply
before baseline/SaveBefore. Torn or unrecognized backup remains fail-closed.
