#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Build a pinned RV32 Newlib locally, without changing the system toolchain."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import urllib.request

VERSION = '4.6.0.20260123'
SHA256 = '6ff27e3bf022666f43f7802255be680eeff722ac181b1725d21e2e8318604ee3'
DEFAULT_MARCH = 'rv32im_zicsr_zifencei'
DEFAULT_MABI = 'ilp32'


def target_flags(march, mabi):
    return (f'-O2 -g0 -march={march} -mabi={mabi} -mstrict-align '
            '-mcmodel=medany -msmall-data-limit=0 -ffunction-sections -fdata-sections')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--cross', default='riscv64-unknown-elf-')
    parser.add_argument('--march', default=DEFAULT_MARCH)
    parser.add_argument('--mabi', default=DEFAULT_MABI)
    parser.add_argument('--jobs', type=int, default=8)
    args = parser.parse_args()
    flags = target_flags(args.march, args.mabi)
    root = args.root.resolve()
    compiler = shutil.which(args.cross + 'gcc')
    if not compiler:
        parser.error(f'{args.cross}gcc is required')
    identity = json.dumps([SHA256, flags, compiler,
                           subprocess.check_output([compiler, '--version'], text=True),
                           hashlib.sha256(Path(__file__).read_bytes()).hexdigest()])
    stamp = root / '.ready'
    prefix = root / 'install'
    if stamp.exists() and stamp.read_text() == identity and (prefix / 'riscv64-unknown-elf/lib/libc.a').exists():
        return
    download = root / 'download'
    download.mkdir(parents=True, exist_ok=True)
    archive = download / f'newlib-{VERSION}.tar.gz'
    if not archive.exists():
        print(f'Downloading Newlib {VERSION}', flush=True)
        temporary = archive.with_suffix('.tmp')
        with urllib.request.urlopen(f'https://sourceware.org/pub/newlib/{archive.name}', timeout=60) as source:
            with temporary.open('wb') as target:
                shutil.copyfileobj(source, target)
        temporary.replace(archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != SHA256:
        raise SystemExit(f'Newlib checksum mismatch: {archive}')
    source = root / f'newlib-{VERSION}'
    if not source.exists():
        with tarfile.open(archive) as bundle:
            bundle.extractall(root, filter='data')
    build = root / 'work'
    if build.exists():
        shutil.rmtree(build)
    build.mkdir()
    env = dict(os.environ, CFLAGS_FOR_TARGET=flags, CC_FOR_TARGET=compiler,
               AR_FOR_TARGET=args.cross + 'ar', RANLIB_FOR_TARGET=args.cross + 'ranlib')
    log_path = root / 'build.log'
    commands = [
        [str(source / 'configure'), '--target=riscv64-unknown-elf', f'--prefix={prefix}',
         '--disable-multilib', '--disable-nls', '--disable-libgloss',
         '--disable-newlib-supplied-syscalls', '--disable-newlib-multithread',
         '--disable-newlib-io-float',
         '--enable-newlib-io-long-long'],
        ['make', f'-j{max(1, args.jobs)}', 'all-target-newlib'],
        ['make', 'install-target-newlib'],
    ]
    print(f'Building Newlib for {args.march}/{args.mabi}; log: {log_path}', flush=True)
    with log_path.open('w') as log:
        for command in commands:
            result = subprocess.run(command, cwd=build, env=env, stdout=log, stderr=subprocess.STDOUT)
            if result.returncode:
                raise SystemExit(f'Newlib build failed; see {log_path}')
    stamp.write_text(identity)
    print(f'Newlib ready: {prefix}', flush=True)


if __name__ == '__main__':
    main()
