#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Check profile accounting, including generated-code drops and old logs."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    with tempfile.TemporaryDirectory(prefix='mgba-profile-') as directory:
        work = Path(directory)
        source = work / 'sample.c'
        source.write_text('int sampled(void) { return 42; }\n')
        elf = work / 'sample.o'
        subprocess.run(['cc', '-c', str(source), '-o', str(elf)], check=True)
        header = ('Frames: 2; video: 240x160; CRC32: 12345678\n'
                  'Benchmark: warmup=1; frames=2; elapsed_us=10000; FPS=200.000\n'
                  'PC profile: samples=10; outside=6; cycles=1000; instret=1200; nominal_hz=500; bin_bytes=4\n'
                  'PC 00000000 4\n')
        tail = 'PC profile end\nAM exit: 0\n'
        generated = ('JIT profile: samples=5; dropped=1; bins=8192; max_probe=8\n'
                     'JIT 1 08000100 00000000 2\n'
                     'JIT 1 08000100 00000004 1\n'
                     'JIT 2 08000100 00000000 1\n')
        valid = header + generated + tail
        cases = {
            'legacy': (header + tail, True),
            'generated': (valid, True),
            'trace-helper': (valid.replace('JIT 2 08000100 00000000 1', 'JIT 8 00000000 0000000c 1'), True),
            'missing-metadata': (valid.replace(generated.splitlines()[0] + '\n', ''), False),
            'bad-total': (valid.replace('samples=5; dropped=1', 'samples=6; dropped=1'), False),
            'duplicate': (valid.replace('samples=5; dropped=1', 'samples=6; dropped=1')
                          .replace(tail, 'JIT 1 08000100 00000004 1\n' + tail), False),
            'bad-offset': (valid.replace('00000004 1', '00000002 1'), False),
            'incomplete': (valid.replace('AM exit: 0\n', ''), False),
            'frame-mismatch': (valid.replace('Frames: 2;', 'Frames: 1;'), False),
        }
        for name, (log, success) in cases.items():
            path = work / (name + '.log')
            out = work / (name + '.json')
            path.write_text(log)
            result = subprocess.run([sys.executable, str(ROOT / 'am/tools/profile_report.py'),
                                     '--elf', str(elf), '--log', str(path), '--out', str(out),
                                     '--nm', 'nm', '--platform', 'spike'], capture_output=True, text=True)
            assert (result.returncode == 0) == success, (name, result.stdout, result.stderr)
            if success:
                report = json.loads(out.read_text())
                assert report['functions'][0]['function'] == 'sampled', report
                assert report['functions'][0]['samples'] == 4
                assert report['platform'] == 'spike'
                if name in ('generated', 'trace-helper'):
                    assert report['generated_samples'] == 5 and report['generated_dropped'] == 1
                    assert report['outside_unknown'] == 1
                    assert report['generated_kinds'] == {'Thumb block': 3, 'ARM block' if name == 'generated' else 'Thumb loop helper': 1}
                    assert len(report['generated_blocks']) == 2 and len(report['generated_points']) == 3
                else:
                    assert report['outside_unknown'] == 6 and not report['generated_blocks']
            print('PASS: profile report', name)


if __name__ == '__main__':
    main()
