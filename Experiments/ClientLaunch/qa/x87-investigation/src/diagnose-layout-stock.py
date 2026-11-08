#!/usr/bin/env python3
"""One exact own-fixture stock retry with Wine SEH diagnostics; no game/helper."""
import json
from pathlib import Path
import run as shared

root=shared.ROOT
out=root/'runs/layout-parallel-stock-diagnostic-oct28';out.mkdir(exist_ok=False)
inputs=[root/'src/diagnose-layout-stock.py',root/'src/layout_parallel_probe.c',root/'bin/layout-parallel-32.exe',
        shared.RUNTIME/'bin/wine',shared.RUNTIME/'bin/wineserver',shared.RUNTIME/'lib/wine/x86_64-unix/ntdll.so',
        Path('/usr/libexec/rosetta/runtime'),Path('/Library/Apple/usr/libexec/oah/libRosettaRuntime')]
hashes={str(p):shared.digest(p) for p in inputs}
(out/'input-hashes.json').write_text(json.dumps(hashes,indent=2)+'\n')
env=shared.harness.environment('r21-stock')
try:
    boot=shared.harness.bounded([env['WINELOADER'],'wineboot','--init'],env,out/'wineboot.log',45)
    if boot['exit'] or boot['timeout']:raise RuntimeError('Private bootstrap failed')
    env['WINEDEBUG']='-all,+seh,+timestamp'
    (out/'environment.json').write_text(json.dumps(env,indent=2)+'\n')
    command=[env['WINELOADER'],str(root/'bin/layout-parallel-32.exe'),'changed-byte','x87','first','2']
    result=shared.harness.bounded(command,env,out/'stock-layout-x87-seh.log',30)
    (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2))
finally:
    shared.harness.stop('r21-stock',env,out)
if any(shared.digest(Path(p))!=sha for p,sha in hashes.items()):raise RuntimeError('Input changed during retry')
