#!/usr/bin/env python3
"""Only our pinned PE32 fixture and its default-off, scope-guarded observer."""
import argparse
from collections import Counter
import json
from pathlib import Path
import re
import time
import run as shared

root = shared.ROOT
helper = root / 'build/helper-owned-map-observer/bin/x87sidecar'
parser = argparse.ArgumentParser()
parser.add_argument('--tag', required=True)
parser.add_argument('--backends', nargs='+', choices=('decode-off', 'latest'), default=['decode-off', 'latest'])
parser.add_argument('--modes', nargs='+', choices=('idle', 'same-byte', 'changed-byte', 'neighbor-changed'), default=['changed-byte'])
parser.add_argument('--kinds', nargs='+', choices=('integer', 'x87'), default=['integer', 'x87'])
parser.add_argument('--rounds', type=int, default=64)
parser.add_argument('--program', choices=('short', 'rep'), default='short')
args = parser.parse_args()
if not args.tag.startswith('owned-recovery-observer-map-') or not args.tag.replace('-', '').isalnum() or not 1 <= args.rounds <= 512:
    raise RuntimeError('Unsafe bounds/tag')
out = root / 'runs' / args.tag
out.mkdir(exist_ok=False)
sources = root / 'source-owned-map-observer/rosetta_loader/src'
program_source = 'rep_active_page_probe.c' if args.program == 'rep' else 'mutable_active_page_probe.c'
program_stem = 'rep-active-page' if args.program == 'rep' else 'mutable-active-page'
inputs = [root / 'src/run-owned-map-observer.py', root / 'src/build-owned-map-observer.sh',
          root / 'src' / program_source, root / f'bin/{program_stem}-32.exe', helper,
          sources / 'main.cpp', sources / 'owned_recovery_observer.cpp', sources / 'owned_recovery_observer.hpp',
          shared.RUNTIME / 'bin/wine', shared.RUNTIME / 'bin/wineserver',
          shared.RUNTIME / 'lib/wine/x86_64-unix/ntdll.so',
          Path('/Library/Apple/usr/libexec/oah/libRosettaRuntime'), Path('/usr/libexec/rosetta/runtime')]
hashes = {str(p): shared.digest(p) for p in inputs}
if hashes['/usr/libexec/rosetta/runtime'] != '5d0e8f9a10093a8af1fbae31b59917be2ad1b248c4a71bf570982ba5966c41c4':
    raise RuntimeError('Recovery PC ranges require the pinned runtime')
(out / 'input-hashes.json').write_text(json.dumps(hashes, indent=2) + '\n')
results = []
for backend in args.backends:
    env = shared.harness.environment('r21-sidecar')
    try:
        boot = shared.harness.bounded([env['WINELOADER'], 'wineboot', '--init'], env,
                                     out / (backend + '-wineboot.log'), 45)
        if boot['exit'] or boot['timeout']: raise RuntimeError('Private prefix bootstrap failed')
        for path in (Path(env['WINEPREFIX']) / 'drive_c/users').glob('*'):
            if path.is_symlink() and not path.resolve().is_relative_to(Path(env['HOME'])):
                raise RuntimeError('Private profile escaped HOME')
        env.update({'X87_SIDECAR_PATH': str(helper), 'X87_LOGS': '1',
                    'X87_NO_DECODE_HOOK': '1' if backend == 'decode-off' else '0',
                    'X87_NATIVE_SLOTS': '1', 'X87_PRECISE_FAULTS': '0'})
        for mode in args.modes:
            for kind in args.kinds:
                name = f'{backend}-32-{mode}-{kind}'
                case_env = dict(env, X87_OWNED_RECOVERY_PROFILE=str(out / (name + '.observer')))
                (out / (name + '-environment.json')).write_text(json.dumps(case_env, indent=2) + '\n')
                log = out / (name + '.log')
                command = [env['WINELOADER'], str(root / f'bin/{program_stem}-32.exe'), mode, kind, str(args.rounds)]
                result = {'backend': backend, 'bits': 32, 'mode': mode, 'kind': kind, 'rounds': args.rounds, 'program': args.program,
                          **shared.harness.bounded(command, case_env, log, 20)}
                text = log.read_text(errors='replace')
                for label in ('writer', 'consumer', 'copy'):
                    matches = re.findall(r'^OBS ' + label + r' (.*)$', text, re.MULTILINE)
                    result[label] = {k: int(v) for k, v in re.findall(r'([a-z-]+)=(-?\d+)', matches[0])} if len(matches) == 1 else {}
                result['hook_installed'] = 'M2: handler installed' in text
                result['decode_installed'] = 'decode_opcode entry patched' in text
                result['decode_off_confirmed'] = 'skipping the decode_opcode hook' in text
                result['fallback'] = 'RUNNING WITHOUT X87 ACCELERATION' in text
                if result['matching_assertion'] or result['other_assertion']: result['status'] = 'ASSERTION'
                if result['status'] == 'PASS':
                    w, c = result['writer'], result['consumer']; operations = 0 if mode == 'idle' else args.rounds
                    valid = (w.get('completed') == args.rounds and w.get('writes') == w.get('flushes') == operations and
                             w.get('changed') == (args.rounds if mode in ('changed-byte', 'neighbor-changed') else 0) and
                             0 < w.get('progress', 0) <= args.rounds and w.get('ticks-before', 0) > 0 and
                             w.get('ticks-after') != w.get('ticks-before') and w.get('observed') == (3 if mode == 'changed-byte' else 1) and
                             w.get('bytes-ok') == w.get('protections-ok') == 1 and w.get('api-error') == 0 and
                             c == {'calls': 1, 'math': 2, 'errors': 0, 'result': 0x54321, 'exit': 0, 'joined': 1} and
                             result['hook_installed'] and not result['fallback'] and
                             ((result['decode_off_confirmed'] and not result['decode_installed']) if backend == 'decode-off' else result['decode_installed']) and
                             'failure-mask=0 joined=1 status=PASS' in text)
                    if args.program == 'rep':
                        valid &= result['copy'] == {'bytes': 1048576, 'source-unchanged': 1, 'destination-ok': 1}
                    if not valid: result['status'] = 'UNVERIFIED'
                results.append(result)
                (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
                print(f"{name}: {result['status']} exit={result['exit']} assertion={result['matching_assertion']} ({result['seconds']}s)", flush=True)
                if result['status'] != 'PASS': raise RuntimeError('Non-pass preserved; stop before further cases')
    finally:
        shared.harness.stop('r21-sidecar', env, out)
    for result in [r for r in results if r['backend'] == backend]:
        stem = f"{backend}-32-{result['mode']}-{result['kind']}"
        # The helper flushes after parent exit; wait only for that owned output.
        deadline = time.monotonic() + 3
        while True:
            files = list(out.glob(stem + '.observer.*.json'))
            try:
                if len(files) == 1:
                    observation = json.loads(files[0].read_text()); break
            except json.JSONDecodeError:
                pass
            if time.monotonic() >= deadline: raise RuntimeError('Observer file unavailable: ' + stem)
            time.sleep(0.05)
        hits = [{'tid': t['tid'], 'offset': k, 'samples': v}
                for t in observation['threads'] for k, v in t['runtime_offsets'].items()
                if 0xb5d4 <= int(k, 0) < 0xb63c or 0xb9b8 <= int(k, 0) < 0xba30]
        if sum(h['samples'] for h in hits) != observation['recovery_region_samples']:
            raise RuntimeError('Independent observer count mismatch')
        result['observer'] = {'path': str(files[0]), 'own_code_guarded': observation['own_code_guarded'],
                              'sweeps': observation['sweeps'], 'state_reads': observation['state_reads'],
                              'state_failures': observation['state_failures'], 'id_failures': observation['id_failures'],
                              'enumeration_failures': observation['enumeration_failures'],
                              'threads_seen': len(observation['threads']), 'limit_reached': observation['limit_reached'],
                              'recovery_region_samples': observation['recovery_region_samples'], 'hits': hits,
                              'first_map_call_samples': observation['first_map_call_samples'],
                              'second_map_call_samples': observation['second_map_call_samples'],
                              'generic_lookup_samples': observation['generic_lookup_samples'],
                              'event_overflow': observation['event_overflow'],
                              'interpretation': 'sampled recovery-region execution' if hits else 'not observed; cannot exclude execution'}
        if not observation['own_code_guarded'] or observation['limit_reached'] or not observation['sweeps'] or not observation['state_reads']:
            raise RuntimeError('Observer coverage unverified: ' + stem)
        (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
        print(f"  {stem}: tree-samples={observation['recovery_region_samples']} first-map={observation['first_map_call_samples']} second-map={observation['second_map_call_samples']} generic-lookup={observation['generic_lookup_samples']}", flush=True)
if any(shared.digest(Path(p)) != digest for p, digest in hashes.items()):
    raise RuntimeError('Input changed during run')
print('Summary: ' + json.dumps(dict(Counter(r['status'] for r in results))), flush=True)
