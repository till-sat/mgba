#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Write an assembler ROM section; paths are quoted for the assembler, not a shell."""
import argparse
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--omit-ff-tail', action='store_true',
                        help='leave a trailing 0xff ROM region for the target RAM to initialize')
    args = parser.parse_args()

    source = args.source.resolve(strict=True)
    quoted = json.dumps(str(source))
    if args.omit_ff_tail:
        data = source.read_bytes()
        stored = max(1, len(data.rstrip(b'\xff'))) if data else 0
        assembly = ('.section .rom,"a",@progbits\n.balign 16\n'
                    '.globl _rom_start, _rom_tail_start, _rom_end\n_rom_start:\n'
                    f'.incbin {quoted}, 0, {stored}\n_rom_tail_start:\n'
                    '.section .rom_tail,"aw",@nobits\n'
                    f'.space {len(data) - stored}\n_rom_end:\n')
    else:
        assembly = ('.section .rodata.rom,"a",@progbits\n.balign 16\n'
                    '.globl _rom_start, _rom_end\n_rom_start:\n'
                    f'.incbin {quoted}\n_rom_end:\n')
    args.output.write_text(assembly)


if __name__ == '__main__':
    main()
