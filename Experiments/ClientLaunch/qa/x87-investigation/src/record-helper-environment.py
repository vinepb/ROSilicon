#!/usr/bin/env python3
"""Record only selected compatibility flags from one exact owned QA helper."""
import json
from pathlib import Path
import re
import subprocess
import sys

if len(sys.argv) != 3 or not sys.argv[1].isdigit(): raise SystemExit(2)
pid, target = int(sys.argv[1]), Path(sys.argv[2]).resolve()
root = Path(__file__).resolve().parents[1]
if not target.is_relative_to(root / "runs") or target.exists(): raise RuntimeError("Unsafe evidence path")
identity = subprocess.check_output(["/bin/ps", "-p", str(pid), "-o", "command="], text=True).strip()
helpers = [root / "build" / name / "bin/x87sidecar"
           for name in ("helper-baseline", "helper-experiment", "helper-trampoline",
                        "helper-near-branch", "helper-install-control", "helper-near-tail",
                        "helper-passthrough", "helper-frame-chain", "helper-upstream-current",
                        "helper-upstream-oct26")]
if not any(identity.startswith(str(helper) + " --cooperative ") for helper in helpers):
    raise RuntimeError("Not an inventoried source-built QA helper")
if "/Applications/ROSilicon QA.app/Contents/Resources/Wine/" not in identity:
    raise RuntimeError("Not the expected QA loader")
environment = subprocess.check_output(["/bin/ps", "eww", "-p", str(pid), "-o", "command="], text=True)
# No unrelated environment keys or credential values are retained. Values that
# contain spaces are not extracted here; prefix identity was checked separately.
names = ["X87_NO_DECODE_HOOK", "X87_DISABLE_HOOK", "X87_ALWAYS_NONE",
         "X87_DISABLE_X87_IR", "X87_DISABLE_CACHE", "X87_TEST_NO_ARPL", "X87_TEST_NO_FCOMP",
         "X87_PRECISE_FAULTS", "X87_NATIVE_SLOTS", "X87_STRICT_ALLOC", "X87_LOGS", "X87_LOG_OPS",
         "X87_TEST_DECODE_TAIL", "X87_TEST_DECODE_RELATIVE", "X87_TEST_DECODE_INSTALL_MODE",
         "X87_TEST_DECODE_PASSTHROUGH",
         "X87_TEST_DECODE_FRAME_CHAIN",
         "X87_SIDECAR_PATH", "WINEDEBUG", "DXVK_ASYNC", "MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS"]
flags = {key: (m.group(1) if (m := re.search(r"(?:^|\s)" + key + r"=([^\s]*)", environment)) else None)
         for key in names}
target.write_text(json.dumps({"pid": pid, "command": identity, "selected_flags": flags}, indent=2) + "\n")
print(json.dumps(flags, indent=2))
