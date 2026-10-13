# mGBA AM validation player

This repository builds a small mGBA player on top of the AM interface in `am/`.
It validates the same emulator and software devices in three environments:

| Path | CPU that executes mGBA | Device path | Purpose |
| --- | --- | --- | --- |
| Native | Linux host | SDL2 | Fast reference and interactive debugging |
| Spike | RV32IM in Spike | Simulated AM devices -> SDL2 | Check the RV32 software and device contract |
| Own core + ysyxSoC | The selected core in a ysyxSoC Verilator wrapper | ysyxSoC GPIO + simulation media -> SDL2 | Validate a CPU/SoC integration |

The media window at `0x40000000` is a simulation-only device: it presents pixels
and captures PCM on the host. It does not represent physical display or audio
hardware. All mGBA instructions, game emulation, software rendering and audio
generation run on the selected CPU.

These paths validate functionality; ysyxSoC Verilator wall-clock time is not
game real-time performance.

For the Chinese version, see [README_zh.md](README_zh.md).

## rv32 branch: independent core window

```sh
make -j6 PLATFORM=spike GBN_RV32=1 run-gba-next
```

This runs the independent core, RV32 native backend and current PPU with a window,
keyboard input and audio. The default ROM is `roms/dragonball.gba`; use `ROM=go`
to select another game. `PLATFORM=spike_zve32x` is also supported.
Controls: arrows, Z/X for A/B, A/S for L/R, Enter for Start, Backspace for Select,
and Escape or window close to quit. `GBN_PLAYER_AUDIO=0` disables audio output;
`GBN_PLAYER_FRAMES=N` limits complete frames (default 0 runs until closed).
Saves currently live in RAM and are discarded on exit.

`run-gba-next-bench` remains the headless fixed-work benchmark. The `run` commands
below use the original mGBA player. See [doc/gba-next.md](doc/gba-next.md) for the
independent core's implementation and validation status.

## Minimal commands

Each command builds the player when needed and starts the selected interactive
path. Native and Spike default to `roms/dragonball.gba`. ysyxsoc defaults to
the bundled 4 KiB test ROM `cinema/gba/obj/2d-wrap/test.gba`, which displays white
and green blocks and reduces Flash boot loading time. These defaults are defined
in the root `Makefile`. Set `ROM=game-name` to load `roms/game-name.gba`; the
`.gba` suffix may be omitted. GB/GBC ROMs can include their suffix, for example
`ROM=game.gb`. Explicit paths remain accepted for temporary test fixtures.

### Native + SDL

```sh
make PLATFORM=native run
```

### Spike + simulated devices + SDL

```sh
make PLATFORM=spike run
```

Spike needs an RV32IM/ILP32 `riscv64-unknown-elf-` toolchain, Spike development
headers and library, a C++20 compiler, Python 3, pkg-config and SDL2 development
files. The first cross build prepares the pinned Newlib copy under
`build/newlib/`. The default `SPIKE_PREFIX` is defined in
`am/platform/spike.mk`; override it on the Make command line when your Spike
installation is elsewhere:

```sh
make PLATFORM=spike SPIKE_PREFIX=/path/to/spike run
```

### Your core + ysyxSoC

```sh
make PLATFORM=ysyxsoc \
  YSYX_NPC=/path/to/your-core/soc/ysyxsoc \
  YSYX_SOC=/path/to/ysyx/ysyxSoC \
  run
```

`YSYX_NPC` and `YSYX_SOC` are required and have no defaults.

### Proto-soc chip model (refsim)

```sh
make PLATFORM=sim \
  SIM_CORE=/path/to/proto-core/quad-issue-rvv \
  run
```

`SIM_CORE` is required and has no default: the core checkout whose
`sim/soc` builds `refsim-soc`, the model of the proto-soc chip (100 MHz,
32-bit SDRAM, the chip's caches). Like `PLATFORM=spike run`, `run` opens a
window: the independent core's player (Dragon Ball, the RV32 translator, the
RVV PPU kernels) with picture, sound and keys, built in `build/sim-media` and
run by `refsim-soc-media` in fast-forward (instruction by instruction, no
cycle timing; a few frames per second). The cycle-exact measurement is a
separate target:

```sh
make PLATFORM=sim SIM_CORE=/path/to/proto-core/quad-issue-rvv run-gba-next-bench
```

It builds the benchmark as for the board (the `PLATFORM=fpga` image, byte for
byte, with its recorded input), runs the warmup functionally and the measured
frames cycle by cycle, and prints the window's cycles, IPC and FPS.

## Variables by platform

### Common

| Variable | Meaning | Default |
| --- | --- | --- |
| `PLATFORM` | Build target: `native`, `spike`, `ysyxsoc` or `sim` | `native` |
| `ROM` | ROM name under `roms/` (embedded for bare-metal targets); `.gba` is added when omitted | Native / Spike: `dragonball`; ysyxsoc: `cinema/gba/obj/2d-wrap/test.gba` |
| `BUILD_DIR` | Build output directory | `build` or a platform-specific directory |
| `RV32_RUNNER` | Experimental ARM/Thumb-to-RV32 block translator on RV32 targets | `0` |
| `GBN_RV32_FAST` | With `GBN_RV32=1`: relaxed-timing translation (bits in the Makefile); `0` is the exact translator, whose frames match the interpreter's (`test-gba-next-rv32` needs it); `31` enables all of it | `0` |

The experimental translator, its validation commands and instruction-count results are described in [doc/rv32-runner.md](doc/rv32-runner.md).

Proto-soc benchmarks (`fpga`, `spike`, `spike_zve32x`, `verilator`) also report
cycles and retired instructions after warmup. `PROFILE=1` enables PC sampling,
including generated RV32 blocks; see the same document for collection and
reporting commands. Spike timing and IPC are functional simulation values.

### Native

| Variable | Meaning | Default |
| --- | --- | --- |
| `ARGS` | Options passed to the native player, such as `--headless --frames 120` | empty |

Native receives runtime options through `ARGS`. For example, run 120 frames
without a window or audio:

```sh
make PLATFORM=native run ARGS="--headless --frames 120"
```

| Option inside `ARGS` | Meaning |
| --- | --- |
| `--frames N` | Run N frames as fast as possible, then exit and print the final frame CRC32 |
| `--headless` | Disable the window and audio; use with `--frames N` |
| `--benchmark N` | Measure performance over N frames without a window or audio |
| `--warmup N` | Run N warmup frames before measuring with `--benchmark`; default: 30 |

The Make variables `HEADLESS`, `AUDIO` and `FRAMES` do not control Native.
Native interactive runs enable audio; there is no separate Native option to
disable only audio.

### Spike

| Variable | Meaning | Default |
| --- | --- | --- |
| `HEADLESS` | Disable video, audio and host pacing | `0` |
| `AUDIO` | Enable simulated audio output | `1` |
| `FRAMES` | Frame limit; `0` means unlimited | `0` interactively; positive when headless |
| `CROSS` | RV32 GCC prefix | `riscv64-unknown-elf-` |
| `NEWLIB_ROOT` | Newlib build and install cache | `build/newlib` |
| `SPIKE_PREFIX` | Spike installation; can be overridden on the Make command line | Defined in `am/platform/spike.mk` |
| `GPIO` | Fixed input mask for Spike | `0` |

Spike saves last for the current run only.

### Sim (proto-soc chip model)

| Variable | Meaning | Default |
| --- | --- | --- |
| `SIM_CORE` | Core checkout that builds `sim/soc/refsim-soc` (and `refsim-soc-media` for `run`) | required |
| `GBN_PLAYER_FRAMES` | `run`: stop after this many frames; `0` runs until the window is closed | `0` |
| `GBN_WARMUP`, `GBN_FRAMES` | `run-gba-next-bench`: warmup frames run functionally, then the frames timed cycle by cycle; the window opens at the warmup line when `GBN_WARMUP` is a multiple of 100, else at the start | `100`, `30`; the standard measurement is `4500`, `120` |
| `GBN_CAPTURE` | `1`: run to the end and write pictures (`shot-*.png`), sound (`audio.wav`) and per-frame CRCs instead of timing | `0` |
| `SIM_REFERENCE` | `capture.bin` of another `GBN_CAPTURE=1` run: report the first differing frame and differing pixels | empty |
| `SIM_PROFILE` | `1`: per-function profile of the window | `0` |
| `SIM_ARGS` | Extra `refsim-soc` options, such as `--ic-kb 128` | empty |
| `SIM_HOST` | Run on that host (copies the ELF, runs its mirror of `SIM_CORE` in the tool image, copies the results back) | empty: here |

`run-gba-next-bench` results go to `build/sim/sim-gba-next-bench/`
(`summary.txt`, `stats.json`, `pcs.tsv`, `console.log`, `capture/`).
`run-mgba` times the mGBA player benchmark instead; the model then executes
its interpreter cycle by cycle, which is slow.

### Own core + ysyxSoC

| Variable | Meaning | Default |
| --- | --- | --- |
| `HEADLESS` | Disable video, audio and host pacing | `0` |
| `AUDIO` | Enable simulated audio output | `1` |
| `FRAMES` | Frame limit; `0` means unlimited | `0` interactively; positive when headless |
| `CROSS` | RV32 GCC prefix | `riscv64-unknown-elf-` |
| `NEWLIB_ROOT` | Newlib build and install cache | platform-managed |
| `YSYX_NPC` | Your core's ysyxSoC wrapper directory | required |
| `YSYX_SOC` | ysyxSoC checkout containing `build/ysyxSoCFull.v` | required |
| `AM_MEDIA_ROOT` | Internal media-bridge source path passed to the wrapper | set automatically |
| `AM_MEDIA_NATIVE` | Internal host SDL object passed to the wrapper | set automatically |
| `MAX_CYCLES` | ysyxSoC Verilator cycle limit; `0` means unlimited | based on `FRAMES` |

ysyxSoC saves last for the current run only.

## Controls and GPIO mapping

SDL maps host keys to logical AM buttons. Spike and the ysyxSoC path read the
same ten-bit GPIO value. A set bit means the button is held.

| AM button | Host key | GPIO bit |
| --- | --- | ---: |
| Up | Up arrow | 0 |
| Down | Down arrow | 1 |
| Left | Left arrow | 2 |
| Right | Right arrow | 3 |
| A | Z | 4 |
| B | X | 5 |
| L | A | 6 |
| R | S | 7 |
| Start | Enter | 8 |
| Select | Backspace | 9 |

Q, Escape and window close request exit. Losing window focus clears all ten
buttons. Logical AM buttons are translated to GBA keys in
`src/platform/am/player.c`; the CPU does not read SDL events directly. Both
guest drivers read GPIO input at `0x10002004`. Spike can use `GPIO=0x10` to hold
A initially in addition to SDL input.
