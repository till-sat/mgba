# Preserved 128-bit RVV PPU experiments

These source patches are retained at the user's request on 2026-10-12. Each applies independently to the accepted ARM V3 + HALT timeline V2 engine. They change the PPU only and can accompany subsequent CPU work. Apply one patch at a time in an isolated checkout; they are alternative implementations, not a stack. The active scalar PPU is unchanged.

- `pair.patch`: fused two-background RGB555 lookup table, RVV palette comparison and indexed lookup. 1,007,359 RV32 operator checks passed; full 4,620-frame video/PCM/CPU/memory replay passed. Ordinary Spike work: 358,583,099 instructions. Extra lookup storage is about 128.5 KiB.
- `packed.patch`: four pixels per 32-bit lane, direct live palette lookup. 4,758,523 RV32 checks passed with `vlenb=16`; ordinary replay passed. Full CRC replay was interrupted, not passed.
- `packed-swar.patch`: successor to `packed.patch` with a shared four-pixel opacity mask. The same 4,758,523 RV32 checks passed with `vlenb=16`; ordinary replay passed. Full CRC replay was not run. Ordinary Spike work: 359,281,535 instructions.

The ordinary FPGA ELFs for `pair` and `packed-swar` passed Spike functional preflight. Their actual FPGA game runs and FPGA operator proofs have not run. The board was powered down at the user's request before the scalar timed window was captured. There is no completed hardware FPS comparison.

Spike wall-time results did not establish a speedup; they do not establish FPGA performance. All variants preserve full rendering and audio. None changes VLEN, ISA, clock frequency, or the CPU translator.

The patches include implementation and focused proof sources. `manifest.json` records base and candidate source hashes. Full local evidence remains in the `build/gba-next/ppu-*-20261012/` directories and `build/gba-next/board-rvv-128-20261012/`. ROMs and generated binaries are not included here.

On a matching clean copy of the accepted source, use `git apply --check experiments/gba-next/rvv128/<variant>.patch` before applying the selected patch. Build with the existing `spike_zve32x` platform and `GBN_PPU_RVV=0`: the new operators have their own `AM_RVV` dispatch; the old RVV4 compositor flag is independent.
