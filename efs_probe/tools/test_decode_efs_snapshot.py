"""Offline decoder unit tests. Run manually; never contacts a device."""
import hashlib
import unittest
from decode_efs_snapshot import decode, MUXD, PARENT


class DecoderTests(unittest.TestCase):
    def capture(self, name='mcfg_sw_muxd_version_Subscription01_39', data=b'\x01\x02\x03\x04'):
        return ('muxd_read_support=1\n'
                '=== STEP: OPENDIR/READDIR /nv/item_files/mcfg ===\n'
                f'target={PARENT}\nentry_name={name}\n'
                '=== STEP: STAT+READ discovered muxd ===\n'
                f'target={PARENT}/{name}\nname={name}\n'
                'directory_entry_seen=true\ncollection_status=VERIFIED\npresence=EXISTS\n'
                f'listed_size={len(data)}\nexpected_size={len(data)}\nbytes_read={len(data)}\n'
                f'sha256={hashlib.sha256(data).hexdigest()}\n'
                f'data_hex_truncated=false\ndata_hex={data.hex()}\noutcome=DONE\n')

    def test_full_bytes_hash_and_path(self):
        manifest, payloads = decode(self.capture())
        self.assertEqual(payloads[0][1], b'\x01\x02\x03\x04')
        self.assertEqual(manifest[0]['status'], 'VERIFIED')
        self.assertEqual(manifest[0]['size'], 4)

    def test_patterns(self):
        for name in ('mcfg_sw_muxd_version', 'mcfg_sw_muxd_version_39',
                     'mcfg_sw_muxd_version_Subscription01_8'):
            self.assertIsNotNone(MUXD.fullmatch(name))
        for name in ('../mcfg_sw_muxd_version', 'mcfg_sw_muxd_version/child',
                     'mcfg_sw_muxd_versionevil', 'mcfg_sw_muxd_version_',
                     'mcfg_sw_muxd_version_39\n', 'mcfg_sw_muxd_version_39\0'):
            self.assertIsNone(MUXD.fullmatch(name))

    def test_undiscovered_rejected(self):
        with self.assertRaises(ValueError):
            decode(self.capture().replace('entry_name=', 'ignored='))

    def test_mismatch_rejected(self):
        for old, new in [('bytes_read=4', 'bytes_read=3'),
                         ('listed_size=4', 'listed_size=3'),
                         ('data_hex=01020304', 'data_hex=01020305'),
                         ('collection_status=VERIFIED', 'collection_status=FAILED'),
                         ('outcome=DONE', 'outcome=FAILED')]:
            with self.subTest(old=old), self.assertRaises(ValueError):
                decode(self.capture().replace(old, new))

    def test_cap_rejected(self):
        with self.assertRaises(ValueError):
            decode(self.capture(data=b'x' * 4097))

    def test_missing_result_rejected(self):
        text = self.capture().split('=== STEP: STAT+READ')[0] + 'outcome=DONE\n'
        with self.assertRaises(ValueError):
            decode(text)

    def test_original_fixed_snapshot_compatible(self):
        data = b'\x05'
        log = ('=== STEP: STAT+READ mcfg_autoselect_by_uim ===\n'
               f'target={PARENT}/mcfg_autoselect_by_uim\npresence=EXISTS\n'
               'bytes_read=1\nexpected_size=1\ndata_hex_truncated=false\ndata_hex=05\n'
               f'sha256={hashlib.sha256(data).hexdigest()}\noutcome=DONE_WITH_WARNINGS\n')
        manifest, payloads = decode(log)
        self.assertEqual(payloads[0][1], data)
        self.assertEqual(manifest[-1]['status'], 'NOT_SUPPORTED_BY_CURRENT_CLI')


if __name__ == '__main__':
    unittest.main()
