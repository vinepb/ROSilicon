#!/usr/bin/env python3
"""Paired owned reporting fixture; all stock-control caveats remain open."""
from collections import Counter
import json
from pathlib import Path
import re
import run as shared

root=shared.ROOT;rounds=16
out=root/'runs/layout-exception-paired-oct28';out.mkdir(exist_ok=False)
helper=root/'build/helper-upstream-oct26/bin/x87sidecar'
binary=root/'bin/layout-exception-32.exe'
inputs=[root/'src/run-layout-exception-paired.py',root/'src/layout_exception_probe.c',
        root/'src/layout_parallel_probe.c',root/'src/build-layout-exception-probe.sh',binary,
        helper,shared.RUNTIME/'bin/wine',shared.RUNTIME/'bin/wineserver',
        shared.RUNTIME/'lib/wine/x86_64-unix/ntdll.so',Path('/usr/libexec/rosetta/runtime'),
        Path('/Library/Apple/usr/libexec/oah/libRosettaRuntime')]
hashes={str(p):shared.digest(p) for p in inputs}
if hashes[str(binary)]!='7882ca4b0ba67cf7f3b65f07597f3cc124d039df1cc100818a135dbc73c898f8':
    raise RuntimeError('Reporter binary changed')
if hashes[str(helper)]!='5295200087c30e76f8f41f99136fef5b002344375f6095a2894edacab3b9ddfb':
    raise RuntimeError('Upstream helper changed')
(out/'input-hashes.json').write_text(json.dumps(hashes,indent=2)+'\n')
def parse(line):return {k:int(v) for k,v in re.findall(r'([a-z-]+)=(-?\d+)',line)}
def valid(text,mode,warm):
    writer=re.findall(r'^OBS writer (.*)$',text,re.M);consumer=re.findall(r'^OBS consumer (.*)$',text,re.M)
    workers=[parse(x) for x in re.findall(r'^OBS worker (.*)$',text,re.M)]
    if len(writer)!=1 or len(consumer)!=1 or len(workers)!=4:return False
    w=parse(writer[0]);c=parse(consumer[0]);n=rounds;ops=0 if mode=='idle' else n*32
    if not (w.get('batches')==n and w.get('writes')==w.get('flushes')==ops
            and w.get('changed')==(ops if mode in ('changed-byte','neighbor-changed') else 0)
            and 0<=w.get('before-first-tick',-1)<=ops and 0<w.get('progress',0)<=n*32
            and w.get('version-checks')==n*32 and w.get('bytes-ok')==1
            and w.get('protection-checks')==n*4 and w.get('api-error')==0
            and c=={'calls':n*4,'warm-calls':n*4 if warm else 0,'joined':4,'exit':0}):return False
    for i,v in enumerate(workers):
        slot=(n-1)*4+i;checksum=(0x12345678+slot+128*0x01010101+128*127//2)&0xffffffff
        if not (v.get('id')==i and v.get('calls')==n and v.get('result')==0x543210+slot
                and v.get('math')==2 and v.get('checksum')==checksum and v.get('errors')==0
                and v.get('ever')==(3 if mode=='changed-byte' else 2) and v.get('ticks',0)>0):return False
    return 'failure-mask=0 status=PASS' in text
results=[]
try:
    for backend in ('stock','upstream-off','upstream-on'):
        prefix='r21-stock' if backend=='stock' else 'r21-sidecar'
        env=shared.harness.environment(prefix)
        try:
            boot=shared.harness.bounded([env['WINELOADER'],'wineboot','--init'],env,out/(backend+'-wineboot.log'),45)
            if boot['exit'] or boot['timeout']:raise RuntimeError('Private bootstrap failed')
            for link in (Path(env['WINEPREFIX'])/'drive_c/users').glob('*'):
                if link.is_symlink() and not link.resolve().is_relative_to(Path(env['HOME'])):
                    raise RuntimeError('Profile escaped private HOME')
            env['WINEDEBUG']='-all,+seh,+timestamp'
            if backend!='stock':
                env.update({'X87_SIDECAR_PATH':str(helper),'X87_LOGS':'1',
                            'X87_NO_DECODE_HOOK':'1' if backend.endswith('-off') else '0',
                            'X87_NATIVE_SLOTS':'1','X87_PRECISE_FAULTS':'0'})
            (out/(backend+'-environment.json')).write_text(json.dumps(env,indent=2)+'\n')
            for mode in ('idle','same-byte','changed-byte','neighbor-changed'):
                for kind in ('integer','x87'):
                    for temperature in ('first','warm'):
                        name=f'{backend}-{mode}-{kind}-{temperature}'
                        command=[env['WINELOADER'],str(binary),mode,kind,temperature,str(rounds)]
                        a={'name':name,'backend':backend,'mode':mode,'kind':kind,
                           'temperature':temperature,'rounds':rounds,
                           **shared.harness.bounded(command,env,out/(name+'.log'),30)}
                        text=Path(a['log']).read_text(errors='replace')
                        a['unhandled_exceptions']=re.findall(r'^OWNED_UNHANDLED .*$',text,re.M)
                        a['hook_installed']='M2: handler installed' in text
                        a['decode_installed']='decode_opcode entry patched' in text
                        a['decode_off_confirmed']='skipping the decode_opcode hook' in text
                        a['fallback']='RUNNING WITHOUT X87 ACCELERATION' in text
                        hook_ok=(not a['hook_installed'] and not a['decode_installed']) if backend=='stock' else (
                            a['hook_installed'] and not a['fallback'] and (
                            a['decode_off_confirmed'] and not a['decode_installed'] if backend.endswith('-off') else a['decode_installed']))
                        if a['matching_assertion'] or a['other_assertion']:a['status']='ASSERTION'
                        if a['status']=='PASS' and (not valid(text,mode,temperature=='warm') or
                                                  not hook_ok or a['unhandled_exceptions']):a['status']='UNVERIFIED'
                        results.append(a);(out/'results.json').write_text(json.dumps(results,indent=2)+'\n')
                        print(f"{name}: {a['status']} exit={a['exit']} reports={len(a['unhandled_exceptions'])}",flush=True)
                        if a['status']!='PASS':raise RuntimeError('Non-pass preserved; paired sequence stopped')
        finally:
            stop=out/('stop-'+backend);stop.mkdir();shared.harness.stop(prefix,env,stop)
finally:
    changes=[p for p,sha in hashes.items() if shared.digest(Path(p))!=sha]
    (out/'input-recheck.json').write_text(json.dumps({'changed':changes},indent=2)+'\n')
    if changes:raise RuntimeError('Paired diagnostic input changed')
print('Summary: '+json.dumps(dict(Counter(a['status'] for a in results))),flush=True)
