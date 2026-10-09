#!/usr/bin/env python3
"""Rebuild the checked-in independent guest BIOS byte array."""
from pathlib import Path
import argparse
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--check", action="store_true", help="verify the array without rewriting it")
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="gba-next-bios-") as directory:
    obj = Path(directory) / "bios.o"
    binary = Path(directory) / "bios.bin"
    subprocess.run(["clang", "--target=arm-none-eabi", "-mcpu=arm7tdmi", "-c",
                    str(root / "src/gba-next/bios.s"), "-o", str(obj)], check=True)
    relocs = subprocess.check_output(["llvm-readelf", "--relocs", str(obj)], text=True)
    if "There are no relocations in this file" not in relocs:
        raise SystemExit("BIOS must be fully resolved by the assembler:\n" + relocs)
    subprocess.run(["llvm-objcopy", "-O", "binary", "--only-section=.text", str(obj), str(binary)], check=True)
    data = binary.read_bytes()
    if len(data) > 16384 or data[0x20:0x24] != bytes.fromhex("f000f0e7"):
        raise SystemExit("Invalid BIOS size or unsupported-service trap position")
    text = "/* SPDX-License-Identifier: MPL-2.0 */\n/* Generated from bios.s by am/tools/gba_next_bios.py. */\n"
    text += "".join("\t" + ", ".join(f"0x{x:02x}" for x in data[i:i+12]) + ",\n" for i in range(0, len(data), 12))
    output = root / "src/gba-next/bios.inc"
    if args.check:
        if output.read_text() != text:
            raise SystemExit("bios.inc is stale; run make regen-gba-next-bios")
        print(f"Verified {len(data)} firmware bytes against bios.s")
    else:
        output.write_text(text)
        print(f"Generated {len(data)} firmware bytes (remaining BIOS space is zero-filled)")
