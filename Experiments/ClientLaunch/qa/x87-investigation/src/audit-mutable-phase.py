#!/usr/bin/env python3
"""Offline/result and read-only host audit of exact owned-test artifacts."""
import hashlib
import itertools
import json
import os
from pathlib import Path
import re
import subprocess

root = Path(__file__).resolve().parents[1]
out = root / 'runs/mutable-active-final-audit-oct27'
out.mkdir(exist_ok=False)
modes = ('idle', 'same-byte', 'changed-byte', 'neighbor-changed')
kinds = ('integer', 'x87')
groups = [
    ('mutable-active-stock-sanity-oct27', ['stock'], [32], ['changed-byte'], 8, 'short', False),
    ('mutable-active-stock-controls-oct27', ['stock'], [32,64], modes, 64, 'short', False),
    ('mutable-active-helper-controls-oct27', ['decode-off','latest'], [32], modes, 64, 'short', False),
    ('mutable-active-sampled-controls-oct27', ['decode-off','latest'], [32], modes, 64, 'short', False),
    ('mutable-active-sampled-changes-oct27', ['decode-off','latest'], [32], ['changed-byte'], 512, 'short', False),
    ('owned-recovery-observer-sanity-oct27', ['decode-off'], [32], ['changed-byte'], 64, 'short', True),
    ('owned-recovery-observer-controls-oct27', ['decode-off','latest'], [32], modes, 512, 'short', True),
    ('rep-active-stock-sanity-oct27', ['stock'], [32], ['changed-byte'], 8, 'rep', False),
    ('rep-active-stock-controls-oct27', ['stock'], [32], modes, 64, 'rep', False),
    ('rep-active-helper-controls-oct27', ['decode-off','latest'], [32], ['idle','changed-byte'], 64, 'rep', False),
    ('owned-recovery-observer-rep-controls-oct27', ['decode-off','latest'], [32], modes, 512, 'rep', True),
]
def digest(path): return hashlib.sha256(path.read_bytes()).hexdigest()
def save(name, obj): (out / name).write_text(json.dumps(obj, indent=2) + '\n')
archived = root / 'references/mutable-runners-v1'
archive_hashes = json.loads((archived / 'sha256.json').read_text())
for name, sha in archive_hashes.items(): assert digest(archived / name) == sha
input_checks = []; coverage = []; positives = []; observer_failures = []
total = 0; writes = 0; changed = 0; state_reads = 0; observer_cases = 0
latched_samples = 0; latched_drops = 0; latched_unavailable = 0; latched_cases = 0
for tag, backends, bits, selected_modes, rounds, program, observed in groups:
    directory = root / 'runs' / tag
    results = json.loads((directory / 'results.json').read_text())
    selected_kinds = ('integer',) if tag == 'owned-recovery-observer-sanity-oct27' else kinds
    expected = set(itertools.product(backends, bits, selected_modes, selected_kinds))
    actual = {(a['backend'],a['bits'],a['mode'],a['kind']) for a in results}
    assert actual == expected and len(results) == len(expected), tag
    for path_text, expected_hash in json.loads((directory / 'input-hashes.json').read_text()).items():
        path = Path(path_text); resolved = path
        if digest(path) != expected_hash:
            candidate = archived / path.name
            assert path.parent == root / 'src' and candidate.is_file() and digest(candidate) == expected_hash, path
            resolved = candidate
        assert digest(resolved) == expected_hash
        input_checks.append({'run':tag,'original_path':path_text,'resolved_path':str(resolved),'sha256':expected_hash})
    for a in results:
        assert a['status'] == 'PASS' and a['exit'] == 0 and not a['timeout']
        assert not a['matching_assertion'] and not a['other_assertion'] and not a['fallback']
        assert a.get('program','short') == program and a['rounds'] == rounds
        assert a['command'][-3:] == [a['mode'], a['kind'], str(rounds)]
        stem = 'rep-active-page' if program == 'rep' else 'mutable-active-page'
        assert a['command'][-4] == str(root / f"bin/{stem}-{a['bits']}.exe")
        log = Path(a['log']).read_text(errors='replace')
        assert 'assertion failed' not in log and 'failure-mask=0 joined=1 status=PASS' in log
        w,c = a['writer'],a['consumer']; operations = 0 if a['mode']=='idle' else rounds
        changes = rounds if a['mode'] in ('changed-byte','neighbor-changed') else 0
        assert w['completed']==rounds and w['writes']==w['flushes']==operations and w['changed']==changes
        assert 0 < w['progress'] <= rounds and w['ticks-before'] > 0 and w['ticks-after'] != w['ticks-before']
        assert w['bytes-ok']==w['protections-ok']==1 and w['api-error']==0
        assert w['observed']==(3 if a['mode']=='changed-byte' else 1)
        assert c == {'calls':1,'math':2,'errors':0,'result':0x54321,'exit':0,'joined':1}
        if program=='rep': assert a['copy']=={'bytes':1048576,'source-unchanged':1,'destination-ok':1}
        if a['backend']!='stock':
            assert a['hook_installed']
            assert a['decode_off_confirmed'] and not a['decode_installed'] if a['backend']=='decode-off' else a['decode_installed']
        if observed:
            o = json.loads(Path(a['observer']['path']).read_text())
            assert o['own_code_guarded'] and o['sweeps']>0 and o['state_reads']>0 and not o['limit_reached']
            assert len(o['threads']) >= 2 and int(o['runtime_base'],0)>0
            samples = sum(v for t in o['threads'] for k,v in t['runtime_offsets'].items()
                          if 0xb5d4<=int(k,0)<0xb63c or 0xb9b8<=int(k,0)<0xba30)
            assert samples==o['recovery_region_samples']==a['observer']['recovery_region_samples']
            positives.extend({'run':tag,'backend':a['backend'],'mode':a['mode'],'kind':a['kind'],**h} for h in a['observer']['hits'])
            observer_cases+=1; state_reads+=o['state_reads']
            observer_failures.append({'run':tag,'case':[a['backend'],a['mode'],a['kind']],
                'state_failures':o['state_failures'],'id_failures':o['id_failures'],'enumeration_failures':o['enumeration_failures']})
        writes+=w['writes']; changed+=w['changed'];total+=1
    if tag in ('mutable-active-sampled-controls-oct27','mutable-active-sampled-changes-oct27'):
        review = json.loads((directory/'recovery-sample-review.json').read_text())
        assert len(review)==len(results)
        for entry in review:
            h=entry['headers'];assert h['latch_state']=='latched' and entry['recovery_region_samples']==0
            # Summary guest_range is an upstream reporting defect here. Verify
            # actual exclusive guest leaves at our pinned allocation instead.
            text=Path(entry['path']).read_text(); leaves=text.split('[leaves]\n',1)[1].split('[host_leaves]',1)[0]
            in_owned=sum(int(line.split()[2]) for line in leaves.splitlines() if line and not line.startswith('#') and 0x25000000<=int(line.split()[1],0)<0x25001000)
            assert in_owned > 0
            latched_cases+=1;latched_samples+=int(h['samples']);latched_drops+=int(h['samples_dropped']);latched_unavailable+=int(h['unavailable'])
    coverage.append({'run':tag,'declared_outcomes':len(results),'program':program,'rounds':rounds,'all_oracles_pass':True})
save('input-verification.json',input_checks)
save('observer-read-failures.json',observer_failures)
summary={'declared_outcomes':total,'coverage':coverage,'self_wpm_writes':writes,'actual_byte_changes':changed,
         'observer_cases':observer_cases,'all_thread_state_reads':state_reads,
         'positive_recovery_region_samples':sum(p['samples'] for p in positives),'positive_samples':positives,
         'later_two_map_lookup_observed':False,'exact_assertion_reproduced':False,
         'latched_profiles':latched_cases,'latched_samples':latched_samples,'latched_ring_drops':latched_drops,
         'latched_unavailable_lookups':latched_unavailable,
         'limits':['focused owned stress; not full conformance','samples are not exact events',
                   'negative samples cannot exclude recovery','observer affects timing',
                   'REP full-buffer oracle is final-only','original runner versions explicitly archived']}
assert total==105 and summary['positive_recovery_region_samples']==11 and observer_cases==33
save('independent-review.json',summary)

source_manifest=json.loads((root/'references/owned-recovery-observer-inputs.json').read_text())['source_manifest']
source=root/'source-owned-recovery-observer'
for rel,expected in source_manifest.items():
    p=source/rel
    if isinstance(expected,dict): assert p.is_symlink() and os.readlink(p)==expected['symlink']
    else: assert digest(p)==expected,rel
checks={}
for label,manifest in [('qa','runs/qa-source-baseline/input-hashes.txt'),('other','runs/qa-source-baseline/untouched-baseline.sha256')]:
    p=subprocess.run(['/usr/bin/shasum','-a','256','-c',str(root/manifest)],capture_output=True,text=True)
    (out/(label+'-inputs-after.txt')).write_text(p.stdout+p.stderr);checks[label]=p.returncode
assert not any(checks.values())
prefs=Path('__USER_HOME__/Library/Preferences/com.rosilicon.launcher.qa.plist')
sha=digest(prefs);(out/'qa-preferences.sha256').write_text(sha+'\n')
assert sha==(root/'runs/mutable-active-preflight-oct27/qa-preferences.sha256').read_text().strip()
identities=[]
for p in [root/'environments/r21-stock/prefix',root/'environments/r21-sidecar/prefix',Path('__USER_HOME__/Library/Application Support/ROSilicon QA/wine')]:
    s=p.stat();coord=Path(f'/private/tmp/.wine-{os.getuid()}/server-{s.st_dev:x}-{s.st_ino:x}')
    identities.append({'path':str(p),'device':s.st_dev,'inode':s.st_ino,'coordination':str(coord),'coordination_exists':coord.exists()})
save('prefix-identities.json',identities)
assert identities==json.loads((root/'runs/mutable-active-preflight-oct27/prefix-identities.json').read_text())
proc=subprocess.run(['/bin/ps','-axo','pid=,comm='],capture_output=True,text=True,check=True)
matched=[line.strip() for line in proc.stdout.splitlines() if re.search(r'(?i)(wine|Ragexe|ROSilicon|x87sidecar|active-page)',line)]
save('process-names.json',matched);assert not matched
crashes=sorted(str(p) for p in Path('__USER_HOME__/Library/Logs/DiagnosticReports').glob('*') if re.search(r'(?i)(wine|Ragexe|ROSilicon|x87sidecar|active-page)',p.name))
save('crash-report-paths.json',crashes)
assert crashes==json.loads((root/'runs/mutable-active-preflight-oct27/crash-report-paths.json').read_text())
status=subprocess.run(['git','status','--short'],capture_output=True,text=True,check=True).stdout
(out/'git-status.txt').write_text(status)
assert status==(root/'runs/mutable-active-preflight-oct27/git-status.txt').read_text()
sdk=Path('/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk/usr/include/mach')
context=[root/'src/audit-mutable-phase.py',root/'references/owned-recovery-observer-source-diff.patch',
         root/'references/owned-recovery-observer-inputs.json',sdk/'mach_exc.defs',sdk/'exception_types.h',sdk/'arm/exception.h',
         Path('/usr/libexec/rosetta/runtime'),Path('/Library/Apple/usr/libexec/oah/libRosettaRuntime')]
save('context-hashes.json',{str(p):digest(p) for p in context})
print(json.dumps({k:summary[k] for k in ('declared_outcomes','self_wpm_writes','actual_byte_changes','observer_cases','all_thread_state_reads','positive_recovery_region_samples','latched_samples')},indent=2))
print('Installed hashes/preferences/prefix identities unchanged; no remaining matching process or new crash-report path.')
