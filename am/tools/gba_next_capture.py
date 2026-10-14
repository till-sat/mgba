#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Picture and sound of a GBN_CAPTURE=1 run, alone or against a reference.

  gba_next_capture.py DUMP OUTDIR [REFERENCE_DUMP]

DUMP is refsim-soc --dump of gbn_capture (struct GbnCapture in
src/platform/gba-next/benchmark.c) taken after the run (PLATFORM=sim does it). Writes OUTDIR/
shot-NNNN.png (every captured frame), audio.wav (the PCM window), and with a
reference: per-frame video agreement, sound agreement, per-shot pixel
differences, compare.png (reference | this | differences, every shot) and
reference.wav. Exit 1 if the dump is malformed.

Timed captures (magic CAP2) bin the sound by the samples' own times: CRCs per
slice of one frame's cycles, and a PCM window with the time of its first
sample, so two windows are compared sample by sample at equal times (also
across rates, the faster one taken at the slower one's sample times). Older
captures (CAPT) split the sound by when the frontend drained it, which moves
with how often the main loop runs: their per-frame audio and PCM index can
differ although the sound is the same; such comparisons say so.
"""
import struct, sys, wave
from pathlib import Path
import numpy as np
from PIL import Image

FRAMES, W, H = 4800, 240, 160


MAGIC_DRAINED, MAGIC_TIMED = 0x43415054, 0x43415032


def load(path):
    raw = Path(path).read_bytes()
    magic, frames, shots, pcm, rate, every, pcm_first, pcm_frames = struct.unpack_from('<8I', raw, 0)
    if magic not in (MAGIC_DRAINED, MAGIC_TIMED):
        sys.exit(f'{path}: bad magic {magic:#x}')
    timed = magic == MAGIC_TIMED
    slices, slice_cycles, pcm_when = struct.unpack_from('<3I', raw, 32) if timed else (frames, 0, 0)
    off = 48 if timed else 32
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
    return dict(timed=timed, frames=frames, shots=shots, pcm=pcm, rate=rate, every=every, pcm_first=pcm_first,
                slices=slices, slice=slice_cycles, pcm_when=pcm_when,
                frame_crc=frame_crc[:frames], audio_crc=audio_crc[:slices], audio_count=audio_count[:slices],
                pcm_data=pcm_data[:pcm], shot=shot[:shots])


def pcm_at_equal_times(cap, ref):
    """The two PCM windows at the sample times they share: (this, reference),
    the faster stream taken at the slower one's times; None if the sample
    grids do not meet (times not a whole number of samples apart)."""
    step_c, step_r = 16777216 // cap['rate'], 16777216 // ref['rate']
    step = max(step_c, step_r)
    start = max(cap['pcm_when'], ref['pcm_when'])
    if (start - cap['pcm_when']) % step_c or (start - ref['pcm_when']) % step_r or \
            (cap['pcm_when'] - ref['pcm_when']) % min(step_c, step_r):
        return None
    ic = (start - cap['pcm_when']) // step_c
    ir = (start - ref['pcm_when']) // step_r
    a = cap['pcm_data'][ic::step // step_c]
    b = ref['pcm_data'][ir::step // step_r]
    n = min(len(a), len(b))
    return a[:n], b[:n]


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
    window = f"cycle {cap['pcm_when']}" if cap['timed'] else f"frame {cap['pcm_first']}"
    print(f"{sys.argv[1]}: {cap['frames']} frames, {cap['audio_count'].sum():,} samples at {cap['rate']} Hz, "
          f"{cap['shots']} shots, {cap['pcm']:,} PCM samples from {window}")
    if len(sys.argv) < 4:
        return
    ref = load(sys.argv[3])
    write_wav(out / 'reference.wav', ref)
    n = min(cap['frames'], ref['frames'])
    video = cap['frame_crc'][:n] == ref['frame_crc'][:n]
    first_v = int(np.argmin(video)) + 1 if not video.all() else None
    timed = cap['timed'] and ref['timed']
    if timed and cap['rate'] == ref['rate']:
        m = min(cap['slices'], ref['slices'])
        audio = cap['audio_crc'][:m] == ref['audio_crc'][:m]
        first_a = int(np.argmin(audio)) if not audio.all() else None
        sound = f"audio equal {int(audio.sum())} of {m} slices, first differing slice {first_a}"
    elif timed:
        sound = f"audio rates differ ({cap['rate']} vs {ref['rate']} Hz): see the PCM window"
    else:
        m = n
        audio = cap['audio_crc'][:n] == ref['audio_crc'][:n]
        first_a = int(np.argmin(audio)) + 1 if not audio.all() else None
        sound = f"audio equal {int(audio.sum())}, first differing frame {first_a} (split by drain time)"
    print(f"frames compared {n}: video equal {int(video.sum())}, first differing frame {first_v}; {sound}")
    if cap['rate'] == ref['rate']:
        m = min(len(cap['audio_count']), len(ref['audio_count']))
        samples = (cap['audio_count'][:m].astype(np.int64) - ref['audio_count'][:m]).sum()
        print(f"audio samples: this {int(cap['audio_count'][:m].sum()):,}, reference {int(ref['audio_count'][:m].sum()):,} (diff {int(samples):+,})")
    if timed:
        pair = pcm_at_equal_times(cap, ref)
        if pair is None:
            print(f"PCM window: sample times do not meet (first at cycle {cap['pcm_when']} vs {ref['pcm_when']})")
        else:
            a, b = pair
            d = int((a != b).any(axis=1).sum())
            print(f"PCM window at equal times: {len(a):,} samples, {d:,} differ"
                  f"{', max difference ' + str(int(np.abs(a.astype(int) - b).max())) if d else ''}")
    else:
        print("PCM window: not compared (an older capture has no sample times)")
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
