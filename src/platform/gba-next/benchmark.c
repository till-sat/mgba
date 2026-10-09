/* SPDX-License-Identifier: MPL-2.0 */
/* Identical fixed-work frontend on native, Spike and FPGA. No mGBA linkage.
 * Headless means no physical output: PPU and APU always run, without skipping.
 * GBN_BENCH_VALIDATE controls hashes and PCM inspection, not simulation. */
#include <gba-next/core.h>
#if GBN_RV32
#include <gba-next/rv32.h>
static struct GbnRv32 backend;
#endif
#include <am.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#ifdef GBN_COUNTERS
#include <am-counters.h>
#endif
#ifdef GBN_PROFILE
#include <am-profile.h>
#if GBN_RV32
/* Classify the actual host instruction without walking the cache in the ISR.
 * Arena offsets are reused, so they are not stable guest PCs. */
static bool resolve_native(uintptr_t pc, struct am_profile_location* location, void* context) {
	const struct GbnRv32* b = context;
	if ((pc & 3) || pc < (uintptr_t) b->code || pc - (uintptr_t) b->code >= sizeof(b->code)) return false;
	uint32_t word = b->code[(pc - (uintptr_t) b->code) / 4];
	unsigned opcode = word & 0x7f, funct3 = word >> 12 & 7;
	unsigned signature = opcode;
	if (opcode == 0x03 || opcode == 0x13 || opcode == 0x23 || opcode == 0x33 || opcode == 0x63 || opcode == 0x67 || opcode == 0x0f || opcode == 0x73)
		signature |= funct3 << 12;
	unsigned funct7 = opcode == 0x33 || (opcode == 0x13 && (funct3 == 1 || funct3 == 5)) ? word >> 25 : 0;
	*location = (struct am_profile_location) {.kind = 10, .address = signature, .offset = funct7};
	return true;
}
#endif
#endif
#if GBN_RV32_STATS && defined(GBN_COUNTERS)
#define GBN_HOST_STATS 1
#else
#define GBN_HOST_STATS 0
#endif

_Static_assert(GBN_BENCH_FRAMES > 0, "Benchmark requires a positive frame count");
_Static_assert(GBN_BENCH_WARMUP >= 0, "Warmup must be nonnegative");
_Static_assert(GBN_BENCH_VALIDATE == 0 || GBN_BENCH_VALIDATE == 1, "Validation must be 0 or 1");
_Static_assert((uint64_t) GBN_BENCH_WARMUP + GBN_BENCH_FRAMES <= UINT32_MAX, "Frame count overflow");
extern const uint8_t _rom_start[], _rom_end[];
#ifdef AM_ROM_OMIT_FF_TAIL
extern uint8_t _rom_tail_start[];
#endif
struct Input { uint32_t frame; uint16_t keys; };
#include "gba-next-input.inc"

static struct Gbn machine;
static struct GbnDevices devices;
static struct GbnSave save;
static uint8_t ewram[GBN_EWRAM_SIZE], iwram[GBN_IWRAM_SIZE];
static uint8_t palette[GBN_PALETTE_SIZE], vram[GBN_VRAM_SIZE], oam[GBN_OAM_SIZE];
static uint8_t save_data[GBN_SAVE_MAX_SIZE];
static uint16_t pixels[GBN_SCREEN_WIDTH * GBN_SCREEN_HEIGHT];
#if GBN_BENCH_VALIDATE
static uint32_t crc_table[256];
static uint32_t video_crc = UINT32_MAX, audio_crc = UINT32_MAX;
static uint64_t audio_samples, audio_nonzero;

static uint32_t crc_byte(uint32_t crc, uint8_t value) {
	return (crc >> 8) ^ crc_table[(crc ^ value) & 255];
}

static uint32_t crc_bytes(const uint8_t* data, uint32_t size) {
	uint32_t crc = UINT32_MAX;
	for (uint32_t i = 0; i < size; ++i) crc = crc_byte(crc, data[i]);
	return crc ^ UINT32_MAX;
}

static uint32_t crc_pixels(uint32_t crc) {
	for (unsigned i = 0; i < GBN_SCREEN_WIDTH * GBN_SCREEN_HEIGHT; ++i) {
		crc = crc_byte(crc, (uint8_t) pixels[i]);
		crc = crc_byte(crc, (uint8_t) (pixels[i] >> 8));
	}
	return crc;
}
#endif

static void drain_audio(void) {
	/* Avoid a function call at every guest instruction when the queue is empty. */
	if (!devices.audio.size) return;
#if GBN_BENCH_VALIDATE
	struct GbnStereo samples[256];
	unsigned count;
	while ((count = gbn_audio_read(&machine, samples, 256)) != 0) for (unsigned i = 0; i < count; ++i) {
		++audio_samples;
		if (samples[i].left || samples[i].right) ++audio_nonzero;
		uint16_t left = (uint16_t) samples[i].left, right = (uint16_t) samples[i].right;
		audio_crc = crc_byte(audio_crc, (uint8_t) left);
		audio_crc = crc_byte(audio_crc, (uint8_t) (left >> 8));
		audio_crc = crc_byte(audio_crc, (uint8_t) right);
		audio_crc = crc_byte(audio_crc, (uint8_t) (right >> 8));
	}
#else
	/* The headless frontend has no audio sink. Consume the completed queue
	 * without copying or examining PCM; preserve its ring position exactly. */
	devices.audio.read = (devices.audio.read + devices.audio.size) & (GBN_AUDIO_CAPACITY - 1);
	devices.audio.size = 0;
#endif
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	struct am_config config = {.title = "Independent GBA fixed-work benchmark"};
	if (!am_init(&config)) { fprintf(stderr, "AM init: %s\n", am_error()); return 1; }
#ifdef AM_ROM_OMIT_FF_TAIL
	memset(_rom_tail_start, 0xff, (uintptr_t) _rom_end - (uintptr_t) _rom_tail_start);
#endif
	uint32_t rom_size = (uint32_t) ((uintptr_t) _rom_end - (uintptr_t) _rom_start);
#if GBN_BENCH_VALIDATE
	for (unsigned i = 0; i < 256; ++i) {
		uint32_t crc = i;
		for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
		crc_table[i] = crc;
	}
#endif
	gbn_init(&machine, ewram, iwram);
	if (!gbn_attach_rom(&machine, _rom_start, rom_size) ||
		!gbn_attach_devices(&machine, &devices, palette, vram, oam) ||
		!gbn_attach_pixels(&machine, pixels, GBN_SCREEN_WIDTH)) return 1;
	gbn_use_builtin_bios(&machine);
	enum GbnSaveType save_type = gbn_detect_save(_rom_start, rom_size);
	if (save_type != GBN_SAVE_NONE) {
		memset(save_data, 255, sizeof(save_data));
		if (!gbn_attach_save(&machine, &save, save_data, sizeof(save_data), save_type)) return 1;
	}
	machine.cpu.cpsr = 0x1f;
	machine.cpu.r[13] = 0x03007f00;
	machine.cpu.banks[2].sp = 0x03007fa0;
	machine.cpu.banks[3].sp = 0x03007fe0;
	if (gbn_enter_arm(&machine, 0x08000000) != GBN_STEP) return 1;
	gbn_video_start(&machine, true);
	gbn_audio_start(&machine);
#if GBN_RV32
	if (!gbn_rv32_available()) return 1;
	gbn_rv32_init(&backend, &machine);
#endif
#ifdef GBN_PROFILE
#if GBN_RV32
	am_profile_set_resolver(resolve_native, &backend);
#endif
	am_profile_prepare();
#endif
	printf("GBN benchmark: ROM=%" PRIu32 "; warmup=%u; frames=%u; input_records=%u; PPU=on; APU=on; BIOS=builtin; save=RAM\n",
		rom_size, (unsigned) GBN_BENCH_WARMUP, (unsigned) GBN_BENCH_FRAMES, input_count);
#if !GBN_BENCH_VALIDATE
	puts("GBN validation: off; CRC=off; PCM_inspection=off; audio_sink=discard");
#endif
#ifdef AM_SPIKE
	puts("Timing: Spike functional counters; no hardware FPS prediction");
#endif
	uint64_t instructions = 0, cycles = 0, first_instruction = 0, first_cycle = 0;
#if GBN_BENCH_VALIDATE
	uint64_t first_samples = 0;
#endif
	uint64_t start_us = 0, elapsed_us = 0;
#ifdef GBN_COUNTERS
	uint64_t start_cycles = 0, start_retired = 0, host_cycles = 0, host_retired = 0;
#endif
	uint32_t completed = 0, last_cycle = machine.now;
	unsigned next_input = 0;
	bool measuring = false;
#if GBN_HOST_STATS
	uint64_t cpu_retired = 0, event_retired = 0, validation_retired = 0, stamp;
#endif
	enum GbnStatus status = GBN_STEP;
	for (;;) {
		if (!measuring && completed == GBN_BENCH_WARMUP) {
			first_instruction = instructions; first_cycle = cycles;
#if GBN_BENCH_VALIDATE
			first_samples = audio_samples;
#endif
			measuring = true;
			start_us = am_uptime_us();
#ifdef GBN_PROFILE
			am_profile_start();
#endif
#ifdef GBN_COUNTERS
			start_cycles = am_counter_cycles(); start_retired = am_counter_retired();
#endif
		}
		if (next_input < input_count && inputs[next_input].frame <= devices.frames) {
			uint16_t keys = 0;
			while (next_input < input_count && inputs[next_input].frame <= devices.frames) keys = inputs[next_input++].keys;
			gbn_set_keys(&machine, keys);
		}
#if GBN_HOST_STATS
		stamp = am_counter_retired();
#endif
		status = gbn_service_events(&machine);
#if GBN_HOST_STATS
		if (measuring) event_retired += am_counter_retired() - stamp;
		stamp = am_counter_retired();
#endif
		drain_audio();
#if GBN_HOST_STATS
		if (measuring) validation_retired += am_counter_retired() - stamp;
#endif
		cycles += (uint32_t) (machine.now - last_cycle); last_cycle = machine.now;
		if (status != GBN_STEP) break;
		if (devices.ppu.frames != completed) {
			completed = devices.ppu.frames;
#if GBN_HOST_STATS
			stamp = am_counter_retired();
#endif
#if GBN_BENCH_VALIDATE
			video_crc = crc_pixels(video_crc);
#endif
#if GBN_HOST_STATS
			if (measuring) validation_retired += am_counter_retired() - stamp;
#endif
			if (completed == (uint32_t) GBN_BENCH_WARMUP + GBN_BENCH_FRAMES) {
#ifdef GBN_PROFILE
				am_profile_stop();
#endif
#ifdef GBN_COUNTERS
				host_retired = am_counter_retired() - start_retired; host_cycles = am_counter_cycles() - start_cycles;
#endif
				elapsed_us = am_uptime_us() - start_us;
				break;
			}
			if (!measuring && completed % 100 == 0) printf("GBN warmup: %" PRIu32 "/%u\n", completed, (unsigned) GBN_BENCH_WARMUP);
			/* Start the interval at exactly the same guest boundary as its end. */
			if (!measuring && completed == GBN_BENCH_WARMUP) continue;
		}
		if (devices.halted) continue;
		uint32_t batch = 0;
#if GBN_HOST_STATS
		stamp = am_counter_retired();
#endif
#if GBN_RV32
		status = gbn_rv32_run_batch(&backend, 256, &batch);
#else
		status = gbn_run_batch(&machine, 256, &batch);
#endif
#if GBN_HOST_STATS
		if (measuring) cpu_retired += am_counter_retired() - stamp;
#endif
		instructions += batch;
		cycles += (uint32_t) (machine.now - last_cycle); last_cycle = machine.now;
		if (status != GBN_STEP && status != GBN_EVENT) break;
		/* Bound a broken ROM even if it disables LCD events or stops progressing. */
		if (instructions >= ((uint64_t) GBN_BENCH_WARMUP + GBN_BENCH_FRAMES + 1) * 1000000) { status = GBN_DEADLINE; break; }
	}
#ifdef GBN_PROFILE
	/* Also restore the interrupt vector on an unsupported guest operation. */
	am_profile_stop();
#endif
#if GBN_RV32 && GBN_RV32_STATS
	printf("GBN RV32: native_instructions=%" PRIu64 "; fallback_instructions=%" PRIu64 "; compiled_blocks=%" PRIu32 "\n",
		backend.native_instructions, backend.fallback_instructions, backend.compiled_blocks);
	printf("GBN RV32 entries: native=%" PRIu64 "; loop=%" PRIu64 "; loop_instructions=%" PRIu64 "\n",
		backend.native_entries, backend.loop_entries, backend.loop_instructions);
	printf("GBN RV32 cache: segment_flushes=%" PRIu32 "; code_bytes=%" PRIu32 "\n", backend.cache_flushes, backend.code_used * 4);
	printf("GBN RV32 chains: blocks=%" PRIu64 "\n", backend.chained_blocks);
	printf("GBN RV32 fixed loops: merged_instructions=%" PRIu64 "\n", backend.merged_instructions);
	printf("GBN RV32 fallback: bios=%" PRIu64 "; ewram=%" PRIu64 "; iwram=%" PRIu64 "; rom_arm=%" PRIu64 "; rom_thumb=%" PRIu64 "\n",
		backend.fallback_kind[0], backend.fallback_kind[1], backend.fallback_kind[2], backend.fallback_kind[3], backend.fallback_kind[4]);
#endif
#if GBN_HOST_STATS
	printf("GBN host work (diagnostic): cpu=%" PRIu64 "; events=%" PRIu64 "; output_validation=%" PRIu64 "\n",
		cpu_retired, event_retired, validation_retired);
#endif
	printf("GBN result: status=%u; rendered=%" PRIu32 "; vblanks=%" PRIu32 "; lines=%" PRIu32 "; inputs=%u/%u\n",
		(unsigned) status, completed, devices.frames, devices.ppu.lines, next_input, input_count);
	printf("GBN guest: instructions=%" PRIu64 "; cycles=%" PRIu64 "; measured_instructions=%" PRIu64 "; measured_cycles=%" PRIu64 "; pc=%08" PRIx32 "; opcode=%08" PRIx32 "; cpsr=%08" PRIx32 "\n",
		instructions, cycles, instructions - first_instruction, cycles - first_cycle, machine.cpu.pc, machine.cpu.pipe[0], machine.cpu.cpsr);
#if GBN_BENCH_VALIDATE
	printf("GBN video: stream_crc=%08" PRIx32 "; final_crc=%08" PRIx32 "\n", video_crc ^ UINT32_MAX, crc_pixels(UINT32_MAX) ^ UINT32_MAX);
	printf("GBN audio: samples=%" PRIu64 "; measured_samples=%" PRIu64 "; nonzero=%" PRIu64 "; dropped=%" PRIu32 "; pcm_crc=%08" PRIx32 "\n",
		audio_samples, audio_samples - first_samples, audio_nonzero, devices.audio.dropped, audio_crc ^ UINT32_MAX);
	printf("GBN memory: ewram=%08" PRIx32 "; iwram=%08" PRIx32 "; vram=%08" PRIx32 "; palette=%08" PRIx32 "; oam=%08" PRIx32 "; save=%08" PRIx32 "\n",
		crc_bytes(ewram, sizeof(ewram)), crc_bytes(iwram, sizeof(iwram)), crc_bytes(vram, sizeof(vram)),
		crc_bytes(palette, sizeof(palette)), crc_bytes(oam, sizeof(oam)), crc_bytes(save_data, save.size));
#else
	puts("GBN video: validation=off");
	printf("GBN audio: validation=off; produced=%" PRIu32 "; dropped=%" PRIu32 "\n",
		devices.audio.produced, devices.audio.dropped);
	puts("GBN memory: validation=off");
#endif
	for (unsigned r = 0; r < 15; ++r) printf("r%u=%08" PRIx32 "%c", r, machine.cpu.r[r], r % 4 == 3 || r == 14 ? '\n' : ' ');
	bool ok = status == GBN_STEP && completed == (uint32_t) GBN_BENCH_WARMUP + GBN_BENCH_FRAMES && !devices.audio.dropped;
	if (ok) {
#ifdef GBN_COUNTERS
		printf("Counters: cycles=%" PRIu64 "; instret=%" PRIu64 "; sampling="
#ifdef GBN_PROFILE
			"1\n"
#else
			"0\n"
#endif
			, host_cycles, host_retired);
#endif
#ifdef AM_SPIKE
		printf("Spike benchmark: warmup=%u; frames=%u; simulated_elapsed_us=%" PRIu64 "\n", (unsigned) GBN_BENCH_WARMUP, (unsigned) GBN_BENCH_FRAMES, elapsed_us);
#else
		if (!elapsed_us) ok = false;
		else {
			uint64_t fps_milli = (uint64_t) GBN_BENCH_FRAMES * 1000000000 / elapsed_us;
			printf("Benchmark: warmup=%u; frames=%u; elapsed_us=%" PRIu64 "; FPS=%" PRIu64 ".%03" PRIu64 "\n",
				(unsigned) GBN_BENCH_WARMUP, (unsigned) GBN_BENCH_FRAMES, elapsed_us, fps_milli / 1000, fps_milli % 1000);
		}
#endif
	}
#ifdef GBN_PROFILE
	am_profile_report();
#endif
	am_shutdown();
	return ok ? 0 : 2;
}
