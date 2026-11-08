#!/usr/bin/env python3
"""Cold publication plus own pthread signals; voluntary native helper only."""
import argparse
from collections import Counter
import json
import re
import run as shared

root = shared.ROOT
helpers = {"stock": None, "current": root / "build/helper-upstream-current/bin/x87sidecar",
           "decode-off": root / "build/helper-upstream-current/bin/x87sidecar",
           "pure-unlinked": root / "build/helper-passthrough/bin/x87sidecar",
           "pure-linked": root / "build/helper-frame-chain/bin/x87sidecar",
           "latest": root / "build/helper-upstream-oct26/bin/x87sidecar",
           "latest-decode-off": root / "build/helper-upstream-oct26/bin/x87sidecar"}
knobs = {"decode-off": {"X87_NO_DECODE_HOOK": "1"},
         "latest-decode-off": {"X87_NO_DECODE_HOOK": "1"},
         "pure-unlinked": {"X87_TEST_DECODE_RELATIVE": "1", "X87_TEST_DECODE_TAIL": "0",
             "X87_TEST_DECODE_PASSTHROUGH": "1", "X87_TEST_NO_ARPL": "1", "X87_TEST_NO_FCOMP": "1"},
         "pure-linked": {"X87_TEST_DECODE_RELATIVE": "1", "X87_TEST_DECODE_TAIL": "0",
             "X87_TEST_DECODE_PASSTHROUGH": "1", "X87_TEST_DECODE_FRAME_CHAIN": "1",
             "X87_TEST_NO_ARPL": "1", "X87_TEST_NO_FCOMP": "1"}}
parser = argparse.ArgumentParser()
parser.add_argument("--tag", required=True)
parser.add_argument("--backends", choices=list(helpers), nargs="+", default=["stock"])
parser.add_argument("--modes", choices=("hot", "rewrite", "remap"), nargs="+", default=["hot", "rewrite", "remap"])
parser.add_argument("--signals", choices=("none", "plain", "modctx"), nargs="+", default=["none", "plain", "modctx"])
parser.add_argument("--aliases", choices=(0, 1), type=int, nargs="+", default=[0])
parser.add_argument("--breakpoints", choices=(0, 1), type=int, default=1)
parser.add_argument("--rounds", type=int, default=1024)
parser.add_argument("--spin", type=int, default=4096)
parser.add_argument("--log-ops", action="store_true")
args = parser.parse_args()
if not args.tag.replace("-", "").isalnum() or not 1 <= args.rounds <= 8192 or not 64 <= args.spin <= 65536:
    raise RuntimeError("Unsafe tag/loop limit")
if args.log_ops and (args.rounds > 8 or args.signals != ["none"]):
    raise RuntimeError("Log-ops is only for tiny no-signal owned activation gates")
out = root / "runs" / args.tag
out.mkdir(parents=True, exist_ok=False)
binary = root / "bin/cold-signal-x86_64"
inputs = [root / "src/cold_signal_probe.c", root / "src/run-cold-signals.py",
          root / "src/build-cold-signal-probe.sh", binary,
          root / "source-upstream-2026-10-12/tests/coop_handshake.c",
          *[helpers[backend] for backend in args.backends if helpers[backend]]]
(out / "input-hashes.json").write_text(json.dumps({str(path): shared.digest(path) for path in inputs}, indent=2) + "\n")
results = []
for backend in args.backends:
    env = shared.harness.environment("native")
    if helpers[backend]:
        env.update({"X87_LOGS": "1", **knobs.get(backend, {})})
        if args.log_ops: env["X87_LOG_OPS"] = "1"
    (out / (backend + "-environment.json")).write_text(json.dumps(env, indent=2) + "\n")
    for alias in args.aliases:
        for mode in args.modes:
            for signals in args.signals:
                name = f"{backend}-{alias}-{mode}-{signals}"
                log = out / (name + ".log")
                command = ([str(helpers[backend]), "--cooperative", str(binary)] if helpers[backend] else
                           ["/usr/bin/arch", "-x86_64", str(binary)]) + [mode, signals, str(alias),
                           "1" if backend in ("current", "latest") else "0", str(args.rounds), str(args.spin), str(args.breakpoints)]
                result = {"backend": backend, "alias": alias, "mode": mode, "signals": signals,
                          "rounds": args.rounds, "spin": args.spin, "breakpoints": args.breakpoints,
                          **shared.harness.bounded(command, env, log, 20)}
                text = log.read_text(errors="replace")
                observation = re.search(r"^OBS completed=(\d+) publications=(\d+) traps=(\d+) alias-sigills=(\d+) signals=(\d+) code-hits=(\d+) publish-hits=(\d+) invoke-hits=(\d+) entry-changes=(\d+) producer-error=(\d+)", text, re.MULTILINE)
                result["observations"] = dict(zip(("completed", "publications", "traps", "alias_sigills",
                    "signals", "code_hits", "publish_hits", "invoke_hits", "entry_changes", "producer_error"),
                    map(int, observation.groups()))) if observation else None
                result["hook_installed"] = "M2: handler installed" in text
                result["decode_installed"] = "decode_opcode entry patched" in text
                result["decode_off_confirmed"] = "skipping the decode_opcode hook" in text
                result["fallback"] = "RUNNING WITHOUT X87 ACCELERATION" in text
                if "status=UNVERIFIED" in text: result["status"] = "UNVERIFIED"
                if result["matching_assertion"] or result["other_assertion"]: result["status"] = "ASSERTION"
                if result["status"] == "PASS" and not observation: result["status"] = "UNVERIFIED"
                if backend != "stock" and result["status"] == "PASS":
                    selected = result["decode_off_confirmed"] if backend in ("decode-off", "latest-decode-off") else result["decode_installed"]
                    if not result["hook_installed"] or not selected or result["fallback"]: result["status"] = "UNVERIFIED"
                    if backend.startswith("pure-"):
                        markers = ["test decoder passthrough=on substitutions=inactive", "test decoder relative-jumps=on",
                                   "test decoder tail-original=off", "test decoder paths: ARPL=off FCOMP=off"]
                        if backend == "pure-linked": markers.append("test decoder frame-chain=on")
                        if not all(marker in text for marker in markers): result["status"] = "UNVERIFIED"
                results.append(result)
                (out / "results.json").write_text(json.dumps(results, indent=2) + "\n")
                print(f"{name}: {result['status']} exit={result['exit']} "
                      f"Rosetta-match={result['matching_assertion']} ({result['seconds']}s)", flush=True)
print("Summary: " + json.dumps(dict(Counter(result["status"] for result in results))), flush=True)
raise SystemExit(0 if all(result["status"] == "PASS" for result in results) else 1)
