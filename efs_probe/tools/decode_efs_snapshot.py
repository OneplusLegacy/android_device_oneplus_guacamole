#!/usr/bin/env python3
"""Offline decoder for original and Round 2.5 EFS snapshots; no device I/O."""
import hashlib
import json
import re
import sys
from pathlib import Path

PARENT = '/nv/item_files/mcfg'
MUXD = re.compile(r'mcfg_sw_muxd_version(?:_(?:[0-9]{1,10}|Subscription01(?:_[0-9]{1,10})?))?\Z')
FIXED = {PARENT + '/' + name for name in (
    'mcfg_sel_db.xml', 'mcfg_autoselect_by_uim', 'mcfg_autoselect_by_uim_Subscription01',
    'mcfg_setting', 'mcfg_setting_1', 'mcfg_setting_Subscription01',
    'mcfg_setting_Subscription01_1', 'mcfg_rfs_sw_digest_version')}
FIXED |= {'/mcfg_debug.log', '/mcfg_debug_old.log', '/enable_mcfg_efslogging'}
KEYS = {'target', 'presence', 'bytes_read', 'expected_size', 'sha256', 'data_hex',
        'data_hex_truncated', 'collection_status', 'listed_size', 'name',
        'directory_entry_seen', 'message'}


def decode(text):
    text = text.replace('\r\n', '\n')
    outcomes = re.findall(r'^outcome=(.*)$', text, re.M)
    if len(outcomes) != 1 or outcomes[0] not in ('DONE', 'DONE_WITH_WARNINGS'):
        raise ValueError('Snapshot did not finish; retain log. Nothing extracted.')
    markers = re.findall(r'^muxd_read_support=(.*)$', text, re.M)
    if markers not in ([], ['1']):
        raise ValueError('Invalid/duplicate muxd support marker')
    supported = markers == ['1']
    blocks = re.split(r'^=== STEP: .* ===\n', text, flags=re.M)[1:]
    parsed, listed = [], set()
    for block in blocks:
        fields = {}
        for line in block.splitlines():
            if '=' not in line:
                continue
            key, value = line.split('=', 1)
            if key in KEYS:
                if key in fields:
                    raise ValueError('Duplicate field: ' + key)
                fields[key] = value
        parsed.append(fields)
        if fields.get('target') == PARENT and 'collection_status' not in fields:
            for name in re.findall(r'^entry_name=(.*)$', block, re.M):
                if MUXD.fullmatch(name):
                    if name in listed:
                        raise ValueError('Duplicate listed muxd target')
                    listed.add(name)
    if len(listed) > 256:
        raise ValueError('Muxd file count cap exceeded')
    manifest, payloads, seen, muxd_seen = [], [], set(), set()
    for fields in parsed:
        target = fields.get('target', '')
        name = target[len(PARENT) + 1:] if target.startswith(PARENT + '/') else ''
        dynamic = bool(MUXD.fullmatch(name))
        if dynamic:
            if not supported or name not in listed or fields.get('name') != name:
                raise ValueError('Muxd target not supported/discovered: ' + target)
            if fields.get('directory_entry_seen') != 'true':
                raise ValueError('Missing muxd listing provenance')
            muxd_seen.add(name)
        elif target not in FIXED:
            if fields.get('collection_status') == 'SKIPPED':
                manifest.append({'target': target, 'status': 'SKIPPED',
                                 'reason': fields.get('message', '')})
            continue
        if target in seen:
            raise ValueError('Duplicate target: ' + target)
        seen.add(target)
        row = {'target': target, 'presence': fields.get('presence', 'UNKNOWN')}
        if 'data_hex' not in fields or fields.get('data_hex_truncated') != 'false':
            row['status'] = 'SKIPPED_NO_FULL_BYTES'
            row['reason'] = fields.get('message', '')
        else:
            if not re.fullmatch(r'(?:[0-9a-fA-F]{2})*', fields['data_hex']):
                raise ValueError('Invalid hex: ' + target)
            data = bytes.fromhex(fields['data_hex'])
            digest = hashlib.sha256(data).hexdigest()
            if len(data) != int(fields['bytes_read']) or len(data) != int(fields['expected_size']):
                raise ValueError('Size mismatch: ' + target)
            if digest != fields['sha256'] or fields.get('presence') != 'EXISTS':
                raise ValueError('Hash/presence mismatch: ' + target)
            if dynamic and (len(data) > 4096 or int(fields['listed_size']) != len(data)
                            or fields.get('collection_status') != 'VERIFIED'):
                raise ValueError('Muxd cap/listing/status mismatch: ' + target)
            relative = Path('raw') / target.lstrip('/')
            row.update(status='VERIFIED', size=len(data), sha256=digest, file=str(relative))
            payloads.append((relative, data))
        manifest.append(row)
    if supported and listed != muxd_seen:
        raise ValueError('Listed muxd target lacks a result; incomplete log')
    for target in sorted(FIXED - seen):
        manifest.append({'target': target, 'status': 'NOT_COLLECTED'})
    manifest.append({'target': PARENT + '/mcfg_sw_muxd_version*',
                     'status': ('COLLECTION_RECORDED' if listed else 'NO_MATCHING_ENTRIES')
                     if supported else 'NOT_SUPPORTED_BY_CURRENT_CLI'})
    return manifest, payloads


def main():
    if len(sys.argv) != 3:
        raise SystemExit('Usage: decode_efs_snapshot.py INPUT_LOG NEW_OUTPUT_DIRECTORY')
    source, output = map(Path, sys.argv[1:])
    try:
        manifest, payloads = decode(source.read_text(encoding='utf-8', errors='strict'))
    except (ValueError, KeyError) as error:
        raise SystemExit(str(error)) from error
    output.mkdir(parents=True, exist_ok=False)  # never overwrite a baseline
    for relative, data in payloads:
        dest = output / relative
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(data)
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'Extracted {len(payloads)} verified files; see manifest for omissions.')


if __name__ == '__main__':
    main()
