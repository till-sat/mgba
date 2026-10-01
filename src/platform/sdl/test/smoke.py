#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Black-box tests for the SDL2 player; ROMs and saves live in a temporary directory."""

import os
from pathlib import Path
import re
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile

from PIL import Image, ImageChops

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]


def command(args, **kwargs):
    return subprocess.run(args, check=True, capture_output=True, text=True, **kwargs)


def make_gb_rom(out, color=False):
    """Draw stripes, play a tone, increment SRAM[0], and record buttons in SRAM[1:3]."""
    rom = bytearray(32768)
    reference = (ROOT / 'cinema/gb/acid/dmg-acid2/test.gb').read_bytes()
    rom[0x104:0x134] = reference[0x104:0x134]
    rom[0x100:0x104] = bytes.fromhex('00 c3 50 01')  # JP $0150
    rom[0x134:0x13d] = b'SMOKETEST'
    rom[0x143] = 0x80 if color else 0
    rom[0x147], rom[0x149] = 3, 2  # MBC1 + RAM + battery, 8 KiB RAM
    code, labels, fixups = bytearray(), {}, []

    def emit(hex_bytes):
        code.extend(bytes.fromhex(hex_bytes))

    def label(name):
        labels[name] = len(code) + 0x150

    def jump(opcode, name):
        emit(opcode)
        fixups.append((len(code), name))
        emit('00 00')

    def write(address, value):
        emit(f'3e {value:02x} ea {address & 255:02x} {address >> 8:02x}')

    emit('f3 31 fe ff')  # DI; LD SP,$fffe
    write(0xff40, 0)  # LCD off
    write(0x0000, 10)  # RAM enable
    emit('fa 00 a0 3c ea 00 a0')  # Increment persisted boot counter
    write(0xa001, 0)
    emit('21 00 98 01 00 04')  # HL = tile map; BC = 1024
    label('clear')
    emit('af 22 0b 78 b1')
    jump('c2', 'clear')
    emit('21 00 80 06 10')  # HL = tile zero; B = 16
    label('tile')
    emit('3e aa 22 05')
    jump('c2', 'tile')
    for address, value in [(0xff47, 0xe4), (0xff26, 0x80), (0xff24, 0x77),
                           (0xff25, 0x11), (0xff10, 0), (0xff11, 0x80),
                           (0xff12, 0xf0), (0xff13, 0), (0xff14, 0x84)]:
        write(address, value)
    if color:
        write(0xff68, 0x80)  # Palette zero: white, light gray, dark gray, black
        for value in (0xff, 0x7f, 0xb5, 0x56, 0x4a, 0x29, 0, 0):
            write(0xff69, value)
    write(0xff40, 0x91)
    label('loop')
    # Read A and Right. SRAM[2] is the current state; SRAM[1] accumulates presses.
    emit('3e 10 e0 00 f0 00 2f e6 01 47 3e 20 e0 00 f0 00 2f e6 01 87 b0')
    emit('ea 02 a0 47 fa 01 a0 b0 ea 01 a0')
    jump('c3', 'loop')
    for offset, name in fixups:
        code[offset:offset + 2] = struct.pack('<H', labels[name])
    rom[0x150:0x150 + len(code)] = code
    rom[0x14d] = (-sum(rom[0x134:0x14d]) - 25) & 255
    out.write_bytes(rom)
    out.with_suffix('.sav').write_bytes(bytes([0x40]) + bytes(8191))


def main():
    if len(sys.argv) != 2:
        sys.exit(f'Usage: {sys.argv[0]} /path/to/mgba')
    executable = Path(sys.argv[1]).resolve(strict=True)
    flags = shlex.split(command(['pkg-config', '--cflags', '--libs', 'sdl2']).stdout)
    with tempfile.TemporaryDirectory(prefix='mgba-player-test-') as directory:
        work = Path(directory)
        command(['cc', '-shared', '-fPIC', str(HERE / 'probe.c'), '-o',
                 str(work / 'probe.so'), *flags, '-ldl'])
        command(['clang', '--target=arm-none-eabi', '-c', str(HERE / 'save-audio.gba.s'),
                 '-o', str(work / 'gba.o')])
        command(['llvm-objcopy', '-O', 'binary', str(work / 'gba.o'), str(work / 'smoke.gba')])
        rom = bytearray((work / 'smoke.gba').read_bytes())
        rom.extend(bytes(32768 - len(rom)))
        rom[0xa0:0xac], rom[0xac:0xb0], rom[0xb2] = b'SMOKETEST   ', b'TST1', 0x96
        rom[0xbd] = (-sum(rom[0xa0:0xbd]) - 0x19) & 255
        (work / 'smoke-gba.gba').write_bytes(rom)
        (work / 'smoke-gba.sav').write_bytes(bytes([0x40]) + bytes(32767))
        make_gb_rom(work / 'smoke-gb.gb')
        make_gb_rom(work / 'smoke-gbc.gbc', color=True)

        env = dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy',
                   LD_PRELOAD=str(work / 'probe.so'))
        # Keep ASan first when checking a sanitizer build with our SDL interposer.
        if os.environ.get('MGBA_TEST_PRELOAD'):
            env['LD_PRELOAD'] = os.environ['MGBA_TEST_PRELOAD'] + ':' + env['LD_PRELOAD']

        def run(args, expected=0, **extra):
            result = subprocess.run([executable, *map(str, args)], env=dict(env, **extra),
                                    cwd=work, capture_output=True, text=True, timeout=10)
            assert result.returncode == expected, (args, result.returncode, result.stdout, result.stderr)
            assert 'PROBE_ERROR' not in result.stdout, result.stdout
            assert 'runtime error:' not in result.stderr, result.stderr
            return result

        for args, expected in [(['--help'], 0), ([], 1), (['one', 'two'], 1), (['missing.gb'], 1)]:
            run(args, expected)
        (work / 'invalid.gb').write_bytes(b'not a ROM')
        run(['invalid.gb'], 1)
        print('PASS: command-line arguments and invalid ROMs')

        for name, source, width, height in [
            ('acid.gb', 'cinema/gb/acid/dmg-acid2', 160, 144),
            ('acid.gbc', 'cinema/gb/acid/cgb-acid2', 160, 144),
            ('sprites.gba', 'cinema/gba/obj/2d-wrap', 240, 160),
        ]:
            shutil.copyfile(ROOT / source / ('test' + Path(name).suffix), work / name)
            result = run([name], MGBA_TEST_SCREEN=str(work / 'frame.bmp'))
            assert 'PROBE_RESIZABLE 1' in result.stdout, result.stdout
            with Image.open(work / 'frame.bmp') as actual, Image.open(ROOT / source / 'baseline_0000.png') as expected:
                assert actual.size == (width * 3, height * 3), (name, actual.size)
                scaled = expected.convert('RGB').resize(actual.size, Image.Resampling.NEAREST)
                assert ImageChops.difference(actual.convert('RGB'), scaled).getbbox() is None, name
            print(f'PASS: {name} matches reference pixels at 3x scale')

        for size, offset in [((900, 480), (90, 0)), ((720, 600), (0, 60))]:
            run(['sprites.gba'], MGBA_TEST_SCREEN=str(work / 'resized.bmp'),
                MGBA_TEST_RESIZE=f'{size[0]}x{size[1]}')
            with Image.open(work / 'resized.bmp') as actual, Image.open(ROOT / 'cinema/gba/obj/2d-wrap/baseline_0000.png') as reference:
                expected = Image.new('RGB', size, 'black')
                expected.paste(reference.convert('RGB').resize((720, 480), Image.Resampling.NEAREST), offset)
                assert actual.size == size, actual.size
                assert ImageChops.difference(actual.convert('RGB'), expected).getbbox() is None, size
            print(f'PASS: resize to {size} preserves aspect ratio and clears borders')

        for ext, keys, quit_env in [('gb', 3, {}), ('gbc', 3, {'MGBA_TEST_ESCAPE': '1'}),
                                    ('gba', 0x11, {'MGBA_TEST_CLOSE': '1'})]:
            name = f'smoke-{ext}.{ext}'
            for boot in (0x41, 0x42):
                result = run([name], **quit_env)
                audio = re.search(r'PROBE_AUDIO bytes=(\d+) nonzero=(\d+)', result.stdout)
                assert audio and int(audio[1]) > 10000 and int(audio[2]) > 10000, result.stdout
                saved = (work / name).with_suffix('.sav').read_bytes()
                assert saved[:3] == bytes([boot, keys, 0]), (name, saved[:3].hex())
            run([name], MGBA_TEST_KEYUP_ONLY='1')
            saved = (work / name).with_suffix('.sav').read_bytes()
            assert saved[:3] == bytes([0x43, 1, 0]), (name, saved[:3].hex())
            print(f'PASS: {ext} audio, input/release, focus release, battery save/reload, normal exit')

        for failure, setting in [('window', {'MGBA_TEST_FAIL': 'window'}),
                                  ('texture', {'MGBA_TEST_FAIL': 'texture'}),
                                  ('update', {'MGBA_TEST_FAIL': 'update'}),
                                  ('audio', {'SDL_AUDIODRIVER': 'no_such_driver'}),
                                  ('video', {'SDL_VIDEODRIVER': 'no_such_driver'})]:
            result = run(['smoke-gb.gb'], 1, **setting)
            assert 'Could not' in result.stderr, result.stderr
            print(f'PASS: {failure} failure exits cleanly')


if __name__ == '__main__':
    main()
