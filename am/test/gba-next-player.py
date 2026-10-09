#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Exercise the independent AM player through real SDL video, input and audio."""
import argparse
import json
import os
from pathlib import Path
import re
import shlex
import subprocess

from PIL import Image, ImageChops

ROOT = Path(__file__).resolve().parents[2]
FIXTURES = ROOT / 'src/platform/am/test'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--platform', choices=['spike', 'spike_zve32x'], default='spike')
    parser.add_argument('--video-driver', default='dummy')
    args = parser.parse_args()
    work = args.build_dir.resolve()
    work.mkdir(parents=True, exist_ok=True)
    (work / 'results.json').unlink(missing_ok=True)
    env = dict(os.environ, SDL_VIDEODRIVER=args.video_driver, SDL_AUDIODRIVER='dummy')
    checks = []

    def run(command, name, extra=None):
        log = work / f'{name}.log'
        with log.open('w') as output:
            result = subprocess.run(list(map(str, command)), cwd=ROOT,
                                    env=dict(env, **(extra or {})), stdout=output,
                                    stderr=subprocess.STDOUT, timeout=300)
        text = log.read_text()
        assert result.returncode == 0 and 'PLAYER_PROBE_ERROR' not in text, f'{name}: {text[-3000:]}'
        return text

    flags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'sdl2'], text=True))
    run(['cc', '-shared', '-fPIC', FIXTURES / 'interactive-probe.c', '-o', work / 'probe.so', *flags, '-ldl'], 'build-probe')
    source = (FIXTURES / 'save-audio.gba.s').read_text().replace('ldr r4, =38400', 'mov r4, #2')
    source = source.replace(' b loop\n', ''' mov r7, #0x06000000
 tst r1, #1
 ldreq r8, =0x001f001f
 ldrne r8, =0x03e003e0
 str r8, [r7]
 b loop
''')
    (work / 'input.s').write_text(source)
    run(['clang', '--target=arm-none-eabi', '-c', work / 'input.s', '-o', work / 'input.o'], 'assemble-input')
    run(['llvm-objcopy', '-O', 'binary', work / 'input.o', work / 'input.gba'], 'link-input')
    rom = bytearray((work / 'input.gba').read_bytes())
    rom.extend(bytes(32768 - len(rom)))
    rom[0xa0:0xac], rom[0xac:0xb0], rom[0xb2] = b'INTERACTIVE ', b'TST1', 0x96
    rom[0xbd] = (-sum(rom[0xa0:0xbd]) - 0x19) & 255
    (work / 'input.gba').write_bytes(rom)
    fixture = ROOT / 'cinema/gba/obj/2d-wrap/test.gba'
    for platform in ['native', args.platform]:
        build = work / platform
        common = ['make', '--no-print-directory', '-j6', f'PLATFORM={platform}',
                  f'BUILD_DIR={build}', f'GBN_RV32={int(platform != "native")}']
        for case, game, frames, audio in [('pixels', fixture, 2, 1),
                                         ('input', work / 'input.gba', 0, 1),
                                         ('silent', work / 'input.gba', 2, 0)]:
            name = f'{platform}-{case}'
            capture = work / name
            capture.mkdir(exist_ok=True)
            for old in capture.glob('frame-*.bmp'):
                old.unlink()
            settings = dict(LD_PRELOAD=str(work / 'probe.so'), AM_TEST_CAPTURE=str(capture))
            if case == 'input':
                settings['AM_TEST_INPUT'] = '1'
            options = [f'ROM={game}', f'GBN_PLAYER_FRAMES={frames}', f'GBN_PLAYER_AUDIO={audio}']
            run([*common, *options, 'gba-next-player', *([] if platform == 'native' else [str(build / 'protosoc.so')])], f'build-{name}')
            # Preload only the player/Spike, never make's configuration probes.
            recipe = run([*common, *options, '--just-print', 'run-gba-next-player'], f'command-{name}')
            command = shlex.split(recipe.splitlines()[-1])
            assert str(build / ('gba-next-player' if platform == 'native' else 'gba-next-player.elf')) in command
            text = run(command, name, settings)
            probe = re.search(r'PLAYER_PROBE frames=(\d+) audio_bytes=(\d+) audio_nonzero=(\d+)', text)
            # The player presents one initial black frame before emulation.
            assert probe and int(probe[1]) == (6 if case == 'input' else 3), text[-2000:]
            assert 'audio_dropped=0' in text, text[-2000:]
            assert ('window-closed' if case == 'input' else 'frame-limit') in text
            if case == 'input':
                assert int(probe[2]) > 0 and int(probe[3]) > 0, text[-2000:]
                for frame, color in [(3, (0, 255, 0)), (5, (255, 0, 0))]:
                    with Image.open(capture / f'frame-{frame:02}.bmp') as actual:
                        assert actual.convert('RGB').getpixel((0, 0)) == color, (name, frame)
            elif case == 'silent':
                assert int(probe[2]) == 0 and 'audio_submitted=0' in text
            else:
                with Image.open(capture / 'frame-03.bmp') as actual, Image.open(fixture.with_name('baseline_0000.png')) as reference:
                    expected = reference.convert('RGB').resize(actual.size, Image.Resampling.NEAREST)
                    assert actual.size == (720, 480)
                    assert ImageChops.difference(actual.convert('RGB'), expected).getbbox() is None, name
            checks.append(name)
            print(f'PASS: {name}', flush=True)
    for case, count in [('pixels', 3), ('input', 6), ('silent', 3)]:
        for frame in range(1, count + 1):
            name = f'frame-{frame:02}.bmp'
            with Image.open(work / f'native-{case}' / name) as a, Image.open(work / f'{args.platform}-{case}' / name) as b:
                assert ImageChops.difference(a.convert('RGB'), b.convert('RGB')).getbbox() is None, (case, frame)
    result = dict(status='PASS', platform=args.platform, video_driver=args.video_driver,
                  audio_driver='dummy', checks=checks, matching_frames=12)
    (work / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print('PASS: 12 displayed frames match native; input, PCM, frame limit and window close verified', flush=True)


if __name__ == '__main__':
    main()
