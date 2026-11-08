#!/usr/bin/env python3
"""One owned remote-write stock sanity case; no existing target process."""
import json
from pathlib import Path
import re
import run as shared

root=shared.ROOT;out=root/'runs/remote-layout-stock-sanity-oct28';out.mkdir(exist_ok=False)
inputs=[root/'src/run-remote-layout-stock.py',root/'src/remote_layout_probe.c',
        root/'build/remote-owned-child/exception_reporter.h',root/'bin/remote-layout-32.exe',
        shared.RUNTIME/'bin/wine',shared.RUNTIME/'bin/wineserver',shared.RUNTIME/'lib/wine/x86_64-unix/ntdll.so',
        Path('/usr/libexec/rosetta/runtime'),Path('/Library/Apple/usr/libexec/oah/libRosettaRuntime')]
hashes={str(p):shared.digest(p) for p in inputs};(out/'input-hashes.json').write_text(json.dumps(hashes,indent=2)+'\n')
env=shared.harness.environment('r21-stock');results=[]
try:
    boot=shared.harness.bounded([env['WINELOADER'],'wineboot','--init'],env,out/'wineboot.log',45)
    if boot['exit'] or boot['timeout']:raise RuntimeError('Private bootstrap failed')
    env['WINEDEBUG']='-all,+seh,+timestamp'
    (out/'environment.json').write_text(json.dumps(env,indent=2)+'\n')
    command=[env['WINELOADER'],str(root/'bin/remote-layout-32.exe'),'--reporter-selfcheck']
    a=shared.harness.bounded(command,env,out/'reporter-selfcheck.log',30)
    text=Path(a['log']).read_text(errors='replace')
    page=re.search(r'OWNED_EXPECTED_FAULT allocation=([0-9a-f]{8})',text)
    report=re.search(r'^OWNED_UNHANDLED code=c0000005 .* access=00000001 fault-address=([0-9a-f]{8})$',text,re.M)
    ok=a['exit']==5 and not a['timeout'] and page and report and page[1]==report[1] and not a['matching_assertion']
    a['status']='EXPECTED_FAILURE' if ok else 'UNVERIFIED'
    (out/'selfcheck-result.json').write_text(json.dumps(a,indent=2)+'\n')
    if not ok:raise RuntimeError('Reporter control failed')
    a=shared.harness.bounded([env['WINELOADER'],str(root/'bin/remote-layout-32.exe'),'same-byte','integer','32'],env,out/'same-byte-integer.log',30)
    text=Path(a['log']).read_text(errors='replace')
    a['reports']=re.findall(r'^OWNED_UNHANDLED .*$',text,re.M)
    if a['status']=='PASS' and not ('END case=remote-layout failure-mask=0 status=PASS' in text and
                                  'child-exit=0 forced=0 child-ok=1' in text and not a['reports']):a['status']='UNVERIFIED'
    if a['matching_assertion'] or a['other_assertion']:a['status']='ASSERTION'
    results.append(a);(out/'results.json').write_text(json.dumps(results,indent=2)+'\n');print(json.dumps(a,indent=2),flush=True)
finally:
    shared.harness.stop('r21-stock',env,out)
    changes=[p for p,sha in hashes.items() if shared.digest(Path(p))!=sha]
    (out/'input-recheck.json').write_text(json.dumps({'changed':changes},indent=2)+'\n')
    if changes:raise RuntimeError('Remote fixture inputs changed')
