"""Offline proposal artifact writer. No device calls, no modem paths, no write gate."""
import argparse
import difflib
import hashlib
import json
from pathlib import Path
from korea_sel_db_generator import generate_korea_proposal, guacamole_confirmed_mapping


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--original', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    original = args.original.read_bytes()
    mapping = guacamole_confirmed_mapping()
    proposal = generate_korea_proposal(original, original, mapping)
    repeated = generate_korea_proposal(original, proposal.proposed_bytes, mapping)
    offset = proposal.insertion_offset
    unchanged = (proposal.proposed_bytes[:offset] +
                 proposal.proposed_bytes[offset+len(proposal.inserted_bytes):]) == original
    if not unchanged or repeated.proposed_bytes != proposal.proposed_bytes:
        raise ValueError('proposal consistency failure')
    # Refuse any existing destination; never overwrite earlier human-review artifacts.
    args.output.mkdir(parents=True, exist_ok=False)
    (args.output / 'mcfg_sel_db.korea.proposed.xml').write_bytes(proposal.proposed_bytes)
    lines = difflib.unified_diff(original.decode().splitlines(True),
        proposal.proposed_bytes.decode().splitlines(True),
        fromfile='mcfg_sel_db.original.xml', tofile='mcfg_sel_db.korea.proposed.xml')
    diff = ''.join(line if line.endswith('\n') else line+'\n\\ No newline at end of file\n'
                   for line in lines)
    (args.output / 'mcfg_sel_db.exact.diff').write_text(diff)
    summary = {
        'offline_only': True, 'live_write_approved': False,
        'evidence_state': 'CONFIRMED_FOR_SEL_DB_GENERATION',
        'original_sha256': proposal.original_sha256,
        'proposed_sha256': proposal.proposed_sha256,
        'original_size': len(original), 'proposed_size': len(proposal.proposed_bytes),
        'records_before': proposal.records_before, 'records_after': proposal.records_after,
        'unchanged_record_count': proposal.records_before,
        'original_bytes_preserved': unchanged,
        'insertion_offset': offset, 'idempotence': repeated.outcome,
        'ordering': ['SKT', 'KT', 'LGU', 'Common fallback'],
        'inserted_records': proposal.inserted_bytes.decode(),
        'network_validation': 'not established by XML generation',
    }
    (args.output / 'proposal.json').write_text(json.dumps(summary, indent=2)+'\n')
    checksums = ''.join(hashlib.sha256(p.read_bytes()).hexdigest()+'  '+p.name+'\n'
                       for p in sorted(args.output.iterdir()) if p.is_file())
    (args.output / 'SHA256SUMS').write_text(checksums)
    print(json.dumps(summary, indent=2))


if __name__ == '__main__':
    main()
