#!/usr/bin/env python3
"""Bounded pinned-upstream correctness/fault tests, only owned native programs.

No game, installed prefix, profiling, protected-process attachment or default
privileged attach. Reject a silent unhooked cooperative fallback.
"""
import argparse
from collections import Counter
import json
import re
import run as shared

parser = argparse.ArgumentParser()
parser.add_argument("--tag", required=True)
args = parser.parse_args()
if not args.tag.replace("-", "").isalnum():
    raise RuntimeError("Unsafe run tag")
root = shared.ROOT
out = root / "runs" / args.tag
out.mkdir(parents=True, exist_ok=False)
helper = root / "build/helper-upstream-current/bin/x87sidecar"
samples = {
    "test_fist_pressure": [],
    "test_fild_fistp_copy": [],
    "test_m80_copy": [],
    "test_flags_handoff": [],
    "test_delphi_move": ["0.08"],
    "test_x87_signal_storm": ["all", "0.08"],
    "test_x87_signal_context": [],
    "test_x87_native_state": [],
    "test_x87_trace_threads": [],
}
binary_root = root / "bin/upstream-current"
inputs = [helper, root / "src/run-upstream-current-tests.py",
          root / "src/build-upstream-current-tests.sh",
          *[binary_root / name for name in (*samples, "test_x87_sync_fault")]]
(out / "input-hashes.json").write_text(json.dumps(
    {str(p): shared.digest(p) for p in inputs}, indent=2) + "\n")
results = []

def record(backend, sample, arguments, extra=None):
    env = shared.harness.environment("native")
    accelerated = backend != "stock"
    if accelerated:
        env.update({"X87_LOGS": "1", "X87_STRICT_ALLOC": "1"})
        if backend == "decode-off": env["X87_NO_DECODE_HOOK"] = "1"
    env.update(extra or {})
    binary = binary_root / sample
    command = ([str(helper), "--cooperative", str(binary)] if accelerated else
               ["/usr/bin/arch", "-x86_64", str(binary)]) + arguments
    stem = backend + "-" + sample
    log = out / (stem + ".log")
    (out / (stem + "-environment.json")).write_text(json.dumps(env, indent=2) + "\n")
    result = {"backend": backend, "sample": sample,
              **shared.harness.bounded(command, env, log, 45)}
    text = log.read_text(errors="replace")
    result["pass_lines"] = len(re.findall(r"^PASS\s", text, re.MULTILINE))
    result["fail_lines"] = len(re.findall(r"^FAIL\s", text, re.MULTILINE))
    result["skip_lines"] = len(re.findall(r"^SKIP\s", text, re.MULTILINE))
    result["hook_installed"] = "M2: handler installed" in text
    result["decode_installed"] = "decode_opcode entry patched" in text
    result["decode_off_confirmed"] = "skipping the decode_opcode hook" in text
    result["unhooked_fallback"] = "RUNNING WITHOUT X87 ACCELERATION" in text
    result["status"] = "TIMEOUT" if result["timeout"] else (
        "PASS" if result["exit"] == 0 and result["pass_lines"] and not result["fail_lines"]
        else "FAIL")
    if result["matching_assertion"] or result["other_assertion"]:
        result["status"] = "ASSERTION"
    if accelerated and result["status"] == "PASS" and (
        not result["hook_installed"] or result["unhooked_fallback"] or
        not (result["decode_off_confirmed"] if backend == "decode-off" else result["decode_installed"])):
        result["status"] = "UNVERIFIED"
    results.append(result)
    (out / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    print(f"{backend} {sample}: {result['status']} exit={result['exit']} "
          f"checks={result['pass_lines']} skipped={result['skip_lines']} "
          f"Rosetta-match={result['matching_assertion']} ({result['seconds']}s)", flush=True)

for backend in ("stock", "default", "decode-off"):
    for sample, arguments in samples.items(): record(backend, sample, arguments)
# The pinned upstream declares later in-run floating-point faults unsupported.
# Do not run its deliberate known-abort/default-attach cases here. Exercise only
# the complete PRECISE group, stock and accelerated with its explicit opt-in.
record("stock", "test_x87_sync_fault", [])
record("precise", "test_x87_sync_fault", [], {"X87_PRECISE_FAULTS": "1"})
print("Summary: " + json.dumps(dict(Counter(r["status"] for r in results))), flush=True)
raise SystemExit(0 if all(r["status"] == "PASS" for r in results) else 1)
