#!/usr/bin/env python3
"""Own instruction-fetch contexts; reuse stopped, game-free Wine prefixes only."""
import argparse
from collections import Counter
import json
from pathlib import Path
import re
import run as shared

MODES = ("data-noaccess", "data-guard", "end-noaccess", "end-guard", "fetch-noaccess", "fetch-guard")
root = shared.ROOT
helpers = {"stock": None,
           "current": root / "build/helper-upstream-current/bin/x87sidecar",
           "decode-off": root / "build/helper-upstream-current/bin/x87sidecar",
           "pure-unlinked": root / "build/helper-passthrough/bin/x87sidecar",
           "pure-linked": root / "build/helper-frame-chain/bin/x87sidecar"}
knobs = {"decode-off": {"X87_NO_DECODE_HOOK": "1"},
         "pure-unlinked": {"X87_TEST_DECODE_RELATIVE": "1", "X87_TEST_DECODE_TAIL": "0",
             "X87_TEST_DECODE_PASSTHROUGH": "1", "X87_TEST_NO_ARPL": "1", "X87_TEST_NO_FCOMP": "1"},
         "pure-linked": {"X87_TEST_DECODE_RELATIVE": "1", "X87_TEST_DECODE_TAIL": "0",
             "X87_TEST_DECODE_PASSTHROUGH": "1", "X87_TEST_DECODE_FRAME_CHAIN": "1",
             "X87_TEST_NO_ARPL": "1", "X87_TEST_NO_FCOMP": "1"}}
parser = argparse.ArgumentParser()
parser.add_argument("--tag", required=True)
parser.add_argument("--rounds", type=int, default=256)
parser.add_argument("--workers", type=int, choices=(1, 4), nargs="+", default=[1])
parser.add_argument("--modes", choices=MODES, nargs="+", default=list(MODES))
parser.add_argument("--backends", choices=list(helpers), nargs="+", default=["stock"])
parser.add_argument("--bits", type=int, choices=(32, 64), nargs="+", default=[32, 64])
args = parser.parse_args()
if not args.tag.replace("-", "").isalnum() or not 1 <= args.rounds <= 4096:
    raise RuntimeError("Unsafe run tag or iteration limit")
out = root / "runs" / args.tag
out.mkdir(parents=True, exist_ok=False)
inputs = [root / "src/fetch_boundary_probe.c", root / "src/run-fetch-boundaries.py",
          root / "src/build-fetch-boundary-probes.sh", shared.RUNTIME / "bin/wine",
          shared.RUNTIME / "bin/wineserver", shared.RUNTIME / "lib/wine/x86_64-unix/ntdll.so",
          *[root / f"bin/fetch-boundary-{bits}.exe" for bits in args.bits],
          *[helpers[backend] for backend in args.backends if helpers[backend]]]
(out / "input-hashes.json").write_text(json.dumps({str(p): shared.digest(p) for p in inputs}, indent=2) + "\n")
results = []
for backend in args.backends:
    prefix = "r21-stock" if backend == "stock" else "r21-sidecar"
    env = shared.harness.environment(prefix)
    try:
        boot = shared.harness.bounded([env["WINELOADER"], "wineboot", "--init"], env,
                                     out / (backend + "-wineboot.log"), 45)
        if boot["exit"] or boot["timeout"]: raise RuntimeError("Private bootstrap failed")
        for path in (Path(env["WINEPREFIX"]) / "drive_c/users").glob("*"):
            if path.is_symlink() and not path.resolve().is_relative_to(Path(env["HOME"])):
                raise RuntimeError("Wine profile escaped private HOME")
        if helpers[backend]:
            env.update({"X87_SIDECAR_PATH": str(helpers[backend]), "X87_LOGS": "1", **knobs.get(backend, {})})
        (out / (backend + "-environment.json")).write_text(json.dumps(env, indent=2) + "\n")
        for bits in args.bits:
            if bits == 64 and backend != "stock": continue
            for workers in args.workers:
                for mode in args.modes:
                    name = f"{backend}-{bits}-{workers}-{mode}"
                    log = out / (name + ".log")
                    command = [env["WINELOADER"], str(root / f"bin/fetch-boundary-{bits}.exe"),
                               mode, str(workers), str(args.rounds)]
                    result = {"backend": backend, "bits": bits, "workers": workers,
                              "mode": mode, "rounds": args.rounds,
                              **shared.harness.bounded(command, env, log, 25)}
                    text = log.read_text(errors="replace")
                    observations = re.findall(
                        r"^OBS worker=(\d+) completed=(\d+) faults=(\d+) traps=(\d+) reads=(\d+) executes=(\d+)",
                        text, re.MULTILINE)
                    result["observations"] = [dict(zip(
                        ("worker", "completed", "faults", "traps", "reads", "executes"), map(int, row)))
                        for row in observations]
                    result["hook_installed"] = "M2: handler installed" in text
                    result["decode_installed"] = "decode_opcode entry patched" in text
                    result["decode_off_confirmed"] = "skipping the decode_opcode hook" in text
                    result["fallback"] = "RUNNING WITHOUT X87 ACCELERATION" in text
                    if result["matching_assertion"] or result["other_assertion"]: result["status"] = "ASSERTION"
                    if result["status"] == "PASS" and len(observations) != workers: result["status"] = "UNVERIFIED"
                    if backend != "stock" and result["status"] == "PASS":
                        selected = result["decode_off_confirmed"] if backend == "decode-off" else result["decode_installed"]
                        if not result["hook_installed"] or not selected or result["fallback"]:
                            result["status"] = "UNVERIFIED"
                        if backend.startswith("pure-"):
                            markers = ["test decoder passthrough=on substitutions=inactive",
                                       "test decoder relative-jumps=on", "test decoder tail-original=off",
                                       "test decoder paths: ARPL=off FCOMP=off"]
                            if backend == "pure-linked": markers.append("test decoder frame-chain=on")
                            if not all(marker in text for marker in markers): result["status"] = "UNVERIFIED"
                    results.append(result)
                    (out / "results.json").write_text(json.dumps(results, indent=2) + "\n")
                    print(f"{name}: {result['status']} exit={result['exit']} "
                          f"Rosetta-match={result['matching_assertion']} ({result['seconds']}s)", flush=True)
                    if result["timeout"]: shared.harness.stop(prefix, env, out)
    finally:
        shared.harness.stop(prefix, env, out)
print("Summary: " + json.dumps(dict(Counter(result["status"] for result in results))), flush=True)
raise SystemExit(0 if all(result["status"] == "PASS" for result in results) else 1)
