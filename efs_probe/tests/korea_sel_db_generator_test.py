"""Offline tests; fixture is unchanged known vendor XML, not proposed live data."""
from pathlib import Path
import sys
import re
import unittest
import xml.etree.ElementTree as ET
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from korea_sel_db_generator import (CarrierIndexEvidence, ConfirmedCarrierMapping,
    EvidenceState, GenerationBlocked, generate_korea_proposal, guacamole_confirmed_mapping)

ORIGINAL = (Path(__file__).parent / 'fixtures/mcfg_sel_db_known_original.xml').read_bytes()


def evidence(index):
    return CarrierIndexEvidence(index, EvidenceState.CONFIRMED, 'SYNTHETIC_HOST_TEST_ONLY')


def mapping():
    # Deliberately artificial unused indices; NOT real mapping evidence.
    return ConfirmedCarrierMapping(evidence(60000), evidence(60001), evidence(60002))


class KoreaGenerator(unittest.TestCase):
    def blocked(self, original, current, values, reason):
        with self.assertRaisesRegex(GenerationBlocked, reason):
            generate_korea_proposal(original, current, values)

    def test_unconfirmed(self):
        self.blocked(ORIGINAL, ORIGINAL, ConfirmedCarrierMapping(), 'UNCONFIRMED')

    def test_missing_carrier_or_reference(self):
        self.blocked(ORIGINAL, ORIGINAL,
                     ConfirmedCarrierMapping(evidence(60000), evidence(60001)), 'UNCONFIRMED')
        self.blocked(ORIGINAL, ORIGINAL, ConfirmedCarrierMapping(
            CarrierIndexEvidence(60000, EvidenceState.CONFIRMED), evidence(60001), evidence(60002)),
            'UNCONFIRMED')

    def test_repeated_index_is_not_a_record_key(self):
        proposal = generate_korea_proposal(ORIGINAL, ORIGINAL,
            ConfirmedCarrierMapping(evidence(31), evidence(31), evidence(31)))
        self.assertEqual(proposal.records_after, proposal.records_before + 3)

    def test_ice40_and_kt40_coexist_original_bytes_untouched(self):
        a = generate_korea_proposal(ORIGINAL, ORIGINAL, guacamole_confirmed_mapping())
        root = ET.fromstring(a.proposed_bytes)
        records = {e.get('carrier_name'): e for e in root.iter('SelRecord')}
        self.assertEqual(records['ICE'].get('mcfg_carrier_index'), '40')
        self.assertEqual(records['KT'].get('mcfg_carrier_index'), '40')
        self.assertEqual(records['SKT'].get('mcfg_carrier_index'), '39')
        self.assertEqual(records['LGU'].get('mcfg_carrier_index'), '38')
        self.assertEqual(ORIGINAL,
            a.proposed_bytes[:a.insertion_offset] + a.proposed_bytes[a.insertion_offset+len(a.inserted_bytes):])

    def test_conflicting_plmn_iin_and_duplicate_korean_rules(self):
        m = guacamole_confirmed_mapping()
        a = generate_korea_proposal(ORIGINAL, ORIGINAL, m)
        for current in (a.proposed_bytes.replace(b'450-08', b'450-05'),
                        a.proposed_bytes.replace(b'898230', b'898205'),
                        a.proposed_bytes.replace(b'carrier_name="KT"', b'carrier_name="SKT"'),
                        a.proposed_bytes.replace(b'mcfg_carrier_index="39"', b'mcfg_carrier_index="38"'),
                        a.proposed_bytes[:a.insertion_offset] + a.inserted_bytes + a.proposed_bytes[a.insertion_offset:]):
            self.blocked(ORIGINAL, current, m, 'PARTIAL_OR_UNKNOWN')

    def test_wrong_original_sha(self):
        self.blocked(ORIGINAL + b'\n', ORIGINAL, mapping(), 'BASELINE_SHA')

    def test_deterministic_and_exact_diff(self):
        a = generate_korea_proposal(ORIGINAL, ORIGINAL, mapping())
        self.assertEqual(a, generate_korea_proposal(ORIGINAL, ORIGINAL, mapping()))
        self.assertEqual(a.records_after, a.records_before + 3)
        self.assertEqual(a.proposed_bytes,
            ORIGINAL[:a.insertion_offset] + a.inserted_bytes + ORIGINAL[a.insertion_offset:])

    def test_idempotence(self):
        a = generate_korea_proposal(ORIGINAL, ORIGINAL, mapping())
        b = generate_korea_proposal(ORIGINAL, a.proposed_bytes, mapping())
        self.assertEqual(b.outcome, 'ALREADY_PATCHED_EXACT')
        self.assertEqual(a.proposed_bytes, b.proposed_bytes)
        self.assertEqual(a.proposed_sha256, b.proposed_sha256)

    def test_partial_and_marker_spoof_rejected(self):
        a = generate_korea_proposal(ORIGINAL, ORIGINAL, mapping())
        for current in (a.proposed_bytes.replace(b'450-05', b'450-99'),
                        a.proposed_bytes.replace(b'60000', b'60003'),
                        ORIGINAL + b'<!-- 898205 898230 898206 -->',
                        a.proposed_bytes + a.inserted_bytes):
            self.blocked(ORIGINAL, current, mapping(), 'PARTIAL_OR_UNKNOWN')

    def test_exact_identifiers_and_order(self):
        a = generate_korea_proposal(ORIGINAL, ORIGINAL, mapping())
        records = list(ET.fromstring(a.proposed_bytes))[-4:-1]
        expected = [('SKT', '898205', '450-05', '60000'),
                    ('KT', '898230', '450-08', '60001'),
                    ('LGU', '898206', '450-06', '60002')]
        for node, (name, iin, plmn, index) in zip(records, expected):
            self.assertEqual(node.find('any_of/iin_in').text, iin)
            self.assertEqual(node.find('any_of/imsi_3gpp_plmn_in').text, plmn)
            self.assertEqual(node.find('then/SelRecord').attrib, {
                'carrier_name': name, 'country_code': '410', 'iin': 'iin',
                'imsi_3gpp': '3gpp_imsi', 'mcfg_carrier_index': index,
                'volte': 'true', 'vowifi': 'false'})
        self.assertEqual(len(records), 3)

    def test_invalid_index_values(self):
        for bad in (-1, 65536, True, '39', None):
            self.blocked(ORIGINAL, ORIGINAL,
                ConfirmedCarrierMapping(evidence(bad), evidence(60001), evidence(60002)),
                'UNCONFIRMED')

    def test_korea_capability_flags_exact(self):
        a = generate_korea_proposal(ORIGINAL, ORIGINAL, guacamole_confirmed_mapping())
        records = list(ET.fromstring(a.proposed_bytes).iter('SelRecord'))
        for name in ('SKT', 'KT', 'LGU'):
            selected = [e for e in records if e.get('carrier_name') == name]
            self.assertEqual(len(selected), 1)
            self.assertEqual(selected[0].get('volte'), 'true')
            self.assertEqual(selected[0].get('vowifi'), 'false')

    def test_common_remains_exact_without_capability_flags(self):
        a = generate_korea_proposal(ORIGINAL, ORIGINAL, guacamole_confirmed_mapping())
        common = lambda data: [record for record in re.findall(rb'<SelRecord\b[^>]*>', data)
                               if b'carrier_name="Common"' in record]
        self.assertEqual(len(common(ORIGINAL)), 1)
        self.assertEqual(common(a.proposed_bytes), common(ORIGINAL))
        self.assertNotIn(b'volte=', common(a.proposed_bytes)[0])
        self.assertNotIn(b'vowifi=', common(a.proposed_bytes)[0])

    def test_all_128_original_records_and_bytes_preserved(self):
        a = generate_korea_proposal(ORIGINAL, ORIGINAL, guacamole_confirmed_mapping())
        records = lambda data: re.findall(rb'<SelRecord\b[^>]*>', data)
        old = records(ORIGINAL)
        retained = [e for e in records(a.proposed_bytes)
                    if not re.search(rb'carrier_name="(?:SKT|KT|LGU)"', e)]
        self.assertEqual(len(old), 128)
        self.assertEqual(retained, old)  # includes ICE40 and Common, with exact spelling/order
        self.assertEqual(a.proposed_bytes[:a.insertion_offset] +
                         a.proposed_bytes[a.insertion_offset + len(a.inserted_bytes):], ORIGINAL)

    def test_missing_or_changed_capability_flags_are_not_already_patched(self):
        m = guacamole_confirmed_mapping()
        a = generate_korea_proposal(ORIGINAL, ORIGINAL, m)
        for insertion in (a.inserted_bytes.replace(b' volte="true" vowifi="false"', b''),
                          a.inserted_bytes.replace(b'volte="true"', b'volte="false"', 1),
                          a.inserted_bytes.replace(b'vowifi="false"', b'vowifi="true"', 1)):
            current = ORIGINAL[:a.insertion_offset] + insertion + ORIGINAL[a.insertion_offset:]
            self.blocked(ORIGINAL, current, m, 'PARTIAL_OR_UNKNOWN')


if __name__ == '__main__':
    unittest.main()
