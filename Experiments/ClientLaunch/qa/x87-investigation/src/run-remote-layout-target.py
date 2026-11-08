#!/usr/bin/env python3
"""One candidate transaction check; targets only the PE's own new child."""
import json
from pathlib import Path
import re
import run as shared

root=shared.ROOT;candidate=root/'build/wine-atomic-target/runtime'
shared.harness.RUNTIMES['r21-stock']=candidate
out=root/'runs/remote-layout-target-sanity-oct28';out.mkdir(exist_ok=False)
binary=root/'bin/remote-layout-32.exe';module=candidate/'lib/wine/x86_64-unix/ntdll.so'
inputs=[root/'src/run-remote-layout-target.py',root/'src/remote_layout_probe.c',
        root/'build/remote-owned-child/exception_reporter.h',binary,candidate/'bin/wine',
        candidate/'bin/wineserver',module,root/'build/wine-atomic-target/source-diff.patch',
        Path('/usr/libexec/rosetta/runtime'),Path('/Library/Apple/usr/libexec/oah/libRosettaRuntime')]
hashes={str(p):shared.digest(p) for p in inputs}
expected=json.loads((root/'build/wine-atomic-target/output-hashes.json').read_text())
for p in (module,candidate/'bin/wineserver'):
    if hashes[str(p)]!=expected[str(p)]:raise RuntimeError('Candidate image changed')
(out/'input-hashes.json').write_text(json.dumps(hashes,indent=2)+'\n')
env=shared.harness.environment('r21-stock')
def fields(text):return {k:int(v) for k,v in re.findall(r'([a-z-]+)=(-?\d+)',text)}
try:
    boot=shared.harness.bounded([env['WINELOADER'],'wineboot','--init'],env,out/'wineboot.log',45)
    if boot['exit'] or boot['timeout']:raise RuntimeError('Matching candidate bootstrap failed')
    env['WINEDEBUG']='-all,+seh,+timestamp'
    control=shared.harness.bounded([env['WINELOADER'],str(binary),'--reporter-selfcheck'],env,out/'reporter-selfcheck.log',30)
    text=Path(control['log']).read_text(errors='replace')
    page=re.search(r'OWNED_EXPECTED_FAULT allocation=([0-9a-f]{8})',text)
    report=re.search(r'^OWNED_UNHANDLED code=c0000005 .* access=00000001 fault-address=([0-9a-f]{8})$',text,re.M)
    ok=(control['exit']==5 and not control['timeout'] and not control['matching_assertion'] and
        not control['other_assertion'] and page and report and page[1]==report[1])
    control['status']='EXPECTED_FAILURE' if ok else 'UNVERIFIED'
    (out/'selfcheck-result.json').write_text(json.dumps(control,indent=2)+'\n')
    if not ok:raise RuntimeError('Reporter fault control failed')
    env['DYLD_PRINT_LIBRARIES']='1'
    (out/'environment.json').write_text(json.dumps(env,indent=2)+'\n')
    a=shared.harness.bounded([env['WINELOADER'],str(binary),'same-byte','integer','32'],env,out/'same-byte-integer.log',30)
    text=Path(a['log']).read_text(errors='replace')
    a['reports']=re.findall(r'^OWNED_UNHANDLED .*$',text,re.M)
    a['parent']=[fields(x) for x in re.findall(r'^REMOTE_PARENT (.*)$',text,re.M)]
    a['child']=[fields(x) for x in re.findall(r'^REMOTE_CHILD joined=(.*)$',text,re.M)]
    a['workers']=[fields(x) for x in re.findall(r'^REMOTE_CHILD worker (.*)$',text,re.M)]
    native_ids=set(re.findall(r'^dyld\[(\d+)\]: .* '+re.escape(str(module))+r'$',text,re.M))
    a['candidate_image_native_pids']=sorted(native_ids,key=int)
    old=str(root.parent/'probes/patched-runtime/lib/wine/x86_64-unix/ntdll.so')
    ok=(len(a['parent'])==1 and len(a['child'])==1 and len(a['workers'])==4 and len(native_ids)>=2 and
        old not in text and not a['reports'] and 'END case=remote-layout failure-mask=0 status=PASS' in text)
    if ok:
        p=a['parent'][0];c=a['child'][0]
        # The child regex consumes joined=; recover its known leading value.
        joined=re.search(r'^REMOTE_CHILD joined=(\d+)',text,re.M)
        ok=(p.get('completed')==32 and p.get('writes')==p.get('flushes')==128 and p.get('changed')==0 and
            0<p.get('progress',0)<=128 and p.get('version-checks')==128 and p.get('api-error')==0 and
            p.get('child-exit')==0 and p.get('forced')==0 and p.get('child-ok')==1 and joined[1]=='4' and
            c.get('protections')==4 and c.get('bytes-ok')==1 and c.get('failure-mask')==0)
        for i,w in enumerate(a['workers']):
            checksum=(0x12345678+i+128*0x01010101+128*127//2)&0xffffffff
            ok &= (w.get('id')==i and w.get('result')==0x543210+i and w.get('math')==2 and
                   w.get('checksum')==checksum and w.get('errors')==0 and w.get('ever')==2 and w.get('ticks',0)>0)
    if a['matching_assertion'] or a['other_assertion']:a['status']='ASSERTION'
    if a['status']=='PASS' and not ok:a['status']='UNVERIFIED'
    (out/'results.json').write_text(json.dumps([a],indent=2)+'\n');print(json.dumps(a,indent=2),flush=True)
finally:
    shared.harness.stop('r21-stock',env,out)
    changes=[p for p,sha in hashes.items() if shared.digest(Path(p))!=sha]
    (out/'input-recheck.json').write_text(json.dumps({'changed':changes},indent=2)+'\n')
    if changes:raise RuntimeError('Candidate test inputs changed')
