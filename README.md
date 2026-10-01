# Minimal mGBA Player

An SDL2 player based on [mGBA](https://github.com/mgba-emu/mgba).
Pass a GB, GBC, or GBA ROM on the command line to play with video, audio,
keyboard controls, and in-game saves.

## Build and Run

The build targets Linux and requires a C11 compiler (GCC by default), GNU Make,
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

Show command-line help with `./build/mgba --help`.
The Makefile compiles the sources directly. CMake, C++, Qt, FFmpeg, OpenGL
development packages, and a separate libmgba shared library are not required.
SDL2 and the system runtime libraries must be installed.

`make` supports incremental builds and rebuilds affected objects when sources,
headers, or compiler options change. `make clean` removes only `build/`, leaving
ROMs and saves intact. Optionally install with `make install PREFIX="$HOME/.local"`.
Staged installation through `DESTDIR` is also supported.

Optimization and LTO are enabled by default. For a debug build, use
`make -j4 CFLAGS='-O0 -g' LTO=`. Override tools and options with `CC`, `CPPFLAGS`,
`CFLAGS`, `LDFLAGS`, `LDLIBS`, and `PKG_CONFIG`.

## Controls and Saves

| Keyboard | Game control |
| --- | --- |
| Arrow keys | Directional pad |
| Z / X | A / B |
| A / S | L / R (GBA) |
| Enter | Start |
| Backspace | Select |
| Q / Escape / Close window | Quit |

Use the game's own save menu. Saves for `game.gba` are written to `game.sav`
in the same directory and loaded automatically on the next run. The ROM directory
must be writable. ROMs with the same basename in one directory share the same
`.sav` file, even if their extensions differ.

The window opens at 3x scale: 480x432 for GB/GBC and 720x480 for GBA.
SGB border dimensions are also supported. Resize the window by dragging its edges;
the picture keeps its aspect ratio with nearest-neighbor scaling and black borders.
The player uses mGBA's built-in BIOS emulation and does not read global user settings.
All game keys are released when the window loses focus.

## Scope

The player provides no menus, debugger, scripting, gamepad controls, multiplayer,
save states, replays, fast-forward, screenshots, recording, cheats, patch loading,
or archive loading. SDL2 is the only supported frontend.

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

Test scripts are in `src/platform/sdl/test/`. The `cinema/` directory contains only
the three ROM fixtures and reference images used by these tests. Checks cover
reference pixels for all three systems, non-silent audio, key presses and releases,
focus loss, battery save persistence across restarts, and error exit paths.
Physical display and sound output still require manual testing.

## Repository Language

Use English for all repository text, including documentation, comments, and
strings printed by programs or build tools. Do not add Chinese text to files.

## License

Licensed under the [Mozilla Public License 2.0](LICENSE). Upstream copyright
notices and the [inih BSD license](src/third-party/inih/LICENSE.txt) are preserved.
