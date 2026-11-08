#!/usr/bin/env python3
"""Own immutable long-lived code only; reuse inventoried game-free prefixes."""
import argparse
from collections import Counter
import json
from pathlib import Path
import re
import run as shared

root = shared.ROOT
helpers = {"stock": None, "latest": root / "build/helper-upstream-oct26/bin/x87sidecar",
           "decode-off": root / "build/helper-upstream-oct26/bin/x87sidecar"}
modes = ("idle", "active-flush", "neighbor-flush", "active-protect", "neighbor-protect",
         "rwx-idle", "active-wpm", "neighbor-wpm")
parser = argparse.ArgumentParser()
parser.add_argument("--tag", required=True)
parser.add_argument("--backends", choices=list(helpers), nargs="+", default=["stock"])
parser.add_argument("--bits", type=int, choices=(32, 64), nargs="+", default=[32, 64])
parser.add_argument("--modes", choices=modes, nargs="+", default=list(modes))
parser.add_argument("--kinds", choices=("integer", "x87"), nargs="+", default=["integer", "x87"])
parser.add_argument("--rounds", type=int, default=256)
args = parser.parse_args()
if not args.tag.replace("-", "").isalnum() or not 1 <= args.rounds <= 4096: raise RuntimeError("Unsafe bounds/tag")
out = root / "runs" / args.tag
out.mkdir(exist_ok=False)
inputs = [root / "src/active_page_probe.c", root / "src/build-active-page-probes.sh",
          root / "src/run-active-pages.py", shared.RUNTIME / "bin/wine",
          shared.RUNTIME / "bin/wineserver", shared.RUNTIME / "lib/wine/x86_64-unix/ntdll.so",
          Path('/Library/Apple/usr/libexec/oah/libRosettaRuntime'), Path('/usr/libexec/rosetta/runtime'),
          *[root / f"bin/active-page-{bits}.exe" for bits in args.bits],
          *[helpers[backend] for backend in args.backends if helpers[backend]]]
(out / "input-hashes.json").write_text(json.dumps({str(p): shared.digest(p) for p in inputs}, indent=2) + "\n")
results = []
for backend in args.backends:
    prefix = "r21-stock" if backend == "stock" else "r21-sidecar"
    env = shared.harness.environment(prefix)
    try:
        boot = shared.harness.bounded([env["WINELOADER"], "wineboot", "--init"], env,
                                     out / (backend + "-wineboot.log"), 45)
        if boot['exit'] or boot['timeout']: raise RuntimeError('Owned prefix bootstrap failed')
        for path in (Path(env['WINEPREFIX']) / 'drive_c/users').glob('*'):
            if path.is_symlink() and not path.resolve().is_relative_to(Path(env['HOME'])): raise RuntimeError('Profile escaped private HOME')
        if helpers[backend]:
            env.update({'X87_SIDECAR_PATH': str(helpers[backend]), 'X87_LOGS': '1',
                        'X87_NO_DECODE_HOOK': '1' if backend == 'decode-off' else '0',
                        'X87_NATIVE_SLOTS': '1', 'X87_PRECISE_FAULTS': '0'})
        (out / (backend + '-environment.json')).write_text(json.dumps(env, indent=2) + '\n')
        for bits in args.bits:
            if bits == 64 and backend != 'stock': continue
            for mode in args.modes:
                for kind in args.kinds:
                    name = f'{backend}-{bits}-{mode}-{kind}'
                    log = out / (name + '.log')
                    command = [env['WINELOADER'], str(root / f'bin/active-page-{bits}.exe'), mode, kind, str(args.rounds)]
                    result = {'backend': backend, 'bits': bits, 'mode': mode, 'kind': kind, 'rounds': args.rounds,
                              'jit_recovery_coverage': 'not measured', **shared.harness.bounded(command, env, log, 25)}
                    text = log.read_text(errors='replace')
                    writer = re.findall(r'^OBS writer completed=(\d+) transitions=(\d+) flushes=(\d+) api-writes=(\d+) progress-rounds=(\d+) ticks-before=(\d+) ticks-after=(\d+) bytes-unchanged=(\d+) protections-ok=(\d+) api-error=(\d+)', text, re.MULTILINE)
                    consumer = re.findall(r'^OBS consumer calls=(\d+) ticks=(\d+) math=(-?\d+) result=(-?\d+) exit=(\d+) joined=(\d+)', text, re.MULTILINE)
                    result['writer'] = dict(zip(('completed', 'transitions', 'flushes', 'api_writes', 'progress_rounds', 'ticks_before', 'ticks_after', 'bytes_unchanged', 'protections_ok', 'api_error'), map(int, writer[0]))) if len(writer) == 1 else None
                    result['consumer'] = dict(zip(('calls', 'ticks', 'math', 'result', 'exit', 'joined'), map(int, consumer[0]))) if len(consumer) == 1 else None
                    result['hook_installed'] = 'M2: handler installed' in text
                    result['decode_installed'] = 'decode_opcode entry patched' in text
                    result['decode_off_confirmed'] = 'skipping the decode_opcode hook' in text
                    result['fallback'] = 'RUNNING WITHOUT X87 ACCELERATION' in text
                    if 'status=UNVERIFIED' in text: result['status'] = 'UNVERIFIED'
                    if result['matching_assertion'] or result['other_assertion']: result['status'] = 'ASSERTION'
                    if result['status'] == 'PASS':
                        w, c = result['writer'], result['consumer']
                        valid = bool(w and c)
                        if valid:
                            protected = mode.endswith('-protect')
                            valid &= w['completed'] == args.rounds and w['transitions'] == (2 * args.rounds if protected else 0)
                            valid &= w['flushes'] == (0 if mode in ('idle', 'rwx-idle') else 2 * args.rounds if protected else args.rounds)
                            valid &= w['api_writes'] == (args.rounds if mode.endswith('-wpm') else 0)
                            valid &= 0 < w['progress_rounds'] <= args.rounds and w['ticks_before'] > 0 and w['ticks_after'] != w['ticks_before']
                            valid &= w['bytes_unchanged'] == w['protections_ok'] == 1 and w['api_error'] == 0
                            valid &= c['calls'] == c['joined'] == 1 and c['math'] == 2 and c['result'] == 0x54321 and c['exit'] == 0
                            valid &= f'completed={args.rounds} joined=1 failure-mask=0 status=PASS' in text
                            valid &= f"code-protect=0x{'40' if mode in ('rwx-idle', 'active-wpm', 'neighbor-wpm') else '20'}" in text
                        if backend != 'stock':
                            valid &= result['hook_installed'] and not result['fallback']
                            valid &= (result['decode_off_confirmed'] and not result['decode_installed']) if backend == 'decode-off' else result['decode_installed']
                        if not valid: result['status'] = 'UNVERIFIED'
                    results.append(result)
                    (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
                    print(f"{name}: {result['status']} exit={result['exit']} Rosetta-match={result['matching_assertion']} ({result['seconds']}s)", flush=True)
                    if result['matching_assertion'] or result['other_assertion']: raise RuntimeError('Assertion preserved; stop and inspect before expanding')
                    if result['timeout']: shared.harness.stop(prefix, env, out)
    finally:
        shared.harness.stop(prefix, env, out)
print('Summary: ' + json.dumps(dict(Counter(r['status'] for r in results))), flush=True)
raise SystemExit(0 if all(r['status'] == 'PASS' for r in results) else 1)
