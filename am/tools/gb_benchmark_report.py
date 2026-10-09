#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Check a pinned Peanut-GB run against Native/Spike before reporting board FPS."""
import argparse
import hashlib
import json
from pathlib import Path
import re


def read_run(path, board=False, baremetal=False):
    text = path.read_text().replace('\r', '')
    config = re.search(r'^Peanut-GB: (.+)$', text, re.M)
    rom = re.search(r'^ROM: bytes=(\d+); CRC32=([0-9a-f]+)$', text, re.M)
    bench = re.search(r'^Benchmark: warmup=(\d+); frames=(\d+); elapsed_us=(\d+); FPS=([\d.]+)$', text, re.M)
    state = re.search(r'^Validation: lines=(\d+); framebuffer=([0-9a-f]+); wram=([0-9a-f]+); vram=([0-9a-f]+); pc=([0-9a-f]+)$', text, re.M)
    if not all((config, rom, bench, state)) or (baremetal and 'AM exit: 0\n' not in text):
        raise ValueError(f'{path}: incomplete benchmark')
    warmup, frames, elapsed = map(int, bench.groups()[:3])
    if not frames or not elapsed or int(state[1]) != frames * 144:
        raise ValueError(f'{path}: missing frames or scanlines')
    result = dict(log=str(path.resolve()), log_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                  config=config[1], rom_bytes=int(rom[1]), rom_crc32=rom[2], warmup=warmup, frames=frames,
                  validation=dict(zip(('lines', 'framebuffer', 'wram', 'vram', 'pc'), state.groups())))
    if board:
        hardware = re.search(r'^Hardware: hz=(\d+); itcm=(\d+); dtcm=(\d+)$', text, re.M)
        counters = re.search(r'^Counters: cycles=(\d+); instret=(\d+); sampling=0$', text, re.M)
        if not hardware or not counters or not int(hardware[1]):
            raise ValueError(f'{path}: missing hardware clock or unsampled counters')
        hz = int(hardware[1])
        cycles, retired = map(int, counters.groups())
        if not cycles or abs(cycles / hz * 1000000 - elapsed) > max(100, elapsed / 1000):
            raise ValueError(f'{path}: counter/timer discrepancy')
        result.update(clock_hz=hz, elapsed_us=elapsed, cycles=cycles, instret=retired,
                      cycles_per_frame=cycles / frames, instructions_per_frame=retired / frames,
                      ipc=retired / cycles, fps=frames * hz / cycles,
                      itcm_bytes=int(hardware[2]), dtcm_bytes=int(hardware[3]))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', type=Path, required=True)
    parser.add_argument('--spike', type=Path, required=True)
    parser.add_argument('--board', type=Path, nargs='+', required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    try:
        native = read_run(args.native)
        spike = read_run(args.spike, baremetal=True)
        boards = [read_run(p, board=True, baremetal=True) for p in args.board]
        keys = ('config', 'rom_bytes', 'rom_crc32', 'warmup', 'frames', 'validation')
        for run in [spike, *boards]:
            for key in keys:
                if run[key] != native[key]:
                    raise ValueError(f'{run["log"]}: {key} differs from Native')
    except ValueError as error:
        parser.error(str(error))
    result = dict(scope='Peanut-GB without APU or physical output; CPU, PPU and RGB565 conversion included. '
                        'Same fixture across hosts; not a new physical Pico measurement or a GBA speed prediction.',
                  native=native, spike=spike, boards=boards)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=2) + '\n')
    for run in boards:
        print(f'{run["fps"]:.3f} FPS, IPC {run["ipc"]:.3f}, '
              f'{run["instructions_per_frame"]:.0f} instructions/frame; Native/Spike state matches')


if __name__ == '__main__':
    main()
