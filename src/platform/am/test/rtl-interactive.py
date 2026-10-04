#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Compare real SDL output and frame-driven game input across native and RV32."""
import argparse
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import time
from PIL import Image, ImageChops

ROOT = Path(__file__).resolve().parents[4]
HERE = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--platform', choices=['spike', 'verilator', 'ysyxsoc'], required=True)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--runner', type=Path)
    parser.add_argument('--spike-prefix', type=Path, default=Path('/home/tillsat/tools/spike-20feb9c2'))
    parser.add_argument('--video-driver', default=os.environ.get('SDL_VIDEODRIVER', 'x11' if os.environ.get('DISPLAY') else 'dummy'))
    args = parser.parse_args()
    build = args.build_dir.resolve()
    work = build / 'interactive-test'
    work.mkdir(parents=True, exist_ok=True)
    (work / 'results.json').unlink(missing_ok=True)
    logs = work / 'logs'
    logs.mkdir(exist_ok=True)
    env = dict(os.environ, SDL_VIDEODRIVER=args.video_driver, SDL_AUDIODRIVER='dummy')
    results = []

    # ysyxSoC's serial Flash boot plus six game frames can exceed 30 minutes
    # on the RTL model; the simulator also enforces its own cycle limit.
    def run(command, name, expected=0, extra=None, timeout=3600):
        start = time.monotonic()
        with (logs / f'{name}.log').open('w') as output:
            result = subprocess.run(list(map(str, command)), cwd=ROOT,
                                    env=dict(env, **(extra or {})), stdout=output,
                                    stderr=subprocess.STDOUT, timeout=timeout)
        text = (logs / f'{name}.log').read_text()
        if result.returncode != expected or 'PLAYER_PROBE_ERROR' in text:
            raise SystemExit(f'FAIL: {name}; see {logs / (name + ".log")}\n{text[-2500:]}')
        results.append(dict(name=name, seconds=round(time.monotonic() - start, 2), exit=result.returncode))
        return text

    def capture(name, input_events=False):
        directory = work / name
        directory.mkdir(exist_ok=True)
        for old in directory.glob('frame-*.bmp'):
            old.unlink()
        settings = dict(LD_PRELOAD=str(work / 'probe.so'), AM_TEST_CAPTURE=str(directory))
        if input_events:
            settings['AM_TEST_INPUT'] = '1'
        return settings

    def compare_frames(left, right, count):
        for name in (left, right):
            assert len(list((work / name).glob('frame-*.bmp'))) == count, name
        for n in range(1, count + 1):
            with Image.open(work / left / f'frame-{n:02}.bmp') as a, Image.open(work / right / f'frame-{n:02}.bmp') as b:
                assert a.size == b.size, (n, a.size, b.size)
                assert ImageChops.difference(a.convert('RGB'), b.convert('RGB')).getbbox() is None, (right, n)

    make = ['make', '--no-print-directory', '-j4']
    native_dir = work / 'native'
    run([*make, 'PLATFORM=native', f'BUILD_DIR={native_dir}', 'all'], 'build-native')
    native = native_dir / 'mgba'
    flags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'sdl2'], text=True))
    run(['cc', '-shared', '-fPIC', HERE / 'interactive-probe.c', '-o', work / 'probe.so', *flags, '-ldl'], 'build-probe')
    cross = [*make, f'PLATFORM={args.platform}', f'BUILD_DIR={work / "guest"}', 'HEADLESS=0', 'AUDIO=1']
    if args.platform == 'spike':
        guest = [args.spike_prefix / 'bin/spike', '--isa=rv32im_zicsr_zifencei_zicbom', '--priv=m',
                 '-m0xa0000000:0x04000000', f'--extlib={build / "protosoc.so"}',
                 '--device=am_protosoc,0', '--device=am_media', work / 'guest/mgba.elf']
    else:
        if not args.runner:
            parser.error('--runner is required for RTL')
        guest = [args.runner.resolve(), work / 'guest/mgba-flash.bin', '-m', '500000000', '-p', '50000000']

    fixture = ROOT / 'cinema/gba/obj/2d-wrap/test.gba'
    # Use a private copy so native cannot load a user's save file.
    rom = work / 'fixture.gba'
    rom.write_bytes(fixture.read_bytes())
    rom.with_suffix('.sav').unlink(missing_ok=True)
    run([native, '--frames', '2', rom], 'native-fixture', extra=capture('native-fixture'))
    run([*cross, 'FRAMES=2', f'ROM={rom}', 'all'], 'build-fixture')
    text = run(guest, 'guest-fixture', extra=capture('guest-fixture'))
    assert 'Frames: 2; video: 240x160; CRC32: C0EA6C09' in text, text[-1000:]
    compare_frames('native-fixture', 'guest-fixture', 2)
    with Image.open(work / 'guest-fixture/frame-02.bmp') as actual, Image.open(fixture.with_name('baseline_0000.png')) as reference:
        expected = reference.convert('RGB').resize(actual.size, Image.Resampling.NEAREST)
        assert actual.size == (720, 480)
        assert ImageChops.difference(actual.convert('RGB'), expected).getbbox() is None
    print(f'PASS: {args.platform} SDL pixels match native and reference; CRC C0EA6C09', flush=True)

    # A GBA program paints two pixels red while A is released, green while held.
    # This checks keyboard -> GPIO -> AM -> mGBA -> guest game -> screen end to end.
    source = (HERE / 'save-audio.gba.s').read_text().replace('ldr r4, =38400', 'mov r4, #2').replace(' b loop\n', ''' mov r7, #0x06000000
 tst r1, #1
 ldreq r8, =0x001f001f
 ldrne r8, =0x03e003e0
 str r8, [r7]
 b loop
''')
    (work / 'input.s').write_text(source)
    run(['clang', '--target=arm-none-eabi', '-c', work / 'input.s', '-o', work / 'input.o'], 'assemble-input')
    run(['llvm-objcopy', '-O', 'binary', work / 'input.o', work / 'input.gba'], 'link-input')
    image = bytearray((work / 'input.gba').read_bytes())
    image.extend(bytes(32768 - len(image)))
    image[0xa0:0xac], image[0xac:0xb0], image[0xb2] = b'INTERACTIVE ', b'TST1', 0x96
    image[0xbd] = (-sum(image[0xa0:0xbd]) - 0x19) & 255
    (work / 'input.gba').write_bytes(image)
    (work / 'input.sav').unlink(missing_ok=True)
    run([native, work / 'input.gba'], 'native-input', extra=capture('native-input', True))
    run([*cross, 'FRAMES=0', f'ROM={work / "input.gba"}', 'all'], 'build-input')
    text = run(guest, 'guest-input', extra=capture('guest-input', True))
    probe = re.search(r'PLAYER_PROBE frames=(\d+) audio_bytes=(\d+) audio_nonzero=(\d+)', text)
    assert probe and int(probe[1]) == 6 and int(probe[2]) > 0 and int(probe[3]) > 0, text[-1500:]
    if args.platform != 'spike':
        assert 'HIT GOOD TRAP' in text, text[-1000:]
    compare_frames('native-input', 'guest-input', 6)
    for frame, color in [(3, (0, 255, 0)), (5, (255, 0, 0))]:
        with Image.open(work / 'guest-input' / f'frame-{frame:02}.bmp') as actual:
            assert actual.convert('RGB').getpixel((0, 0)) == color, (frame, actual.getpixel((0, 0)))
    print(f'PASS: {args.platform} game responds to A press/release, emits nonzero audio, and exits on window close', flush=True)

    if args.platform != 'spike':
        limited = [*guest]
        limited[limited.index('-m') + 1] = '1'
        text = run(limited, 'cycle-limit', expected=1)
        assert 'TIMEOUT' in text
    invalid = work / 'invalid.gba'
    invalid.write_bytes(b'not a game')
    run([*cross, 'FRAMES=1', f'ROM={invalid}', 'all'], 'build-invalid')
    text = run(guest, 'invalid-rom', expected=1)
    assert 'Could not initialize embedded ROM.' in text
    print(f'PASS: {args.platform} failures propagate to the host', flush=True)
    (work / 'results.json').write_text(json.dumps(dict(platform=args.platform, status='PASS', video_driver=args.video_driver,
                                                    audio_driver='dummy', checks=results), indent=2) + '\n')
    print(f'Logs and captured frames: {work}', flush=True)


if __name__ == '__main__':
    main()
