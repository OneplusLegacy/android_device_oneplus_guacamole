#!/usr/bin/env python3
"""Offline comparison of safe Macro A captures. Never infers carrier/index mapping."""
import argparse
import hashlib
import json
import re
from pathlib import Path
from capture_mapping_state import CORE, IDS
from decode_efs_snapshot import MUXD


def load_capture(directory):
    summary_path = directory / 'summary.json'
    if summary_path.is_symlink():
        raise ValueError('summary symlink refused')
    checks = (directory / 'SHA256SUMS').read_text().splitlines()
    candidates = [line for line in checks if line.endswith('  summary.json')]
    if len(candidates) != 1 or not re.fullmatch(r'[0-9a-f]{64}  summary.json', candidates[0]):
        raise ValueError('missing/duplicate summary checksum')
    raw = summary_path.read_bytes()
    if hashlib.sha256(raw).hexdigest() != candidates[0][:64]:
        raise ValueError('summary checksum mismatch')
    summary = json.loads(raw)
    if summary.get('schema') != 1:
        raise ValueError('unsupported capture schema')
    return summary


def changed(before, after):
    return {key: {'before': before.get(key), 'after': after.get(key)}
            for key in sorted(set(before) | set(after)) if before.get(key) != after.get(key)}


def compare(before, after):
    # Only capture-produced observations; never emit arbitrary directory names.
    bm = {k: v for k, v in before.get('muxd', {}).items() if MUXD.fullmatch(k)}
    am = {k: v for k, v in after.get('muxd', {}).items() if MUXD.fullmatch(k)}
    core_diff = changed({k: v for k, v in before.get('core', {}).items() if k in CORE},
                        {k: v for k, v in after.get('core', {}).items() if k in CORE})
    warnings = sorted(set(before.get('warnings', []) + after.get('warnings', [])))
    if before.get('outcome') != 'CAPTURED_OBSERVATIONS_ONLY' or after.get('outcome') != 'CAPTURED_OBSERVATIONS_ONLY':
        warnings.append('INCOMPLETE_CAPTURE')
    if before.get('local_probe_sha256') != after.get('local_probe_sha256'):
        warnings.append('DIFFERENT_PROBE_GENERATIONS')
    return {
        'interpretation': 'OBSERVATIONS_ONLY_NO_MAPPING_DECISION',
        'selected_before_diff': changed(before.get('selected_before', {}), after.get('selected_before', {})),
        'selected_after_diff': changed(before.get('selected_after', {}), after.get('selected_after', {})),
        'profile_info_diff': changed({k: v for k, v in before.get('profiles', {}).items() if k in IDS},
                                     {k: v for k, v in after.get('profiles', {}).items() if k in IDS}),
        'core_hash_diff': core_diff,
        'sel_db_diff': core_diff.get('mcfg_sel_db.xml'),
        'muxd_added': {k: am[k] for k in sorted(am.keys() - bm.keys())},
        'muxd_removed': {k: bm[k] for k in sorted(bm.keys() - am.keys())},
        'muxd_changed': changed({k: bm[k] for k in bm.keys() & am.keys()},
                                {k: am[k] for k in bm.keys() & am.keys()}),
        'cards_observation_diff': changed(before.get('cards', {}), after.get('cards', {})),
        'warnings': warnings,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('before', type=Path)
    parser.add_argument('after', type=Path)
    args = parser.parse_args()
    try:
        result = compare(load_capture(args.before), load_capture(args.after))
    except (ValueError, OSError, KeyError, TypeError):
        raise SystemExit('Capture validation failed; no comparison emitted')
    print(json.dumps(result, indent=2, sort_keys=True))


if __name__ == '__main__':
    main()
