#!/usr/bin/env python3
"""Build one isolated Unix module; never mutate the existing Wine build."""
import difflib
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess

root=Path(__file__).resolve().parents[1]
old=root.parent/'wine-11.19/build'
source=root.parent/'wine-11.19/source/dlls/ntdll/unix/virtual.c'
build=root/'build/wine-atomic-self'
baseline=root.parent/'probes/patched-runtime'
def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
if (build/'runtime').exists() or (build/'ntdll.so').exists():raise RuntimeError('Output already exists')
if digest(source)!=digest(build/'virtual.c.original'):raise RuntimeError('Source baseline changed')
dry=subprocess.run(['/usr/bin/make','-n','-W',str(source),'dlls/ntdll/ntdll.so'],cwd=old,
                   capture_output=True,text=True,check=True).stdout
(build/'make-dry-run.txt').write_text(dry)
commands=[shlex.split(line) for line in dry.replace('\\\n',' ').splitlines() if line.startswith('/Applications/Xcode.app/')]
compiles=[c for c in commands if '-c' in c and 'dlls/ntdll/unix/virtual.o' in c]
links=[c for c in commands if '-dynamiclib' in c and 'dlls/ntdll/ntdll.so' in c]
if len(compiles)!=1 or len(links)!=1:raise RuntimeError('Unexpected existing build commands')
compile=list(compiles[0]);link=list(links[0])
objects=[old/p for p in link if p.endswith('.o')]
inputs=[source,old/'Makefile',old/'include/config.h',Path(compile[0]),*objects,
        baseline/'bin/wine',baseline/'bin/wineserver',baseline/'lib/wine/x86_64-unix/ntdll.so',
        root/'src/build-wine-atomic-self.py',build/'virtual.c',build/'virtual.c.original']
hashes={str(p):digest(p) for p in inputs}
(build/'input-hashes.json').write_text(json.dumps(hashes,indent=2)+'\n')
compile=[str(build/'virtual.o') if p=='dlls/ntdll/unix/virtual.o' else
         str(build/'virtual.c') if p==str(source) else p for p in compile]
compile += ['-iquote',str(source.parent)]
link=[str(build/'ntdll.so') if p=='dlls/ntdll/ntdll.so' else
      str(build/'virtual.o') if p=='dlls/ntdll/unix/virtual.o' else p for p in link]
sdk=subprocess.run(['/usr/bin/xcrun','--show-sdk-path'],capture_output=True,text=True,check=True).stdout.strip()
env=dict(os.environ,SDKROOT=sdk,MACOSX_DEPLOYMENT_TARGET='14.0',TMPDIR=str(root/'tmp')+'/',
         CLANG_MODULE_CACHE_PATH=str(root/'tmp/clang-module-cache'))
(build/'commands.json').write_text(json.dumps({'cwd':str(old),'compile':compile,'link':link,
        'SDKROOT':sdk,'MACOSX_DEPLOYMENT_TARGET':'14.0','TMPDIR':env['TMPDIR']},indent=2)+'\n')
for command in (compile,link):
    print(shlex.join(command),flush=True)
    subprocess.run(command,cwd=old,env=env,check=True)
subprocess.run(['/usr/bin/codesign','--force','--sign','-',str(build/'ntdll.so')],check=True)
subprocess.run(['/usr/bin/codesign','--verify','--strict',str(build/'ntdll.so')],check=True)
subprocess.run(['/bin/cp','-cR',str(baseline),str(build/'runtime')],check=True)
shutil.copy2(build/'ntdll.so',build/'runtime/lib/wine/x86_64-unix/ntdll.so')
patch=''.join(difflib.unified_diff((build/'virtual.c.original').read_text().splitlines(True),
                                  (build/'virtual.c').read_text().splitlines(True),
                                  fromfile='a/dlls/ntdll/unix/virtual.c',tofile='b/dlls/ntdll/unix/virtual.c'))
(root/'references/wine-atomic-self.patch').write_text(patch)
changes=[p for p,sha in hashes.items() if digest(Path(p))!=sha]
(build/'input-recheck.json').write_text(json.dumps({'changed':changes},indent=2)+'\n')
if changes:raise RuntimeError('Build input changed')
(build/'output-hashes.json').write_text(json.dumps({str(build/p):digest(build/p) for p in
    ('virtual.o','ntdll.so','runtime/lib/wine/x86_64-unix/ntdll.so')},indent=2)+'\n')
print('Isolated module built; no installed runtime or shared source changed.',flush=True)
