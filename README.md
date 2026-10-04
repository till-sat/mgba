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

## Minimal commands

Each command builds the player when needed and starts the selected interactive
path. Native and Spike default to `roms/dragonball.gba`. ysyxsoc defaults to
the bundled 4 KiB test ROM `cinema/gba/obj/2d-wrap/test.gba`, which displays white
and green blocks and reduces Flash boot loading time. These defaults are defined
in the root `Makefile`. Append `ROM=/path/to/game.gba` to use another ROM.

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

## Variables by platform

### Common

| Variable | Meaning | Default |
| --- | --- | --- |
| `PLATFORM` | Build target: `native`, `spike` or `ysyxsoc` | `native` |
| `ROM` | GB/GBC/GBA ROM to load (embedded for bare-metal targets) | Native / Spike: `roms/dragonball.gba`; ysyxsoc: `cinema/gba/obj/2d-wrap/test.gba` |
| `BUILD_DIR` | Build output directory | `build` or a platform-specific directory |

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
