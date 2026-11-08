#!/usr/bin/env python3
"""Bounded decoder compatibility/map controls; owned code and private prefixes."""
import argparse
import json
from pathlib import Path
import run as shared

root = shared.ROOT
cases = ["arpl-reg", "arpl-reg-int3", "arpl-reg-guard", "arpl-memory-guard",
         "arpl-memory-fault", "arpl-reg-step", "arpl-memory-step", "arpl-memory-fault-step",
         "arpl-canonical-reg-step", "arpl-canonical-memory-step",
         "arpl-canonical-memory-fault-step",
         "arpl-overlap-int3", "arpl-wpm-int3", "arpl-direct-int3",
         "arpl-opcode-wpm-int3", "arpl-opcode-direct-int3",
         "arpl-partial-guard", "arpl-partial-memory-guard",
         "fcomp-alias", "fcomp-alias-int3", "fcomp-alias-guard",
         "fcomp-alias-wpm-int3", "fcomp-overlap-int3", "fcomp-canonical-int3",
         "fcomp-opcode-wpm-int3", "fcomp-opcode-direct-int3", "fcomp-partial-guard"]
helpers = {"stock": None, "bundled": shared.HELPER,
           "built": root / "build/helper-baseline/bin/x87sidecar",
           "decode-off": shared.HELPER,
           **{name: root / "build/helper-upstream-current/bin/x87sidecar" for name in
              ("upstream-current", "upstream-decode-off")},
           **{name: root / "build/helper-upstream-oct26/bin/x87sidecar" for name in
              ("latest", "latest-decode-off")},
           **{name: root / "build/helper-frame-chain/bin/x87sidecar" for name in
              ("frame-built", "frame-pure-unlinked", "frame-pure-linked")},
           **{name: root / "build/helper-passthrough/bin/x87sidecar" for name in
              ("passthrough-built", "passthrough-no-substitutions", "passthrough-pure")},
           **{name: root / "build/helper-near-tail/bin/x87sidecar" for name in
              ("near-tail-built", "near-tail-original", "near-tail-relative", "near-tail-wrapper-relative",
               "near-tail-wrapper-no-substitutions")},
           **{name: root / "build/helper-install-control/bin/x87sidecar" for name in
              ("install-built", "install-original", "install-cache")},
           **{name: root / "build/helper-near-branch/bin/x87sidecar" for name in
              ("near-built", "near-relative")},
           **{name: root / "build/helper-trampoline/bin/x87sidecar" for name in
              ("tail-built", "tail-original")},
           **{name: root / "build/helper-experiment/bin/x87sidecar" for name in
              ("experiment", "arpl-only", "fcomp-only", "no-substitutions")}}
knobs = {"decode-off": {"X87_NO_DECODE_HOOK": "1"},
         "upstream-decode-off": {"X87_NO_DECODE_HOOK": "1"},
         "latest-decode-off": {"X87_NO_DECODE_HOOK": "1"},
         "frame-pure-unlinked": {"X87_TEST_DECODE_RELATIVE": "1", "X87_TEST_DECODE_TAIL": "0",
                                  "X87_TEST_DECODE_PASSTHROUGH": "1", "X87_TEST_DECODE_FRAME_CHAIN": "0",
                                  "X87_TEST_NO_ARPL": "1", "X87_TEST_NO_FCOMP": "1"},
         "frame-pure-linked": {"X87_TEST_DECODE_RELATIVE": "1", "X87_TEST_DECODE_TAIL": "0",
                                "X87_TEST_DECODE_PASSTHROUGH": "1", "X87_TEST_DECODE_FRAME_CHAIN": "1",
                                "X87_TEST_NO_ARPL": "1", "X87_TEST_NO_FCOMP": "1"},
         "passthrough-no-substitutions": {"X87_TEST_DECODE_RELATIVE": "1", "X87_TEST_DECODE_TAIL": "0",
                                           "X87_TEST_DECODE_PASSTHROUGH": "0",
                                           "X87_TEST_NO_ARPL": "1", "X87_TEST_NO_FCOMP": "1"},
         "passthrough-pure": {"X87_TEST_DECODE_RELATIVE": "1", "X87_TEST_DECODE_TAIL": "0",
                               "X87_TEST_DECODE_PASSTHROUGH": "1",
                               "X87_TEST_NO_ARPL": "1", "X87_TEST_NO_FCOMP": "1"},
         "near-tail-original": {"X87_TEST_DECODE_TAIL": "1", "X87_TEST_NO_ARPL": "1",
                                "X87_TEST_NO_FCOMP": "1"},
         "near-tail-relative": {"X87_TEST_DECODE_TAIL": "1", "X87_TEST_DECODE_RELATIVE": "1",
                                "X87_TEST_NO_ARPL": "1", "X87_TEST_NO_FCOMP": "1"},
         "near-tail-wrapper-relative": {"X87_TEST_DECODE_RELATIVE": "1"},
         "near-tail-wrapper-no-substitutions": {"X87_TEST_DECODE_RELATIVE": "1", "X87_TEST_DECODE_TAIL": "0",
                                                "X87_TEST_NO_ARPL": "1", "X87_TEST_NO_FCOMP": "1"},
         "install-original": {"X87_TEST_DECODE_INSTALL_MODE": "original-entry"},
         "install-cache": {"X87_TEST_DECODE_INSTALL_MODE": "cache-only"},
         "near-relative": {"X87_TEST_DECODE_RELATIVE": "1"},
         "tail-original": {"X87_TEST_DECODE_TAIL": "1", "X87_TEST_NO_ARPL": "1",
                           "X87_TEST_NO_FCOMP": "1"},
         "arpl-only": {"X87_TEST_NO_FCOMP": "1"},
         "fcomp-only": {"X87_TEST_NO_ARPL": "1"},
         "no-substitutions": {"X87_TEST_NO_ARPL": "1", "X87_TEST_NO_FCOMP": "1"}}

parser = argparse.ArgumentParser()
parser.add_argument("--tag", required=True)
parser.add_argument("--backends", nargs="+", choices=list(helpers),
                    default=["stock", "bundled", "built", "decode-off"])
parser.add_argument("--cases", nargs="+", choices=cases, default=cases)
parser.add_argument("--native-only", action="store_true")
args = parser.parse_args()
if not args.tag.replace("-", "").isalnum():
    raise RuntimeError("Unsafe run tag")
out = root / "runs" / args.tag
out.mkdir(parents=True, exist_ok=False)
inputs = [root / "src/decoder_probe.c", root / "src/run-decoder.py", root / "bin/decoder-32.exe",
          root / "bin/decoder-upstream/test_decoder_arpl",
          root / "bin/decoder-upstream/test_decoder_fcomp_st",
          shared.RUNTIME / "bin/wine", shared.RUNTIME / "bin/wineserver",
          shared.RUNTIME / "lib/wine/x86_64-unix/ntdll.so",
          *[helper for backend, helper in helpers.items() if backend in args.backends and helper]]
(out / "input-hashes.json").write_text(json.dumps({str(path): shared.digest(path) for path in inputs}, indent=2) + "\n")
results = []

def record(backend, name, command, env, native=False):
    log = out / (backend + "-" + name + ".log")
    result = {"backend": backend, "case": name, "native": native,
              **shared.harness.bounded(command, env, log, 25)}
    text = log.read_text(errors="replace")
    install_mode = {"install-original": "original-entry", "install-cache": "cache-only"}.get(backend)
    decoder = backend not in ("stock", "decode-off", "upstream-decode-off", "latest-decode-off",
                              "install-original", "install-cache")
    path_enabled = decoder and backend not in ("no-substitutions", "tail-original",
                                               "near-tail-original", "near-tail-relative",
                                               "near-tail-wrapper-no-substitutions",
                                               "frame-pure-unlinked", "frame-pure-linked",
                                               "passthrough-no-substitutions", "passthrough-pure") and (
        backend != "fcomp-only" if "arpl" in name else backend != "arpl-only")
    result["hook_installed"] = "M2: handler installed" in text
    result["decode_installed"] = "decode_opcode entry patched" in text
    result["decode_off_confirmed"] = "skipping the decode_opcode hook" in text
    result["unhooked_fallback"] = "RUNNING WITHOUT X87 ACCELERATION" in text
    result["install_control_confirmed"] = bool(install_mode and
        f"decoder install-control: mode={install_mode} substitutions=inactive" in text)
    result["expected_support"] = "partial" not in name and (
        path_enabled or "canonical" in name or "overlap" in name)
    if native:
        result["status"] = "TIMEOUT" if result["timeout"] else (
            "PASS" if (result["exit"] == 0 if path_enabled else result["exit"] == 1 and "SIGILL" in text)
            else "FAIL")
    else:
        result["actual_support"] = "OBS support=supported" in text
        if result["status"] == "PASS" and result["expected_support"] != result["actual_support"]:
            result["status"] = "SUPPORT_MISMATCH"
    if result["matching_assertion"] or result["other_assertion"]:
        result["status"] = "ASSERTION"
    if backend != "stock" and result["status"] == "PASS":
        correct_hook = result["hook_installed"] and (
            result["decode_installed"] if decoder else
            result["install_control_confirmed"] if install_mode else result["decode_off_confirmed"])
        if not correct_hook or result["unhooked_fallback"]: result["status"] = "UNVERIFIED"
        if backend == "tail-original" and "test decoder tail-original=on" not in text:
            result["status"] = "UNVERIFIED"
        if backend == "near-relative" and (
            "test decoder relative-jumps=on" not in text or
            "test decoder paths: ARPL=on FCOMP=on" not in text):
            result["status"] = "UNVERIFIED"
        if backend.startswith("near-tail-"):
            tail = "on" if backend in ("near-tail-original", "near-tail-relative") else "off"
            relative = "on" if backend in ("near-tail-relative", "near-tail-wrapper-relative",
                                           "near-tail-wrapper-no-substitutions") else "off"
            paths = "off" if tail == "on" or backend == "near-tail-wrapper-no-substitutions" else "on"
            if (f"test decoder tail-original={tail}" not in text or
                f"test decoder relative-jumps={relative}" not in text or
                f"test decoder paths: ARPL={paths} FCOMP={paths}" not in text):
                result["status"] = "UNVERIFIED"
        if backend.startswith("passthrough-"):
            pure = "on substitutions=inactive" if backend == "passthrough-pure" else "off"
            relative = "off" if backend == "passthrough-built" else "on"
            paths = "on" if backend == "passthrough-built" else "off"
            if (f"test decoder passthrough={pure}" not in text or
                "test decoder tail-original=off" not in text or
                f"test decoder relative-jumps={relative}" not in text or
                f"test decoder paths: ARPL={paths} FCOMP={paths}" not in text):
                result["status"] = "UNVERIFIED"
        if backend.startswith("frame-"):
            pure = "off" if backend == "frame-built" else "on substitutions=inactive"
            relative = "off" if backend == "frame-built" else "on"
            paths = "on" if backend == "frame-built" else "off"
            linked = "on" if backend == "frame-pure-linked" else "off"
            if (f"test decoder passthrough={pure}" not in text or
                "test decoder tail-original=off" not in text or
                f"test decoder relative-jumps={relative}" not in text or
                f"test decoder frame-chain={linked}" not in text or
                f"test decoder paths: ARPL={paths} FCOMP={paths}" not in text):
                result["status"] = "UNVERIFIED"
    results.append(result)
    (out / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    print(f"{backend} {name}: {result['status']} exit={result['exit']} "
          f"Rosetta-match={result['matching_assertion']} ({result['seconds']}s)", flush=True)
    return result

for backend in args.backends:
    env = shared.harness.environment("native")
    if backend != "stock":
        env.update({"X87_LOGS": "1"})
        env.update(knobs.get(backend, {}))
    for name in ("test_decoder_arpl", "test_decoder_fcomp_st"):
        binary = root / "bin/decoder-upstream" / name
        command = (["/usr/bin/arch", "-x86_64", str(binary)] if backend == "stock"
                   else [str(helpers[backend]), "--cooperative", str(binary)])
        record(backend, name, command, env, True)
    if args.native_only: continue
    prefix_name = "r21-stock" if backend == "stock" else "r21-sidecar"
    env = shared.harness.environment(prefix_name)
    try:
        boot = shared.harness.bounded([env["WINELOADER"], "wineboot", "--init"], env,
                                     out / (backend + "-wineboot.log"), 45)
        if boot["exit"] or boot["timeout"]: raise RuntimeError("Prefix init failed")
        for link in (Path(env["WINEPREFIX"]) / "drive_c/users").glob("*"):
            if link.is_symlink() and not link.resolve().is_relative_to(Path(env["HOME"])):
                raise RuntimeError("Wine user profile escaped private HOME")
        if backend != "stock":
            env.update({"X87_SIDECAR_PATH": str(helpers[backend]), "X87_LOGS": "1"})
            env.update(knobs.get(backend, {}))
        (out / (backend + "-environment.json")).write_text(json.dumps(env, indent=2) + "\n")
        for name in args.cases:
            result = record(backend, name, [env["WINELOADER"], str(root / "bin/decoder-32.exe"), name], env)
            if result["timeout"]: shared.harness.stop(prefix_name, env, out)
    finally:
        shared.harness.stop(prefix_name, env, out)
print("Summary: " + json.dumps({status: sum(r["status"] == status for r in results)
      for status in ("PASS", "FAIL", "TIMEOUT", "SUPPORT_MISMATCH", "UNVERIFIED", "ASSERTION")}), flush=True)
raise SystemExit(0 if all(r["status"] == "PASS" for r in results) else 1)
