#!/usr/bin/env python3
"""Read only profiles of the owned fixture. Missing samples do not rule out BRK7."""
import argparse
import json
from pathlib import Path
import re

RUNTIME_SHA = '5d0e8f9a10093a8af1fbae31b59917be2ad1b248c4a71bf570982ba5966c41c4'
# These exclusive blocks are reached AFTER the pinned runtime checks BRK #7.
# Do not include its shared entry, the shared map lookup, or assertion routines.
RECOVERY_RANGES = ((0xb5d4, 0xb63c), (0xb9b8, 0xba30))


def review(path):
    text = path.read_text(errors='replace')
    headers, sections = {}, {}
    section = None
    for line in text.splitlines():
        if line.startswith('#') or not line.strip():
            continue
        if re.fullmatch(r'\[\w+\]', line):
            section = line[1:-1]
            sections[section] = []
        elif section:
            sections[section].append(line)
        else:
            key, _, value = line.partition(' ')
            headers[key] = value
    base = int(headers['runtime_base'], 0)
    hits = []
    for line in sections.get('host_leaves', []):
        tid, pc, count, reason = line.split()
        offset = int(pc, 0) - base
        if any(lo <= offset < hi for lo, hi in RECOVERY_RANGES):
            hits.append({'tid': tid, 'pc': pc, 'runtime_offset': hex(offset),
                         'samples': int(count), 'reason': reason})
    return {'path': str(path), 'headers': headers, 'recovery_region_hits': hits,
            'recovery_region_samples': sum(r['samples'] for r in hits),
            'interpretation': 'positive sampled execution; NOT exact event counts' if hits else
                              'not observed; sampling cannot exclude execution',
            'runtime_sha256_required': RUNTIME_SHA,
            'ranges': [[hex(lo), hex(hi)] for lo, hi in RECOVERY_RANGES],
            'coverage_limits': ['only the latched thread after discovery',
                                'transient paths can fall between samples',
                                'does not read the saved exception PC or private map variants',
                                'sampling affects timing; retain unsampled controls']}


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('profiles', type=Path, nargs='+')
    args = parser.parse_args()
    print(json.dumps([review(p) for p in args.profiles], indent=2))
