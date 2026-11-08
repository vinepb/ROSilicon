#!/usr/bin/env python3
"""Audit only retained OWNED fixture logs, not protected-game output."""
import hashlib
import json
from pathlib import Path
import re
import sys

root = Path(__file__).resolve().parents[1]
if len(sys.argv) != 2 or not re.fullmatch(r"[A-Za-z0-9-]+", sys.argv[1]):
    raise SystemExit(2)
run = root / "runs" / sys.argv[1]
target = run / "install-control-audit.json"
if not run.is_dir() or target.exists(): raise RuntimeError("Unsafe evidence path")
results = json.loads((run / "results.json").read_text())
if len(results) != 87 or any(r["status"] != "PASS" for r in results):
    raise RuntimeError("Require the completed 3-backend, 29-case matrix")
audits = []
for result in results:
    path = Path(result["log"]).resolve()
    if path.parent != run: raise RuntimeError("Unexpected fixture log path")
    text = path.read_text(errors="replace")
    match = re.search(r"decode_opcode \(scanned\) = 0x([0-9a-f]+)", text)
    if not match: raise RuntimeError("Decoder site not verified")
    entry = int(match.group(1), 16)
    protections = re.findall(r"Adjusting memory protection at 0x([0-9a-f]+) - 0x([0-9a-f]+)", text)
    count = sum(int(lo, 16) <= entry < int(hi, 16) for lo, hi in protections)
    expected = 0 if result["backend"] == "install-cache" else 2
    if count != expected: raise RuntimeError("Unexpected decoder-entry protection calls")
    audits.append({"backend": result["backend"], "case": result["case"],
                   "decoder_entry_protection_calls": count,
                   "sha256": hashlib.sha256(path.read_bytes()).hexdigest()})
target.write_text(json.dumps({"checks": len(audits), "status": "PASS", "logs": audits}, indent=2) + "\n")
print("END case=owned-installation-path-audit checks=87 status=PASS")
