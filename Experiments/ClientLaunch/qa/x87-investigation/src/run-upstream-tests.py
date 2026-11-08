#!/usr/bin/env python3
"""Focused, bounded upstream signal/concurrency regressions; no game or ptrace."""
import json
import run as shared

root = shared.ROOT
run = root / "runs/upstream-signals-first"
run.mkdir(parents=True, exist_ok=False)
samples = {
    "test_x87_signal_storm": ["all", "0.08"],
    "test_x87_signal_context": [],
    "test_x87_native_state": [],
    "test_x87_trace_threads": [],
}
(run / "input-hashes.json").write_text(json.dumps({
    str(path): shared.digest(path) for path in [shared.HELPER, *[root / "bin/upstream" / s for s in samples]]
}, indent=2) + "\n")
results = []
for backend in ("stock", "sidecar", "decode-off", "always-none", "ir-off"):
    env = shared.harness.environment("native")
    if backend != "stock":
        env.update({"X87_LOGS": "1", **shared.CONFIGS[backend]})
    for name, args in samples.items():
        binary = root / "bin/upstream" / name
        command = (["/usr/bin/arch", "-x86_64", str(binary)] if backend == "stock"
                   else [str(shared.HELPER), "--cooperative", str(binary)]) + args
        log = run / f"{backend}-{name}.log"
        result = {"backend": backend, "sample": name, **shared.harness.bounded(command, env, log, 40)}
        # Upstream samples use PASS/failure(s), not our own END status token.
        output = log.read_text(errors="replace")
        result["status"] = "TIMEOUT" if result["timeout"] else "PASS" if result["exit"] == 0 else "FAIL"
        result["hook_installed"] = "M2: handler installed" in output
        if backend != "stock" and result["status"] == "PASS" and not result["hook_installed"]:
            result["status"] = "UNVERIFIED"
        results.append(result)
        (run / "results.json").write_text(json.dumps(results, indent=2) + "\n")
        print(f"{backend} {name}: {result['status']} exit={result['exit']} "
              f"Rosetta-match={result['matching_assertion']} ({result['seconds']}s)", flush=True)
