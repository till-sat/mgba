#!/usr/bin/env python3
"""Validate a version-1 FPGA PMU benchmark and summarize its observations."""
import argparse
import hashlib
import json
from pathlib import Path
import re


NAMES = (
    'cycles issue0 issue1 issue2 issue3 issue4 '
    'zero_flush zero_hold zero_lsu_window zero_empty zero_score zero_unit_busy zero_other '
    'partial_empty partial_decode_slot0 partial_control partial_lsu_group '
    'partial_mul_group partial_score partial_unit_busy partial_other '
    'ibus_wait dbus_wait conditional_redirect all_branch_redirect load_issue store_issue '
    'mdu_issue vector_hold'
).split()


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def parse(text, crc, warmup, frames, image_sha):
    text = text.replace('\r', '')
    required = [
        'App SDRAM (UART load, CRC verified)\n',
        'RAM loader: clock_hz=50000000\n',
        f'SHA256: {image_sha}\n', f'CRC32: {crc}\n', 'AM exit: 0\n',
        'PMU: version=1; diagnostic=1; partitions=PASS\n',
    ]
    for marker in required:
        if marker not in text:
            raise ValueError(f'Missing success evidence: {marker.strip()}')
    if 'AM exit: 1' in text or 'ERROR:' in text or 'partitions=FAIL' in text:
        raise ValueError('Log contains a failed run')
    rows = re.findall(r'^PMU (\d+) (\w+)=(\d+)$', text, re.M)
    if len(rows) != 29:
        raise ValueError(f'Expected 29 counters, got {len(rows)}')
    counts = {}
    for (number, name, count), (index, expected) in zip(rows, enumerate(NAMES, 3)):
        if int(number) != index or name != expected:
            raise ValueError(f'Counter map mismatch: {number} {name}')
        counts[name] = int(count)
    cycles = counts['cycles']
    if not cycles or any(value > cycles for value in counts.values()):
        raise ValueError('Invalid boolean-event counter range')
    widths = [counts[f'issue{n}'] for n in range(5)]
    if sum(widths) != cycles:
        raise ValueError('Issue width histogram does not cover all cycles')
    if sum(counts[name] for name in NAMES[6:13]) != widths[0]:
        raise ValueError('Zero-issue categories do not partition zero-issue cycles')
    if sum(counts[name] for name in NAMES[13:21]) != sum(widths[1:4]):
        raise ValueError('Partial-issue categories do not partition partial groups')
    if counts['conditional_redirect'] > counts['all_branch_redirect']:
        raise ValueError('Conditional redirects exceed all redirects')
    timing = re.search(r'^Benchmark: warmup=(\d+); frames=(\d+); elapsed_us=(\d+); FPS=([\d.]+)$', text, re.M)
    retired = re.search(r'^Counters: cycles=(\d+); instret=(\d+); sampling=0$', text, re.M)
    if not timing or not retired or tuple(map(int, timing.group(1, 2))) != (warmup, frames):
        raise ValueError('Missing or mismatched benchmark interval')
    core_cycles, instret = map(int, retired.groups())
    elapsed = int(timing[3])
    if not core_cycles or not elapsed:
        raise ValueError('Empty benchmark')
    return dict(
        counters=counts, percent_of_cycles={name: value * 100 / cycles for name, value in counts.items()},
        issued_per_cycle=sum(n * count for n, count in enumerate(widths)) / cycles,
        retired_per_cycle=instret / core_cycles,
        benchmark=dict(warmup=warmup, frames=frames, crc=crc, cycles=core_cycles, instret=instret,
                       elapsed_us=elapsed, diagnostic_fps=float(timing[4]), pmu_cycles=cycles),
        limitations=['Diagnostic ELF and bitstream; do not substitute its FPS for release timing.',
                     'Priority categories are exclusive observations, not proof of a unique causal bottleneck.',
                     'Bus waits and branch/unit events overlap categories; bus waits are not cache misses.',
                     'Issued instructions can be killed; retirement and PMU windows include different boundary instructions.'],
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    parser.add_argument('--image', required=True, type=Path)
    parser.add_argument('--bitstream', required=True, type=Path)
    parser.add_argument('--crc', required=True)
    parser.add_argument('--warmup', required=True, type=int)
    parser.add_argument('--frames', required=True, type=int)
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    result = parse(args.log.read_text(), args.crc, args.warmup, args.frames, sha(args.image))
    result['inputs'] = {key: dict(path=str(path.resolve()), sha256=sha(path))
                        for key, path in [('log', args.log), ('image', args.image), ('bitstream', args.bitstream)]}
    args.out.write_text(json.dumps(result, indent=2) + '\n')
    print(f"Retired IPC {result['retired_per_cycle']:.3f}; issued/cycle {result['issued_per_cycle']:.3f}")
    for name in NAMES[1:]:
        print(f"{name}: {result['counters'][name]} ({result['percent_of_cycles'][name]:.3f}% of PMU cycles)")


if __name__ == '__main__':
    main()
