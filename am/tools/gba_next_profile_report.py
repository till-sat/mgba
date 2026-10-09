#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Resolve independent-core PC samples; never treat Spike time as board FPS."""
import argparse
from bisect import bisect_right
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import subprocess
from gba_next_benchmark_report import load


def native_mnemonic(signature, funct7):
    opcode, funct3 = signature & 0x7f, signature >> 12 & 7
    if opcode == 0x33:
        if funct7 == 1:
            return ('mul', 'mulh', 'mulhsu', 'mulhu', 'div', 'divu', 'rem', 'remu')[funct3]
        if funct7 == 0x20 and funct3 in (0, 5):
            return 'sub' if funct3 == 0 else 'sra'
        if funct7 == 0:
            return ('add', 'sll', 'slt', 'sltu', 'xor', 'srl', 'or', 'and')[funct3]
    elif opcode == 0x13:
        if funct3 == 5 and funct7 == 0x20:
            return 'srai'
        if funct7 == 0:
            return ('addi', 'slli', 'slti', 'sltiu', 'xori', 'srli', 'ori', 'andi')[funct3]
    elif opcode == 0x03:
        return {0: 'lb', 1: 'lh', 2: 'lw', 4: 'lbu', 5: 'lhu'}.get(funct3, 'unknown load')
    elif opcode == 0x23:
        return {0: 'sb', 1: 'sh', 2: 'sw'}.get(funct3, 'unknown store')
    elif opcode == 0x63:
        return {0: 'beq', 1: 'bne', 4: 'blt', 5: 'bge', 6: 'bltu', 7: 'bgeu'}.get(funct3, 'unknown branch')
    elif opcode in (0x17, 0x37, 0x67, 0x6f):
        return {0x17: 'auipc', 0x37: 'lui', 0x67: 'jalr', 0x6f: 'jal'}[opcode]
    elif opcode == 0x0f:
        return 'fence.i' if funct3 == 1 else 'fence'
    return f'opcode {signature:04x}/{funct7:02x}'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--elf', type=Path, required=True)
    parser.add_argument('--log', type=Path, required=True)
    parser.add_argument('--platform', choices=['spike', 'fpga'], required=True)
    parser.add_argument('--performance', action='store_true', help='profile a GBN_VALIDATE=0 run')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    text, _, frames, warmup = load(args.log, args.performance)
    match = re.search(r'^PC profile: samples=(\d+); outside=(\d+); cycles=(\d+); instret=(\d+); nominal_hz=(\d+); bin_bytes=4$', text, re.M)
    if not match or 'PC profile end\n' not in text or 'AM exit: 0\n' not in text:
        parser.error('incomplete sampled run')
    samples, outside, cycles, retired, hz = map(int, match.groups())
    if not re.search(r'^Counters: cycles=\d+; instret=\d+; sampling=1$', text, re.M):
        parser.error('expected a sampling=1 diagnostic run')
    points = [(int(pc, 16), int(n)) for pc, n in re.findall(r'^PC ([0-9a-f]+) (\d+)$', text, re.M)]
    jit = re.search(r'^JIT profile: samples=(\d+); dropped=(\d+); bins=\d+; max_probe=\d+$', text, re.M)
    native_points = [(int(kind), int(signature, 16), int(funct7, 16), int(n)) for kind, signature, funct7, n in
                     re.findall(r'^JIT (\d+) ([0-9a-f]+) ([0-9a-f]+) (\d+)$', text, re.M)]
    generated, dropped = map(int, jit.groups()) if jit else (0, 0)
    if not samples or sum(n for _, n in points) + outside != samples or len({pc for pc, _ in points}) != len(points):
        parser.error('inconsistent sample accounting')
    if (generated > outside or sum(n for _, _, _, n in native_points) + dropped != generated or
            len({(kind, signature, funct7) for kind, signature, funct7, _ in native_points}) != len(native_points) or
            any(kind not in (9, 10) or n <= 0 or (kind == 9 and (signature or funct7)) for kind, signature, funct7, n in native_points)):
        parser.error('inconsistent generated-code accounting')
    opcodes = Counter()
    for kind, signature, funct7, n in native_points:
        if kind == 10:
            opcodes[native_mnemonic(signature, funct7)] += n
    nm = subprocess.check_output(['riscv64-unknown-elf-nm', '-S', '-n', '--defined-only', str(args.elf)], text=True)
    symbols = []
    for line in nm.splitlines():
        m = re.fullmatch(r'([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+[tT]\s+(.+)', line)
        if m:
            symbols.append((int(m[1], 16), int(m[2], 16), m[3]))
    symbols.sort()
    starts = [s[0] for s in symbols]
    counts = Counter()
    # LTO inlines much of the CPU into main. Debug line information resolves
    # the innermost source function; otherwise keep the enclosing ELF symbol.
    resolved = subprocess.check_output(['riscv64-unknown-elf-addr2line', '-f', '-e', str(args.elf),
                                       *[hex(pc) for pc, _ in points]], text=True).splitlines() if points else []
    if len(resolved) != 2 * len(points):
        parser.error('incomplete source-address resolution')
    sources = 0
    for index, (pc, n) in enumerate(points):
        i = bisect_right(starts, pc) - 1
        name = symbols[i][2] if i >= 0 and pc < symbols[i][0] + symbols[i][1] else '<unresolved text>'
        if resolved[2 * index] != '??':
            name = resolved[2 * index]
            sources += n
        counts[name] += n
    counts['<generated RV32 code>'] += generated - dropped
    counts['<unresolved outside text>'] += outside - generated + dropped
    rows = [dict(function=name, samples=n, percent=n * 100 / samples) for name, n in counts.most_common() if n]
    result = dict(platform=args.platform, frames=frames, warmup=warmup, samples=samples,
                  validation_enabled=not args.performance,
                  generated_samples=generated, dropped=dropped, hz=hz, cycles=cycles, instret=retired,
                  method='jittered timer PC samples; innermost DWARF function when available, otherwise ELF symbol; sampling changes code layout',
                  source_resolved_samples=sources,
                  scope='instruction work in functional Spike' if args.platform == 'spike' else 'hardware time including sampling overhead',
                  elf_sha256=hashlib.sha256(args.elf.read_bytes()).hexdigest(),
                  log_sha256=hashlib.sha256(args.log.read_bytes()).hexdigest(), functions=rows,
                  native_opcodes=[dict(mnemonic=name, samples=n, percent_generated=n * 100 / generated,
                                       percent_total=n * 100 / samples) for name, n in opcodes.most_common()])
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    for row in rows[:25]:
        print(f'{row["percent"]:6.2f}% {row["samples"]:7d} {row["function"]}')
    if opcodes:
        print('Generated RV32 instruction classes (share of generated samples):')
        for name, n in opcodes.most_common():
            print(f'{n * 100 / generated:6.2f}% {n:7d} {name}')


if __name__ == '__main__':
    main()
