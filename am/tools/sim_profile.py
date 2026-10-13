#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Summarise a refsim-soc run (stats.json + pcs.tsv) per function.

  sim_profile.py <run dir> <elf>

pcs.tsv charges every window cycle to the pc at lane 0 (head_cycles; the
last head while the queue is empty) and every lost issue slot to the reason
of the first lane that did not dispatch. Functions come from the ELF's
symbols; gba-next's translated code (the 16 MiB arena inside its `backend`
symbol, GBN_RV32_CODE_MIB=16) is one bucket.
Checks: retired and head cycles sum to the window totals.
"""
import bisect, csv, json, subprocess, sys
from collections import defaultdict
from pathlib import Path

run, elf = Path(sys.argv[1]), sys.argv[2]
st = json.load(open(run / 'stats.json'))
rows = list(csv.DictReader(open(run / 'pcs.tsv'), delimiter='\t'))
reasons = [k for k in rows[0] if k not in ('pc', 'retired', 'head_cycles')]

nm = subprocess.check_output(['riscv64-unknown-elf-nm', '-S', '-n', elf], text=True)
syms = []
for line in nm.splitlines():
    p = line.split()
    if len(p) == 4 and p[2] in 'tTbBdD' and int(p[1], 16) > 0:
        syms.append((int(p[0], 16), int(p[1], 16), p[3]))
backend = [s for s in syms if s[2].startswith('backend')]
assert len(backend) <= 1, backend
if backend:
    b0, bsz, _ = backend[0]
    tail = 1024 * 4 + 13 * 8
    code0 = b0 + (bsz - (16 << 20) - tail) // 16 * 16
else:
    code0 = 0  # no translator in this image
funcs = sorted((a, s, n) for a, s, n in syms if not n.startswith('backend'))
starts = [a for a, _, _ in funcs]

def owner(pc):
    if code0 and code0 <= pc < code0 + (16 << 20):
        return 'generated code (backend.code)'
    i = bisect.bisect_right(starts, pc) - 1
    if i >= 0 and pc < funcs[i][0] + funcs[i][1]:
        return funcs[i][2]
    return f'unknown {pc:08x}'

agg = defaultdict(lambda: defaultdict(int))
for r in rows:
    pc = int(r['pc'], 16)
    a = agg[owner(pc)]
    a['retired'] += int(r['retired'])
    a['head_cycles'] += int(r['head_cycles'])
    for k in reasons:
        a[k] += int(r[k])

tot_ret = sum(a['retired'] for a in agg.values())
tot_cyc = sum(a['head_cycles'] for a in agg.values())
cyc, ret = st['window_cycles'], st['window_retired']
print(f"window: {cyc:,} cycles, {ret:,} retired, IPC {ret / cyc:.4f}  (memory: {st['memory']})")
print(f"check: per-pc retired sum {tot_ret:,} {'OK' if tot_ret == ret else 'MISMATCH'}; "
      f"head cycles sum {tot_cyc:,} {'OK' if tot_cyc == cyc else 'MISMATCH'}")
w = st['issue_width']
print('issue width: ' + '  '.join(f'{i}:{100 * n / cyc:.2f}%' for i, n in enumerate(w)))
lost = st['lost_slots']
tl = sum(lost.values())
print(f"lost issue slots: {tl:,} of {4 * cyc:,} ({100 * tl / (4 * cyc):.2f}%)")
for k, v in sorted(lost.items(), key=lambda kv: -kv[1]):
    if v:
        print(f"  {k:16s} {v:>14,d} {100 * v / tl:6.2f}%   zero-issue cycles {st['zero_issue_cycles'][k]:>12,d}"
              f"  partial {st['partial_issue_cycles'][k]:>12,d}")
print(f"\n{'function':44s} {'cycles%':>8s} {'retired%':>8s} {'IPC':>6s}  top lost-slot reasons")
for name, a in sorted(agg.items(), key=lambda kv: -kv[1]['head_cycles'])[:30]:
    top = sorted(((a[k], k) for k in reasons), reverse=True)[:3]
    ipc = a['retired'] / a['head_cycles'] if a['head_cycles'] else 0
    print(f"{name[:44]:44s} {100 * a['head_cycles'] / cyc:7.2f}% {100 * a['retired'] / ret:7.2f}% {ipc:6.2f}  "
          + ', '.join(f'{k} {100 * v / max(1, sum(a[r] for r in reasons)):.0f}%' for v, k in top if v))
json.dump({k: dict(v) for k, v in agg.items()}, open(run / 'by-function.json', 'w'), indent=1)
