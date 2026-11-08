#!/usr/bin/env python3
"""Only the owned PE32 fixture, fixed test prefixes, and optional map observer."""
import argparse
from collections import Counter
import json
from pathlib import Path
import re
import time
import run as shared

root=shared.ROOT
helpers={'stock':None,'upstream-on':root/'build/helper-upstream-oct26/bin/x87sidecar',
         'upstream-off':root/'build/helper-upstream-oct26/bin/x87sidecar',
         'map-on':root/'build/helper-owned-map-observer/bin/x87sidecar',
         'map-off':root/'build/helper-owned-map-observer/bin/x87sidecar'}
p=argparse.ArgumentParser()
p.add_argument('--tag',required=True)
p.add_argument('--backends',nargs='+',choices=list(helpers),default=['stock'])
p.add_argument('--modes',nargs='+',choices=('idle','same-byte','changed-byte','neighbor-changed'),default=['idle','changed-byte'])
p.add_argument('--kinds',nargs='+',choices=('integer','x87'),default=['integer','x87'])
p.add_argument('--temperatures',nargs='+',choices=('first','warm'),default=['first','warm'])
p.add_argument('--rounds',type=int,default=2)
args=p.parse_args()
if not args.tag.replace('-','').isalnum() or not 1<=args.rounds<=64:raise RuntimeError('Unsafe tag/bounds')
if any(b.startswith('map-') for b in args.backends) and not args.tag.startswith('owned-recovery-observer-map-'):
    raise RuntimeError('Observed output requires the owned observer path prefix')
out=root/'runs'/args.tag;out.mkdir(exist_ok=False)
inputs=[root/'src/run-cold-parallel.py',root/'src/cold_parallel_probe.c',root/'src/build-cold-parallel-probe.sh',
        root/'bin/cold-parallel-32.exe',shared.RUNTIME/'bin/wine',shared.RUNTIME/'bin/wineserver',shared.RUNTIME/'lib/wine/x86_64-unix/ntdll.so',
        Path('/usr/libexec/rosetta/runtime'),Path('/Library/Apple/usr/libexec/oah/libRosettaRuntime')]
inputs += [helpers[b] for b in args.backends if helpers[b]]
if any(b.startswith('map-') for b in args.backends):
    source=root/'source-owned-map-observer/rosetta_loader/src'
    inputs += [root/'src/build-owned-map-observer.sh',*[source/n for n in ('main.cpp','owned_recovery_observer.cpp','owned_recovery_observer.hpp')]]
hashes={str(q):shared.digest(q) for q in inputs}
if hashes['/usr/libexec/rosetta/runtime']!='5d0e8f9a10093a8af1fbae31b59917be2ad1b248c4a71bf570982ba5966c41c4':
    raise RuntimeError('Map observations require the pinned runtime')
(out/'input-hashes.json').write_text(json.dumps(hashes,indent=2)+'\n')
def parse(line):return {k:int(v) for k,v in re.findall(r'([a-z-]+)=(-?\d+)',line)}
def valid(a):
    w,c=a['writer'],a['consumer'];n=args.rounds;mode=a['mode'];operations=0 if mode=='idle' else n*32
    ok=(w.get('batches')==n and w.get('writes')==w.get('flushes')==operations and
        w.get('changed')==(operations if mode in ('changed-byte','neighbor-changed') else 0) and
        0<=w.get('before-first-tick',-1)<=operations and 0<w.get('progress',0)<=n*32 and
        w.get('version-checks')==n*32 and w.get('bytes-ok')==1 and w.get('protection-checks')==n*4 and w.get('api-error')==0 and
        c=={'calls':n*4,'warm-calls':n*4 if a['temperature']=='warm' else 0,'joined':4,'exit':0} and len(a['workers'])==4)
    if ok:
        for i,worker in enumerate(a['workers']):
            slot=(n-1)*4+i;expected_checksum=(0x12345678+slot+128*0x01010101+128*127//2)&0xffffffff
            ok &= worker.get('id')==i and worker.get('calls')==n and worker.get('result')==0x543210+slot and worker.get('math')==2
            ok &= worker.get('errors')==0 and worker.get('ever')==(3 if mode=='changed-byte' else 1) and worker.get('checksum')==expected_checksum and worker.get('ticks',0)>0
    if a['backend']!='stock':
        ok &= a['hook_installed'] and not a['fallback']
        ok &= (a['decode_off_confirmed'] and not a['decode_installed']) if a['backend'].endswith('-off') else a['decode_installed']
    return ok
def observe(a):
    deadline=time.monotonic()+3
    while True:
        files=list(out.glob(a['name']+'.observer.*.json'))
        try:
            if len(files)==1:o=json.loads(files[0].read_text());break
        except json.JSONDecodeError:pass
        if time.monotonic()>=deadline:raise RuntimeError('Missing observer file: '+a['name'])
        time.sleep(.05)
    tree=sum(v for t in o['threads'] for k,v in t['runtime_offsets'].items() if 0xb5d4<=int(k,0)<0xb63c or 0xb9b8<=int(k,0)<0xba30)
    first=sum(0x1b5b8<=int(e['pc_offset'],0)<0x1badc and int(e['lr_offset'],0)==0xb9dc for e in o['recovery_events'])
    second=sum(0x1b5b8<=int(e['pc_offset'],0)<0x1badc and int(e['lr_offset'],0)==0xb9f0 for e in o['recovery_events'])
    if not o['own_code_guarded'] or o['limit_reached'] or not o['sweeps'] or not o['state_reads'] or tree!=o['recovery_region_samples']:
        raise RuntimeError('Observer coverage unverified: '+a['name'])
    if not o['event_overflow'] and (first!=o['first_map_call_samples'] or second!=o['second_map_call_samples']):
        raise RuntimeError('Map-call recount mismatch')
    a['observer']={'path':str(files[0]),'threads_seen':len(o['threads']),**{k:o[k] for k in
        ('state_reads','state_failures','id_failures','enumeration_failures','recovery_region_samples','generic_lookup_samples','first_map_call_samples','second_map_call_samples','event_overflow')}}
    print(f"  {a['name']}: tree={tree} first-map={first} second-map={second} generic-lookup={o['generic_lookup_samples']}",flush=True)
results=[]
for backend in args.backends:
    prefix='r21-stock' if backend=='stock' else 'r21-sidecar';env=shared.harness.environment(prefix)
    try:
        boot=shared.harness.bounded([env['WINELOADER'],'wineboot','--init'],env,out/(backend+'-wineboot.log'),45)
        if boot['exit'] or boot['timeout']:raise RuntimeError('Private bootstrap failed')
        for q in (Path(env['WINEPREFIX'])/'drive_c/users').glob('*'):
            if q.is_symlink() and not q.resolve().is_relative_to(Path(env['HOME'])):raise RuntimeError('Private profile escaped HOME')
        if helpers[backend]:env.update({'X87_SIDECAR_PATH':str(helpers[backend]),'X87_LOGS':'1','X87_NO_DECODE_HOOK':'1' if backend.endswith('-off') else '0',
                                       'X87_NATIVE_SLOTS':'1','X87_PRECISE_FAULTS':'0'})
        for mode in args.modes:
            for kind in args.kinds:
                for temperature in args.temperatures:
                    name=f'{backend}-32-{mode}-{kind}-{temperature}';case_env=dict(env)
                    if backend.startswith('map-'):case_env['X87_OWNED_RECOVERY_PROFILE']=str(out/(name+'.observer'))
                    (out/(name+'-environment.json')).write_text(json.dumps(case_env,indent=2)+'\n')
                    command=[env['WINELOADER'],str(root/'bin/cold-parallel-32.exe'),mode,kind,temperature,str(args.rounds)]
                    log=out/(name+'.log');a={'name':name,'backend':backend,'mode':mode,'kind':kind,'temperature':temperature,'rounds':args.rounds,
                        'cold_decoder_overlap':'not directly measured',**shared.harness.bounded(command,case_env,log,30)}
                    text=log.read_text(errors='replace')
                    for label in ('writer','consumer'):
                        rows=re.findall('^OBS '+label+r' (.*)$',text,re.M);a[label]=parse(rows[0]) if len(rows)==1 else {}
                    a['workers']=[parse(row) for row in re.findall(r'^OBS worker (.*)$',text,re.M)]
                    a.update({'hook_installed':'M2: handler installed' in text,'decode_installed':'decode_opcode entry patched' in text,
                        'decode_off_confirmed':'skipping the decode_opcode hook' in text,'fallback':'RUNNING WITHOUT X87 ACCELERATION' in text})
                    if a['matching_assertion'] or a['other_assertion']:a['status']='ASSERTION'
                    if a['status']=='PASS' and (not valid(a) or 'failure-mask=0 status=PASS' not in text):a['status']='UNVERIFIED'
                    results.append(a);(out/'results.json').write_text(json.dumps(results,indent=2)+'\n')
                    print(f"{name}: {a['status']} exit={a['exit']} assertion={a['matching_assertion']} ({a['seconds']}s)",flush=True)
                    if a['status']!='PASS':raise RuntimeError('Non-pass preserved; stop before more cases')
    finally:
        stop=out/('stop-'+backend);stop.mkdir();shared.harness.stop(prefix,env,stop)
    if backend.startswith('map-'):
        for a in [a for a in results if a['backend']==backend]:observe(a)
        (out/'results.json').write_text(json.dumps(results,indent=2)+'\n')
if any(shared.digest(Path(q))!=sha for q,sha in hashes.items()):raise RuntimeError('Input changed during run')
print('Summary: '+json.dumps(dict(Counter(a['status'] for a in results))),flush=True)
