#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Exercise the FPGA host runner against a UART peer on a pseudo-terminal.

Checks transfer bytes/CRCs, a successful report, rejection, failed application,
missing benchmark report, and a bounded wait. Does not validate physical UART.
"""
import argparse
import os
from pathlib import Path
import pty
import select
import struct
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--protosoc', type=Path, required=True)
    args = parser.parse_args()
    sys.path.insert(0, str(args.protosoc.resolve() / 'sw/tools'))
    from boot_image import APP_MAGIC, BOOT_MAGIC, unpack

    with tempfile.TemporaryDirectory(prefix='mgba-uart-test-') as directory:
        work = Path(directory)
        loader = bytes(range(256)) * 2 + b'loader'
        app = bytes(reversed(range(256))) * 3 + b'application'
        (work / 'loader.bin').write_bytes(loader)
        (work / 'app.bin').write_bytes(app)
        for case in ('success', 'custom-baud', 'zcu102-baud', 'bad-crc', 'bad-exit', 'missing-report', 'timeout'):
            baud = {'custom-baud': 625000, 'zcu102-baud': 1041667}.get(case, 500000)
            master, slave = pty.openpty()
            process = subprocess.Popen([
                sys.executable, str(ROOT / 'am/tools/run_fpga.py'),
                '--protosoc', str(args.protosoc), '--port', os.ttyname(slave),
                '--loader', str(work / 'loader.bin'), '--app', str(work / 'app.bin'),
                '--baud', str(baud),
                '--watch', '0.5', '--log', str(work / f'{case}.log'),
            ], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

            def receive(count):
                end = time.monotonic() + 5
                data = bytearray()
                while len(data) < count and time.monotonic() < end:
                    if select.select([master], [], [], 0.1)[0]:
                        data.extend(os.read(master, count - len(data)))
                assert len(data) == count, (case, count, len(data))
                return bytes(data)

            def payload(expected):
                data = bytearray()
                for offset in range(0, len(expected), 256):
                    data.extend(receive(min(256, len(expected) - offset)))
                    os.write(master, b'K')
                assert data == expected, case
                return data

            try:
                header = receive(32)
                os.write(master, b'K')
                unpack(header + payload(loader), BOOT_MAGIC)
                os.write(master, b'GRAM loader: clock_hz=40000000\r\nRAM READY\r\n')
                header = receive(32)
                assert struct.unpack('<I', receive(4))[0] == baud
                os.write(master, b'K')
                unpack(header + payload(app), APP_MAGIC)
                os.write(master, b'E' if case == 'bad-crc' else b'G')
                if case != 'bad-crc':
                    time.sleep(0.15)  # Match the physical baud-switch interval.
                    report = b'Benchmark: warmup=1; frames=2; elapsed_us=2000000; FPS=1.000\n'
                    if case in ('success', 'custom-baud', 'zcu102-baud'):
                        os.write(master, report + b'AM exit: 0\n')
                    elif case == 'bad-exit':
                        os.write(master, report + b'AM exit: 1\n')
                    elif case == 'missing-report':
                        os.write(master, b'AM exit: 0\n')
                output, _ = process.communicate(timeout=5)
                succeeded = case in ('success', 'custom-baud', 'zcu102-baud')
                assert process.returncode == (0 if succeeded else 1), (case, output)
                assert ('ERROR:' in output) != succeeded, (case, output)
                assert 'SHA256:' in (work / f'{case}.log').read_text()
                print(f'PASS: UART runner {case}')
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()
                os.close(master)
                os.close(slave)


if __name__ == '__main__':
    main()
