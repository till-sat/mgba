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

JIT_KINDS = {
    1: 'Thumb block', 2: 'ARM block', 3: 'Thumb dispatcher',
    4: 'ARM dispatcher', 5: 'Thumb branch helper', 6: 'ARM branch helper',
    7: 'memory helper', 8: 'Thumb loop helper',
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--elf', type=Path, required=True)
    parser.add_argument('--log', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--nm', default='riscv64-unknown-elf-nm')
    parser.add_argument('--platform', choices=('fpga', 'spike', 'verilator'), default='fpga')
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
    if not samples or not cycles or sum(count for _, count in points) + outside != samples:
        parser.error('empty or inconsistent PC histogram')
    if len({pc for pc, _ in points}) != len(points):
        parser.error('duplicate PC histogram entries')
    if int(frame[1]) != int(benchmark[2]):
        parser.error('frame count differs between benchmark and CRC report')
    jit_metadata = re.search(r'^JIT profile: samples=(\d+); dropped=(\d+); bins=(\d+); max_probe=(\d+)$', log, re.M)
    jit_points = [(int(kind), int(address, 16), int(offset, 16), int(count)) for kind, address, offset, count in
                  re.findall(r'^JIT (\d+) ([0-9a-f]+) ([0-9a-f]+) (\d+)$', log, re.M)]
    generated = dropped = 0
    if jit_metadata:
        generated, dropped, capacity, max_probe = map(int, jit_metadata.groups())
        if (generated > outside or sum(p[3] for p in jit_points) + dropped != generated or
                len(jit_points) > capacity or not max_probe or
                any(kind not in JIT_KINDS or offset % 4 or not count for kind, _, offset, count in jit_points)):
            parser.error('inconsistent JIT histogram')
        if len({p[:3] for p in jit_points}) != len(jit_points):
            parser.error('duplicate JIT histogram entries')
    elif jit_points:
        parser.error('JIT histogram has no metadata')
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
    block_counts = Counter()
    kind_counts = Counter()
    for kind, address, _, count in jit_points:
        block_counts[kind, address] += count
        kind_counts[JIT_KINDS[kind]] += count
    generated_blocks = [{'kind': JIT_KINDS[kind], 'address': f'0x{address:08x}',
                         'samples': count, 'percent': 100 * count / samples}
                        for (kind, address), count in block_counts.most_common()]
    result = {
        'elf': str(args.elf.resolve()),
        'elf_sha256': hashlib.sha256(args.elf.read_bytes()).hexdigest(),
        'log': str(args.log.resolve()),
        'log_sha256': hashlib.sha256(args.log.read_bytes()).hexdigest(),
        'method': 'jittered CLINT timer interrupts; mepc histogram; function self samples, not inclusive call stacks',
        'platform': args.platform,
        'timing_scope': ('functional simulation; cycles, IPC and FPS do not model hardware performance'
                         if args.platform == 'spike' else 'measured on ' + args.platform + '; includes sampling overhead'),
        'nominal_sample_hz': rate, 'bin_bytes': bin_bytes, 'samples': samples,
        'outside_text': outside, 'cycles_including_sampling': cycles,
        'generated_samples': generated, 'generated_dropped': dropped,
        'outside_unknown': outside - generated,
        'generated_kinds': dict(kind_counts.most_common()),
        'generated_blocks': generated_blocks,
        'generated_points': [{'kind': JIT_KINDS[kind], 'address': f'0x{address:08x}',
                              'native_offset': offset, 'samples': count}
                             for kind, address, offset, count in jit_points],
        'instructions_including_sampling': retired,
        'ipc_including_sampling': retired / cycles if cycles else None,
        'warmup': int(benchmark[1]), 'frames': int(benchmark[2]),
        'sampled_elapsed_us': int(benchmark[3]), 'sampled_fps_reported': benchmark[4],
        'frame_crc32': frame[3], 'functions': functions,
        'limitations': 'Sampling changes code layout and cache activity; interrupt delivery has skid. Percentages estimate time by current function, not cache-miss or branch-stall causes. Generated PCs are resolved at sample time; address means guest block start or shared memory helper index. Native offsets can combine recompiled versions and are not offsets in the final ELF. Dropped generated samples are excluded from block rankings, but included in the total.',
    }
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=2) + '\n')
    print(f'{samples} samples; outside text {outside}; IPC {result["ipc_including_sampling"]:.3f}; CRC {frame[3]}')
    print(result['timing_scope'])
    print(f'Generated: {generated}; dropped: {dropped}; unknown outside text: {outside - generated}')
    for row in functions[:25]:
        print(f'{row["percent"]:6.2f}% {row["samples"]:6d}  {row["function"]}')
    for row in generated_blocks[:25]:
        print(f'{row["percent"]:6.2f}% {row["samples"]:6d}  {row["kind"]} {row["address"]}')


if __name__ == '__main__':
    main()
