#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Write an assembler ROM section; paths are quoted for the assembler, not a shell."""
import json
from pathlib import Path
import sys

source = Path(sys.argv[1]).resolve(strict=True)
output = Path(sys.argv[2])
output.write_text('.section .rodata.rom,"a",@progbits\n.balign 16\n'
                  '.globl _rom_start, _rom_end\n_rom_start:\n'
                  f'.incbin {json.dumps(str(source))}\n_rom_end:\n')
