#!/usr/bin/env python3
"""Report only our own unhandled exception; stock Rosetta/private prefix."""
import json
from pathlib import Path
import re
import run as shared

root=shared.ROOT
out=root/'runs/layout-exception-stock-oct28'
out.mkdir(exist_ok=False)
inputs=[root/'src/run-layout-exception-stock.py',root/'src/layout_exception_probe.c',
        root/'src/layout_parallel_probe.c',root/'src/build-layout-exception-probe.sh',
        root/'bin/layout-exception-32.exe',shared.RUNTIME/'bin/wine',
        shared.RUNTIME/'bin/wineserver',shared.RUNTIME/'lib/wine/x86_64-unix/ntdll.so',
        Path('/usr/libexec/rosetta/runtime'),Path('/Library/Apple/usr/libexec/oah/libRosettaRuntime')]
hashes={str(p):shared.digest(p) for p in inputs}
if hashes[str(root/'src/layout_parallel_probe.c')]!='e2fd4352a9920ac179a948891cb3719aaa0c3935ac271c90927f755f6e876bc5':
    raise RuntimeError('Original fixture changed')
(out/'input-hashes.json').write_text(json.dumps(hashes,indent=2)+'\n')
env=shared.harness.environment('r21-stock')
if any(k.startswith('X87_') for k in env):raise RuntimeError('Unexpected helper environment')
env['WINEDEBUG']='-all,+seh,+timestamp'
(out/'environment.json').write_text(json.dumps(env,indent=2)+'\n')
results=[]
def parse(row):return {k:int(v) for k,v in re.findall(r'([a-z-]+)=(-?\d+)',row)}
def verify(text,n):
    writer=re.findall(r'^OBS writer (.*)$',text,re.M)
    consumer=re.findall(r'^OBS consumer (.*)$',text,re.M)
    workers=[parse(x) for x in re.findall(r'^OBS worker (.*)$',text,re.M)]
    if len(writer)!=1 or len(consumer)!=1 or len(workers)!=4:return False
    w=parse(writer[0]);c=parse(consumer[0]);ops=n*32
    if not (w.get('batches')==n and w.get('writes')==w.get('changed')==w.get('flushes')==ops
            and 0<=w.get('before-first-tick',-1)<=ops and 0<w.get('progress',0)<=ops
            and w.get('version-checks')==ops and w.get('bytes-ok')==1
            and w.get('protection-checks')==n*4 and w.get('api-error')==0
            and c=={'calls':n*4,'warm-calls':0,'joined':4,'exit':0}):return False
    for i,v in enumerate(workers):
        slot=(n-1)*4+i
        checksum=(0x12345678+slot+128*0x01010101+128*127//2)&0xffffffff
        if not (v.get('id')==i and v.get('calls')==n and v.get('result')==0x543210+slot
                and v.get('math')==2 and v.get('checksum')==checksum and v.get('errors')==0
                and v.get('ever')==3 and v.get('ticks',0)>0):return False
    return 'failure-mask=0 status=PASS' in text
try:
    boot=shared.harness.bounded([env['WINELOADER'],'wineboot','--init'],env,out/'wineboot.log',45)
    if boot['exit'] or boot['timeout']:raise RuntimeError('Private bootstrap failed')
    for link in (Path(env['WINEPREFIX'])/'drive_c/users').glob('*'):
        if link.is_symlink() and not link.resolve().is_relative_to(Path(env['HOME'])):
            raise RuntimeError('Profile escaped private HOME')
    control=shared.harness.bounded([env['WINELOADER'],str(root/'bin/layout-exception-32.exe'),
                                   '--reporter-selfcheck'],env,out/'reporter-selfcheck.log',30)
    text=Path(control['log']).read_text(errors='replace')
    page=re.search(r'OWNED_EXPECTED_FAULT allocation=([0-9a-f]{8})',text)
    fault=re.search(r'^OWNED_UNHANDLED code=c0000005 address=([0-9a-f]{8}).* access=00000001 fault-address=([0-9a-f]{8})$',text,re.M)
    verified=(control['exit']==5 and not control['timeout'] and not control['matching_assertion']
              and not control['other_assertion'] and page and fault and page[1]==fault[2])
    control['status']='EXPECTED_FAILURE' if verified else 'UNVERIFIED'
    (out/'selfcheck-result.json').write_text(json.dumps(control,indent=2)+'\n')
    print(f"reporter-selfcheck: {control['status']} exit={control['exit']}",flush=True)
    if not verified:raise RuntimeError('Reporter selfcheck failed; no stress cases run')
    for attempt in range(1,17):
        rounds=2 if attempt<=4 else 16
        command=[env['WINELOADER'],str(root/'bin/layout-exception-32.exe'),
                 'changed-byte','x87','first',str(rounds)]
        a={'attempt':attempt,'rounds':rounds,**shared.harness.bounded(command,env,out/f'attempt-{attempt}.log',30)}
        text=Path(a['log']).read_text(errors='replace')
        a['unhandled_exceptions']=re.findall(r'^OWNED_UNHANDLED .*$',text,re.M)
        if a['status']=='PASS' and (not verify(text,rounds) or a['unhandled_exceptions']):a['status']='UNVERIFIED'
        if a['matching_assertion'] or a['other_assertion']:a['status']='ASSERTION'
        results.append(a);(out/'results.json').write_text(json.dumps(results,indent=2)+'\n')
        print(f"attempt={attempt} rounds={rounds} status={a['status']} exit={a['exit']} reports={len(a['unhandled_exceptions'])}",flush=True)
        if a['status']!='PASS':break
finally:
    shared.harness.stop('r21-stock',env,out)
    changes=[p for p,sha in hashes.items() if shared.digest(Path(p))!=sha]
    (out/'input-recheck.json').write_text(json.dumps({'changed':changes},indent=2)+'\n')
    if changes:raise RuntimeError('Diagnostic input changed')
