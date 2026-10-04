#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Run a volatile SDRAM benchmark on a board already waiting at BOOT UART."""
import argparse
import fcntl
import hashlib
import os
from pathlib import Path
import re
import select
import struct
import sys
import termios
import time


def set_baud(fd, baud):
    standard = getattr(termios, f'B{baud}', None)
    if standard is not None:
        settings = termios.tcgetattr(fd)
        settings[4] = settings[5] = standard
        termios.tcsetattr(fd, termios.TCSADRAIN, settings)
        return
    # Linux termios2 BOTHER, including 50 MHz / (16 * 3) on ZCU102.
    if not sys.platform.startswith('linux'):
        raise ValueError(f'{baud} baud requires Linux termios2')
    termios.tcdrain(fd)
    settings = bytearray(44)
    fcntl.ioctl(fd, 0x802c542a, settings)  # TCGETS2
    flags = struct.unpack_from('=I', settings, 8)[0]
    flags = (flags & ~0x100f) | 0x1000  # CBAUD -> BOTHER
    struct.pack_into('=I', settings, 8, flags)
    struct.pack_into('=II', settings, 36, baud, baud)
    fcntl.ioctl(fd, 0x402c542b, settings)  # TCSETS2
    fcntl.ioctl(fd, 0x802c542a, settings)  # Drivers may quantize custom rates.
    actual = struct.unpack_from('=II', settings, 36)
    if any(abs(rate - baud) > baud / 50 for rate in actual):
        raise ValueError(f'serial driver changed {baud} baud to {actual}; select a compatible rate')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--protosoc', type=Path, required=True)
    parser.add_argument('--port', required=True)
    parser.add_argument('--loader', type=Path, required=True)
    parser.add_argument('--app', type=Path, required=True)
    parser.add_argument('--baud', type=int, choices=(115200, 500000, 625000, 1041667), default=500000)
    parser.add_argument('--watch', type=float, default=1800)
    parser.add_argument('--log', type=Path, required=True)
    args = parser.parse_args()
    sys.path.insert(0, str(args.protosoc.resolve() / 'sw/tools'))
    from boot_image import APP_MAGIC, BOOT_MAGIC, image
    from uart_boot import Serial, send_chunks

    app = args.app.read_bytes()
    loader = args.loader.read_bytes()
    if not 0 < len(loader) <= 65504 or not 0 < len(app) <= 64 * 1024 * 1024:
        parser.error('loader or application exceeds RAM capacity')
    if args.watch <= 0:
        parser.error('--watch must be positive')
    application = image(app, APP_MAGIC, 0xa0000000)
    recovery = image(loader, BOOT_MAGIC, 0x0f000000)
    args.log.parent.mkdir(parents=True, exist_ok=True)
    port = None
    with args.log.open('w') as log:
        def report(message):
            print(message, flush=True)
            print(message, file=log, flush=True)

        def speed(baud):
            set_baud(port.fd, baud)

        try:
            report(f'Application: {args.app.resolve()} ({len(app)} bytes)')
            report(f'SHA256: {hashlib.sha256(app).hexdigest()}')
            port = Serial(args.port)
            fcntl.flock(port.fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            port.discard_input()
            port.write(recovery[:32])
            try:
                port.ack()
            except TimeoutError:
                raise TimeoutError('No Boot ROM response. Enter the board\'s UART recovery mode, then retry.') from None
            send_chunks(port, recovery[32:])
            banner = bytearray()
            deadline = time.monotonic() + 10
            while not banner.replace(b'\r', b'').endswith(b'RAM READY\n'):
                banner.extend(port.byte(deadline))
                if len(banner) > 4096:
                    raise ValueError('Unexpected RAM loader response')
            report(banner.decode(errors='replace').strip())
            port.write(application[:32] + struct.pack('<I', args.baud))
            port.ack()
            speed(args.baud)
            report(f'Loading SDRAM at {args.baud} baud; awaiting per-block acknowledgements')
            for offset in range(0, len(app), 256):
                port.write(app[offset:offset + 256])
                port.ack()
                if offset // (1024 * 1024) != (offset + 256) // (1024 * 1024):
                    report(f'Transferred {min(offset + 256, len(app))}/{len(app)} bytes')
            report('Transfer complete; board is verifying SDRAM CRC')
            if port.byte(time.monotonic() + 180) != b'G':
                raise ValueError('Board rejected the application CRC')
            speed(115200)
            deadline = time.monotonic() + args.watch
            captured = bytearray()
            while time.monotonic() < deadline:
                if not select.select([port.fd], [], [], 0.1)[0]:
                    continue
                block = os.read(port.fd, 4096)
                if not block:
                    raise OSError('serial disconnected')
                captured.extend(block)
                text = block.decode(errors='replace')
                print(text, end='', flush=True)
                log.write(text); log.flush()
                text = captured.decode(errors='replace').replace('\r', '')
                status = re.search(r'^AM exit: (\d+)\n', text, re.M)
                if status:
                    if status[1] != '0' or not re.search(r'^Benchmark: .*FPS=\d+\.\d{3}\n', text, re.M):
                        raise ValueError('Benchmark failed or did not report FPS')
                    return
            raise TimeoutError('Benchmark did not finish within --watch seconds')
        except (OSError, ValueError, TimeoutError) as exc:
            report(f'ERROR: {exc}')
            raise SystemExit(1) from None
        finally:
            if port:
                port.close()


if __name__ == '__main__':
    main()
