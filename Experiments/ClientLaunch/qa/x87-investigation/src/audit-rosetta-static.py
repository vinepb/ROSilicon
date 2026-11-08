#!/usr/bin/env python3
"""Cache read-only static system-runtime evidence in the owned diagnostic root."""
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
out = root / 'references/rosetta-static-oct27'
runtime = Path('/Library/Apple/usr/libexec/oah/libRosettaRuntime')
expected = 'a9bf23ac1b8a785060d4bec7337e4403d6bb4ad234e99100a489c5c3de4f292e'
digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
assert digest(runtime) == expected, 'Host runtime changed; inspect before using fixed ranges'
out.mkdir(exist_ok=False)
version = subprocess.run(['/usr/bin/sw_vers'], capture_output=True, text=True, check=True).stdout
objdump = subprocess.run(['/usr/bin/xcrun', '--find', 'llvm-objdump'], capture_output=True, text=True, check=True).stdout.strip()
sdk = subprocess.run(['/usr/bin/xcrun', '--show-sdk-path'], capture_output=True, text=True, check=True).stdout.strip()
tool_version = subprocess.run([objdump, '--version'], capture_output=True, text=True, check=True).stdout
identity = {'captured_utc': datetime.now(timezone.utc).isoformat(), 'client_date': '2026-10-27',
            'host_sw_vers': version, 'runtime': str(runtime), 'runtime_sha256': expected,
            'runtime_bytes': runtime.stat().st_size, 'llvm_objdump': objdump,
            'llvm_objdump_sha256': digest(Path(objdump)), 'llvm_version': tool_version,
            'sdk_path': sdk, 'scope': 'offline read-only files; no installed app/prefix/process launch'}
(out / 'identity.json').write_text(json.dumps(identity, indent=2) + '\n')
commands = []
for name, flags in [
    ('sections', ['--section-headers']),
    ('headers', ['--macho', '--private-headers']),
    ('symbols', ['--macho', '--syms']),
    ('function-starts', ['--macho', '--function-starts=addrs']),
    ('unwind', ['--macho', '--unwind-info']),
    ('dwarf-frames', ['--dwarf=frames']),
    ('text-disassembly', ['--disassemble', '--section=__text'])
]:
    command = [objdump, '--no-debuginfod', *flags, str(runtime)]
    result = subprocess.run(command, capture_output=True, timeout=45)
    (out / (name + '.txt')).write_bytes(result.stdout)
    (out / (name + '.stderr.txt')).write_bytes(result.stderr)
    commands.append({'name': name, 'command': command, 'exit': result.returncode,
                     'stdout_bytes': len(result.stdout), 'stderr_bytes': len(result.stderr)})
    (out / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
    print(f'{name}: exit={result.returncode} stdout={len(result.stdout)} stderr={len(result.stderr)}', flush=True)
    if result.returncode: raise RuntimeError('Inspect preserved tool output: ' + name)
assert digest(runtime) == expected, 'Runtime changed during static read'
(out / 'output-sha256.json').write_text(json.dumps({path.name: digest(path) for path in sorted(out.iterdir()) if path.is_file()}, indent=2) + '\n')
print('Static extraction complete; runtime unchanged; no app/game/prefix execution.', flush=True)
