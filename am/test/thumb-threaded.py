#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Compare the threaded Thumb loop with original single stepping; no ROM needed."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]

BLOCKS = [
    (0x08000000, (0x2001, 0x2102, 0x1840, 0x3001, 0x2804, 0x4249, 0xA201, 0xB081, 0x4008)),
    (0x08000040, (0x2001, 0x2102, 0x8011, 0x2203, 0x2304, 0x2405, 0x8813, 0x4770)),
    (0x08000080, (0x2001, 0x2102, 0xE000, 0x2203, 0x2304, 0x2405)),
    (0x080000C0, (0x2001, 0x2102, 0x4770)),
    (0x08000100, (0x2800, 0xD000, 0x2001, 0x2102, 0x2203, 0x2304, 0x2405)),
    (0x08000140, (0x2001, 0x2102, 0x2203, 0xE75B)),
    (0x08000180, (0x2001, 0x2102, 0x2203, 0xE7FB)),
    (0x080001FE, (0x2707,)),  # Final ROM halfword: prefetch wraps to the start.
]

HARNESS = r'''
#include <mgba/internal/arm/isa-thumb.h>
#include <mgba/internal/arm/isa-inlines.h>
#include <stdio.h>
#include <stdlib.h>

static uint32_t templateCode[128];

static void events(struct ARMCore* cpu) { cpu->nextEvent = INT32_MAX; }
static void region(struct ARMCore* cpu, uint32_t address) { (void) cpu; (void) address; }
static uint32_t load16(struct ARMCore* cpu, uint32_t address, int* cycles) {
    uint16_t value;
    LOAD_16(value, address & 510, cpu->memory.activeRegion);
    *cycles += 2;
    return value;
}
static void store16(struct ARMCore* cpu, uint32_t address, int16_t value, int* cycles) {
    STORE_16(value, address & 510, (uint32_t*) cpu->memory.activeRegion);
    *cycles += 2;
}

static void check_threaded(const struct ARMCore* initial, unsigned block, unsigned trial) {
    uint32_t code[128], referenceCode[128];
    memcpy(code, initial->memory.activeRegion, sizeof(code));
    memcpy(referenceCode, code, sizeof(code));
    struct ARMCore actual = *initial, expected = *initial;
    actual.memory.activeRegion = code;
    expected.memory.activeRegion = referenceCode;
    int deadline = initial->nextEvent;
    ARMRunThumbThreaded(&actual);
    while (expected.cycles < deadline && expected.executionMode == MODE_THUMB) {
        expected.nextEvent = deadline;
        ARMRun(&expected);
    }
    if (memcmp(&actual.regs, &expected.regs, sizeof(actual.regs)) ||
        memcmp(actual.prefetch, expected.prefetch, sizeof(actual.prefetch)) ||
        memcmp(code, referenceCode, sizeof(code)) || actual.cycles != expected.cycles ||
        actual.executionMode != expected.executionMode) {
        fprintf(stderr, "threaded block %u trial %u differs\n", block, trial);
        exit(1);
    }
}

static void check(unsigned block, uint32_t address, const uint16_t* words, unsigned length) {
    for (unsigned trial = 0; trial < 256; ++trial) {
        uint32_t code[128];
        memcpy(code, templateCode, sizeof(code));
        uint32_t key = address + ((trial / 96) * 0x02000000u);
        for (unsigned i = 0; i < length; ++i)
            STORE_16(words[i], (key + i * 2) & 510, code);
        struct ARMCore actual = {0};
        for (unsigned r = 0; r < 16; ++r) actual.gprs[r] = (trial * 0x31415927u) ^ r;
        actual.gprs[ARM_PC] = key + 2;
        actual.gprs[ARM_LR] = 0x080001A0u | (trial & 1);
        actual.gprs[2] = key + 8; /* STRH changes an instruction already prefetched. */
        actual.cpsr.packed = ((trial & 15) << 28) | 0xF3u;
        actual.executionMode = MODE_THUMB;
        actual.nextEvent = trial & 31;
        actual.memory.activeRegion = code;
        actual.memory.activeMask = 510;
        actual.memory.activeSeqCycles16 = 2;
        actual.memory.activeNonseqCycles16 = 4;
        actual.memory.setActiveRegion = region;
        actual.memory.load16 = load16;
        actual.memory.store16 = store16;
        actual.irqh.processEvents = events;
        actual.prefetch[0] = words[0];
        LOAD_16(actual.prefetch[1], (key + 2) & 510, code);
        /* Independently exercise stale prefetch and patched later instructions. */
        if ((trial & 0x60) == 0x20) actual.prefetch[1] = 0x2707;
        if ((trial & 0x60) == 0x40) {
            STORE_16(0x2707, (key + 6) & 510, code);
        }
        if ((trial & 0x60) == 0x60) actual.prefetch[0] = 0x2707;
        check_threaded(&actual, block, trial);
    }
}
'''


def main():
    with tempfile.TemporaryDirectory(prefix="mgba-thumb-threaded-") as directory:
        temporary = Path(directory)
        harness = temporary / "test.c"
        with harness.open("w") as output:
            output.write(HARNESS)
            output.write("int main(void) {\n")
            for index, (address, words) in enumerate(BLOCKS):
                literals = ", ".join(f"0x{word:04X}" for word in words)
                output.write(f"const uint16_t words{index}[] = {{{literals}}};\n")
                for position, word in enumerate(words):
                    output.write(f"STORE_16(0x{word:04X}, {(address + position * 2) & 510}, templateCode);\n")
            for index, (address, words) in enumerate(BLOCKS):
                output.write(f"check({index}, 0x{address:08X}, words{index}, {len(words)});\n")
            output.write('puts("PASS: threaded Thumb pipeline, event exits, patches, branches, mode changes and fetch wrapping"); return 0; }\n')
        binary = temporary / "test"
        subprocess.run(["cc", "-std=c11", "-fwrapv", "-O2", "-D_GNU_SOURCE",
                        "-DMGBA_RUNNER_THREADED", "-Iinclude", "-Isrc", "-include",
                        "mgba/flags.h", str(harness), "src/arm/arm.c",
                        "src/arm/isa-arm.c", "src/arm/isa-thumb.c", "-o", str(binary)],
                       cwd=ROOT, check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
