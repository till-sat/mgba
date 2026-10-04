#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Resolve a proto-soc timer PC histogram against the exact sampled ELF."""
import argparse
from bisect import bisect_right
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--elf', type=Path, required=True)
    parser.add_argument('--log', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--nm', default='riscv64-unknown-elf-nm')
    args = parser.parse_args()
    log = args.log.read_text().replace('\r', '')
    metadata = re.search(r'^PC profile: samples=(\d+); outside=(\d+); cycles=(\d+); '
                         r'instret=(\d+); nominal_hz=(\d+); bin_bytes=(\d+)$', log, re.M)
    benchmark = re.search(r'^Benchmark: warmup=(\d+); frames=(\d+); elapsed_us=(\d+); FPS=([\d.]+)$', log, re.M)
    frame = re.search(r'^Frames: (\d+); video: (\d+x\d+); CRC32: ([0-9A-F]+)$', log, re.M)
    if not metadata or not benchmark or not frame or 'PC profile end\n' not in log or 'AM exit: 0\n' not in log:
        parser.error('log does not contain a complete successful sampled benchmark')
    samples, outside, cycles, retired, rate, bin_bytes = map(int, metadata.groups())
    points = [(int(pc, 16), int(count)) for pc, count in re.findall(r'^PC ([0-9a-f]+) (\d+)$', log, re.M)]
    if not samples or sum(count for _, count in points) + outside != samples:
        parser.error('empty or inconsistent PC histogram')
    if len({pc for pc, _ in points}) != len(points):
        parser.error('duplicate PC histogram entries')
    symbols = []
    nm = subprocess.check_output([args.nm, '-S', '-n', '--defined-only', str(args.elf)], text=True)
    for line in nm.splitlines():
        match = re.fullmatch(r'([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+[tT]\s+(.+)', line)
        if match:
            address, size, name = match.groups()
            symbols.append((int(address, 16), int(size, 16), name))
    symbols.sort()
    starts = [address for address, _, _ in symbols]
    counts = Counter()
    for pc, count in points:
        index = bisect_right(starts, pc) - 1
        if index >= 0 and pc < symbols[index][0] + symbols[index][1]:
            counts[symbols[index][2]] += count
        else:
            counts['<unresolved>'] += count
    functions = [{'function': name, 'samples': count, 'percent': 100 * count / samples}
                 for name, count in counts.most_common()]
    result = {
        'elf': str(args.elf.resolve()),
        'elf_sha256': hashlib.sha256(args.elf.read_bytes()).hexdigest(),
        'log': str(args.log.resolve()),
        'log_sha256': hashlib.sha256(args.log.read_bytes()).hexdigest(),
        'method': 'jittered CLINT timer interrupts; mepc histogram; function self samples, not inclusive call stacks',
        'nominal_sample_hz': rate, 'bin_bytes': bin_bytes, 'samples': samples,
        'outside_text': outside, 'cycles_including_sampling': cycles,
        'instructions_including_sampling': retired,
        'ipc_including_sampling': retired / cycles if cycles else None,
        'warmup': int(benchmark[1]), 'frames': int(benchmark[2]),
        'sampled_elapsed_us': int(benchmark[3]), 'sampled_fps_reported': benchmark[4],
        'frame_crc32': frame[3], 'functions': functions,
        'limitations': 'Sampling changes code layout and cache activity; interrupt delivery has skid. Percentages estimate time by current function, not cache-miss or branch-stall causes.',
    }
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=2) + '\n')
    print(f'{samples} samples; outside text {outside}; IPC {result["ipc_including_sampling"]:.3f}; CRC {frame[3]}')
    for row in functions[:25]:
        print(f'{row["percent"]:6.2f}% {row["samples"]:6d}  {row["function"]}')


if __name__ == '__main__':
    main()
