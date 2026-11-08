#!/usr/bin/env python3
"""Bounded own-code stress; optional existing read-only sampler on that PE only."""
import argparse
from collections import Counter
import json
from pathlib import Path
import re
import run as shared

root = shared.ROOT
helper = root / 'build/helper-upstream-oct26/bin/x87sidecar'
modes = ('idle', 'same-byte', 'changed-byte', 'neighbor-changed')
parser = argparse.ArgumentParser()
parser.add_argument('--tag', required=True)
parser.add_argument('--backends', nargs='+', choices=('stock', 'latest', 'decode-off'), default=['stock'])
parser.add_argument('--bits', type=int, choices=(32, 64), nargs='+', default=[32])
parser.add_argument('--modes', choices=modes, nargs='+', default=list(modes))
parser.add_argument('--kinds', choices=('integer', 'x87'), nargs='+', default=['integer', 'x87'])
parser.add_argument('--rounds', type=int, default=16)
parser.add_argument('--sample', action='store_true')
parser.add_argument('--program', choices=('short', 'rep'), default='short')
args = parser.parse_args()
if not args.tag.replace('-', '').isalnum() or not 1 <= args.rounds <= 512:
    raise RuntimeError('Unsafe bounds/tag')
if args.sample and ('stock' in args.backends or 64 in args.bits):
    raise RuntimeError('Existing sampler requires the PE32 helper; stock stays unsampled')
if args.program == 'rep' and 64 in args.bits: raise RuntimeError('The separate REP fixture is PE32 only')
out = root / 'runs' / args.tag
out.mkdir(exist_ok=False)
program_source = 'rep_active_page_probe.c' if args.program == 'rep' else 'mutable_active_page_probe.c'
program_stem = 'rep-active-page' if args.program == 'rep' else 'mutable-active-page'
builder = 'build-rep-active-page-probe.sh' if args.program == 'rep' else 'build-mutable-active-page-probes.sh'
inputs = [root / 'src' / program_source, root / 'src' / builder,
          root / 'src/run-mutable-active-pages.py', root / 'src/review-recovery-samples.py',
          shared.RUNTIME / 'bin/wine', shared.RUNTIME / 'bin/wineserver',
          shared.RUNTIME / 'lib/wine/x86_64-unix/ntdll.so',
          Path('/Library/Apple/usr/libexec/oah/libRosettaRuntime'), Path('/usr/libexec/rosetta/runtime'),
          *[root / f'bin/{program_stem}-{bits}.exe' for bits in args.bits]]
if any(b != 'stock' for b in args.backends): inputs.append(helper)
hashes = {str(p): shared.digest(p) for p in inputs}
if hashes['/usr/libexec/rosetta/runtime'] != '5d0e8f9a10093a8af1fbae31b59917be2ad1b248c4a71bf570982ba5966c41c4':
    raise RuntimeError('Static recovery ranges require the pinned current runtime')
(out / 'input-hashes.json').write_text(json.dumps(hashes, indent=2) + '\n')
results = []
for backend in args.backends:
    prefix = 'r21-stock' if backend == 'stock' else 'r21-sidecar'
    env = shared.harness.environment(prefix)
    try:
        boot = shared.harness.bounded([env['WINELOADER'], 'wineboot', '--init'], env,
                                     out / (backend + '-wineboot.log'), 45)
        if boot['exit'] or boot['timeout']: raise RuntimeError('Owned prefix bootstrap failed')
        for path in (Path(env['WINEPREFIX']) / 'drive_c/users').glob('*'):
            if path.is_symlink() and not path.resolve().is_relative_to(Path(env['HOME'])):
                raise RuntimeError('Profile escaped private HOME')
        if backend != 'stock':
            env.update({'X87_SIDECAR_PATH': str(helper), 'X87_LOGS': '1',
                        'X87_NO_DECODE_HOOK': '1' if backend == 'decode-off' else '0',
                        'X87_NATIVE_SLOTS': '1', 'X87_PRECISE_FAULTS': '0'})
        for bits in args.bits:
            if bits == 64 and backend != 'stock': continue
            for mode in args.modes:
                for kind in args.kinds:
                    name = f'{backend}-{bits}-{mode}-{kind}'
                    case_env = dict(env)
                    if args.sample:
                        case_env.update({'X87_SAMPLE': str(out / (name + '.profile')),
                                         'X87_NO_UNWIND': '1', 'X87_SAMPLE_HZ': '10000',
                                         'X87_SAMPLE_SWEEP_HZ': '1000', 'X87_SAMPLE_REPORT': '0.2',
                                         'X87_SAMPLE_WINDOWS': '0',
                                         'X87_GUEST_RANGE': '0x25000000-0x25001000'})
                    (out / (name + '-environment.json')).write_text(json.dumps(case_env, indent=2) + '\n')
                    command = [env['WINELOADER'], str(root / f'bin/{program_stem}-{bits}.exe'),
                               mode, kind, str(args.rounds)]
                    log = out / (name + '.log')
                    result = {'backend': backend, 'bits': bits, 'mode': mode, 'kind': kind, 'program': args.program,
                              'rounds': args.rounds, 'sampled': args.sample,
                              **shared.harness.bounded(command, case_env, log, 20)}
                    text = log.read_text(errors='replace')
                    fields = {}
                    for label in ('writer', 'consumer', 'copy'):
                        matches = re.findall(r'^OBS ' + label + r' (.*)$', text, re.MULTILINE)
                        fields[label] = {k: int(v) for k, v in re.findall(r'([a-z-]+)=(-?\d+)', matches[0])} if len(matches) == 1 else {}
                    result.update(fields)
                    result['hook_installed'] = 'M2: handler installed' in text
                    result['decode_installed'] = 'decode_opcode entry patched' in text
                    result['decode_off_confirmed'] = 'skipping the decode_opcode hook' in text
                    result['fallback'] = 'RUNNING WITHOUT X87 ACCELERATION' in text
                    if result['matching_assertion'] or result['other_assertion']: result['status'] = 'ASSERTION'
                    if result['status'] == 'PASS':
                        w, c = fields['writer'], fields['consumer']
                        expected = 0 if mode == 'idle' else args.rounds
                        valid = (w.get('completed') == args.rounds and w.get('writes') == expected and
                                 w.get('changed') == (args.rounds if mode in ('changed-byte', 'neighbor-changed') else 0) and
                                 w.get('flushes') == expected and 0 < w.get('progress', 0) <= args.rounds and
                                 w.get('ticks-before', 0) > 0 and w.get('ticks-after') != w.get('ticks-before') and
                                 w.get('observed') == (3 if mode == 'changed-byte' else 1) and
                                 w.get('bytes-ok') == w.get('protections-ok') == 1 and w.get('api-error') == 0 and
                                 c == {'calls': 1, 'math': 2, 'errors': 0, 'result': 0x54321, 'exit': 0, 'joined': 1} and
                                 'failure-mask=0 joined=1 status=PASS' in text)
                        if backend != 'stock':
                            valid &= result['hook_installed'] and not result['fallback']
                            valid &= (result['decode_off_confirmed'] and not result['decode_installed']) if backend == 'decode-off' else result['decode_installed']
                        if args.program == 'rep':
                            valid &= result['copy'] == {'bytes': 1048576, 'source-unchanged': 1, 'destination-ok': 1}
                        if not valid: result['status'] = 'UNVERIFIED'
                    results.append(result)
                    (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
                    print(f"{name}: {result['status']} exit={result['exit']} assertion={result['matching_assertion']} ({result['seconds']}s)", flush=True)
                    if result['status'] != 'PASS':
                        raise RuntimeError('Non-pass preserved; stop before further cases')
    finally:
        shared.harness.stop(prefix, env, out)
if any(shared.digest(Path(p)) != digest for p, digest in hashes.items()):
    raise RuntimeError('Input changed during the run')
print('Summary: ' + json.dumps(dict(Counter(r['status'] for r in results))), flush=True)
