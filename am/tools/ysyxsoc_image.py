#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Package a ysyxSoC FSBL/SSBL application, or a direct-Flash XIP image."""
import argparse
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app', type=Path, required=True)
    parser.add_argument('--boot', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--offset', type=lambda x: int(x, 0), default=0x4000)
    parser.add_argument('--limit', type=lambda x: int(x, 0), default=0x1000000)
    parser.add_argument('--xip', action='store_true')
    args = parser.parse_args()
    app = args.app.read_bytes()
    if args.xip:
        image = app
    else:
        if not args.boot or args.offset < 4 or args.offset % 4:
            parser.error('a boot image and an aligned positive offset are required')
        boot = args.boot.read_bytes()
        if len(boot) > args.offset:
            parser.error('bootloader overlaps the application size header')
        image = boot.ljust(args.offset, b'\0') + len(app).to_bytes(4, 'little') + app
    image += b'\0' * (-len(image) % 4)
    if len(image) > args.limit:
        parser.error(f'image is {len(image)} bytes, exceeds {args.limit}-byte Flash')
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(image)


if __name__ == '__main__':
    main()
