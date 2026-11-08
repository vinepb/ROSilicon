#!/usr/bin/env python3
"""Bounded subprocess harness. Only these fixed, game-free prefixes are writable.

It intentionally captures program output to files, not inherited pipes, because
Wine background services can otherwise keep a pipe open after the test exits.
No debugger attachment, launcher preference writes, DLL hooks or game execution.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
RUNTIMES = {
    "wine-11.15": Path("/Applications/ROSilicon.app/Contents/Resources/Wine"),
    # Pin the retained r20 baseline now that the installed QA app is r21.
    "wine-11.19": ROOT.parent / "backups/ROSilicon QA.r20.pre-invalidation.app/Contents/Resources/Wine",
    "wine-11.19-patched": ROOT / "patched-runtime",
}
CASES = ["control", "static-int3", "raised", "int3-rx", "int3-rwx", "int03-rx",
         "int03-rwx", "rep-int3", "icebp", "ud2", "boundary-int03",
         "single-step", "mutate-rwx", "mutate-wx", "trap-restore",
         "writeproc-rwalloc", "writeproc-execalloc", "writeproc-explicit-toggle",
         "writeproc-mixed", "writeproc-child", "thread-int3", "thread-mutate"]


def environment(name):
    base = ROOT / name if name in RUNTIMES else ROOT / "native"
    for leaf in ("home", "tmp"):
        (base / leaf).mkdir(parents=True, exist_ok=True)
    env = {"HOME": str(base / "home"), "TMPDIR": str(base / "tmp") + "/",
           "PATH": "/usr/bin:/bin:/usr/sbin:/sbin", "LANG": "en_US.UTF-8"}
    if name in RUNTIMES:
        runtime = RUNTIMES[name]
        prefix = base / "prefix"
        prefix.mkdir(mode=0o700, exist_ok=True)
        env.update({"WINEPREFIX": str(prefix), "WINEARCH": "win64", "WINEDEBUG": "-all",
                    "WINELOADER": str(runtime / "bin/wine"),
                    "WINESERVER": str(runtime / "bin/wineserver"),
                    "WINEDLLOVERRIDES": "mscoree,mshtml=",
                    "DYLD_LIBRARY_PATH": str(runtime / "lib/external"),
                    "PATH": str(runtime / "bin") + ":" + env["PATH"]})
        if prefix.is_symlink() or prefix.parent != ROOT / name:
            raise RuntimeError("Unsafe prefix identity")
        stat = prefix.stat()
        coordination = f"/private/tmp/.wine-{os.getuid()}/server-{stat.st_dev:x}-{stat.st_ino:x}"
        identity = {"prefix": str(prefix), "home": env["HOME"], "runtime": str(runtime),
                    "device": stat.st_dev, "inode": stat.st_ino, "coordination": coordination}
        (base / "identity.json").write_text(json.dumps(identity, indent=2) + "\n")
    return env


def bounded(command, env, log, seconds):
    start = time.monotonic()
    timed_out = False
    with log.open("wb") as out:
        process = subprocess.Popen(command, env=env, cwd=ROOT, stdout=out,
                                   stderr=subprocess.STDOUT, start_new_session=True)
        try:
            code = process.wait(timeout=seconds)
        except subprocess.TimeoutExpired:
            timed_out = True
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            code = process.wait(timeout=5)
    text = log.read_text(errors="replace")
    return {"exit": code, "timeout": timed_out, "seconds": round(time.monotonic() - start, 3),
            "matching_assertion": "found_x86_offset" in text,
            "other_assertion": "assertion failed" in text and "found_x86_offset" not in text,
            "status": "TIMEOUT" if timed_out else "PASS" if code == 0 and "status=PASS" in text else "FAIL",
            "log": str(log), "command": [str(x) for x in command]}


def stop(name, env, run_dir):
    for flag in ("-k", "-w"):
        result = bounded([env["WINESERVER"], flag], env,
                         run_dir / (name + "-stop-" + flag[1:] + ".log"), 15)
        if result["timeout"] or result["exit"]:
            raise RuntimeError("Isolated probe server did not stop: " + json.dumps(result))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--targets", nargs="+", choices=["native", *RUNTIMES],
                        default=["native", *RUNTIMES])
    parser.add_argument("--cases", nargs="+", choices=CASES, default=CASES)
    parser.add_argument("--timeout", type=int, default=12)
    parser.add_argument("--tag", required=True)
    args = parser.parse_args()
    if not args.tag.replace("-", "").isalnum():
        raise RuntimeError("Unsafe run tag")
    run_dir = ROOT / "runs" / args.tag
    run_dir.mkdir(parents=True, exist_ok=False)
    results = []
    (run_dir / "binary-hashes.json").write_text(json.dumps({
        path.name: hashlib.sha256(path.read_bytes()).hexdigest()
        for path in (ROOT / "bin").glob("probe-*")}, indent=2) + "\n")
    (run_dir / "runtime-hashes.json").write_text(json.dumps({
        target: {leaf: hashlib.sha256((RUNTIMES[target] / leaf).read_bytes()).hexdigest()
                 for leaf in ("bin/wine", "bin/wineserver", "lib/wine/x86_64-unix/ntdll.so")}
        for target in args.targets if target in RUNTIMES}, indent=2) + "\n")
    for target in args.targets:
        env = environment(target)
        try:
            if target in RUNTIMES:
                init = bounded([env["WINELOADER"], "wineboot", "--init"], env,
                               run_dir / (target + "-wineboot.log"), 60)
                print(f"{target} init exit={init['exit']} timeout={init['timeout']}", flush=True)
                if init["timeout"] or init["exit"]:
                    raise RuntimeError("Probe prefix initialization failed; inspect " + init["log"])
                for link in (ROOT / target / "prefix/drive_c/users").glob("*"):
                    if link.is_symlink() and not link.resolve().is_relative_to(ROOT / target / "home"):
                        raise RuntimeError("Wine user-profile link escapes isolated HOME: " + str(link))
                bits_list = [32, 64]
            else:
                bits_list = [64]
            for bits in bits_list:
                for case in args.cases:
                    if target == "native" and case.startswith("writeproc-"):
                        continue  # Win32 API; not comparable to a direct native write.
                    binary = ROOT / "bin" / (f"probe-{bits}.exe" if target in RUNTIMES
                                              else "probe-macos-x86_64")
                    command = ([env["WINELOADER"]] if target in RUNTIMES
                               else ["/usr/bin/arch", "-x86_64"]) + [str(binary), case]
                    log = run_dir / f"{target}-{bits}-{case}.log"
                    result = {"target": target, "bits": bits, "case": case,
                              **bounded(command, env, log, args.timeout)}
                    results.append(result)
                    (run_dir / "results.json").write_text(json.dumps(results, indent=2) + "\n")
                    print(f"{target} {bits} {case}: {result['status']} exit={result['exit']} "
                          f"Rosetta-match={result['matching_assertion']} ({result['seconds']}s)", flush=True)
                    if result["timeout"]:
                        stop(target, env, run_dir) if target in RUNTIMES else None
        finally:
            if target in RUNTIMES:
                stop(target, env, run_dir)
    print("Summary: " + json.dumps({status: sum(r["status"] == status for r in results)
                                    for status in ("PASS", "FAIL", "TIMEOUT")}), flush=True)


if __name__ == "__main__":
    main()
