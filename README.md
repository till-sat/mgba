# mGBA AM Player

An AM-based player using the [mGBA](https://github.com/mgba-emu/mgba) core.
Pass a GB, GBC, or GBA ROM on the command line to play with video, audio,
logical buttons, and in-game saves. Native Linux uses SDL2 for display, audio,
and keyboard-to-button mapping. The RV32 bare-metal build runs on Spike with
a small model of the proto-soc registers used by AM.

## Build and Run

The native build targets Linux and requires a C11 compiler (GCC by default), GNU Make,
pkg-config, and the SDL2 development package. Install dependencies on Ubuntu or Debian:

```sh
sudo apt install build-essential pkg-config libsdl2-dev
```

Build and run:

```sh
make -j"$(nproc)"
make run
```

`make run` starts `roms/dragonball.gba` by default and builds the player if needed.
To use another ROM, run `make run ROM="/path/to/game.gba"` or
`./build/mgba "/path/to/game.gba"`.

`PLATFORM=native` is the default.
For a bounded run without a window, sound device, or wall-clock pacing:

```sh
make run ROM=cinema/gb/acid/dmg-acid2/test.gb ARGS="--headless --frames 120"
```

The final frame's RGB checksum is printed for comparison across platforms.
Bounded runs fix emulated RTC time to 2000-01-01 UTC; matching ROM, save data,
and input are also needed for reproducible results. `--headless` requires
`--frames N`. Normal `make run` remains interactive and paced at the game's rate.

Show command-line help with `./build/mgba --help`.
The Makefile compiles the sources directly. CMake, C++, Qt, FFmpeg, OpenGL
development packages, and a separate libmgba shared library are not required.
SDL2 and the system runtime libraries must be installed for native builds.
The Spike device plugin additionally needs a host C++20 compiler.

`make` supports incremental builds and rebuilds affected objects when sources,
headers, or compiler options change. `make clean` removes only `build/`, leaving
ROMs and saves intact. This also removes cached cross-build dependencies.
Optionally install the native player with `make install PREFIX="$HOME/.local"`.
Staged installation through `DESTDIR` is also supported.

Optimization and LTO are enabled by default. For a debug build, use
`make -j4 CFLAGS='-O0 -g' LTO=`. Override tools and options with `CC`, `CPPFLAGS`,
`CFLAGS`, `LDFLAGS`, `LDLIBS`, and `PKG_CONFIG`.

## Bare-metal Spike

Install a `riscv64-unknown-elf-` GCC toolchain with RV32IM/ILP32 support and use
the Spike installation from proto-core (including its headers and libraries).
The default `SPIKE_PREFIX` is `/home/tillsat/tools/spike-20feb9c2`; override it
when your installation is elsewhere. Python 3.12+, GNU Make, and a host C++20
compiler, pkg-config, and SDL2 development files are also required. The first cross build downloads and builds a
checksum-pinned Newlib under `build/newlib/`; it does not change system tools.

```sh
make -j4 PLATFORM=spike
make PLATFORM=spike run
make PLATFORM=spike run AUDIO=0
make PLATFORM=spike run HEADLESS=1 FRAMES=120
make PLATFORM=spike test-runtime
make PLATFORM=spike test-media
make test-spike
```

`make PLATFORM=spike run` opens an SDL window with audio and keyboard input and
runs until Q, Escape, or window close. The window uses the same controls and
3x scale as native. `build/spike/mgba.elf` embeds `ROM`, which defaults to
`roms/dragonball.gba`. `AUDIO=0` disables sound. Spike is much slower than native;
audio may break up when emulation cannot keep up with playback.

Interactive runs default to `FRAMES=0` (unlimited). A positive `FRAMES` stops
after that many frames and prints an RGB checksum. `HEADLESS=1` disables the
window and audio, defaulting to 120 frames for automated checks. Changing
`ROM`, `HEADLESS`, `AUDIO`, or `FRAMES` rebuilds the affected input and executable.
`GPIO=0x10` holds A in addition to keyboard input; see
[the AM documentation](am/README.md) for the bit map.

This is a single-hart RV32IM baseline without RVV optimization. It uses our
startup code, bounded heap, UART, CLINT timer, and GPIO driver. It has no Linux
or proxy-kernel dependency. The ELF loads directly into 64 MiB of SDRAM at
`0xa0000000`; the BootROM/NOR boot path is not implemented here. Saves live in
RAM for one run. The [simulated peripherals](am/sim/README.md) supply a framebuffer,
audio output, and keyboard input through guest MMIO. Display/audio registers
are an explicitly separate simulation extension, not existing proto-soc hardware.

Spike validates software against the modeled registers. It does not connect to
or validate quad-issue-rvv/proto-soc RTL, peripheral timing, physical buttons,
or real-time emulation performance. `make PLATFORM=spike clean` removes only
cross-build outputs and keeps the downloaded Newlib dependency.

## Controls and Saves

| Keyboard | Game control |
| --- | --- |
| Arrow keys | Directional pad |
| Z / X | A / B |
| A / S | L / R (GBA) |
| Enter | Start |
| Backspace | Select |
| Q / Escape / Close window | Quit |

On native, use the game's own save menu. Saves for `game.gba` are written to `game.sav`
in the same directory and loaded automatically on the next run. The ROM directory
must be writable. ROMs with the same basename in one directory share the same
`.sav` file, even if their extensions differ.
Spike saves currently last only for the current run.

The window opens at 3x scale: 480x432 for GB/GBC and 720x480 for GBA.
SGB border dimensions are also supported. Resize the window by dragging its edges;
the picture keeps its aspect ratio with nearest-neighbor scaling and black borders.
The player uses mGBA's built-in BIOS emulation and does not read global user settings.
All game keys are released when the window loses focus.

## Scope

The player provides no menus, debugger, scripting, gamepad controls, multiplayer,
save states, replays, fast-forward, screenshots, recording, cheats, patch loading,
or archive loading.

## Platform Boundary

The new platform library lives in `am/` and builds as `build/libam.a`.
Its [public interface](am/include/am.h) has no SDL or mGBA dependencies.
The [shared AM integration](src/platform/am/player.c) runs the core one frame at a time,
translates logical buttons and pixels, and resamples audio into a bounded queue.
The emulator uses no worker thread; SDL's device workers never call into mGBA.

Native maps keyboard events to logical buttons. The proto-soc driver maps GPIO
levels to the same button states for our intended `quad-issue-rvv` +
proto-soc platform. ysyx AM is a design reference, not an interface compatibility
requirement, and ysyxSoC is not a target.

The native entry point owns file ROMs and persistent saves. The embedded entry
point owns an embedded ROM and RAM save storage, using Newlib and our platform
runtime. Both share the same frame loop. RTL boot, persistent embedded storage,
physical button wiring, and display/audio hardware remain future work.
See [am/README.md](am/README.md).

The minimal core excludes video logging, video proxies, and optional link modules.
The repository keeps only the sources, headers, third-party inih library, and
regression fixtures needed by this player. Other upstream frontends, unused
dependencies and resources, old packaging tools, and CMake build files have been removed.
Internally referenced serialization, caching, and related core code remains to
preserve game compatibility.

## Automated Tests (Linux)

```sh
make test
```

Tests additionally require Python 3, Pillow, pkg-config, a C compiler, Clang, and
llvm-objcopy. They use SDL dummy video/audio devices and temporary ROMs; commercial
game files are not required.

Test scripts are in `src/platform/am/test/`. The `cinema/` directory contains only
the three ROM fixtures and reference images used by these tests. Checks cover
reference pixels for all three systems, non-silent audio, key presses and releases,
focus loss, battery save persistence across restarts, and error exit paths.
Headless runs compare RGB checksums against the reference images. `make test-am`
also tests the AM library independently: all ten buttons, simultaneous presses,
device capabilities, timing, bounded audio, and initialization/cleanup.
Physical display and sound output still require manual testing.

`make test-spike` (or `make PLATFORM=spike test`) additionally checks the bare-metal
runtime, GPIO input, timer, allocation, integer libc, success/failure exit status,
invalid ROM handling, and native/Spike RGB checksum agreement after 120 frames
for each of the GB, GBC, and GBA fixtures. Logs are saved under
`build/spike/test-logs/`. These checks use fresh saves in temporary storage.
`make PLATFORM=spike test-media` exercises the guest MMIO path with SDL dummy
devices: exact pixels and scaling, frame stride and size changes, exact stereo
PCM samples, all ten keys, key releases, focus loss, and window close.

## Repository Language

Use English for all repository text, including documentation, comments, and
strings printed by programs or build tools. Do not add Chinese text to files.

## License

Licensed under the [Mozilla Public License 2.0](LICENSE). Upstream copyright
notices and the [inih BSD license](src/third-party/inih/LICENSE.txt) are preserved.
Cross builds also use Newlib under its own component licenses; the downloaded
source includes `COPYING`, `COPYING.NEWLIB`, and other license notices.
