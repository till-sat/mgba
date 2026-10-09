/* SPDX-License-Identifier: MPL-2.0 */
#ifndef GBA_NEXT_CORE_H
#define GBA_NEXT_CORE_H

/* Independent execution core: no mGBA types, callbacks or host allocation.
 * CPU, bus, peripherals and scanline rendering are implemented; cycle-level
 * display behavior and full compatibility remain under development. */
#include <stdbool.h>
#include <stdint.h>

#define GBN_EWRAM_SIZE 0x40000u
#define GBN_IWRAM_SIZE 0x8000u
#define GBN_BIOS_SIZE 0x4000u
#define GBN_ROM_MAX_SIZE 0x2000000u
#define GBN_PALETTE_SIZE 0x400u
#define GBN_VRAM_SIZE 0x18000u
#define GBN_OAM_SIZE 0x400u
#define GBN_SAVE_MAX_SIZE 0x20000u
#define GBN_EVENT_COUNT 16u
#define GBN_MAX_DELAY 0x3fffffffu
#define GBN_EVENT_SAVE 5u
#define GBN_EVENT_SIO 6u
#define GBN_EVENT_AUDIO_FRAME 7u
#define GBN_EVENT_TIMER0 8u
#define GBN_EVENT_AUDIO 12u
#define GBN_EVENT_DMA 13u
#define GBN_EVENT_VIDEO 14u
#define GBN_EVENT_IRQ 15u

enum GbnStatus {
	GBN_STEP,
	GBN_EVENT,
	GBN_DEADLINE,
	GBN_UNSUPPORTED_INSTRUCTION,
	GBN_UNSUPPORTED_ADDRESS,
	GBN_UNSUPPORTED_MODE,
	GBN_INVALID_ARGUMENT,
	GBN_UNSUPPORTED_DEVICE
};

struct GbnCpu {
	uint32_t r[15];
	uint32_t pc; /* Address of the next instruction; visible PC is +4/+8. */
	uint32_t cpsr;
	uint32_t pipe[2]; /* Architecturally relevant for self-modifying RAM code. */
	uint32_t shifter_carry; /* Latched by ARM operand shifts, used by MULS. */
	uint32_t spsr;
	/* User/System, FIQ, IRQ, Supervisor, Abort, Undefined. The active bank's
	 * live SP/LR/SPSR are above; backing slots are filled when leaving it. */
	struct { uint32_t sp, lr, spsr; } banks[6];
	uint32_t high_banks[2][5]; /* Shared and FIQ r8-r12 backing slots. */
};

struct GbnEvent {
	uint32_t when;
	uint8_t priority; /* Smaller values first; event ID breaks equal ties. */
	bool active;
};

struct GbnDma {
	uint32_t source, dest; /* Address-register values, separate from live cursors. */
	uint32_t next_source, next_dest;
	uint32_t count, remaining, when, latch;
	uint16_t control;
	bool finishing; /* Last bus beat completed; two-cycle release may be pending. */
	bool fifo; /* Custom audio burst: four words, fixed destination. */
};

struct GbnTimer {
	uint32_t epoch;
	uint16_t value, reload, control;
};

#define GBN_AUDIO_CAPACITY 1024u
/* Timestamp permits a frontend to consume samples across SOUNDBIAS rate
 * changes and cycle wrap without guessing which rate a queued sample used. */
struct GbnStereo { int16_t left, right; uint32_t when; };
struct GbnPsgChannel {
	uint32_t updated, remainder;
	uint16_t frequency, length, lfsr, sweep_shadow;
	uint8_t phase, duty, volume, envelope, envelope_ticks, sample;
	uint8_t sweep, sweep_ticks, noise;
	bool enabled, dac, length_enable, swept_down;
};
struct GbnFifo {
	uint8_t data[32], read, size;
	int8_t sample;
	uint32_t requests;
};
struct GbnAudio {
	struct GbnPsgChannel psg[4];
	struct GbnFifo fifo[2];
	uint8_t wave[32], frame;
	uint32_t frame_when, produced, dropped;
	bool running;
	struct GbnStereo samples[GBN_AUDIO_CAPACITY];
	unsigned read, size;
};

struct GbnSio {
	uint16_t control;
	uint8_t mode, transfer_mode;
};

enum GbnSaveType { GBN_SAVE_NONE, GBN_SAVE_EEPROM, GBN_SAVE_SRAM, GBN_SAVE_FLASH64, GBN_SAVE_FLASH128 };
struct GbnSave {
	uint8_t* data;
	uint32_t capacity, size, busy_until, address;
	enum GbnSaveType type;
	uint8_t phase, bits, address_bits, pending[8];
	uint8_t flash_unlock, flash_command, bank;
	uint16_t settling;
	bool dirty, busy;
};

#define GBN_SCREEN_WIDTH 240u
#define GBN_SCREEN_HEIGHT 160u
struct GbnPpu {
	uint16_t* pixels; /* GBA BGR555: red occupies bits 0..4. Caller-owned. */
	unsigned stride;
	int32_t reference[2][2]; /* Current BG2/BG3 affine scanline origins. */
	uint32_t frames, lines, seen[5];
};

struct GbnDevices {
	uint16_t io[0x210 / 2];
	uint8_t* palette;
	uint8_t* vram;
	uint8_t* oam;
	uint32_t frames; /* VBlank entries, not rendered frames. */
	bool video_running;
	uint8_t postflag;
	bool halted, stopped;
	struct GbnDma dma[4];
	int active_dma;
	bool dma_blocked, dma_access, dma_bus_valid;
	uint32_t dma_bus, dma_pc;
	struct GbnTimer timers[4];
	struct GbnAudio audio;
	struct GbnSio sio;
	struct GbnPpu ppu;
};

struct Gbn {
	struct GbnCpu cpu;
	uint32_t now; /* Modular GBA cycles; scheduling horizon is GBN_MAX_DELAY. */
	uint8_t* ewram;
	uint8_t* iwram;
	const uint8_t* rom;
	uint32_t rom_size, rom_mask;
	uint32_t rom_generation; /* Invalidates native blocks on image replacement. */
	const uint8_t* bios;
	bool builtin_bios;
	uint32_t bios_latch;
	uint16_t waitcnt;
	uint8_t rom_nonseq[3], rom_seq[3];
	uint32_t prefetched_pc;
	uint8_t ewram_wait; /* Additional waits per 16-bit beat; reset = 2. */
	/* Cached instruction mapping is refreshed by entry/branch, not each step. */
	const uint8_t* code;
	uint32_t code_mask;
	uint8_t code_wait;
	uint8_t code_word_wait;
	uint8_t code_nonseq, code_word_nonseq;
	uint8_t code_region;
	bool cpu_access; /* Distinguishes an instruction's bus phase from inspection. */
	struct GbnDevices* devices;
	struct GbnSave* save;
	struct GbnEvent events[GBN_EVENT_COUNT];
	int next_event;
	/* Canonical order is separate from next_event so replacing a non-head
	 * event preserves the existing equal-time/priority winner. */
	uint8_t event_head, event_prev[GBN_EVENT_COUNT], event_next[GBN_EVENT_COUNT];
};

struct GbnAccess {
	uint32_t value;
	uint32_t cycles; /* Bus cycles only. A CPU load adds its internal cycle. */
};

/* Caller supplies distinct RAM buffers of the sizes above; init clears machine
 * state but preserves RAM contents. Neither buffer may overlap the machine. */
void gbn_init(struct Gbn*, uint8_t* ewram, uint8_t* iwram);
/* Read-only images remain owned by the caller. ROM need not be padded or a
 * power of two. Replacing either image invalidates the current code mapping. */
bool gbn_attach_rom(struct Gbn*, const uint8_t*, uint32_t size);
bool gbn_attach_bios(struct Gbn*, const uint8_t*, uint32_t size);
/* Optional independent guest firmware. Implemented services execute guest
 * instructions; other services stop with UNSUPPORTED_DEVICE. */
void gbn_use_builtin_bios(struct Gbn*);
enum GbnSaveType gbn_detect_save(const uint8_t* rom, uint32_t size);
/* Storage is caller-owned and preserved. EEPROM needs 8 KiB capacity to permit
 * DMA command length detection; active size starts at 512 bytes. */
bool gbn_attach_save(struct Gbn*, struct GbnSave*, uint8_t* data, uint32_t capacity, enum GbnSaveType);
void gbn_set_waitcnt(struct Gbn*, uint16_t value);
/* Device storage is caller-owned, distinct and nonoverlapping. Attaching
 * initializes registers but preserves memory. Slots 5..15 become reserved. */
bool gbn_attach_devices(struct Gbn*, struct GbnDevices*, uint8_t* palette, uint8_t* vram, uint8_t* oam);
void gbn_video_start(struct Gbn*, bool skip_bios);
/* NULL detaches output. Stride is in uint16 pixels. Scanline sampling currently
 * occurs at HBlank entry; sub-scanline register changes/contention are pending. */
bool gbn_attach_pixels(struct Gbn*, uint16_t*, unsigned stride);
void gbn_audio_start(struct Gbn*);
unsigned gbn_audio_rate(const struct Gbn*);
unsigned gbn_audio_read(struct Gbn*, struct GbnStereo* output, unsigned capacity);
void gbn_set_keys(struct Gbn*, uint16_t pressed);
void gbn_request_irq(struct Gbn*, uint16_t mask);
/* Process built-in due events, advancing time while DMA owns the bus. HALT
 * advances through one future event boundary per call; STEP may leave the CPU
 * halted, so callers check devices->halted before executing it. Other event IDs
 * stay pending for the caller. STOP currently returns UNSUPPORTED_DEVICE. */
enum GbnStatus gbn_service_events(struct Gbn*);
/* Bootstrap/reposition a Thumb pipeline without advancing time; not a guest
 * branch or a complete GBA reset. Registers other than PC/CPSR.T are preserved. */
enum GbnStatus gbn_enter_thumb(struct Gbn*, uint32_t pc);
enum GbnStatus gbn_enter_arm(struct Gbn*, uint32_t pc);
/* CPU exception entry; an attached BIOS supplies vectors and handler code.
 * IRQ is ignored when CPSR.I is set. This does not implement the IO controller. */
enum GbnStatus gbn_raise_irq(struct Gbn*);
enum GbnStatus gbn_read(const struct Gbn*, uint32_t address, unsigned width, struct GbnAccess*);
enum GbnStatus gbn_write(struct Gbn*, uint32_t address, unsigned width, uint32_t value, uint32_t* cycles);

/* No automatic fallback: unsupported operations leave guest state unchanged.
 * Instructions complete atomically, including a possible deadline overshoot. */
enum GbnStatus gbn_step(struct Gbn*);
/* Execute up to max_instructions, stopping before the next instruction if an
 * event is due or the CPU is blocked by DMA/HALT/STOP. Instructions remain
 * atomic and may overshoot a deadline, exactly as in gbn_step(). The count
 * includes only completed instructions; exhausting the cap returns GBN_STEP.
 * No events are serviced here. The cap and count pointer must be nonzero. */
enum GbnStatus gbn_run_batch(struct Gbn*, uint32_t max_instructions, uint32_t* executed);
enum GbnStatus gbn_run_for(struct Gbn*, uint32_t budget);
bool gbn_schedule(struct Gbn*, unsigned id, uint32_t delay, uint8_t priority);
/* Absolute scheduling also permits an overdue event, within MAX_DELAY. */
bool gbn_schedule_at(struct Gbn*, unsigned id, uint32_t when, uint8_t priority);
void gbn_cancel(struct Gbn*, unsigned id);
/* Consume the earliest due event; an event handler lives outside the CPU loop.
 * Returns its ID, or -1. Lateness is measured at the instruction boundary. */
int gbn_take_event(struct Gbn*, uint32_t* lateness);

#endif
