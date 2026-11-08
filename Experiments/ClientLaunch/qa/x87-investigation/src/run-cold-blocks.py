#!/usr/bin/env python3
"""Bounded own-code translation lifecycle tests; private game-free Wine prefixes."""
import argparse
from collections import Counter
import json
from pathlib import Path
import re
import run as shared

parser = argparse.ArgumentParser()
parser.add_argument("--tag", required=True)
parser.add_argument("--rounds", type=int, default=256)
parser.add_argument("--threads", nargs="+", type=int, choices=(1, 4), default=[1, 4])
parser.add_argument("--modes", nargs="+", choices=("warm", "wx", "rwx", "wpm", "cross-wpm"),
                    default=["warm", "wx", "rwx", "wpm", "cross-wpm"])
parser.add_argument("--backends", nargs="+", choices=("stock", "current", "decode-off"),
                    default=["stock", "current", "decode-off"])
parser.add_argument("--bits", nargs="+", type=int, choices=(32, 64), default=[32, 64])
parser.add_argument("--chain-depth", type=int, choices=(0, 1, 2), default=0)
args = parser.parse_args()
if not args.tag.replace("-", "").isalnum() or not 1 <= args.rounds <= 4096:
    raise RuntimeError("Unsafe tag or unbounded iterations")
root = shared.ROOT
out = root / "runs" / args.tag
out.mkdir(parents=True, exist_ok=False)
helper = root / "build/helper-upstream-current/bin/x87sidecar"
inputs = [root / "src/cold_block_probe.c", root / "src/run-cold-blocks.py", helper,
          shared.RUNTIME / "bin/wine", shared.RUNTIME / "bin/wineserver",
          shared.RUNTIME / "lib/wine/x86_64-unix/ntdll.so",
          *[root / f"bin/cold-block-{bits}.exe" for bits in args.bits]]
(out / "input-hashes.json").write_text(json.dumps(
    {str(p): shared.digest(p) for p in inputs}, indent=2) + "\n")
results = []
for backend in args.backends:
    prefix_name = "r21-stock" if backend == "stock" else "r21-sidecar"
    env = shared.harness.environment(prefix_name)
    try:
        boot = shared.harness.bounded([env["WINELOADER"], "wineboot", "--init"], env,
                                     out / (backend + "-wineboot.log"), 45)
        if boot["exit"] or boot["timeout"]: raise RuntimeError("Private prefix init failed")
        for link in (Path(env["WINEPREFIX"]) / "drive_c/users").glob("*"):
            if link.is_symlink() and not link.resolve().is_relative_to(Path(env["HOME"])):
                raise RuntimeError("Wine user profile escaped private HOME")
        if backend != "stock":
            env.update({"X87_SIDECAR_PATH": str(helper), "X87_LOGS": "1"})
            if backend == "decode-off": env["X87_NO_DECODE_HOOK"] = "1"
        (out / (backend + "-environment.json")).write_text(json.dumps(env, indent=2) + "\n")
        for bits in args.bits:
            # This Wine loader accelerates PE32; PE64 stock is an architecture
            # control, not an unverifiable claim of hooked 64-bit execution.
            if bits == 64 and backend != "stock": continue
            for threads in args.threads:
                for mode in args.modes:
                    name = f"{backend}-{bits}-{threads}-{mode}"
                    log = out / (name + ".log")
                    command = [env["WINELOADER"], str(root / f"bin/cold-block-{bits}.exe"),
                               mode, str(threads), str(args.rounds)]
                    if args.chain_depth:
                        command += [str(args.chain_depth), "1" if backend == "current" else "0"]
                    result = {"backend": backend, "bits": bits, "threads": threads,
                              "mode": mode, "rounds": args.rounds, "chain_depth": args.chain_depth,
                              **shared.harness.bounded(command, env, log, 25)}
                    text = log.read_text(errors="replace")
                    result["hook_installed"] = "M2: handler installed" in text
                    result["decode_installed"] = "decode_opcode entry patched" in text
                    result["decode_off_confirmed"] = "skipping the decode_opcode hook" in text
                    result["unhooked_fallback"] = "RUNNING WITHOUT X87 ACCELERATION" in text
                    result["observed_processes"] = len(re.findall(r"^OBS process=", text, re.MULTILINE))
                    result["observed_passes"] = len(re.findall(r"^END case=cold-block .*status=PASS", text, re.MULTILINE))
                    result["hook_markers"] = text.count("M2: handler installed")
                    result["decode_markers"] = text.count("decode_opcode entry patched")
                    result["decode_off_markers"] = text.count("skipping the decode_opcode hook")
                    if result["matching_assertion"] or result["other_assertion"]:
                        result["status"] = "ASSERTION"
                    if backend != "stock" and result["status"] == "PASS" and (
                        result["unhooked_fallback"] or not result["hook_installed"] or
                        not (result["decode_off_confirmed"] if backend == "decode-off" else result["decode_installed"])):
                        result["status"] = "UNVERIFIED"
                    if args.chain_depth and result["status"] == "PASS":
                        expected = args.chain_depth + 1
                        if result["observed_processes"] != expected or result["observed_passes"] != expected:
                            result["status"] = "UNVERIFIED"
                        if backend != "stock" and (result["hook_markers"] < expected or
                            (result["decode_off_markers"] if backend == "decode-off" else result["decode_markers"]) < expected):
                            result["status"] = "UNVERIFIED"
                    results.append(result)
                    (out / "results.json").write_text(json.dumps(results, indent=2) + "\n")
                    print(f"{name}: {result['status']} exit={result['exit']} "
                          f"Rosetta-match={result['matching_assertion']} ({result['seconds']}s)", flush=True)
                    if result["timeout"]: shared.harness.stop(prefix_name, env, out)
    finally:
        shared.harness.stop(prefix_name, env, out)
print("Summary: " + json.dumps(dict(Counter(r["status"] for r in results))), flush=True)
raise SystemExit(0 if all(r["status"] == "PASS" for r in results) else 1)
