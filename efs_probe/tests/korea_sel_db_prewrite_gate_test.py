"""B.4 offline gate tests. All durability/runtime flags are synthetic evidence."""
from pathlib import Path
from dataclasses import replace
import json
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from korea_sel_db_generator import generate_korea_proposal, guacamole_confirmed_mapping
from korea_sel_db_prewrite_gate import (
    COMMON_ID, RETAINED_AUTO_PENDING, DEVICE_WRITE_OPEN, BackupEvidence,
    PrewriteEvidence, exact_transition, evaluate_prewrite, assess_readback,
    ReadbackState, rollback_review_eligible)

ORIGINAL = (Path(__file__).parent / 'fixtures/mcfg_sel_db_known_original.xml').read_bytes()


def evidence():
    return PrewriteEvidence('SYNTHETIC-capture', True, True, True, True, 'fresh-worker-nonce',
        True, COMMON_ID, None, 3, True, 0, False, RETAINED_AUTO_PENDING, True,
        'SYNTHETIC-reviewed-B4', True,
        BackupEvidence(ORIGINAL, True, True, True, 'transaction-1', 'SYNTHETIC-backup'))


class PrewriteGate(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.proposed = generate_korea_proposal(ORIGINAL, ORIGINAL, guacamole_confirmed_mapping()).proposed_bytes

    def test_exact_source_and_target_only(self):
        self.assertTrue(exact_transition(ORIGINAL, self.proposed))
        for a, b in [(ORIGINAL+b'\n', self.proposed), (ORIGINAL, self.proposed+b'\n'),
                     (self.proposed, self.proposed), (ORIGINAL, ORIGINAL)]:
            self.assertFalse(exact_transition(a, b))

    def test_candidate_is_never_write_authority(self):
        result = evaluate_prewrite(ORIGINAL, self.proposed, evidence())
        self.assertTrue(result.candidate)
        self.assertFalse(result.device_write_authorized)
        self.assertFalse(DEVICE_WRITE_OPEN)

    def test_fresh_full_read_and_reap_required(self):
        for k, v in [('same_invocation_verified', False), ('full_live_read_verified', False),
                     ('fresh_read_worker', False), ('read_worker_reaped', False),
                     ('read_worker_nonce', ''), ('capture_reference', ''), ('read_worker_nonce', None)]:
            self.assertFalse(evaluate_prewrite(ORIGINAL, self.proposed, replace(evidence(), **{k:v})).candidate)

    def test_common_none_mandatory(self):
        for fields in [dict(active_id='unknown'), dict(pending_id=COMMON_ID), dict(selected_verified=False)]:
            self.assertFalse(evaluate_prewrite(ORIGINAL, self.proposed, replace(evidence(), **fields)).candidate)

    def test_feature_drift_not_repaired_by_gate(self):
        for fields in [dict(slot0_mode=0), dict(slot0_flex=False), dict(slot1_mode=3),
                       dict(slot1_flex=True), dict(slot0_mode=None), dict(slot1_mode=False)]:
            self.assertFalse(evaluate_prewrite(ORIGINAL, self.proposed, replace(evidence(), **fields)).candidate)

    def test_retained_pending_is_required_not_cleared(self):
        e = evidence()
        self.assertTrue(evaluate_prewrite(ORIGINAL, self.proposed, e).candidate)
        self.assertEqual(e.retained_auto_pending, RETAINED_AUTO_PENDING)
        for fields in [dict(retained_auto_pending=b''), dict(retained_auto_pending=b'torn'),
                       dict(auto_experiment_reviewed=False), dict(auto_review_reference='')]:
            self.assertFalse(evaluate_prewrite(ORIGINAL, self.proposed, replace(e, **fields)).candidate)

    def test_auto_record_does_not_override_efs_recovery(self):
        self.assertFalse(evaluate_prewrite(ORIGINAL, self.proposed,
            replace(evidence(), efs_recovery_scan_clean=False)).candidate)

    def test_durable_backup_full_reread_required(self):
        e = evidence()
        for backup in [None, replace(e.backup, file_fsynced=False), replace(e.backup, directory_fsynced=False),
                       replace(e.backup, independent_reread_verified=False), replace(e.backup, reread_bytes=ORIGINAL[:-1]),
                       replace(e.backup, transaction_id=''), replace(e.backup, provenance=''), replace(e.backup, transaction_id=None)]:
            self.assertFalse(evaluate_prewrite(ORIGINAL, self.proposed, replace(e, backup=backup)).candidate)

    def test_readback_exact_proposed_source_and_mismatch(self):
        for data, state in [(self.proposed, ReadbackState.PROPOSED), (ORIGINAL, ReadbackState.UNCHANGED),
                            (b'fully-read-different-content', ReadbackState.MISMATCH)]:
            self.assertIs(assess_readback(data, full_verified=True, fresh_process=True, reaped=True), state)

    def test_partial_timeout_or_old_process_is_hard_stop(self):
        for kwargs in [dict(full_verified=False, fresh_process=True, reaped=True),
                       dict(full_verified=True, fresh_process=False, reaped=True),
                       dict(full_verified=True, fresh_process=True, reaped=False)]:
            self.assertIs(assess_readback(self.proposed, **kwargs), ReadbackState.HARD_STOP)
        self.assertIs(assess_readback(None, full_verified=True, fresh_process=True, reaped=True), ReadbackState.HARD_STOP)

    def test_rollback_only_proven_mismatch_and_explicit_opt_in(self):
        for state in ReadbackState:
            self.assertFalse(rollback_review_eligible(state))
            self.assertEqual(rollback_review_eligible(state, operator_opt_in=True, before_image_verified=True),
                             state is ReadbackState.MISMATCH)
        self.assertFalse(rollback_review_eligible(ReadbackState.MISMATCH, operator_opt_in=True))


class B4Evidence(unittest.TestCase):
    def test_reported_persistence_and_non_schema_inferences(self):
        path = Path(__file__).resolve().parents[1] / 'evidence/macro_b4/auto_persistence.json'
        data = json.loads(path.read_text())
        self.assertFalse(data['pre_skt_baseline']['get_feature_observed'])
        self.assertEqual(data['pre_skt_baseline']['label'], 'pre-SKT stock EFS baseline')
        self.assertEqual(data['wire_slot0_enable_hex'], '010400000000001104000300000013010001')
        for stage in ['immediate', 'after_reboot']:
            self.assertEqual(data[stage]['slot0'], {'selection_mode':3, 'flex_mapping':True})
            self.assertEqual(data[stage]['slot1'], data['before']['slot1'])
            self.assertEqual(data[stage]['active'], 'Common')
            self.assertIsNone(data[stage]['pending'])
        self.assertEqual(set(data['immediate']['efs_changes']), {'mcfg_autoselect_by_uim', 'mcfg_setting'})
        self.assertEqual(data['immediate']['efs_changes']['mcfg_autoselect_by_uim']['after_hex'], '87')
        self.assertEqual(data['after_reboot']['additional_changes'], ['mcfg_setting_1'])
        self.assertEqual(data['after_reboot']['subscription01_files'], 'UNCHANGED')
        self.assertEqual(data['after_reboot']['sel_db_muxd_digest'], 'UNCHANGED')
        self.assertEqual(data['inference']['setting_0x04_selection_mode'], 'STRONG_EMPIRICAL_CORRELATION_NOT_SCHEMA_CONFIRMED')
        self.assertEqual(data['inference']['setting_0x05_flex_mapping'], 'STRONG_EMPIRICAL_CORRELATION_NOT_SCHEMA_CONFIRMED')
        self.assertTrue(data['retain_slot0_pending_permanently'])
        self.assertEqual(data['auto_experiment'], 'CLOSED')


if __name__ == '__main__':
    unittest.main()
