#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Verify fixed-work native/Spike/board logs and report target counters."""
import argparse
import hashlib
import json
from pathlib import Path
import re


def field(text, name):
    match = re.search(r'(?:^|[ ;])' + re.escape(name) + r'=([0-9]+)(?:[ ;\n/]|$)', text, re.M)
    if not match:
        raise ValueError(f'missing field {name}')
    return int(match[1])


def load(path, performance=False):
    text = path.read_text().replace('\r', '')
    prefixes = ('GBN benchmark:', 'GBN result:', 'GBN guest:', 'GBN video:', 'GBN audio:', 'GBN memory:', 'r0=', 'r4=', 'r8=', 'r12=')
    validation = []
    for prefix in prefixes:
        matches = [line for line in text.splitlines() if line.startswith(prefix)]
        if len(matches) != 1:
            raise ValueError(f'{path}: expected exactly one {prefix}')
        validation.append(matches[0])
    if 'PPU=on; APU=on; BIOS=builtin; save=RAM' not in validation[0]:
        raise ValueError(f'{path}: unexpected workload configuration')
    disabled = 'GBN validation: off; CRC=off; PCM_inspection=off; audio_sink=discard\n' in text
    if disabled != performance:
        raise ValueError(f'{path}: validation mode mismatch; use --performance only for GBN_VALIDATE=0 logs')
    frames, warmup = field(validation[0], 'frames'), field(validation[0], 'warmup')
    if field(validation[1], 'status') or field(validation[1], 'rendered') != warmup + frames:
        raise ValueError(f'{path}: incomplete workload')
    if performance and (validation[3] != 'GBN video: validation=off' or
                        not validation[4].startswith('GBN audio: validation=off; ') or
                        validation[5] != 'GBN memory: validation=off'):
        raise ValueError(f'{path}: unexpected performance output format')
    if field(validation[4], 'dropped') or not field(validation[4], 'produced' if performance else 'measured_samples'):
        raise ValueError(f'{path}: missing or dropped audio')
    cycles = field(validation[2], 'measured_cycles')
    # Without warmup, skip-BIOS starts on line 126 and includes an initial
    # partial scan before the first complete rendered frame. With warmup,
    # both interval boundaries are complete frames at the same LCD phase.
    expected = frames * 280896
    valid_cycles = expected <= cycles <= expected + 280896 if warmup == 0 else abs(cycles - expected) <= 256
    if not valid_cycles:
        raise ValueError(f'{path}: inconsistent guest time interval')
    return text, validation, frames, warmup


def describe(path):
    return {'path': str(path.resolve()), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}


def report(native, spike, board=None, require_audible=False, performance=False):
    if performance and require_audible:
        raise ValueError('audible sample validation requires GBN_VALIDATE=1')
    _, reference, frames, warmup = load(native, performance)
    if require_audible and field(reference[4], 'nonzero') == 0:
        raise ValueError('expected audible samples, found silence')
    result = {'frames': frames, 'warmup': warmup,
              'ppu': True, 'apu': True, 'physical_media': False,
              'validation_enabled': not performance,
              'validation': ('guest progress, produced/dropped audio counts and final CPU registers; no pixel, PCM or memory validation'
                             if performance else 'all complete-frame pixel stream CRC, raw PCM CRC/counts, CPU registers and memory CRCs'),
              'guest_instructions': field(reference[2], 'measured_instructions'),
              'guest_cycles': field(reference[2], 'measured_cycles'),
              'native': describe(native)}
    for name, path in [('spike', spike), ('board', board)]:
        if path is None:
            continue
        text, validation, _, _ = load(path, performance)
        if validation != reference:
            differences = [(a, b) for a, b in zip(reference, validation) if a != b]
            raise ValueError(f'{name}: workload/output differs from native: {differences}')
        if 'AM exit: 0\n' not in text:
            raise ValueError(f'{name}: no successful exit')
        counters = re.search(r'^Counters: cycles=(\d+); instret=(\d+); sampling=0$', text, re.M)
        if not counters or not int(counters[1]) or not int(counters[2]):
            raise ValueError(f'{name}: missing counters')
        cycles, retired = map(int, counters.groups())
        entry = {**describe(path), 'cycles': cycles, 'instret': retired,
                 'instret_per_frame': retired / frames,
                 ('native_state_matches' if performance else 'native_output_matches'): True}
        if result['guest_instructions']:
            entry['instret_per_guest_instruction'] = retired / result['guest_instructions']
        if name == 'spike':
            if 'Spike benchmark:' not in text or re.search(r'^Benchmark: .*FPS=', text, re.M):
                raise ValueError('Spike must not claim hardware FPS')
            entry['counter_meaning'] = 'functional instruction simulation; mcycle is not FPGA cycles'
        else:
            timing = re.search(r'^Benchmark: .*elapsed_us=(\d+); FPS=(\d+\.\d{3})$', text, re.M)
            if not timing or not int(timing[1]):
                raise ValueError('board: no elapsed time / FPS')
            clock = re.search(r'^RAM loader: clock_hz=(\d+)$', text, re.M)
            if not clock or not int(clock[1]):
                raise ValueError('board: no hardware timer frequency')
            entry.update(elapsed_us=int(timing[1]), fps=float(timing[2]), cycles_per_frame=cycles / frames,
                         ipc=retired / cycles, clock_hz=int(clock[1]))
            if abs(float(timing[2]) - frames * 1e6 / int(timing[1])) > 0.001:
                raise ValueError('board: inconsistent FPS')
            expected = int(timing[1]) * int(clock[1]) / 1e6
            if abs(cycles - expected) > max(10000, expected * 0.001):
                raise ValueError('board: cycle counter and elapsed timer disagree')
        result[name] = entry
    result['board_measured'] = board is not None
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', type=Path, required=True)
    parser.add_argument('--spike', type=Path, required=True)
    parser.add_argument('--board', type=Path)
    parser.add_argument('--require-audible', action='store_true')
    parser.add_argument('--performance', action='store_true', help='accept only GBN_VALIDATE=0 logs; no image/PCM validation claim')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    try:
        result = report(args.native, args.spike, args.board, args.require_audible, args.performance)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    text = json.dumps(result, indent=2, ensure_ascii=False) + '\n'
    if args.output:
        args.output.write_text(text)
    print(text, end='')


if __name__ == '__main__':
    main()
