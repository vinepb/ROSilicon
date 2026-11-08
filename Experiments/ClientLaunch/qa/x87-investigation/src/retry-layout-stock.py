#!/usr/bin/env python3
"""Bounded repeats of only the owned intermittent stock case; stop on failure."""
import json
from pathlib import Path
import run as shared

root=shared.ROOT;out=root/'runs/layout-parallel-stock-retries-oct28';out.mkdir(exist_ok=False)
inputs=[root/'src/retry-layout-stock.py',root/'src/layout_parallel_probe.c',root/'bin/layout-parallel-32.exe',
        shared.RUNTIME/'bin/wine',shared.RUNTIME/'bin/wineserver',shared.RUNTIME/'lib/wine/x86_64-unix/ntdll.so',
        Path('/usr/libexec/rosetta/runtime'),Path('/Library/Apple/usr/libexec/oah/libRosettaRuntime')]
hashes={str(p):shared.digest(p) for p in inputs};(out/'input-hashes.json').write_text(json.dumps(hashes,indent=2)+'\n')
env=shared.harness.environment('r21-stock');results=[]
try:
    boot=shared.harness.bounded([env['WINELOADER'],'wineboot','--init'],env,out/'wineboot.log',45)
    if boot['exit'] or boot['timeout']:raise RuntimeError('Private bootstrap failed')
    for attempt in range(1,9):
        case_env=dict(env);case_env['WINEDEBUG']='-all,+seh,+timestamp' if attempt%2==0 else '-all'
        (out/f'attempt-{attempt}-environment.json').write_text(json.dumps(case_env,indent=2)+'\n')
        command=[env['WINELOADER'],str(root/'bin/layout-parallel-32.exe'),'changed-byte','x87','first','2']
        result={'attempt':attempt,'seh_logging':attempt%2==0,**shared.harness.bounded(command,case_env,out/f'attempt-{attempt}.log',30)}
        results.append(result);(out/'results.json').write_text(json.dumps(results,indent=2)+'\n')
        print(f"attempt={attempt} SEH={result['seh_logging']} status={result['status']} exit={result['exit']} assertion={result['matching_assertion']}",flush=True)
        if result['status']!='PASS':break
finally:shared.harness.stop('r21-stock',env,out)
if any(shared.digest(Path(p))!=sha for p,sha in hashes.items()):raise RuntimeError('Input changed during retries')
