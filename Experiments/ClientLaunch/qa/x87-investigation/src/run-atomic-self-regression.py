#!/usr/bin/env python3
"""Existing owned memory/child/thread controls against the isolated module."""
import json
from pathlib import Path
import run as shared

root=shared.ROOT
candidate=root/'build/wine-atomic-self/runtime'
shared.harness.RUNTIMES['r21-stock']=candidate
out=root/'runs/layout-atomic-self-regression-oct28';out.mkdir(exist_ok=False)
cases=('control','writeproc-rwalloc','writeproc-execalloc','writeproc-explicit-toggle',
       'writeproc-mixed','writeproc-child','thread-int3','thread-mutate')
inputs=[root/'src/run-atomic-self-regression.py',root.parent/'probes/src/breakpoint_probe.c',
        root.parent/'probes/bin/probe-32.exe',root.parent/'probes/bin/probe-64.exe',
        candidate/'bin/wine',candidate/'bin/wineserver',candidate/'lib/wine/x86_64-unix/ntdll.so',
        root/'build/wine-atomic-self/virtual.c',Path('/usr/libexec/rosetta/runtime'),
        Path('/Library/Apple/usr/libexec/oah/libRosettaRuntime')]
hashes={str(p):shared.digest(p) for p in inputs}
(out/'input-hashes.json').write_text(json.dumps(hashes,indent=2)+'\n')
env=shared.harness.environment('r21-stock')
(out/'environment.json').write_text(json.dumps(env,indent=2)+'\n')
results=[]
try:
    boot=shared.harness.bounded([env['WINELOADER'],'wineboot','--init'],env,out/'wineboot.log',45)
    if boot['exit'] or boot['timeout']:raise RuntimeError('Private bootstrap failed')
    for bits in (32,64):
        for case in cases:
            command=[env['WINELOADER'],str(root.parent/f'probes/bin/probe-{bits}.exe'),case]
            a={'bits':bits,'case':case,**shared.harness.bounded(command,env,out/f'{bits}-{case}.log',30)}
            text=Path(a['log']).read_text(errors='replace')
            if a['matching_assertion'] or a['other_assertion']:a['status']='ASSERTION'
            if a['status']=='PASS' and f'END case={case} status=PASS exit=0' not in text:a['status']='UNVERIFIED'
            results.append(a);(out/'results.json').write_text(json.dumps(results,indent=2)+'\n')
            print(f"{bits} {case}: {a['status']} exit={a['exit']}",flush=True)
            if a['status']!='PASS':raise RuntimeError('Regression non-pass preserved; stopped')
finally:
    shared.harness.stop('r21-stock',env,out)
    changes=[p for p,sha in hashes.items() if shared.digest(Path(p))!=sha]
    (out/'input-recheck.json').write_text(json.dumps({'changed':changes},indent=2)+'\n')
    if changes:raise RuntimeError('Regression inputs changed')
