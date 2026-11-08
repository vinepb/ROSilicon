#!/usr/bin/env python3
"""Independently retain successes, unexpected failures, and coverage limits."""
from collections import Counter
import hashlib
import itertools
import json
import os
from pathlib import Path
import re
import subprocess

root=Path(__file__).resolve().parents[1]
out=root/'runs/map-lookup-final-audit-oct28';out.mkdir(exist_ok=False)
pre=root/'runs/map-lookup-preflight-oct28'
repo=root.parents[4]
modes=('idle','same-byte','changed-byte','neighbor-changed')
kinds=('integer','x87');temps=('first','warm')
def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def save(name,obj):(out/name).write_text(json.dumps(obj,indent=2)+'\n')
def parse(text):return {k:int(v) for k,v in re.findall(r'([a-z-]+)=(-?\d+)',text)}
def product(backends,selected_modes,temperatures):return set(itertools.product(backends,selected_modes,kinds,temperatures))
groups=[
 ('owned-recovery-observer-map-sanity-oct27',1,None),
 ('cold-parallel-stock-sanity-oct28',4,product(('stock',),('idle','changed-byte'),('first',))),
 ('cold-parallel-stock-controls-oct28',16,product(('stock',),modes,temps)),
 ('owned-recovery-observer-map-calibration-oct28',8,None),
 ('cold-parallel-upstream-controls-oct28',16,product(('upstream-off','upstream-on'),('idle','changed-byte'),temps)),
 ('owned-recovery-observer-map-cold-controls-oct28',32,product(('map-off','map-on'),modes,temps)),
 ('layout-parallel-stock-sanity-oct28',4,product(('stock',),('idle','changed-byte'),('first',))),
 ('layout-parallel-stock-diagnostic-oct28',1,None),
 ('layout-parallel-stock-retries-oct28',8,None),
 ('layout-exception-stock-oct28',16,None),
 ('layout-exception-paired-oct28',5,None),
 ('layout-atomic-self-controls-oct28',48,product(('stock','upstream-off','upstream-on'),modes,temps)),
 ('layout-atomic-self-regression-oct28',16,None),
]
input_checks=[];coverage=[];failures=[];profiles=[];counts=Counter()
for tag,n,expected in groups:
    directory=root/'runs'/tag
    result_path=directory/('result.json' if tag=='layout-parallel-stock-diagnostic-oct28' else 'results.json')
    data=json.loads(result_path.read_text());rows=data if isinstance(data,list) else [data]
    assert len(rows)==n,(tag,len(rows),n)
    if expected is not None:
        actual={(a['backend'],a['mode'],a['kind'],a['temperature']) for a in rows}
        assert actual==expected and len(actual)==n,tag
    if tag=='owned-recovery-observer-map-sanity-oct27':
        assert [(a['backend'],a['mode'],a['kind'],a['rounds']) for a in rows]==[('decode-off','changed-byte','integer',64)]
    if tag=='owned-recovery-observer-map-calibration-oct28':
        assert {(a['backend'],a['mode'],a['kind'],a['rounds']) for a in rows}==set(itertools.product(('decode-off','latest'),('same-byte','changed-byte'),kinds,(512,)))
    if tag=='layout-exception-paired-oct28':
        assert [(a['backend'],a['mode'],a['kind'],a['temperature']) for a in rows]==[
            ('stock','idle','integer','first'),('stock','idle','integer','warm'),
            ('stock','idle','x87','first'),('stock','idle','x87','warm'),
            ('stock','same-byte','integer','first')]
    if 'retries' in tag or tag=='layout-exception-stock-oct28':
        assert [a['attempt'] for a in rows]==list(range(1,n+1))
        for a in rows:
            rounds=2 if 'retries' in tag or a['attempt']<=4 else 16
            assert a['command'][-4:]==['changed-byte','x87','first',str(rounds)]
    if tag=='layout-atomic-self-regression-oct28':
        cases=('control','writeproc-rwalloc','writeproc-execalloc','writeproc-explicit-toggle',
               'writeproc-mixed','writeproc-child','thread-int3','thread-mutate')
        assert {(a['bits'],a['case']) for a in rows}==set(itertools.product((32,64),cases))
    for path_text,sha in json.loads((directory/'input-hashes.json').read_text()).items():
        assert digest(Path(path_text))==sha,(tag,path_text)
        input_checks.append({'run':tag,'path':path_text,'sha256':sha})
    group_counts=Counter()
    for a in rows:
        text=Path(a['log']).read_text(errors='replace')
        assert not a['timeout'] and not a['matching_assertion'] and not a['other_assertion'],a['log']
        assert 'assertion failed' not in text,a['log']
        counts[a['status']]+=1;group_counts[a['status']]+=1
        if a['status']!='PASS':
            assert a['status']=='FAIL' and a['exit']==5 and 'END case=' not in text,a['log']
            failures.append({'run':tag,'log':a['log'],'exit':a['exit'],
                             'report':re.findall(r'^OWNED_UNHANDLED .*$',text,re.M)})
            continue
        assert a['exit']==0 and 'status=PASS' in text,a['log']
        assert 'OWNED_UNHANDLED' not in text,a['log']
        command=a['command'];binary=Path(command[1]).name
        if binary in ('cold-parallel-32.exe','layout-parallel-32.exe','layout-exception-32.exe'):
            mode,kind,temperature,round_text=command[-4:];rounds=int(round_text)
            writers=re.findall(r'^OBS writer (.*)$',text,re.M);consumers=re.findall(r'^OBS consumer (.*)$',text,re.M)
            workers=[parse(x) for x in re.findall(r'^OBS worker (.*)$',text,re.M)]
            assert len(writers)==len(consumers)==1 and len(workers)==4,a['log']
            w=parse(writers[0]);c=parse(consumers[0]);ops=0 if mode=='idle' else rounds*32
            assert w['batches']==rounds and w['writes']==w['flushes']==ops
            assert w['changed']==(ops if mode in ('changed-byte','neighbor-changed') else 0)
            assert 0<=w['before-first-tick']<=ops and 0<w['progress']<=rounds*32
            assert w['version-checks']==rounds*32 and w['bytes-ok']==1 and w['protection-checks']==rounds*4 and not w['api-error']
            assert c=={'calls':rounds*4,'warm-calls':rounds*4 if temperature=='warm' else 0,'joined':4,'exit':0}
            for i,v in enumerate(workers):
                slot=(rounds-1)*4+i;checksum=(0x12345678+slot+128*0x01010101+128*127//2)&0xffffffff
                assert v['id']==i and v['calls']==rounds and v['result']==0x543210+slot
                assert v['checksum']==checksum and v['math']==2 and not v['errors'] and v['ticks']>0
                assert v['ever']==(3 if mode=='changed-byte' else 1 if binary=='cold-parallel-32.exe' else 2)
            if binary!='cold-parallel-32.exe':
                code=re.findall(r'^CODE ([0-9a-f]+)$',text,re.M)
                assert len(code)==1 and bytes.fromhex(code[0])==(pre/f'generated-layout-32-{kind}-mov.bin').read_bytes()
        if 'backend' in a:
            backend=a['backend']
            if backend=='stock':assert not a.get('hook_installed') and not a.get('decode_installed')
            else:
                assert a['hook_installed'] and not a['fallback']
                if backend in ('decode-off','upstream-off','map-off'):assert a['decode_off_confirmed'] and not a['decode_installed']
                else:assert a['decode_installed']
        if 'observer' in a:
            profile=json.loads(Path(a['observer']['path']).read_text())
            assert profile['own_code_guarded'] and profile['sweeps'] and profile['state_reads'] and not profile['limit_reached']
            assert not profile['event_overflow']
            tree=sum(v for t in profile['threads'] for k,v in t['runtime_offsets'].items()
                     if 0xb5d4<=int(k,0)<0xb63c or 0xb9b8<=int(k,0)<0xba30)
            assert tree==profile['recovery_region_samples']
            first=second=0
            for event in profile['recovery_events']:
                pc=int(event['pc_offset'],0);lr=int(event['lr_offset'],0)
                first+=0x1b5b8<=pc<0x1badc and lr==0xb9dc
                second+=0x1b5b8<=pc<0x1badc and lr==0xb9f0
                if pc in (0xb9dc,0xb9f0):assert event['return_bit'] in (0,1)
            assert first==profile['first_map_call_samples'] and second==profile['second_map_call_samples']
            profiles.append({'run':tag,'log':a['log'],'tree_samples':tree,'first_map_samples':first,'second_map_samples':second,
                'state_reads':profile['state_reads'],'state_failures':profile['state_failures'],
                'id_failures':profile['id_failures'],'enumeration_failures':profile['enumeration_failures'],
                'events':profile['recovery_events']})
    coverage.append({'run':tag,'executed':len(rows),'outcomes':dict(group_counts)})
assert counts=={'PASS':173,'FAIL':2},counts
assert len(failures)==2 and failures[0]['report']==[]
assert failures[1]['report']==['OWNED_UNHANDLED code=c0000005 address=25000024 eip=25000024 eax=00000002 ecx=0036f630 esp=012fff1c thread=000001d4 parameters=00000002 access=00000008 fault-address=25000024']
selfchecks=[]
for tag in ('layout-exception-stock-oct28','layout-atomic-self-controls-oct28'):
    a=json.loads((root/'runs'/tag/'selfcheck-result.json').read_text())
    text=Path(a['log']).read_text();assert a['status']=='EXPECTED_FAILURE' and a['exit']==5 and not a['timeout']
    page=re.search(r'OWNED_EXPECTED_FAULT allocation=([0-9a-f]{8})',text)
    fault=re.search(r'^OWNED_UNHANDLED code=c0000005 .* access=00000001 fault-address=([0-9a-f]{8})$',text,re.M)
    assert page and fault and page[1]==fault[1]
    selfchecks.append({'run':tag,'status':a['status'],'fault_address':page[1]})
summary={'executed':sum(counts.values()),'outcomes':dict(counts),'coverage':coverage,
         'unexpected_failures':failures,'expected_reporter_faults':selfchecks,
         'profile_count':len(profiles),'recovery_tree_or_caller_samples':sum(p['tree_samples'] for p in profiles),
         'first_lookup_samples':sum(p['first_map_samples'] for p in profiles),
         'second_lookup_samples':sum(p['second_map_samples'] for p in profiles),
         'exact_assertion_reproduced':False,'game_tested_this_phase':False,
         'candidate_limit':'Self-process invalidation only. Remote APC-pair race not corrected.'}
save('independent-review.json',summary);save('input-verification.json',input_checks);save('observer-review.json',profiles)

manifest=json.loads((root/'references/map-observer-inputs.json').read_text())['source_manifest']
for relative,value in manifest.items():
    p=root/'source-owned-map-observer'/relative
    if isinstance(value,dict):assert p.is_symlink() and os.readlink(p)==value['symlink']
    else:assert digest(p)==value,relative
for item in ('input-hashes.json','output-hashes.json'):
    for path_text,sha in json.loads((root/'build/wine-atomic-self'/item).read_text()).items():assert digest(Path(path_text))==sha,path_text
def entries(base):
    result={}
    for parent,dirs,files in os.walk(base,followlinks=False):
        for name in dirs+files:
            p=Path(parent)/name;relative=str(p.relative_to(base))
            if p.is_symlink():result[relative]={'symlink':os.readlink(p)}
            elif p.is_file():result[relative]={'sha256':digest(p),'size':p.stat().st_size}
    return result
before=entries(root.parent/'probes/patched-runtime');after=entries(root/'build/wine-atomic-self/runtime')
assert set(before)==set(after)
differences=[p for p in before if before[p]!=after[p]]
assert differences==['lib/wine/x86_64-unix/ntdll.so'],differences
save('candidate-runtime-manifest.json',after);save('candidate-runtime-differences.json',differences)
for label,path_text in (('qa','runs/qa-source-baseline/input-hashes.txt'),('other','runs/qa-source-baseline/untouched-baseline.sha256')):
    p=subprocess.run(['/usr/bin/shasum','-a','256','-c',str(root/path_text)],cwd=repo,capture_output=True,text=True)
    (out/(label+'-inputs-after.txt')).write_text(p.stdout+p.stderr);assert not p.returncode
prefs=Path('__USER_HOME__/Library/Preferences/com.rosilicon.launcher.qa.plist')
assert digest(prefs)==(pre/'qa-preferences.sha256').read_text().strip()
(out/'qa-preferences.sha256').write_text(digest(prefs)+'\n')
identities=[]
for p in (root/'environments/r21-stock/prefix',root/'environments/r21-sidecar/prefix',Path('__USER_HOME__/Library/Application Support/ROSilicon QA/wine')):
    stat=p.stat();coord=Path(f'/private/tmp/.wine-{os.getuid()}/server-{stat.st_dev:x}-{stat.st_ino:x}')
    identities.append({'path':str(p),'device':stat.st_dev,'inode':stat.st_ino,'coordination':str(coord),'coordination_exists':coord.exists()})
save('prefix-identities.json',identities);assert identities==json.loads((pre/'prefix-identities.json').read_text())
p=subprocess.run(['/bin/ps','-axo','pid=,comm='],capture_output=True,text=True,check=True)
matched=[line.strip() for line in p.stdout.splitlines() if re.search(r'(?i)(wine|Ragexe|ROSilicon|x87sidecar|parallel-32|exception-32)',line)]
save('process-names.json',matched);assert not matched,matched
crashes=sorted(str(p) for p in Path('__USER_HOME__/Library/Logs/DiagnosticReports').glob('*') if re.search(r'(?i)(wine|Ragexe|ROSilicon|x87sidecar|active-page)',p.name))
save('crash-report-paths.json',crashes);assert crashes==json.loads((pre/'crash-report-paths.json').read_text())
status=subprocess.run(['git','status','--short'],cwd=repo,capture_output=True,text=True,check=True).stdout
(out/'git-status.txt').write_text(status);assert status==(pre/'git-status.txt').read_text()
print(json.dumps({k:summary[k] for k in ('executed','outcomes','profile_count','recovery_tree_or_caller_samples','first_lookup_samples','second_lookup_samples')},indent=2))
print('Installed inputs/prefs and prefix identities unchanged; no remaining matching process or new recorded crash path.')
