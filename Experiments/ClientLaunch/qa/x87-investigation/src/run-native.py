#!/usr/bin/env python3
"""Native cooperative controls using upstream's voluntary handshake shim."""
import argparse
import json
from pathlib import Path
import run as shared


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--backends", nargs="+", choices=list(shared.CONFIGS), default=["stock", "sidecar"])
    parser.add_argument("--cases", nargs="+", required=True)
    parser.add_argument("--tag", required=True)
    args = parser.parse_args()
    if not args.tag.replace("-", "").isalnum():
        raise RuntimeError("Unsafe tag")
    root = shared.ROOT
    run = root / "runs" / args.tag
    run.mkdir(parents=True, exist_ok=False)
    binary = root / "bin/x87-breakpoint-macos"
    (run / "input-hashes.json").write_text(json.dumps({
        str(path): shared.digest(path) for path in (binary, shared.HELPER,
                                                   root / "src/x87_breakpoint_probe.c")
    }, indent=2) + "\n")
    results = []
    env = shared.harness.environment("native")
    for backend in args.backends:
        case_env = dict(env)
        command = ["/usr/bin/arch", "-x86_64", str(binary)]
        if backend != "stock":
            case_env.update({"X87_LOGS": "1", "X87_LOG_OPS": "1", **shared.CONFIGS[backend]})
            command = [str(shared.HELPER), "--cooperative", str(binary)]
        (run / (backend + "-environment.json")).write_text(json.dumps(case_env, indent=2) + "\n")
        for case in args.cases:
            log = run / f"native-{backend}-{case}.log"
            result = {"backend": backend, "case": case,
                      **shared.harness.bounded(command + [case], case_env, log, 15)}
            output = log.read_text(errors="replace")
            result["hook_installed"] = "M2: handler installed" in output
            result["handshake_completed"] = "handshake reply kr=0x0" in output
            if backend != "stock" and result["status"] == "PASS" and not result["handshake_completed"]:
                result["status"] = "UNVERIFIED"
            results.append(result)
            (run / "results.json").write_text(json.dumps(results, indent=2) + "\n")
            print(f"native {backend} {case}: {result['status']} exit={result['exit']} "
                  f"Rosetta-match={result['matching_assertion']} ({result['seconds']}s)", flush=True)
    print("Summary: " + json.dumps({status: sum(r["status"] == status for r in results)
                                    for status in ("PASS", "FAIL", "TIMEOUT", "UNVERIFIED")}), flush=True)


if __name__ == "__main__":
    main()
