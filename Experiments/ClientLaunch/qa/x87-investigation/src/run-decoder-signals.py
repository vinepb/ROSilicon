#!/usr/bin/env python3
"""Owned decoder signal fixture with bounded cooperative controls."""
import argparse
import json
import run as shared

parser = argparse.ArgumentParser()
parser.add_argument("--tag", required=True)
args = parser.parse_args()
if not args.tag.replace("-", "").isalnum(): raise RuntimeError("Unsafe tag")
root = shared.ROOT
out = root / "runs" / args.tag
out.mkdir(parents=True, exist_ok=False)
binary = root / "bin/decoder-signal-x86_64"
helpers = {"stock": None, "bundled": shared.HELPER,
           "built": root / "build/helper-baseline/bin/x87sidecar", "decode-off": shared.HELPER}
inputs = [root / "src/decoder_signal_probe.c", binary, *[h for h in helpers.values() if h]]
(out / "input-hashes.json").write_text(json.dumps({str(p): shared.digest(p) for p in inputs}, indent=2) + "\n")
results = []
for backend, helper in helpers.items():
    env = shared.harness.environment("native")
    if helper:
        env["X87_LOGS"] = "1"
        if backend == "decode-off": env["X87_NO_DECODE_HOOK"] = "1"
    (out / (backend + "-environment.json")).write_text(json.dumps(env, indent=2) + "\n")
    for mode in ("none", "plain", "modctx"):
        command = ([str(helper), "--cooperative", str(binary)] if helper else
                   ["/usr/bin/arch", "-x86_64", str(binary)]) + [mode]
        log = out / (backend + "-" + mode + ".log")
        r = {"backend": backend, "case": mode, **shared.harness.bounded(command, env, log, 25)}
        text = log.read_text(errors="replace")
        hooked = backend in ("bundled", "built")
        if not hooked and r["exit"] == 1 and "OBS support=unsupported SIGILL" in text:
            r["status"] = "PASS"
        r["hook_installed"] = "M2: handler installed" in text
        r["decode_installed"] = "decode_opcode entry patched" in text
        if helper and r["status"] == "PASS" and not (r["hook_installed"] and (
            r["decode_installed"] if hooked else "skipping the decode_opcode hook" in text)):
            r["status"] = "UNVERIFIED"
        if r["matching_assertion"] or r["other_assertion"]: r["status"] = "ASSERTION"
        results.append(r)
        (out / "results.json").write_text(json.dumps(results, indent=2) + "\n")
        print(f"{backend} {mode}: {r['status']} exit={r['exit']} Rosetta-match={r['matching_assertion']}", flush=True)
print("Summary:", {s: sum(r["status"] == s for r in results) for s in {r["status"] for r in results}}, flush=True)
raise SystemExit(0 if all(r["status"] == "PASS" for r in results) else 1)
