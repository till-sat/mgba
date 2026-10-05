# mGBA on standard ysyx AM

This directory is an independent Abstract Machine application. It uses the
upstream ysyx AM `Makefile` and the standard `ARCH=riscv32-ysyxsoc` platform,
while the parent mGBA build remains unchanged.

Set `AM_HOME` to the ysyx `abstract-machine` checkout and build the bundled
small GBA ROM:

```sh
AM_HOME=/path/to/abstract-machine \
  make -C ysyx image ARCH=riscv32-ysyxsoc
```

The resulting files are `ysyx/build/mgba-riscv32-ysyxsoc.elf` and `.bin`.
`ROM=/path/to/game.gba` selects another embedded ROM and `FRAMES=N` sets the
bounded run length at compile time. Generated ROM and frame configuration files
are kept under `ysyx/src/` and are refreshed when those variables change.

Enable the optional threaded Thumb interpreter with the same switch as the
standalone build:

```sh
AM_HOME=/path/to/abstract-machine \
  make -C ysyx image ARCH=riscv32-ysyxsoc RUNNER_THREADED=1
```

It uses the shared Thumb instruction definitions and requires no ROM trace.
`RUNNER_THREADED=0` is the default; changing the switch rebuilds the affected
objects automatically. Run the ROM-independent differential checks from the
repository root with `make test-threaded`. The earlier FPGA FPS measurements
cover the standalone frontend; this AM target needs its own performance test.

To boot the same image through the standard ysyxSoC/NPC path, provide the
usual `NPC_HOME` (and, when enabled by NPC, `NEMU_HOME`) environment variables:

```sh
AM_HOME=/path/to/abstract-machine \
NPC_HOME=/path/to/npc \
  make -C ysyx test ARCH=riscv32-ysyxsoc FRAMES=1
```

`image` is the standard AM compile/packaging target; `test` delegates to the
platform's standard `run` target. The stock ysyxSoC AM backend has no audio
device and its keyboard input backend is empty, so this target validates ROM
loading, CPU execution, video drawing, timer access, and AM termination.
