#!/usr/bin/env python3
"""Macro A read-only capture. No arbitrary remote command/path API, no raw logs."""
import argparse
import datetime
import hashlib
import json
import re
import subprocess
import uuid
from pathlib import Path
from decode_efs_snapshot import decode, MUXD, PARENT

CORE = tuple('mcfg_sel_db.xml mcfg_autoselect_by_uim mcfg_autoselect_by_uim_Subscription01 '
             'mcfg_setting mcfg_setting_1 mcfg_setting_Subscription01 '
             'mcfg_setting_Subscription01_1 mcfg_rfs_sw_digest_version'.split())
PROBES = ('guacamole_efs_probe', 'guacamole_pdc_probe',
          'guacamole_pdc_mapping_probe', 'guacamole_pdc_carrier_probe')
# Remote command arguments can ONLY come from this table, never from CLI input.
COMMANDS = {
    'hashes': ('sha256sum', *(f'/vendor/bin/{name}' for name in PROBES)),
    'selected': ('/vendor/bin/guacamole_pdc_carrier_probe', 'selected'),
    'info': ('/vendor/bin/guacamole_pdc_carrier_probe', 'inspect-all'),
    'snapshot': ('/vendor/bin/guacamole_efs_probe', 'snapshot-mcfg-readonly'),
}
IDS = {
    'common': '0bd71c00fc30d2ff8af86a8b16d1e01f69e6b493',
    'skt': 'ca9c9dcb8d6d2be6e4f6b49ca3e4b5420769a7d1',
    'kt': '79bd2e28f61310fd538c0a5b9bcbd19f143da762',
    'lgu': '3a87a2ce830826e7de1f1a8cb2faf5012b4b71bc',
}
STATUSES = {'OK', 'INIT_FAILED', 'REQUEST_SEND_FAILED', 'REQUEST_REJECTED',
            'TIMEOUT', 'MALFORMED_RESPONSE', 'INDICATION_REJECTED'}


def remote_command(key):
    if key not in COMMANDS:
        raise ValueError('remote operation not allowlisted')
    return ['adb', 'shell', *COMMANDS[key]]


def run_readonly(key):
    # stdout/stderr stay in RAM. Never echo error output or store it in a raw log.
    try:
        result = subprocess.run(remote_command(key), stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, timeout=180, check=False)
    except (OSError, subprocess.TimeoutExpired) as error:
        raise ValueError('read-only subprocess failed; no retry') from error
    if result.returncode != 0 or len(result.stdout) > 64 * 1024 * 1024:
        raise ValueError('read-only subprocess nonzero/oversized; no retry')
    try:
        return result.stdout.decode('utf-8', errors='strict').replace('\r\n', '\n')
    except UnicodeError as error:
        raise ValueError('non-UTF8 probe output') from error


def fields(text):
    result = {}
    for line in text.splitlines():
        if '=' not in line:
            continue
        key, value = line.split('=', 1)
        if key in result:
            raise ValueError('duplicate observation field')
        result[key] = value
    return result


def status(f, prefix):
    value = f.get(prefix + '_status')
    if value not in STATUSES:
        raise ValueError('missing/invalid status')
    return value


def number(f, key):
    value = f.get(key, '')
    if not re.fullmatch(r'[0-9]{1,10}', value) or int(value) > 0xffffffff:
        raise ValueError('invalid numeric field')
    return int(value)


def selected(text):
    f = fields(text)
    s = status(f, 'pre')
    out = {'status': s, 'error_status': number(f, 'pre_error_status'),
           'error_code': number(f, 'pre_error_code')}
    for name in ('active', 'pending'):
        value = f.get('pre_' + name, 'UNKNOWN')
        out[name] = value if value in set(IDS.values()) | {'NONE', 'UNKNOWN'} else 'UNRECOGNIZED_REDACTED'
    return out


def safe_description(value):
    if not re.fullmatch(r'(?:[0-9a-f]{2}){0,255}', value):
        raise ValueError('invalid description encoding')
    data = bytes.fromhex(value)
    # Reject arbitrary text; expected profile descriptions are simple ASCII names.
    text = data.decode('ascii', errors='replace')
    if not re.fullmatch(r'[A-Za-z0-9 _.,+()/\-]{0,255}', text):
        return 'REDACTED'
    return re.sub(r'[0-9]{14,}', '[IDENTIFIER_REDACTED]', text)


def info(text):
    f = fields(text)
    out = {}
    for carrier, exact_id in IDS.items():
        if f.get(carrier + '_expected_config_id') != exact_id:
            raise ValueError('unexpected profile identity')
        row = {'expected_config_id': exact_id, 'status': status(f, carrier)}
        for name in ('error_status', 'error_code', 'protocol_error', 'has_size', 'size',
                     'has_version', 'version', 'has_description'):
            row[name] = number(f, carrier + '_' + name)
        for name, allowed in (
            ('presence', {'EXISTS', 'ABSENT', 'UNKNOWN'}),
            ('runtime_verified', {'true', 'false'}),
            ('protocol_error_source', {'INDICATION_RESULT', 'SYNC_OPERATION_RESULT', 'NONE_OR_UNKNOWN'})):
            value = f.get(carrier + '_' + name)
            if value not in allowed:
                raise ValueError('invalid profile observation')
            row[name] = value
        row['description'] = safe_description(f.get(carrier + '_description_hex', ''))
        out[carrier] = row
    return out


def device_hashes(text):
    out = {}
    for line in text.splitlines():
        match = re.fullmatch(r'([0-9a-f]{64})\s+/vendor/bin/(guacamole_[a-z_]+)', line)
        if not match or match[2] not in PROBES or match[2] in out:
            raise ValueError('invalid binary hash output')
        out[match[2]] = match[1]
    if set(out) != set(PROBES):
        raise ValueError('missing binary hash')
    return out


def decode_private_snapshot(text):
    """Invoke existing decoder in RAM; discard cards, debug logs and all wire hex."""
    outcomes = re.findall(r'^outcome=(.*)$', text, re.M)
    if len(outcomes) != 1 or outcomes[0] not in ('DONE', 'DONE_WITH_WARNINGS'):
        raise ValueError('snapshot failed/incomplete')
    if re.findall(r'^muxd_read_support=(.*)$', text, re.M) != ['1']:
        raise ValueError('muxd support not proven')
    core_targets = {PARENT + '/' + n for n in CORE}
    filtered = ['muxd_read_support=1']
    cards = {'observed': False, 'presence': 'UNKNOWN', 'entry_count': None}
    for block in re.split(r'^=== STEP: .* ===\n', text, flags=re.M)[1:]:
        targets = re.findall(r'^target=(.*)$', block, re.M)
        if len(targets) != 1:
            raise ValueError('invalid snapshot target')
        target = targets[0]
        if target == PARENT + '/cards':
            presence = re.findall(r'^presence=(EXISTS|ABSENT|UNKNOWN)$', block, re.M)
            presence = presence[0] if len(presence) == 1 else 'UNKNOWN'
            cards = {'observed': True, 'presence': presence,
                     'entry_count': len(re.findall(r'^entry_name=', block, re.M)) if presence == 'EXISTS' else None}
            continue  # never retain card filenames, wire hex, or identifier hashes
        basename = target[len(PARENT) + 1:] if target.startswith(PARENT + '/') else ''
        if target == PARENT:
            filtered += ['=== STEP: safe directory ===', 'target=' + PARENT]
            filtered += ['entry_name=' + name for name in re.findall(r'^entry_name=(.*)$', block, re.M)
                         if MUXD.fullmatch(name)]
        elif target in core_targets or MUXD.fullmatch(basename):
            filtered += ['=== STEP: safe file ===']
            keys = {'target', 'presence', 'bytes_read', 'expected_size', 'sha256', 'data_hex',
                    'data_hex_truncated', 'collection_status', 'listed_size', 'name', 'directory_entry_seen'}
            filtered += [line for line in block.splitlines() if line.partition('=')[0] in keys]
    filtered.append('outcome=' + outcomes[0])
    manifest, payloads = decode('\n'.join(filtered) + '\n')
    raw = {str(path): data for path, data in payloads}
    core, muxd, warnings = {}, {}, []
    for row in manifest:
        target = row['target']
        basename = target[len(PARENT) + 1:] if target.startswith(PARENT + '/') else ''
        if target not in core_targets and not MUXD.fullmatch(basename):
            continue
        if row.get('status') != 'VERIFIED':
            raise ValueError('required core/muxd read not verified')
        data = raw[row['file']]
        item = {'size': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
        if target in core_targets:
            core[basename] = item  # deliberately do not persist core bytes/XML
        else:
            # Observed muxd markers are u32. Unknown-length blobs may contain
            # identifiers, so retain only size/hash rather than dumping bytes.
            item['data_hex'] = data.hex() if len(data) == 4 else None
            item['le_u32'] = int.from_bytes(data, 'little') if len(data) == 4 else None
            if len(data) != 4:
                warnings.append('NON_U32_MUXD_BYTES_REDACTED:' + basename)
            muxd[basename] = item
    if set(core) != set(CORE):
        raise ValueError('core eight incomplete')
    if outcomes[0] == 'DONE_WITH_WARNINGS':
        warnings.append('SNAPSHOT_WARNINGS_DETAILS_NOT_RETAINED')
    return {'core': core, 'muxd': muxd, 'cards': cards, 'warnings': warnings,
            'snapshot_outcome': outcomes[0], 'decoder': 'existing decode_efs_snapshot.decode in memory'}


def create_output(output):
    output.mkdir(mode=0o700, parents=False, exist_ok=False)


def write_json(path, obj):
    path.write_text(json.dumps(obj, indent=2, sort_keys=True) + '\n', encoding='utf-8')


def capture(output, product_out):
    create_output(output)  # refuses ANY existing directory before running adb
    summary = {'schema': 1, 'generation_id': uuid.uuid4().hex,
               'timestamp_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
               'outcome': 'INCOMPLETE', 'warnings': []}
    try:
        summary['stage'] = 'LOCAL_HASHES'
        host = {name: hashlib.sha256((product_out / 'vendor/bin' / name).read_bytes()).hexdigest()
                for name in PROBES}
        summary['stage'] = 'DEVICE_HASHES_BEFORE'
        device = device_hashes(run_readonly('hashes'))
        summary.update(local_probe_sha256=host, device_probe_sha256=device)
        if host != device:
            raise ValueError('probe generation mismatch')
        summary['stage'] = 'SELECTED_BEFORE'
        summary['selected_before'] = selected(run_readonly('selected'))
        if summary['selected_before']['status'] != 'OK':
            raise ValueError('selected before failed')
        summary['stage'] = 'EXACT_PROFILE_INFO'
        summary['profiles'] = info(run_readonly('info'))
        if any(v['status'] not in {'OK', 'INDICATION_REJECTED', 'REQUEST_REJECTED'} for v in summary['profiles'].values()):
            raise ValueError('profile transport/timeout/parser failure')
        summary['stage'] = 'EFS_SNAPSHOT_DECODE'
        summary.update(decode_private_snapshot(run_readonly('snapshot')))
        summary['stage'] = 'SELECTED_AFTER'
        summary['selected_after'] = selected(run_readonly('selected'))
        summary['stage'] = 'DEVICE_HASHES_AFTER'
        summary['device_probe_sha256_after'] = device_hashes(run_readonly('hashes'))
        if summary['selected_after']['status'] != 'OK' or summary['device_probe_sha256_after'] != device:
            raise ValueError('selected after failed or generation changed')
        if summary['selected_before'] != summary['selected_after']:
            summary['warnings'].append('SELECTED_CHANGED_DURING_CAPTURE')
        for name, row in summary['profiles'].items():
            if row['presence'] == 'UNKNOWN':
                summary['warnings'].append('PROFILE_UNKNOWN:' + name)
        summary['stage'] = 'COMPLETE'
        summary['outcome'] = 'CAPTURED_OBSERVATIONS_ONLY'
    except (OSError, ValueError, KeyError) as error:
        # Do not persist exception text/paths; failed raw output could contain SIM data.
        summary['failure'] = type(error).__name__
        summary['warnings'].append('CAPTURE_INCOMPLETE_NO_RETRY')
    write_json(output / 'summary.json', summary)
    paths = sorted(p for p in output.iterdir() if p.is_file())
    (output / 'SHA256SUMS').write_text(''.join(hashlib.sha256(p.read_bytes()).hexdigest() + '  ' + p.name + '\n' for p in paths))
    return summary['outcome'] == 'CAPTURED_OBSERVATIONS_ONLY'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True, help='new directory; parent must already exist')
    parser.add_argument('--product-out', type=Path, required=True, help='local out/target/product/guacamole')
    args = parser.parse_args()
    try:
        ok = capture(args.output, args.product_out)
    except FileExistsError:
        raise SystemExit('Existing output refused; no adb invocation')
    print('CAPTURED_OBSERVATIONS_ONLY' if ok else 'INCOMPLETE: inspect safe summary; do not auto-retry')
    raise SystemExit(0 if ok else 1)


if __name__ == '__main__':
    main()
