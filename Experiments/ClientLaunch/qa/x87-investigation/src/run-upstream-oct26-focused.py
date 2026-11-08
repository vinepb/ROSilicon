#!/usr/bin/env python3
"""Focused current-source native tests; voluntary helpers/owned processes only."""
import argparse
from collections import Counter
import json
import re
import run as shared

parser = argparse.ArgumentParser()
parser.add_argument("--tag", required=True)
args = parser.parse_args()
if not args.tag.replace("-", "").isalnum(): raise RuntimeError("Unsafe run tag")
root = shared.ROOT
out = root / "runs" / args.tag
out.mkdir(parents=True, exist_ok=False)
helper = root / "build/helper-upstream-oct26/bin/x87sidecar"
binaries = root / "bin/upstream-oct26"
samples = ("test_x87_cross_reply_exact", "test_x87_signal_context",
           "test_decoder_arpl", "test_decoder_fcomp_st")
inputs = [helper, root / "src/run-upstream-oct26-focused.py",
          root / "src/build-upstream-oct26-focused.sh",
          *[binaries / name for name in (*samples, "test_opcode_host_table")]]
(out / "input-hashes.json").write_text(json.dumps({str(path): shared.digest(path) for path in inputs}, indent=2) + "\n")
results = []

def record(backend, sample):
    env = shared.harness.environment("native")
    accelerated = backend not in ("stock", "native-host")
    if accelerated:
        env["X87_LOGS"] = "1"
        if backend == "latest-decode-off": env["X87_NO_DECODE_HOOK"] = "1"
    binary = binaries / sample
    command = ([str(helper), "--cooperative", str(binary)] if accelerated else
               [str(binary)] if backend == "native-host" else ["/usr/bin/arch", "-x86_64", str(binary)])
    stem = backend + "-" + sample
    (out / (stem + "-environment.json")).write_text(json.dumps(env, indent=2) + "\n")
    log = out / (stem + ".log")
    result = {"backend": backend, "sample": sample,
              **shared.harness.bounded(command, env, log, 30)}
    text = log.read_text(errors="replace")
    result["pass_lines"] = len(re.findall(r"^PASS\s", text, re.MULTILINE))
    result["fail_lines"] = len(re.findall(r"^\s*FAIL\s", text, re.MULTILINE))
    result["skip_lines"] = len(re.findall(r"^\s*SKIP\s", text, re.MULTILINE))
    result["expected_unsupported"] = sample.startswith("test_decoder_") and backend in ("stock", "latest-decode-off")
    if backend == "native-host":
        ok = result["exit"] == 0 and "0 failure(s)" in text
    elif result["expected_unsupported"]:
        ok = result["exit"] == 1 and "SIGILL" in text
    else:
        ok = result["exit"] == 0 and result["pass_lines"] > 0 and not result["fail_lines"] and not result["skip_lines"]
    result["status"] = "TIMEOUT" if result["timeout"] else "PASS" if ok else "FAIL"
    result["hook_installed"] = "M2: handler installed" in text
    result["decode_installed"] = "decode_opcode entry patched" in text
    result["decode_off_confirmed"] = "skipping the decode_opcode hook" in text
    result["fallback"] = "RUNNING WITHOUT X87 ACCELERATION" in text
    if result["matching_assertion"] or result["other_assertion"]: result["status"] = "ASSERTION"
    if accelerated and result["status"] == "PASS":
        selected = result["decode_off_confirmed"] if backend == "latest-decode-off" else result["decode_installed"]
        if not result["hook_installed"] or not selected or result["fallback"]: result["status"] = "UNVERIFIED"
    results.append(result)
    (out / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    print(f"{backend} {sample}: {result['status']} exit={result['exit']} "
          f"expected-unsupported={result['expected_unsupported']} ({result['seconds']}s)", flush=True)

record("native-host", "test_opcode_host_table")
for backend in ("stock", "latest", "latest-decode-off"):
    for sample in samples: record(backend, sample)
print("Summary: " + json.dumps(dict(Counter(result["status"] for result in results))), flush=True)
raise SystemExit(0 if all(result["status"] == "PASS" for result in results) else 1)
