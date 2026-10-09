#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Compile a frontend key replay, never a ROM-specific execution shortcut."""
import argparse
from pathlib import Path
import re


def compile_input(path):
    records = []
    if path is not None:
        for number, line in enumerate(path.read_text().splitlines(), 1):
            line = line.split('#', 1)[0].strip()
            if not line:
                continue
            match = re.fullmatch(r'([0-9]+)\s+((?:0[xX])?[0-9a-fA-F]+)', line)
            if match is None:
                raise ValueError(f'{path}:{number}: expected decimal frame and hexadecimal keys')
            frame, keys = int(match[1]), int(match[2], 16)
            if frame > 0xffffffff or keys > 0x3ff or (records and frame < records[-1][0]) or len(records) == 4096:
                raise ValueError(f'{path}:{number}: invalid frame, keys, order, or record count')
            records.append((frame, keys))
    return ('/* Generated key replay. */\nstatic const struct Input inputs[] = {\n' +
            ''.join(f'\t{{{frame}u, 0x{keys:03x}u}},\n' for frame, keys in records) +
            '\t{0, 0} /* Sentinel, excluded from input_count. */\n};\n' +
            f'static const unsigned input_count = {len(records)}u;\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        text = compile_input(args.input)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    if not args.output.exists() or args.output.read_text() != text:
        args.output.write_text(text)


if __name__ == '__main__':
    main()
