# Simulated peripherals

This directory adds interactive peripherals to the local Spike build. It does
not instantiate proto-soc RTL or change that repository's address map.

- `protosoc.cc`: polled GPIO and SYSCTRL subset of proto-soc v3.
- `media.cc`: framebuffer, PCM output, quit, and host pacing device.
- `media.h`: register definitions shared by the host model and guest driver.

The host side reuses `am/src/native/native.c` for SDL display, keyboard events,
and audio queueing. It never calls the emulator: mGBA continues to execute as
RV32 instructions inside Spike. The guest sends pixels and samples using MMIO.
The model polls SDL between instruction batches to keep the window responsive.
Keyboard state is exposed through the existing GPIO input register. Closing
the window sets a separate quit register, letting the guest exit through HTIF.

```sh
make PLATFORM=spike run
make PLATFORM=spike run AUDIO=0
make PLATFORM=spike run HEADLESS=1 FRAMES=120
make PLATFORM=spike test-media
```

## Simulation extension

The media device (`--device=am_media`) occupies `0x40000000..0x400fffff`.
These are simulation-only addresses, separate from proto-soc v3. Registers,
pixels, and stereo sample pairs require aligned 32-bit little-endian accesses.
Invalid bus accesses fail; invalid commands set the error bit and print a host
diagnostic. Data must be written before commands, with an I/O fence.

| Offset | Register | Contract |
| --- | --- | --- |
| `0x00` | ID | Read `0x414d5301` (interface version 1) |
| `0x04` | CONTROL | Write video bit 0 / audio bit 1 to initialize; 0 shuts down |
| `0x08` | STATUS | Enabled flags; bit 31 signals a device error |
| `0x0c`, `0x10` | WIDTH, HEIGHT | Frame dimensions; maximum 256 x 256 |
| `0x14` | PRESENT | Write to present WIDTH x HEIGHT packed framebuffer pixels |
| `0x18` | QUIT | Read latched Q, Escape, or window-close request |
| `0x1c`, `0x20` | TIME_LO, TIME_HI | Read low first to latch host monotonic microseconds |
| `0x24` | SLEEP_US | Sleep up to 10000 host microseconds and poll events |
| `0x28` | AUDIO_RATE | Write requested rate before CONTROL; read actual rate afterward |
| `0x2c` | AUDIO_CAP | Queue capacity in stereo frames |
| `0x30` | AUDIO_QUEUED | Queued stereo frames, excluding the device's own buffer |
| `0x34` | AUDIO_SUBMIT | Submit up to 2048 stereo frames from PCM staging memory |
| `0x38` | AUDIO_WRITTEN | Accepted frame count from the last submission |
| `0x1000` | Framebuffer | Up to 256 x 256 numeric `0x00RRGGBB` pixels |
| `0x41000` | PCM staging | 2048 packed S16 stereo pairs: left low 16 bits, right high 16 bits |

Commands consume/copy their buffers synchronously. Audio submission is bounded
and nonblocking. The guest may immediately reuse the buffers after a command
completes. The framebuffer is device storage, not a DMA descriptor or a host
pointer into guest RAM.

Interactive pacing uses host time through this extension. Headless checks keep
using CLINT virtual time and do not open SDL devices. Neither clock measures
quad-issue-rvv hardware performance. Actual board display, audio, and persistent
save storage need their own hardware contracts and drivers.
