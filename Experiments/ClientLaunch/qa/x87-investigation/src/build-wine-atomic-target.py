#!/usr/bin/env python3
"""Build matching private ntdll/wineserver with a versioned atomic APC."""
import difflib
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess

root=Path(__file__).resolve().parents[1];build=root/'build/wine-atomic-target'
source=build/'source';original=root.parent/'wine-11.19/source';old=root.parent/'wine-11.19/build'
baseline=root.parent/'probes/patched-runtime'
def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def save(name,obj):(build/name).write_text(json.dumps(obj,indent=2)+'\n')
if (build/'runtime').exists() or (build/'objects').exists():raise RuntimeError('Output exists')
before=(original/'include/wine/server_protocol.h').read_text()
after=(source/'include/wine/server_protocol.h').read_text()
old_version=int(re.search(r'#define SERVER_PROTOCOL_VERSION (\d+)',before)[1])
new_version=int(re.search(r'#define SERVER_PROTOCOL_VERSION (\d+)',after)[1])
assert new_version==old_version+1
def enum_values(s):return [v.strip() for v in re.search(r'enum apc_type\s*\{(.*?)\};',s,re.S)[1].split(',')]
assert enum_values(after)==enum_values(before)+['APC_VIRTUAL_INVALIDATE_CODE']
normalized=after.replace('    APC_DUP_HANDLE,\n    APC_VIRTUAL_INVALIDATE_CODE\n','    APC_DUP_HANDLE\n')
normalized,n=re.subn(r'    struct\n    \{[^}]*\} virtual_invalidate_code;\n','',normalized)
assert n==2
normalized=normalized.replace(f'#define SERVER_PROTOCOL_VERSION {new_version}',f'#define SERVER_PROTOCOL_VERSION {old_version}')
assert normalized==before,'Unexpected existing protocol/message layout change'
save('protocol-review.json',{'before':old_version,'candidate':new_version,
    'existing_enum_values_and_message_layouts':'identical after removing the two new union members',
    'new_operation':'APC_VIRTUAL_INVALIDATE_CODE','required_right':'PROCESS_VM_OPERATION'})
# The generator also drops an unrelated status-name alias. Preserve its raw
# output and restore that generated trace table, whose request layouts did not
# change; the hand-written APC trace cases remain in server/trace.c.
shutil.copy2(source/'server/request_trace.h',build/'request_trace.generated.h')
shutil.copy2(original/'server/request_trace.h',source/'server/request_trace.h')
changed=('server/protocol.def','include/wine/server_protocol.h','server/thread.c','server/trace.c',
         'dlls/ntdll/unix/virtual.c','dlls/ntdll/unix/server.c','dlls/ntdll/unix/unix_private.h')
patch=''.join(''.join(difflib.unified_diff((original/p).read_text().splitlines(True),
    (source/p).read_text().splitlines(True),fromfile='a/'+p,tofile='b/'+p)) for p in changed)
(build/'source-diff.patch').write_text(patch)
compile_objects={'dlls/ntdll/unix/virtual.o':'dlls/ntdll/unix/virtual.c',
    'dlls/ntdll/unix/server.o':'dlls/ntdll/unix/server.c',
    'server/thread.o':'server/thread.c','server/trace.o':'server/trace.c'}
command=['/usr/bin/make','-n']
for p in compile_objects.values():command += ['-W',str(original/p)]
command += ['dlls/ntdll/ntdll.so','server/wineserver']
dry=subprocess.run(command,cwd=old,capture_output=True,text=True,check=True).stdout
(build/'make-dry-run.txt').write_text(dry)
commands=[shlex.split(line) for line in dry.replace('\\\n',' ').splitlines() if line.startswith('/Applications/Xcode.app/')]
compiles={obj:[c for c in commands if '-c' in c and obj in c] for obj in compile_objects}
assert all(len(c)==1 for c in compiles.values())
links={target:[c for c in commands if '-c' not in c and target in c] for target in ('dlls/ntdll/ntdll.so','server/wineserver')}
assert all(len(c)==1 for c in links.values())
old_objects={old/p for values in links.values() for p in values[0] if p.endswith('.o')}
inputs=[root/'src/build-wine-atomic-target.py',old/'Makefile',old/'include/config.h',
        original/'server/request_trace.h',source/'server/request_trace.h',*old_objects,
        *[original/p for p in changed],*[source/p for p in changed],
        baseline/'bin/wine',baseline/'bin/wineserver',baseline/'lib/wine/x86_64-unix/ntdll.so']
hashes={str(p):digest(p) for p in inputs};save('input-hashes.json',hashes)
(build/'objects').mkdir()
object_map={p:str(build/'objects'/p.replace('/','_')) for p in compile_objects}
def adapt(tokens):
    result=[]
    for token in tokens:
        if token in object_map:token=object_map[token]
        elif token.startswith('-I'+str(original)):token='-I'+str(source)+token[len('-I'+str(original)):]
        elif token.startswith(str(original)):token=str(source)+token[len(str(original)):]
        result.append(token)
    return [result[0],'-I'+str(source/'include'),*result[1:]]
compiled=[adapt(compiles[obj][0]) for obj in compile_objects]
linked=[]
for target,values in links.items():
    tokens=adapt(values[0]);tokens=[str(build/Path(target).name) if p==target else p for p in tokens];linked.append(tokens)
sdk=subprocess.run(['/usr/bin/xcrun','--show-sdk-path'],capture_output=True,text=True,check=True).stdout.strip()
env=dict(os.environ,SDKROOT=sdk,MACOSX_DEPLOYMENT_TARGET='14.0',TMPDIR=str(root/'tmp')+'/',
         CLANG_MODULE_CACHE_PATH=str(root/'tmp/clang-module-cache'))
save('commands.json',{'cwd':str(old),'compile':compiled,'link':linked,'SDKROOT':sdk,
    'MACOSX_DEPLOYMENT_TARGET':'14.0','TMPDIR':env['TMPDIR']})
abi=build/'abi-check.c'
abi.write_text('#include "config.h"\n#include "wine/server_protocol.h"\n'
    '_Static_assert(sizeof(union apc_call)==64,"APC call ABI");\n'
    '_Static_assert(sizeof(union apc_result)==40,"APC result ABI");\n'
    '_Static_assert(APC_DUP_HANDLE==15,"Existing operation values");\n'
    '_Static_assert(APC_VIRTUAL_INVALIDATE_CODE==16,"Appended operation");\n')
check=list(compiled[0]);index=check.index('-o');del check[index:index+2]
check[check.index('-c')]='-fsyntax-only'
check=[str(abi) if p==str(source/compile_objects['dlls/ntdll/unix/virtual.o']) else p for p in check]
for c in [check,*compiled,*linked]:
    print(shlex.join(c),flush=True);subprocess.run(c,cwd=old,env=env,check=True)
for p in (build/'ntdll.so',build/'wineserver'):
    subprocess.run(['/usr/bin/codesign','--force','--sign','-',str(p)],check=True)
    subprocess.run(['/usr/bin/codesign','--verify','--strict',str(p)],check=True)
subprocess.run(['/bin/cp','-cR',str(baseline),str(build/'runtime')],check=True)
shutil.copy2(build/'ntdll.so',build/'runtime/lib/wine/x86_64-unix/ntdll.so')
shutil.copy2(build/'wineserver',build/'runtime/bin/wineserver')
changes=[p for p,sha in hashes.items() if digest(Path(p))!=sha];save('input-recheck.json',{'changed':changes})
if changes:raise RuntimeError('Original or candidate build inputs changed')
paths=[build/'ntdll.so',build/'wineserver',build/'runtime/lib/wine/x86_64-unix/ntdll.so',build/'runtime/bin/wineserver']
save('output-hashes.json',{str(p):digest(p) for p in paths})
print('Matching private ntdll/wineserver candidate built; baseline untouched.',flush=True)
