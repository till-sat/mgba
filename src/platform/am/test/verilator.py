#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Boot the GBA fixture through proto-soc RTL and compare its rendered pixels."""
import argparse
from pathlib import Path
import re
import shutil
import subprocess
import time
import zlib

from PIL import Image

ROOT = Path(__file__).resolve().parents[4]
FIXTURE = ROOT / 'cinema/gba/obj/2d-wrap'
REPORT = re.compile(r'^Frames: (\d+); video: (\d+)x(\d+); CRC32: ([0-9A-F]{8})$', re.M)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--make', default='make')
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--runner', type=Path, required=True)
    parser.add_argument('--protosoc', type=Path, required=True)
    parser.add_argument('--cross', required=True)
    parser.add_argument('--newlib-root', type=Path, required=True)
    parser.add_argument('--max-cycles', type=int, default=500000000)
    args = parser.parse_args()
    build = args.build_dir.resolve()
    logs = build / 'test-logs'
    logs.mkdir(parents=True, exist_ok=True)
    work = build / 'test-player'
    work.mkdir(parents=True, exist_ok=True)

    def run(command, name, expected=0, timeout=900):
        log = logs / f'{name}.log'
        with log.open('w') as output:
            try:
                result = subprocess.run(command, cwd=ROOT, stdout=output,
                                        stderr=subprocess.STDOUT, timeout=timeout)
            except subprocess.TimeoutExpired:
                raise SystemExit(f'Timeout: {name}; see {log}') from None
        text = log.read_text()
        if result.returncode != expected:
            raise SystemExit(f'Failed: {name} (exit {result.returncode}); see {log}\n'
                             + '\n'.join(text.splitlines()[-15:]))
        return text

    make = [args.make, '--no-print-directory', '-j4']
    cross_make = [*make, 'PLATFORM=verilator', f'BUILD_DIR={work}',
                  f'PROTOSOC={args.protosoc}', f'CROSS={args.cross}',
                  f'NEWLIB_ROOT={args.newlib_root}', 'HEADLESS=1', 'AUDIO=0', 'FRAMES=2']
    runner = [str(args.runner), str(work / 'mgba-flash.bin'),
              '-m', str(args.max_cycles), '-p', '10000000']

    # An invalid game must still boot the emulator and propagate failure via ebreak/a0.
    invalid = work / 'invalid.gba'
    invalid.write_bytes(b'not a valid ROM')
    run([*cross_make, f'ROM={invalid}', 'all'], 'build-invalid', timeout=300)
    output = run(runner, 'invalid-rom', expected=1)
    if ('App SDRAM' not in output or 'Could not initialize embedded ROM.' not in output
            or 'HIT BAD TRAP' not in output or 'a0 = 1,' not in output):
        raise SystemExit(f'Missing invalid-ROM/RTL failure diagnostic; see {logs}')
    print('PASS: invalid embedded ROM boots through SDRAM and returns failure to the host')

    rom = work / 'test.gba'
    shutil.copyfile(FIXTURE / 'test.gba', rom)
    rom.with_suffix('.sav').unlink(missing_ok=True)
    native_dir = build / 'native'
    run([*make, 'PLATFORM=native', f'BUILD_DIR={native_dir}', 'all'], 'build-native', timeout=300)
    native = run([str(native_dir / 'mgba'), '--headless', '--frames', '2', str(rom)], 'native-gba')
    run([*cross_make, f'ROM={rom}', 'all'], 'build-gba', timeout=300)

    timeout = run([str(args.runner), str(work / 'mgba-flash.bin'), '-m', '1', '-p', '0'],
                  'cycle-limit', expected=1)
    if '[soc] TIMEOUT' not in timeout:
        raise SystemExit(f'The cycle limit was not reported as a failure; see {logs}')
    print('PASS: exhausted RTL cycle limit returns failure')

    start = time.monotonic()
    actual = run(runner, 'rtl-gba')
    # Check both actual execution of the boot stages and successful RTL termination.
    markers = ['Boot ROM v1', 'Stage2 SRAM', 'App SDRAM', 'AM RISC-V/proto-soc:',
               'Frames: 2;', 'HIT GOOD TRAP']
    cursor = 0
    for marker in markers:
        index = actual.find(marker, cursor)
        if index < 0:
            raise SystemExit(f'Missing or out-of-order boot marker {marker!r}; see {logs}')
        cursor = index + len(marker)
    expected_report, actual_report = REPORT.search(native), REPORT.search(actual)
    if not expected_report or not actual_report or expected_report.groups() != actual_report.groups():
        raise SystemExit(f'Native/RTL frame mismatch; see {logs}')
    # Two frames include the fixture drawing, rather than only the first blank frame.
    with Image.open(FIXTURE / 'baseline_0000.png') as baseline:
        rgb = baseline.convert('RGB')
        expected = ('2', str(rgb.width), str(rgb.height), f'{zlib.crc32(rgb.tobytes()):08X}')
    if actual_report.groups() != expected:
        raise SystemExit(f'RTL/reference image mismatch: {actual_report.groups()} != {expected}')
    print(f'PASS: native/RTL/reference pixels match: {actual_report.group(0)} '
          f'({time.monotonic() - start:.1f}s)')
    print(f'Logs: {logs}')


if __name__ == '__main__':
    main()
