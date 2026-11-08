#!/usr/bin/env python3
"""Offline classification of profiles from exact owned test directories."""
import argparse
import json
from pathlib import Path

root=Path(__file__).resolve().parents[1]
def review(directory):
    if directory.parent!=root/'runs' or not directory.name.startswith('owned-recovery-observer-map-'):
        raise RuntimeError('Only the exact owned map-observer runs are supported')
    hashes=json.loads((directory/'input-hashes.json').read_text())
    if hashes['/usr/libexec/rosetta/runtime']!='5d0e8f9a10093a8af1fbae31b59917be2ad1b248c4a71bf570982ba5966c41c4':
        raise RuntimeError('Input does not match static control-flow evidence')
    results=json.loads((directory/'results.json').read_text());rows=[]
    for result in results:
        if 'observer' not in result:raise RuntimeError('Incomplete observer result')
        path=Path(result['observer']['path'])
        if path.parent!=directory:raise RuntimeError('Profile escaped owned run directory')
        observation=json.loads(path.read_text());events=[]
        counters={'tree_search':0,'map_setup':0,'first_call_direct':0,'second_call_direct':0,
                  'first_return_true':0,'first_return_false':0,'second_return_true':0,'second_return_false':0,
                  'second_map_setup':0,'downstream_success_path':0}
        for event in observation['recovery_events']:
            pc=int(event['pc_offset'],0);lr=int(event['lr_offset'],0);stage=None
            if 0xb5d4<=pc<0xb63c:stage='tree_search'
            elif 0xb9b8<=pc<0xb9dc:stage='map_setup'
            elif pc==0xb9dc:stage='first_return_true' if event['return_bit']==1 else 'first_return_false'
            elif 0xb9e0<=pc<0xb9f0:stage='second_map_setup'
            elif pc==0xb9f0:stage='second_return_true' if event['return_bit']==1 else 'second_return_false'
            elif 0xb9f4<=pc<0xba30:stage='downstream_success_path'
            elif 0x1b5b8<=pc<0x1badc and lr==0xb9dc:stage='first_call_direct'
            elif 0x1b5b8<=pc<0x1badc and lr==0xb9f0:stage='second_call_direct'
            if stage is None:raise RuntimeError('Unexpected event classification')
            counters[stage]+=1;events.append({**event,'stage':stage})
        if not observation['event_overflow']:
            assert counters['first_call_direct']==observation['first_map_call_samples']
            assert counters['second_call_direct']==observation['second_map_call_samples']
            assert sum(counters.values())==observation['recovery_region_samples']+observation['first_map_call_samples']+observation['second_map_call_samples']
        rows.append({'backend':result['backend'],'mode':result['mode'],'kind':result['kind'],
            'temperature':result.get('temperature','na'),'fixture_status':result['status'],'profile':str(path),
            'sampled_stages':counters,'events':events,'generic_lookup_samples':observation['generic_lookup_samples'],
            'event_overflow':observation['event_overflow'],'state_failures':observation['state_failures'],
            'id_failures':observation['id_failures'],'enumeration_failures':observation['enumeration_failures'],
            'interpretation':'Downstream success implies at least one lookup succeeded, based on pinned static control flow; samples are not event counts. Absence is inconclusive.'})
    return rows
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('directory',type=Path);args=p.parse_args()
    rows=review(args.directory)
    (args.directory/'map-stage-review.json').write_text(json.dumps(rows,indent=2)+'\n')
    print(json.dumps({'profiles':len(rows),'stages':{key:sum(r['sampled_stages'][key] for r in rows) for key in rows[0]['sampled_stages']}},indent=2))
