#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Compare bounded native and RV32 bare-metal runs using fresh save storage."""
import argparse
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[4]
REPORT = re.compile(r'^Frames: (\d+); video: (\d+)x(\d+); CRC32: ([0-9A-F]{8})$', re.M)
FIXTURES = (
    ('gb', 'cinema/gb/acid/dmg-acid2/test.gb'),
    ('gbc', 'cinema/gb/acid/cgb-acid2/test.gbc'),
    ('gba', 'cinema/gba/obj/2d-wrap/test.gba'),
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--make', default='make')
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--spike-prefix', type=Path, required=True)
    parser.add_argument('--cross', required=True)
    parser.add_argument('--newlib-root', type=Path, required=True)
    parser.add_argument('--isa', required=True)
    args = parser.parse_args()
    build = args.build_dir.resolve()
    logs = build / 'test-logs'
    logs.mkdir(parents=True, exist_ok=True)

    def run(command, name, timeout=180, expected=0):
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
                             + '\n'.join(text.splitlines()[-20:]))
        return text

    make = [args.make, '--no-print-directory', '-j4']
    native_dir = build / 'native'
    run([*make, 'PLATFORM=native', f'BUILD_DIR={native_dir}', 'all'], 'build-native', 300)
    spike = [str(args.spike_prefix / 'bin/spike'), f'--isa={args.isa}', '--priv=m',
             '-m0xa0000000:0x04000000', f'--extlib={build / "protosoc.so"}']
    cross_make = [*make, 'PLATFORM=spike', f'BUILD_DIR={build}', f'CROSS={args.cross}',
                  f'SPIKE_PREFIX={args.spike_prefix}', f'NEWLIB_ROOT={args.newlib_root}', 'HEADLESS=1']

    # A nonzero bare-metal return must reach the host through HTIF as a failure.
    failed = run([*spike, '--device=am_protosoc,0', str(build / 'runtime-test.elf')],
                 'runtime-failure', expected=1)
    if 'FAIL line' not in failed:
        raise SystemExit('Missing runtime failure diagnostic')
    print('PASS: bare-metal failure returns a nonzero host status')

    with tempfile.TemporaryDirectory(prefix='mgba-spike-test-') as directory:
        work = Path(directory)
        for name, source in FIXTURES:
            rom = work / Path(source).name
            shutil.copyfile(ROOT / source, rom)
            rom.with_suffix('.sav').unlink(missing_ok=True)
            native = run([str(native_dir / 'mgba'), '--headless', '--frames', '120', str(rom)],
                         f'native-{name}')
            run([*cross_make, f'ROM={rom}', 'FRAMES=120', 'all'], f'build-{name}', 300)
            start = time.monotonic()
            actual = run([*spike, '--device=am_protosoc,0', str(build / 'mgba.elf')], f'spike-{name}')
            expected_report, actual_report = REPORT.search(native), REPORT.search(actual)
            if not expected_report or not actual_report or expected_report.groups() != actual_report.groups():
                raise SystemExit(f'Frame mismatch for {name}; see {logs}')
            if actual_report.group(1) != '120':
                raise SystemExit(f'Incomplete frame run for {name}; see {logs}')
            print(f'PASS: {name.upper()} native/Spike match: {actual_report.group(0)} '
                  f'({time.monotonic() - start:.1f}s)')

        invalid = work / 'invalid.gba'
        invalid.write_bytes(b'not a valid ROM')
        run([*cross_make, f'ROM={invalid}', 'FRAMES=1', 'all'], 'build-invalid', 300)
        output = run([*spike, '--device=am_protosoc,0', str(build / 'mgba.elf')],
                     'invalid-rom', expected=1)
        if 'Could not initialize embedded ROM.' not in output:
            raise SystemExit('Missing invalid-ROM diagnostic')
        print('PASS: invalid embedded ROM exits cleanly')

    print(f'Logs: {logs}')


if __name__ == '__main__':
    main()
