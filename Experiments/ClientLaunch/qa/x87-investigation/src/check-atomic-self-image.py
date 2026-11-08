#!/usr/bin/env python3
"""Confirm the real native image loaded by one owned Wine test process."""
import json
from pathlib import Path
import re
import run as shared

root=shared.ROOT;candidate=root/'build/wine-atomic-self/runtime'
out=root/'runs/layout-atomic-self-load-check-oct28';out.mkdir(exist_ok=False)
shared.harness.RUNTIMES['r21-stock']=candidate
env=shared.harness.environment('r21-stock')
binary=root/'bin/layout-exception-32.exe';module=candidate/'lib/wine/x86_64-unix/ntdll.so'
inputs=[root/'src/check-atomic-self-image.py',binary,candidate/'bin/wine',candidate/'bin/wineserver',module]
hashes={str(p):shared.digest(p) for p in inputs}
(out/'input-hashes.json').write_text(json.dumps(hashes,indent=2)+'\n')
try:
    boot=shared.harness.bounded([env['WINELOADER'],'wineboot','--init'],env,out/'wineboot.log',45)
    if boot['exit'] or boot['timeout']:raise RuntimeError('Private bootstrap failed')
    env['DYLD_PRINT_LIBRARIES']='1'
    (out/'environment.json').write_text(json.dumps(env,indent=2)+'\n')
    a=shared.harness.bounded([env['WINELOADER'],str(binary),'same-byte','integer','first','16'],env,out/'image-check.log',30)
    text=Path(a['log']).read_text(errors='replace')
    a['native_module_lines']=[line for line in text.splitlines() if 'ntdll.so' in line]
    a['candidate_image_loaded']=any(str(module) in line and line.startswith('dyld[') for line in a['native_module_lines'])
    old=str(root.parent/'probes/patched-runtime/lib/wine/x86_64-unix/ntdll.so')
    a['baseline_image_loaded']=any(old in line for line in a['native_module_lines'])
    workers=re.findall(r'^OBS worker (.*)$',text,re.M)
    a['oracles_present']=len(workers)==4 and 'failure-mask=0 status=PASS' in text and 'bytes-ok=1 protection-checks=64 api-error=0' in text
    if not (a['candidate_image_loaded'] and not a['baseline_image_loaded'] and a['oracles_present']):a['status']='UNVERIFIED'
    (out/'result.json').write_text(json.dumps(a,indent=2)+'\n')
    print(json.dumps(a,indent=2),flush=True)
    if a['status']!='PASS' or a['matching_assertion'] or a['other_assertion']:raise RuntimeError('Image check failed')
finally:
    shared.harness.stop('r21-stock',env,out)
    changes=[p for p,sha in hashes.items() if shared.digest(Path(p))!=sha]
    (out/'input-recheck.json').write_text(json.dumps({'changed':changes},indent=2)+'\n')
    if changes:raise RuntimeError('Image-check input changed')
