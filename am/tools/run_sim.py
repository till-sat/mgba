#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Run an AM image on refsim-soc --chip (PLATFORM=sim), here or on a server.

  run_sim.py --elf APP.elf --out DIR [--refsim PATH] [--host HOST --remote-root
             DIR --cpu N] [--warmup N | --begin TEXT] [--end TEXT] [--stop TEXT]
             [--profile] [--reference CAPTURE.bin] [-- REFSIM_OPTIONS...]

The statistics window opens at --begin (default: the gba-next benchmark's
"GBN warmup: N/N" line when N is a multiple of 100, which is when it prints
one, else its first line) and closes at --end ("GBN result:"). An image with a
gbn_capture symbol (GBN_CAPTURE=1) instead runs to its result line, where the
model dumps the capture; am/tools/gba_next_capture.py decodes it into DIR/
capture (and compares it with --reference). DIR receives stats.json, pcs.tsv,
console.log and the summary. With --host the ELF is copied to
REMOTE_ROOT/build/sim-runs/NAME on that host and run there by the mirror's
build/chiprun.sh (its tool image, CPU --cpu); the outputs are copied back.
Exit status 1 if the model fails, the window does not close, or the program
exits non-zero.
"""
import argparse, json, re, shlex, subprocess, sys, time
from pathlib import Path

TOOLS = Path(__file__).resolve().parent


def symbol(elf, name):
    out = subprocess.check_output(['riscv64-unknown-elf-nm', '-S', str(elf)], text=True)
    for line in out.splitlines():
        p = line.split()
        if len(p) == 4 and p[3] == name:
            return int(p[0], 16), int(p[1], 16)
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--elf', required=True, type=Path)
    ap.add_argument('--out', required=True, type=Path)
    ap.add_argument('--refsim', default='/home/tillsat/proto-core/quad-issue-rvv/sim/soc/refsim-soc')
    ap.add_argument('--host')
    ap.add_argument('--remote-root', default='proto-core/quad-issue-rvv-wide-peak')
    ap.add_argument('--remote-home', default='/home/tillsat', help='the home directory inside the tool image')
    ap.add_argument('--cpu', default='300')
    ap.add_argument('--warmup', type=int)
    ap.add_argument('--begin')
    ap.add_argument('--end', default='GBN result:')
    ap.add_argument('--stop', default='AM exit:')
    ap.add_argument('--profile', action='store_true')
    ap.add_argument('--reference', type=Path)
    ap.add_argument('extra', nargs='*', help='refsim-soc options after --')
    a = ap.parse_args()

    elf = a.elf.resolve()
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    capture = symbol(elf, 'gbn_capture')
    if capture:
        begin = 'GBN result:'
    elif a.begin:
        begin = a.begin
    elif a.warmup and a.warmup % 100 == 0:
        begin = f'GBN warmup: {a.warmup}/{a.warmup}'
    else:
        begin = 'GBN benchmark:'
    opts = ['--chip', '--begin', begin, '--end', a.end, '--stop', a.stop]
    if capture:
        opts += ['--dump', f'{capture[0]:#x}:{capture[1]}:capture']
    else:
        opts += ['--pcs', 'pcs.tsv']
    opts += ['--stats', 'stats.json', '--log', 'console.log'] + a.extra
    print(f"run_sim: {elf.name}, window from {begin!r} to {a.end!r}"
          f"{', capture run' if capture else ''}{', on ' + a.host if a.host else ''}", flush=True)

    fetch = ['stats.json', 'console.log'] + (['capture.open'] if capture else ['pcs.tsv'])
    for f in fetch:
        (out / f).unlink(missing_ok=True)
    if a.host:
        name = f"{out.name}-{time.strftime('%Y%m%d-%H%M%S')}"
        rel = f"{a.remote_root}/build/sim-runs/{name}"
        subprocess.run(['ssh', '-o', 'BatchMode=yes', a.host, f'mkdir -p {shlex.quote(rel)}'], check=True)
        subprocess.run(['scp', '-q', str(elf), f'{a.host}:{rel}/app.elf'], check=True)
        refsim = f"{a.remote_home}/{a.remote_root}/sim/soc/refsim-soc"
        inner = f"cd build/sim-runs/{name} && " + ' '.join(shlex.quote(x) for x in [refsim, 'app.elf'] + opts)
        remote = (f"cd {shlex.quote(a.remote_root)} && MEM=4g build/chiprun.sh sim-{name} {shlex.quote(a.cpu)} "
                  + shlex.quote(inner))
        status = subprocess.run(['ssh', '-o', 'BatchMode=yes', a.host, remote]).returncode
        for f in fetch:
            subprocess.run(['scp', '-q', f'{a.host}:{rel}/{f}', str(out / f)])
        print(f"run_sim: remote outputs kept in {a.host}:{rel}")
    else:
        status = subprocess.run([a.refsim, str(elf)] + opts, cwd=out).returncode

    ok = status == 0
    console = (out / 'console.log').read_text(errors='replace') if (out / 'console.log').exists() else ''
    exit_match = re.search(r'AM exit: (-?\d+)', console)
    if not exit_match or exit_match.group(1) != '0':
        print(f"run_sim: program did not exit cleanly ({exit_match.group(0) if exit_match else 'no AM exit line'})")
        ok = False
    summary = []
    stats = json.loads((out / 'stats.json').read_text()) if (out / 'stats.json').exists() else None
    if stats and not capture:
        if not stats.get('window_closed'):
            print('run_sim: the statistics window did not close'); ok = False
        c, r = stats['window_cycles'], stats['window_retired']
        summary.append(f"window: {c:,} cycles, {r:,} retired, IPC {r / c:.3f}" if c else 'window: empty')
    for pattern in (r'Counters: .*', r'Benchmark: .*', r'GBN result: .*'):
        m = re.search(pattern, console)
        if m:
            summary.append(m.group(0))
    if capture:
        cap = out / 'capture.bin'
        if (out / 'capture.open').exists():
            (out / 'capture.open').replace(cap)
            cmd = [sys.executable, str(TOOLS / 'gba_next_capture.py'), str(cap), str(out / 'capture')]
            if a.reference:
                cmd.append(str(a.reference))
            if subprocess.run(cmd).returncode:
                ok = False
        else:
            print('run_sim: no capture dump'); ok = False
    elif a.profile and (out / 'pcs.tsv').exists():
        prof = subprocess.run([sys.executable, str(TOOLS / 'sim_profile.py'), str(out), str(elf)],
                              capture_output=True, text=True)
        (out / 'profile.txt').write_text(prof.stdout + prof.stderr)
        summary += prof.stdout.splitlines()[:24]
    (out / 'summary.txt').write_text('\n'.join(summary) + '\n')
    print('\n'.join(summary))
    print(f"run_sim: {'ok' if ok else 'FAILED'}; outputs in {out}")
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
