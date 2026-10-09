"""Offline tests; mocked subprocesses only. User runs these after review."""
import copy
import hashlib
import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch
from capture_mapping_state import (CORE, COMMANDS, remote_command, capture, decode_private_snapshot,
                                   safe_description, info, IDS, selected)
from compare_mapping_capture import compare, load_capture


def snapshot(data=b'\x01\x02\x03\x04'):
    p = '/nv/item_files/mcfg'
    text = ['muxd_read_support=1', '=== STEP: directory ===', 'target=' + p,
            'entry_name=mcfg_sw_muxd_version_39', 'entry_name=cards',
            '=== STEP: cards ===', 'target=' + p + '/cards', 'presence=EXISTS',
            'entry_name=8982051234567890123', 'response_hex=38393832303531323334353637383930313233',
            '=== STEP: debug ===', 'target=/mcfg_debug.log', 'data_hex=1234567890123456']
    for name in (*CORE, 'mcfg_sw_muxd_version_39'):
        blob = data if name.startswith('mcfg_sw_muxd') else b'sensitive-core-content-450051234567890'
        text += ['=== STEP: file ===', 'target=' + p + '/' + name,
                 'presence=EXISTS', 'bytes_read=' + str(len(blob)), 'expected_size=' + str(len(blob)),
                 'sha256=' + hashlib.sha256(blob).hexdigest(), 'data_hex=' + blob.hex(), 'data_hex_truncated=false']
        if name.startswith('mcfg_sw_muxd'):
            text += ['name=' + name, 'directory_entry_seen=true', 'collection_status=VERIFIED',
                     'listed_size=' + str(len(blob))]
    return '\n'.join(text + ['outcome=DONE_WITH_WARNINGS', ''])


class CaptureTests(unittest.TestCase):
    def test_command_allowlist_no_mutation_route(self):
        self.assertEqual(set(COMMANDS), {'hashes', 'selected', 'info', 'snapshot'})
        for command in ('load-kt-once', 'select-lgu-once', 'reboot', 'activate', 'shell', '../snapshot'):
            with self.assertRaises(ValueError):
                remote_command(command)
        self.assertEqual(remote_command('info'), ['adb', 'shell', '/vendor/bin/guacamole_pdc_carrier_probe', 'inspect-all'])
        self.assertEqual(remote_command('snapshot')[-1], 'snapshot-mcfg-readonly')

    def test_cards_and_core_raw_bytes_are_not_copied(self):
        result = decode_private_snapshot(snapshot())
        encoded = json.dumps(result)
        for secret in ('8982051234567890123', '450051234567890', b'450051234567890'.hex(),
                       '38393832303531323334353637383930313233'):
            self.assertNotIn(secret, encoded)
        self.assertEqual(result['cards'], {'observed': True, 'presence': 'EXISTS', 'entry_count': 1})
        self.assertEqual(set(result['core']), set(CORE))
        marker = result['muxd']['mcfg_sw_muxd_version_39']
        self.assertEqual(marker['data_hex'], '01020304')
        self.assertEqual(marker['le_u32'], 0x04030201)
        self.assertEqual(marker['sha256'], hashlib.sha256(bytes.fromhex('01020304')).hexdigest())

    def test_non_u32_muxd_only_size_hash(self):
        r = decode_private_snapshot(snapshot(b'8982051234567890123'))
        self.assertIsNone(r['muxd']['mcfg_sw_muxd_version_39']['data_hex'])
        self.assertNotIn('8982051234567890123', json.dumps(r))

    def test_core_missing_or_corrupt_aborts(self):
        for text in (snapshot().replace('target=/nv/item_files/mcfg/mcfg_sel_db.xml', 'target=/ignored'),
                     snapshot().replace('bytes_read=4', 'bytes_read=3'),
                     snapshot().replace('outcome=DONE_WITH_WARNINGS', 'outcome=FAILED')):
            with self.assertRaises(ValueError):
                decode_private_snapshot(text)

    def test_existing_output_refused_before_adb(self):
        with tempfile.TemporaryDirectory() as temp, patch('capture_mapping_state.subprocess.run') as run:
            with self.assertRaises(FileExistsError):
                capture(Path(temp), Path(temp))
            run.assert_not_called()

    def test_failure_records_no_raw_error_or_retry(self):
        with tempfile.TemporaryDirectory() as temp, patch('capture_mapping_state.run_readonly') as run:
            out = Path(temp) / 'new'
            self.assertFalse(capture(out, Path(temp) / 'missing-local-output'))
            run.assert_not_called()
            self.assertEqual(load_capture(out)['outcome'], 'INCOMPLETE')

    def test_description_identifier_redaction(self):
        self.assertEqual(safe_description(b'Commercial-KT'.hex()), 'Commercial-KT')
        self.assertNotIn('450051234567890', safe_description(b'450051234567890'.hex()))
        self.assertEqual(safe_description(b'\x00\x01'.hex()), 'REDACTED')

    def test_exact_muxd_diff(self):
        before = decode_private_snapshot(snapshot())
        after = decode_private_snapshot(snapshot(b'\x02\x02\x03\x04'))
        for r in (before, after):
            r['outcome'] = 'CAPTURED_OBSERVATIONS_ONLY'
        after['muxd']['mcfg_sw_muxd_version_40'] = copy.deepcopy(after['muxd']['mcfg_sw_muxd_version_39'])
        delta = compare(before, after)
        self.assertEqual(delta['muxd_changed']['mcfg_sw_muxd_version_39']['before']['le_u32'], 0x04030201)
        self.assertEqual(delta['muxd_changed']['mcfg_sw_muxd_version_39']['after']['le_u32'], 0x04030202)
        self.assertIn('mcfg_sw_muxd_version_40', delta['muxd_added'])
        self.assertEqual(delta['interpretation'], 'OBSERVATIONS_ONLY_NO_MAPPING_DECISION')
        self.assertFalse(delta['core_hash_diff'])

    def test_unknown_selected_id_is_not_copied(self):
        r = selected('pre_status=OK\npre_error_status=0\npre_error_code=0\n'
                     'pre_active=8982051234567890123\npre_pending=NONE\n')
        self.assertEqual(r['active'], 'UNRECOGNIZED_REDACTED')
        self.assertNotIn('8982051234567890123', json.dumps(r))

    def test_inspect_41_preserves_unknown_fields(self):
        lines = []
        for name, config_id in IDS.items():
            row = {'expected_config_id': config_id, 'status': 'INDICATION_REJECTED',
                   'error_status': '41', 'error_code': '0', 'protocol_error': '41',
                   'protocol_error_source': 'INDICATION_RESULT', 'presence': 'UNKNOWN',
                   'runtime_verified': 'false', 'has_size': '0', 'size': '0',
                   'has_version': '0', 'version': '0', 'has_description': '0', 'description_hex': ''}
            lines += [name + '_' + key + '=' + value for key, value in row.items()]
        result = info('\n'.join(lines))
        self.assertEqual(result['kt']['presence'], 'UNKNOWN')
        self.assertEqual(result['lgu']['protocol_error'], 41)

    def test_summary_checksum_required(self):
        with tempfile.TemporaryDirectory() as temp:
            p = Path(temp)
            (p / 'summary.json').write_text('{"schema": 1}')
            (p / 'SHA256SUMS').write_text('0' * 64 + '  summary.json\n')
            with self.assertRaises(ValueError):
                load_capture(p)


if __name__ == '__main__':
    unittest.main()
