#!/usr/bin/env python3
"""Build the owned parent/child PE with the frozen exception reporter body."""
import hashlib
import json
import os
from pathlib import Path
import subprocess

root=Path(__file__).resolve().parents[1];build=root/'build/remote-owned-child';build.mkdir(exist_ok=False)
def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
original=root/'src/layout_exception_probe.c'
if digest(original)!='c2f6d7dbacb7d1604338d9114ca17027931e43aa813288a0fc8c713f5f745ec6':raise RuntimeError('Reporter source changed')
text=original.read_text();body=text[text.index('static char *report_text'):text.index('int main(')]
(build/'exception_reporter.h').write_text(body)
inputs=[root/'src/build-remote-layout-probe.py',root/'src/remote_layout_probe.c',
        root/'src/layout_parallel_probe.c',original,build/'exception_reporter.h']
hashes={str(p):digest(p) for p in inputs};(build/'input-hashes.json').write_text(json.dumps(hashes,indent=2)+'\n')
command=['/opt/homebrew/bin/i686-w64-mingw32-gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-static-libgcc',
         '-Wl,--no-insert-timestamp','-I'+str(build),'-o',str(root/'bin/remote-layout-32.exe'),str(root/'src/remote_layout_probe.c')]
(build/'command.json').write_text(json.dumps(command,indent=2)+'\n')
subprocess.run(command,env=dict(os.environ,TMPDIR=str(root/'tmp')+'/'),check=True)
if any(digest(Path(p))!=sha for p,sha in hashes.items()):raise RuntimeError('Build inputs changed')
result={'binary':str(root/'bin/remote-layout-32.exe'),'sha256':digest(root/'bin/remote-layout-32.exe')}
(build/'output.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
