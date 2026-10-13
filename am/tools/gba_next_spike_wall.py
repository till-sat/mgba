#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Measure complete GBA frames against the host monotonic clock in Spike."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import selectors
import subprocess
import time

from gba_next_benchmark_report import load


def host_state(pid, cpu):
    state = {'load_average': os.getloadavg()}
    try:
        runtime, wait, slices = map(int, Path(f'/proc/{pid}/schedstat').read_text().split())
        state.update(cpu_runtime_ns=runtime, runqueue_wait_ns=wait, timeslices=slices)
    except (OSError, ValueError):
        pass
    try:
        state['cpu_frequency_khz'] = int(Path(
            f'/sys/devices/system/cpu/cpu{cpu}/cpufreq/scaling_cur_freq').read_text())
    except (OSError, ValueError):
        pass
    return state


def measure(command, log, cpu, timeout):
    command = ['taskset', '-c', str(cpu)] + command
    start = end = None
    states = {}
    pending = b''
    selector = selectors.DefaultSelector()
    deadline = time.monotonic() + timeout
    with log.open('wb') as output:
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=0)
        selector.register(process.stdout, selectors.EVENT_READ)
        try:
            while True:
                if time.monotonic() >= deadline:
                    raise ValueError('Spike timed out; no valid FPS')
                if not selector.select(min(1, max(0, deadline - time.monotonic()))):
                    continue
                data = os.read(process.stdout.fileno(), 65536)
                stamp = time.perf_counter_ns()
                if not data:
                    break
                output.write(data)
                output.flush()
                pending += data.replace(b'\r', b'')
                while b'\n' in pending:
                    line, pending = pending.split(b'\n', 1)
                    if line == b'GBN wall: begin':
                        if start is not None or end is not None:
                            raise ValueError('duplicate or unordered start marker')
                        start = stamp
                        states['start'] = host_state(process.pid, cpu)
                    elif line == b'GBN wall: end':
                        if start is None or end is not None:
                            raise ValueError('duplicate or unordered end marker')
                        end = stamp
                        states['end'] = host_state(process.pid, cpu)
                    elif line.startswith(b'GBN warmup:'):
                        print(line.decode('ascii', 'replace'), flush=True)
            status = process.wait(timeout=10)
        finally:
            selector.close()
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            process.stdout.close()
    if status or start is None or end is None or end <= start:
        raise ValueError('Spike failed or timing markers are missing/buffered; no valid FPS')
    return command, end - start, states


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cpu', type=int, default=1, help='pin Spike to this host CPU (default: 1)')
    parser.add_argument('--log', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--native', type=Path, help='optional matching GBN_VALIDATE=0 native log')
    parser.add_argument('--timeout', type=float, default=1800)
    parser.add_argument('command', nargs=argparse.REMAINDER, help='-- spike [options] benchmark.elf')
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ['--'] else args.command
    if not command or args.timeout <= 0 or args.cpu not in os.sched_getaffinity(0):
        parser.error('supply a Spike command, positive timeout and an allowed CPU')
    # Keep the ELF and run identity alongside the measured result.
    try:
        elf = Path(command[-1])
        paths = [elf.resolve(), args.log.resolve(), args.output.resolve()]
        if args.native:
            paths.append(args.native.resolve())
        if len(set(paths)) != len(paths):
            raise ValueError('ELF, log, output and reference must have distinct paths')
        elf_digest = hashlib.sha256(elf.read_bytes()).hexdigest()
        args.output.unlink(missing_ok=True)
        command, elapsed_ns, states = measure(command, args.log, args.cpu, args.timeout)
        text, records, frames, warmup = load(args.log, performance=True)
        counters = re.search(r'^Counters: cycles=(\d+); instret=(\d+); sampling=0$', text, re.M)
        if (not counters or 'AM exit: 0\n' not in text or 'Spike benchmark:' not in text
                or 'GBN RV32:' in text):
            raise ValueError('require successful Spike performance build with sampling/statistics disabled')
        if warmup == 0:
            raise ValueError('use positive warmup so both boundaries enclose complete frames')
        if args.native and records != load(args.native, performance=True)[1]:
            raise ValueError('guest records differ from native reference')
        retired = int(counters[2])
        if not retired:
            raise ValueError('missing retired instruction count')
        result = {
            'frames': frames, 'warmup': warmup, 'wall_elapsed_ns': elapsed_ns,
            'wall_FPS': frames * 1e9 / elapsed_ns,
            'spike_instret': retired, 'instructions_per_frame': retired / frames,
            'affinity_cpu': args.cpu, 'command': command,
            'host_state': states,
            'elf_sha256': elf_digest,
            'log_sha256': hashlib.sha256(args.log.read_bytes()).hexdigest(),
            'native_records_match': True if args.native else None,
            'native_sha256': hashlib.sha256(args.native.read_bytes()).hexdigest() if args.native else None,
            'timing': 'Host monotonic time between explicit begin/end line arrivals; excludes build, warmup and result reporting; includes small marker/output/scheduling overhead. Not FPGA timing.',
        }
        for field in ('cpu_runtime_ns', 'runqueue_wait_ns'):
            if all(field in states[key] for key in ('start', 'end')):
                result['host_' + field] = states['end'][field] - states['start'][field]
        args.output.write_text(json.dumps(result, indent=2) + '\n')
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        parser.error(str(error))
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
