# AM prototype

This directory owns a small platform interface inspired by Abstract Machine.
It is a new implementation, not an ABI-compatible copy of the ysyx AM library.
The public header uses only standard integer, size, and Boolean types. The
library has no dependency on mGBA. All repository text is English.

## Layout

- `include/am.h`: lifecycle, capabilities, logical buttons, monotonic time,
  video, and audio contracts.
- `src/native/native.c`: Linux implementation using SDL2.
- `src/protosoc/`: polled UART, CLINT timer, GPIO, and platform identification.
- `src/ysyxsoc/`: ysyxSoC UART, CLINT, GPIO and startup for proto-core;
  the optional RV32E target also retains its heap and VGA driver.
- `src/riscv/`: RV32 startup, fatal traps, linker script, libc hooks, and the
  shared simulation media driver (`sim-media.c`).
- `platform/rv32.mk`: shared RV32 compiler, runtime, and ROM embedding rules.
- `platform/spike.mk`: Spike run/test rules.
- `platform/verilator.mk`: proto-soc NOR image packaging and RTL run/test rules.
- `platform/ysyxsoc.mk`: ysyxSoC Flash packaging and proto-core RTL run/test
  rules, with optional RV32E NPC compatibility.
- `platform/rtl-media.mk`: shared RTL media and interactive acceptance rules.
- `src/protosoc/ram-loader.c`: SRAM receiver for CRC-checked SDRAM applications.
- `tools/`: pinned Newlib builder and ROM embedding.
- `sim/`: Spike GPIO/SYSCTRL model, shared SDL media peripheral and optional
  RTL AXI bridge; see its README for the simulation-only register contract.
- `test/`: standalone AM and bare-metal runtime tests.
- `../src/platform/am/player.c`: shared frame loop, button/pixel translation,
  audio resampling, pacing, and final frame checksum.
- `../src/platform/am/main.c`: native command line, file ROMs, persistent saves.
- `../src/platform/am/embedded.c`: embedded ROM and RAM save storage.

## Contracts

The application calls AM from one thread. Device implementations may have
internal workers, but they never access the emulator. Native uses SDL's audio
queue, so no host callback enters mGBA. Audio writes are bounded and may accept
only part of a request. Counts and capacities use stereo frames; samples are
interleaved signed 16-bit integers at the reported output rate.

Input is a complete held-button bitmask plus a separate host quit request.
Native translates keyboard events; proto-soc reads GPIO input. A/B, directions,
shoulders, Start, and Select have no dependency on SDL keycodes or mGBA's key
numbering. Native releases every button on focus loss. Physical GPIO wiring is
one of the backend's responsibilities.

Video uses numeric `0x00RRGGBB` pixels and a stride measured in pixels. The
backend consumes or copies caller-owned data before returning. Native creates
a resizable 3x window and preserves aspect ratio with nearest-neighbor scaling.
Presentation may update the frame dimensions, including SGB borders.

Initialization requests devices, and capabilities describe those available in
the active session. A requested native device that fails to open is an error;
headless sessions explicitly request neither video nor audio. Callers check
capabilities before issuing device operations. `am_error()` retains the last
error through cleanup. Time is monotonic microseconds since initialization;
sleep can overshoot. Native yields to the host; proto-soc polls the timer.
Interactive Spike sessions use host time and sleep through the media device
so frame pacing does not busy-wait on Spike's virtual clock.

## Native

```sh
make -j4
make run
make test
make test-am
```

The root Makefile links `build/libmgba.a` and `build/libam.a` into `build/mgba`.
mGBA runs one frame at a time without its worker thread, pthread synchronization,
or anonymous mmap allocations. Native still uses the host C/math runtime and
file VFS for ROMs and battery saves. SDL can use internal device threads.

For reproducible comparisons, use `--headless --frames N`. Bounded runs fix the
emulated RTC to 2000-01-01 UTC, remove wall-clock pacing, and report the final
frame's CRC32 over packed RGB bytes. ROM, initial save data, and input must also
match. The checksum covers rendered output, not the complete emulator state or
CPU timing. Headless runs still emulate audio, but discard it without playback.

## RV32 on Spike

```sh
make -j4 PLATFORM=spike
make PLATFORM=spike run
make PLATFORM=spike run AUDIO=0
make PLATFORM=spike run HEADLESS=1 FRAMES=120
make PLATFORM=spike test-runtime
make PLATFORM=spike test-media
make PLATFORM=spike test
```

The output is `build/spike/mgba.elf`. `ROM` defaults to the game name `dragonball`,
which resolves to `roms/dragonball.gba`, and
is embedded into the executable. The default run opens an SDL window with
audio and keyboard input, then runs until Q, Escape, or window close.
`FRAMES=0` means unlimited and is the interactive default. A positive value
ends the run after that many frames and prints an RGB checksum over UART.
`AUDIO=0` disables output when sound is unwanted or no host audio device is
available. Spike can be substantially slower than real time, causing audio gaps.

`HEADLESS=1` disables media output and defaults to 120 frames. Headless runs
require a positive frame limit and do not initialize SDL devices.
Both native and embedded entry points use the same emulator configuration and
frame loop. Embedded saves use an expandable memory VFile and disappear on exit.
Spike's media peripheral lives under `sim/`. Its MMIO registers and framebuffer
at `0x40000000` are a simulation-only extension, not part of proto-soc v3.
The AM driver accesses this extension only in an `AM_SIM_MEDIA` build with media
requested. Spike and both interactive Verilator targets enable that flag; physical
display/audio still require hardware contracts.

The ELF uses RV32IM/ILP32 with Zicsr, Zifencei, and Zicbom enabled in the toolchain,
matching the scalar baseline of quad-issue-rvv. It requires no F/D floating-point
extension, A atomics, C compressed instructions, or RVV. AM-reachable audio and
BIOS helpers use integer arithmetic; floating-point configuration and formatting
are disabled on this target. The Spike image omits libm and Newlib floating-point
I/O, and the build rejects floating-point helper symbols. Execution uses one hart
in machine mode;
interrupts remain disabled and unexpected traps print a diagnostic before exit.

Startup sets the stack and global pointer and clears BSS. The image loads
directly at `0xa0000000` in 64 MiB of SDRAM. A bounded heap ends below a 256 KiB
application stack and a separate 4 KiB fatal-trap stack. There is no Linux or
proxy kernel. Newlib provides the required standard C functions; our libc hooks
provide allocation, UART I/O, and time. Spike HTIF is used only to report the
exit status. A real board needs its own completion policy.

### Build dependencies

Cross builds need a `riscv64-unknown-elf-` GCC toolchain with RV32IM/ILP32 libgcc,
GNU Make, Python 3.12+, host C/C++20 compilers, pkg-config, and SDL2 development
files. `CROSS` selects the tool prefix.
The first build downloads Newlib 4.6.0.20260123 from sourceware.org, verifies
SHA-256, and builds a local RV32 library under `build/newlib/`. See
`tools/build_newlib.py` for the pinned hash and build options. The extracted
source preserves Newlib's component license notices. No system installation
is changed. `make PLATFORM=spike runtime` prepares this dependency on its own;
`NEWLIB_ROOT` overrides its cache/install directory. The download is only
required on the first build, and a failed build records `build/newlib/build.log`.

`SPIKE_PREFIX` defaults to `/home/tillsat/tools/spike-20feb9c2`, the installation
used by proto-core. It must contain `bin/spike`, `include/riscv/abstract_device.h`,
and `lib/libriscv.so`. The device plugin is compiled against those headers and
libraries. Spike plugin APIs can vary between revisions. Native remains the
default; `PLATFORM=spike` isolates its objects under `build/spike/`.

## proto-core on ysyxSoC

```sh
make -j4 PLATFORM=ysyxsoc run
make PLATFORM=ysyxsoc HEADLESS=1 AUDIO=0 FRAMES=2 ysyxsoc-image
make PLATFORM=ysyxsoc test-interactive
```

The default target uses proto-core `quad-issue-rvv`, RV32IM/ILP32, and its
existing ysyxSoC Verilator wrapper. The standard FSBL/SSBL image boots from
Flash into 32 MiB SDRAM. This platform defaults to the bundled 4 KiB ROM
`cinema/gba/obj/2d-wrap/test.gba` to reduce Flash boot loading time.
The simulation media bridge provides video/audio,
and keyboard input enters GPIO at `0x10002004`. The wrapper's CLINT counts
CPU clocks; AM converts using `CPU_MHZ`, while interactive pacing uses host
time. See [README.md](../README.md). `YSYXSOC_CORE=rv32e` retains the older
headless compatibility target with its separate Newlib build and ABI.

## proto-soc RTL boot and interaction

`make PLATFORM=verilator run` boots the same player on proto-core
`quad-issue-rvv` inside proto-soc and opens the SDL media devices. It defaults
to unlimited interactive operation with `roms/dragonball.gba`. Use
`HEADLESS=1 AUDIO=0 FRAMES=2` to check the reference pixels without output.
`BRANCH=single-issue` selects the earlier baseline core.

The normal Boot ROM and stage2 execute the NOR -> SRAM -> SDRAM sequence,
including DMA copying and boot CRC checks. The application is linked at
`0xa0000000`. The simulator's optional AXI media bridge forwards normal bus
requests into the SoC and services `0x40000000` media accesses through DPI.
All emulator instructions execute on RTL. SDL keyboard state drives GPIO
input pins, and `ebreak` with status in `a0` terminates the simulator.

`PROTOSOC`, `BRANCH`, `ICACHE`, `DCACHE`, and `MAX_CYCLES` configure the run.
An unlimited interactive run uses `MAX_CYCLES=0`; bounded runs default to
500 million cycles, and exhaustion is a failure. `make test-interactive`
checks SDL pixels against native/reference images, actual game response to
press/release, nonzero PCM, and window-close exit on both RTL platforms.
See [README.md](../README.md) for exact coverage and commands.

## Platform model and GPIO mapping

The proto-soc driver follows our proto-soc address-map v3, defined in
`/home/tillsat/proto-core/soc/sw/include/soc_map.h` and
`/home/tillsat/proto-core/soc/docs/address-map.md`. It does not use ysyxSoC.

| Device | Base | Used interface |
| --- | --- | --- |
| CLINT | `0x02000000` | Stable high/low/high reads of `mtime` at `+0xbff8` |
| UART | `0x10000000` | Byte-wide NS16550 registers, polled transmit |
| GPIO | `0x10002000` | 32-bit input register at `+0x04` |
| SYSCTRL | `0x1000f000` | Map version at `+0x14`, clock frequency at `+0x38` |
| SDRAM | `0xa0000000` | 64 MiB RAM for image, embedded ROM, heap, stacks |

Spike supplies its standard UART and CLINT. `sim/protosoc.cc` adds the
polled GPIO and SYSCTRL subset used here. The model reports Spike's 10 MHz
virtual timer frequency through SYSCTRL so the same microsecond conversion
works with the RTL's reported system clock. This virtual rate is not a prediction
of hardware performance. Keep Spike's device-tree initialization enabled; this
revision skips device construction when `--disable-dtb` is used.

The initial GPIO convention is active-high, matching the AM bits:

| GPIO bit | Button | GPIO bit | Button |
| --- | --- | --- | --- |
| 0 | Up | 5 | B |
| 1 | Down | 6 | L |
| 2 | Left | 7 | R |
| 3 | Right | 8 | Start |
| 4 | A | 9 | Select |

`GPIO` sets a fixed input mask for the whole Spike run, for example `GPIO=0x10`
for A or `GPIO=0x310` for A + Start + Select. SDL keyboard buttons are ORed into
this mask: arrows, Z/X, A/S, Enter, and Backspace have the native mappings.
Focus loss releases keyboard buttons; the fixed mask remains held.
GPIO output/segment/enable registers
and SYSCTRL identification/scratch/capacity reads are modeled. GPIO interrupts,
physical wiring, and debounce are not implemented. The plugin
is a functional model of the interfaces used by this program, not a complete
proto-soc simulation.

## Validation and remaining hardware work

On `PLATFORM=spike`, `test-runtime` checks startup, allocation/reallocation, integer formatting,
UART output, timer progress, and GPIO input. The image check rejects any
software floating-point helper symbols.
`test-media` runs a separate RV32 program against SDL dummy devices and verifies
exact video pixels, row stride, 3x scaling, dimension changes, stereo PCM samples,
all ten keyboard/GPIO bits, simultaneous presses, focus release, and quit.
The Spike `test` target includes both and compares native and Spike after 120 frames for the GB, GBC, and GBA fixtures,
checks failure exit status, and rejects an invalid embedded ROM. It uses fresh
temporary save storage. Logs are stored in `build/spike/test-logs/`.

The Verilator target adds proto-core execution through the real
BootROM/NOR/SDRAM loading path. Spike does not instantiate or communicate with
that RTL. Headless mGBA execution has also passed on physical single-issue
DE0-CV and quad-rvv ZCU102 boards for the documented benchmark. Remaining work
includes physical button wiring, persistent saves, display/audio hardware,
broader game compatibility, RVV optimization, and reaching real-time performance.
