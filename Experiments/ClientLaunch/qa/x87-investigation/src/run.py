#!/usr/bin/env python3
"""Own-process Wine/x87 tests; reuse the original bounded probe harness.

No installed prefix, game executable, launcher preference or shared cache write.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
PREVIOUS = ROOT.parent / "probes"
spec = importlib.util.spec_from_file_location("bounded_probe", PREVIOUS / "src/run.py")
harness = importlib.util.module_from_spec(spec)
spec.loader.exec_module(harness)
harness.ROOT = ROOT / "environments"
RUNTIME = PREVIOUS / "patched-runtime"
HELPER = ROOT.parent / "probes/qa-app-r21/ROSilicon QA.app/Contents/Resources/x87sidecar"
CONFIGS = {
    "stock": {},
    "sidecar": {},
    "hook-off": {"X87_DISABLE_HOOK": "1"},
    "decode-off": {"X87_NO_DECODE_HOOK": "1"},
    "ir-off": {"X87_DISABLE_X87_IR": "1"},
    "cache-off": {"X87_DISABLE_CACHE": "1"},
    "bridge-off": {"X87_ENABLE_BRIDGE": "0"},
    "ipc-cache-off": {"X87_NO_TCO_CACHE": "1", "X87_NO_IR_CACHE": "1"},
    "always-none": {"X87_ALWAYS_NONE": "1"},
}
harness.RUNTIMES = {"r21-" + name: RUNTIME for name in CONFIGS}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--backends", nargs="+", choices=list(CONFIGS), default=["stock", "sidecar"])
    parser.add_argument("--program", choices=["legacy", "x87"], default="legacy")
    parser.add_argument("--cases", nargs="+", required=True)
    parser.add_argument("--bits", nargs="+", choices=[32, 64], type=int, default=[32])
    parser.add_argument("--timeout", type=int, default=15)
    parser.add_argument("--tag", required=True)
    args = parser.parse_args()
    if not args.tag.replace("-", "").isalnum():
        raise RuntimeError("Unsafe run tag")
    if args.program == "legacy" and any(case not in harness.CASES for case in args.cases):
        raise RuntimeError("Unknown legacy case")
    if args.timeout < 1 or args.timeout > 45:
        raise RuntimeError("Timeout out of bounds")
    run = ROOT / "runs" / args.tag
    run.mkdir(parents=True, exist_ok=False)
    harness.ROOT.mkdir(parents=True, exist_ok=True)
    paths = {"helper": HELPER, "wine": RUNTIME / "bin/wine", "server": RUNTIME / "bin/wineserver",
             "ntdll": RUNTIME / "lib/wine/x86_64-unix/ntdll.so"}
    for bits in args.bits:
        paths["probe-" + str(bits)] = (PREVIOUS / "bin" / f"probe-{bits}.exe" if args.program == "legacy"
                                      else ROOT / "bin" / f"x87-breakpoint-{bits}.exe")
    (run / "input-hashes.json").write_text(json.dumps({
        name: {"path": str(path), "sha256": digest(path)} for name, path in paths.items()
    }, indent=2) + "\n")
    results = []
    for backend in args.backends:
        target = "r21-" + backend
        # Knobs affect only each newly created process, not prefix state.
        # Reuse the stopped synthetic sidecar prefix sequentially to avoid
        # duplicating Windows bootstrap files for every optimization knob.
        env = harness.environment("r21-stock" if backend == "stock" else "r21-sidecar")
        try:
            # Bootstrap uses stock Rosetta and no optional installers. Enable
            # cooperative acceleration only for our synthetic case process.
            init = harness.bounded([env["WINELOADER"], "wineboot", "--init"], env,
                                   run / (target + "-wineboot.log"), 60)
            if init["timeout"] or init["exit"]:
                raise RuntimeError("Prefix bootstrap failed: " + init["log"])
            private_home = Path(env["HOME"])
            users = Path(env["WINEPREFIX"]) / "drive_c/users"
            for link in users.glob("*"):
                if link.is_symlink() and not link.resolve().is_relative_to(private_home):
                    raise RuntimeError("Wine user profile escaped private HOME")
            case_env = dict(env)
            if backend != "stock":
                case_env.update({"X87_SIDECAR_PATH": str(HELPER), "X87_LOGS": "1",
                                 "X87_LOG_OPS": "1", **CONFIGS[backend]})
            (run / (target + "-environment.json")).write_text(json.dumps(case_env, indent=2) + "\n")
            for bits in args.bits:
                for case in args.cases:
                    binary = paths["probe-" + str(bits)]
                    log = run / f"{target}-{bits}-{case}.log"
                    result = {"backend": backend, "bits": bits, "case": case,
                              **harness.bounded([env["WINELOADER"], str(binary), case],
                                                case_env, log, args.timeout)}
                    output = log.read_text(errors="replace")
                    # Wine's -all logging hides its own ERR-level handshake
                    # reply line. The helper independently records the ports
                    # received from our loader; the completed fixture proves
                    # the blocked loader was then released.
                    result["cooperative_handshake"] = "cooperative attach: task=" in output
                    result["hook_installed"] = "M2: handler installed" in output
                    result["hook_off_confirmed"] = "passthrough mode; releasing without hook" in output
                    if backend != "stock" and bits == 32 and result["status"] == "PASS":
                        active = result["cooperative_handshake"] and (
                            result["hook_off_confirmed"] if backend == "hook-off" else result["hook_installed"])
                        if not active:
                            result["status"] = "UNVERIFIED"
                    results.append(result)
                    (run / "results.json").write_text(json.dumps(results, indent=2) + "\n")
                    print(f"{backend} {bits} {case}: {result['status']} "
                          f"exit={result['exit']} Rosetta-match={result['matching_assertion']} "
                          f"hook={result['hook_installed']} ({result['seconds']}s)", flush=True)
                    if result["timeout"]:
                        harness.stop(target, env, run)
        finally:
            harness.stop(target, env, run)
    print("Summary: " + json.dumps({status: sum(r["status"] == status for r in results)
                                    for status in ("PASS", "FAIL", "TIMEOUT", "UNVERIFIED")}), flush=True)


if __name__ == "__main__":
    main()
