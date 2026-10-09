"""B.4 OFFLINE evidence checks only. No CLI, file/network/process I/O or write permit.

All observations/provenance are caller-supplied. This evaluator cannot establish
runtime freshness/fsync/reap truth itself and is NOT a production authorization.
"""
from dataclasses import dataclass
from enum import Enum
import hashlib

SOURCE_SHA256 = 'b81f1d34e8279547c789b6665e82db41a07d53451ea7a15bed7e628eb829f155'
TARGET_SHA256 = 'b233088abcb1ea75df1c0d127e85508353728d40f180f230823ffeae5bd1a896'
SOURCE_SIZE = 57655
TARGET_SIZE = 58941
COMMON_ID = '0bd71c00fc30d2ff8af86a8b16d1e01f69e6b493'
# Exact retained B.3 marker. It must NOT be cleared or reused as an EFS journal.
RETAINED_AUTO_PENDING = (b'GUACAMOLE_AUTO_SLOT0_EXPERIMENT_V1\nstate=REVIEW_REQUIRED\n'
                         b'message=0x2e\nwire=010400000000001104000300000013010001\n')
DEVICE_WRITE_OPEN = False


@dataclass(frozen=True)
class BackupEvidence:
    reread_bytes: bytes = b''
    file_fsynced: bool = False
    directory_fsynced: bool = False
    independent_reread_verified: bool = False
    transaction_id: str = ''
    provenance: str = ''


@dataclass(frozen=True)
class PrewriteEvidence:
    capture_reference: str = ''
    same_invocation_verified: bool = False
    full_live_read_verified: bool = False
    fresh_read_worker: bool = False
    read_worker_reaped: bool = False
    read_worker_nonce: str = ''
    selected_verified: bool = False
    active_id: str = ''
    pending_id: str | None = None
    slot0_mode: int | None = None
    slot0_flex: bool | None = None
    slot1_mode: int | None = None
    slot1_flex: bool | None = None
    retained_auto_pending: bytes = b''
    auto_experiment_reviewed: bool = False
    auto_review_reference: str = ''
    efs_recovery_scan_clean: bool = False
    backup: BackupEvidence | None = None


@dataclass(frozen=True)
class GateResult:
    candidate: bool
    reason: str
    # Computed, never supplied by the caller as a gate bypass.
    @property
    def device_write_authorized(self):
        return False


def _matches(data, size, digest):
    return isinstance(data, bytes) and len(data) == size and hashlib.sha256(data).hexdigest() == digest


def exact_transition(live: bytes, proposed: bytes) -> bool:
    return (_matches(live, SOURCE_SIZE, SOURCE_SHA256) and
            _matches(proposed, TARGET_SIZE, TARGET_SHA256))


def _text(value):
    return isinstance(value, str) and bool(value.strip())


def backup_matches_live(backup, live):
    return (isinstance(backup, BackupEvidence) and
            backup.file_fsynced is True and backup.directory_fsynced is True and
            backup.independent_reread_verified is True and
            _text(backup.transaction_id) and _text(backup.provenance) and
            _matches(backup.reread_bytes, SOURCE_SIZE, SOURCE_SHA256) and
            backup.reread_bytes == live)


def evaluate_prewrite(live: bytes, proposed: bytes, evidence: PrewriteEvidence) -> GateResult:
    if not exact_transition(live, proposed):
        return GateResult(False, 'BLOCKED_EXACT_SOURCE_TARGET')
    e = evidence
    if not isinstance(e, PrewriteEvidence):
        return GateResult(False, 'BLOCKED_EVIDENCE')
    if not (_text(e.capture_reference) and _text(e.read_worker_nonce) and
            e.same_invocation_verified is True and e.full_live_read_verified is True and
            e.fresh_read_worker is True and e.read_worker_reaped is True):
        return GateResult(False, 'BLOCKED_FRESH_LIVE_READ')
    if not (e.selected_verified is True and e.active_id == COMMON_ID and e.pending_id is None):
        return GateResult(False, 'BLOCKED_SELECTED')
    if not (type(e.slot0_mode) is int and e.slot0_mode == 3 and e.slot0_flex is True and
            type(e.slot1_mode) is int and e.slot1_mode == 0 and e.slot1_flex is False):
        return GateResult(False, 'BLOCKED_AUTO_FEATURE_DRIFT')
    if not (e.retained_auto_pending == RETAINED_AUTO_PENDING and
            e.auto_experiment_reviewed is True and _text(e.auto_review_reference)):
        return GateResult(False, 'BLOCKED_AUTO_RECORD_MISSING_OR_UNREVIEWED')
    if e.efs_recovery_scan_clean is not True:
        return GateResult(False, 'BLOCKED_EFS_RECOVERY')
    if not backup_matches_live(e.backup, live):
        return GateResult(False, 'BLOCKED_DURABLE_BEFORE_IMAGE')
    return GateResult(True, 'FUTURE_WRITE_CANDIDATE_DEVICE_WRITE_CLOSED')


class ReadbackState(Enum):
    HARD_STOP = 'READBACK_UNKNOWN_HARD_STOP'
    PROPOSED = 'EXACT_PROPOSED'
    UNCHANGED = 'EXACT_SOURCE_UNCHANGED'
    MISMATCH = 'PROVEN_CONTENT_MISMATCH'


def assess_readback(data: bytes | None, *, full_verified=False,
                    fresh_process=False, reaped=False) -> ReadbackState:
    # Ambiguous DIAG response never permits a retry. Only independent full bytes
    # establish proposed/unchanged/mismatch; failed reads cannot justify rollback.
    if not (isinstance(data, bytes) and full_verified is True and
            fresh_process is True and reaped is True):
        return ReadbackState.HARD_STOP
    if _matches(data, TARGET_SIZE, TARGET_SHA256):
        return ReadbackState.PROPOSED
    if _matches(data, SOURCE_SIZE, SOURCE_SHA256):
        return ReadbackState.UNCHANGED
    return ReadbackState.MISMATCH


def rollback_review_eligible(state, *, operator_opt_in=False, before_image_verified=False):
    # Eligibility for a FUTURE reviewed rollback only; no device write entry.
    return (state is ReadbackState.MISMATCH and operator_opt_in is True and
            before_image_verified is True)
