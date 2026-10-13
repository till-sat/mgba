#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Picture and sound of a GBN_CAPTURE=1 run, alone or against a reference.

  gba_next_capture.py DUMP OUTDIR [REFERENCE_DUMP]

DUMP is refsim-soc --dump of gbn_capture (struct GbnCapture in
src/platform/gba-next/benchmark.c) taken after the run (PLATFORM=sim does it). Writes OUTDIR/
shot-NNNN.png (every captured frame), audio.wav (the PCM window), and with a
reference: per-frame video/audio agreement, the first differing frame,
per-shot pixel differences, compare.png (reference | this | differences, every
shot) and reference.wav. Exit 1 if the dump is malformed.
"""
import struct, sys, wave
from pathlib import Path
import numpy as np
from PIL import Image

FRAMES, W, H = 4800, 240, 160


def load(path):
    raw = Path(path).read_bytes()
    magic, frames, shots, pcm, rate, every, pcm_first, pcm_frames = struct.unpack_from('<8I', raw, 0)
    if magic != 0x43415054:
        sys.exit(f'{path}: bad magic {magic:#x}')
    off = 32
    arr = lambda n, dt: np.frombuffer(raw, dtype=dt, count=n, offset=off)
    frame_crc = arr(FRAMES, '<u4'); off += 4 * FRAMES
    audio_crc = arr(FRAMES, '<u4'); off += 4 * FRAMES
    audio_count = arr(FRAMES, '<u4'); off += 4 * FRAMES
    pcm_cap = pcm_frames * 1200
    pcm_data = np.frombuffer(raw, dtype='<i2', count=2 * pcm_cap, offset=off).reshape(-1, 2); off += 4 * pcm_cap
    shot_cap = FRAMES // every
    shot = np.frombuffer(raw, dtype='<u2', count=shot_cap * W * H, offset=off).reshape(shot_cap, H, W)
    if off + 2 * shot_cap * W * H != len(raw):
        sys.exit(f'{path}: {len(raw)} bytes, layout needs {off + 2 * shot_cap * W * H}')
    return dict(frames=frames, shots=shots, pcm=pcm, rate=rate, every=every, pcm_first=pcm_first,
                frame_crc=frame_crc[:frames], audio_crc=audio_crc[:frames], audio_count=audio_count[:frames],
                pcm_data=pcm_data[:pcm], shot=shot[:shots])


def rgb(frame):
    """GBA BGR555 (red in bits 0..4) to 8-bit RGB."""
    r = (frame & 31).astype(np.uint8); g = ((frame >> 5) & 31).astype(np.uint8); b = ((frame >> 10) & 31).astype(np.uint8)
    return np.stack([(r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2)], axis=-1)


def write_wav(path, c):
    with wave.open(str(path), 'wb') as f:
        f.setnchannels(2); f.setsampwidth(2); f.setframerate(c['rate'])
        f.writeframes(c['pcm_data'].astype('<i2').tobytes())


def main():
    cap = load(sys.argv[1]); out = Path(sys.argv[2]); out.mkdir(parents=True, exist_ok=True)
    for i, s in enumerate(cap['shot']):
        Image.fromarray(rgb(s)).save(out / f'shot-{(i + 1) * cap["every"]:04d}.png')
    write_wav(out / 'audio.wav', cap)
    print(f"{sys.argv[1]}: {cap['frames']} frames, {cap['audio_count'].sum():,} samples at {cap['rate']} Hz, "
          f"{cap['shots']} shots, {cap['pcm']:,} PCM samples from frame {cap['pcm_first']}")
    if len(sys.argv) < 4:
        return
    ref = load(sys.argv[3])
    write_wav(out / 'reference.wav', ref)
    n = min(cap['frames'], ref['frames'])
    video = cap['frame_crc'][:n] == ref['frame_crc'][:n]
    audio = cap['audio_crc'][:n] == ref['audio_crc'][:n]
    first_v = int(np.argmin(video)) + 1 if not video.all() else None
    first_a = int(np.argmin(audio)) + 1 if not audio.all() else None
    print(f"frames compared {n}: video equal {int(video.sum())}, first differing frame {first_v}; "
          f"audio equal {int(audio.sum())}, first differing frame {first_a}")
    samples = (cap['audio_count'][:n].astype(np.int64) - ref['audio_count'][:n]).sum()
    print(f"audio samples: this {int(cap['audio_count'][:n].sum()):,}, reference {int(ref['audio_count'][:n].sum()):,} (diff {int(samples):+,})")
    k = min(cap['shots'], ref['shots'])
    rows = []
    for i in range(k):
        a, b = ref['shot'][i], cap['shot'][i]
        diff = a != b
        count = int(diff.sum())
        print(f"  frame {(i + 1) * cap['every']:4d}: {count:5d} pixels differ ({100 * count / diff.size:.3f}%)")
        d = np.zeros((H, W, 3), np.uint8); d[diff] = (255, 0, 255)
        rows.append(np.concatenate([rgb(a), np.full((H, 4, 3), 64, np.uint8), rgb(b),
                                    np.full((H, 4, 3), 64, np.uint8), d], axis=1))
    sheet = np.concatenate([np.concatenate([r, np.full((4, r.shape[1], 3), 64, np.uint8)]) for r in rows])
    Image.fromarray(sheet).save(out / 'compare.png')


if __name__ == '__main__':
    main()
