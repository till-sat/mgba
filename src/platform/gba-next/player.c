/* SPDX-License-Identifier: MPL-2.0 */
/* Interactive AM frontend for the independent core. The fixed-work benchmark
 * stays separate: host pacing, keyboard and physical output live here. */
#include <gba-next/core.h>
#if GBN_RV32
#include <gba-next/rv32.h>
static struct GbnRv32 backend;
#endif
#include <am.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

extern const uint8_t _rom_start[], _rom_end[];
#ifdef AM_ROM_OMIT_FF_TAIL
extern uint8_t _rom_tail_start[];
#endif
static struct Gbn machine;
static struct GbnDevices devices;
static struct GbnSave save;
static uint8_t ewram[GBN_EWRAM_SIZE], iwram[GBN_IWRAM_SIZE];
static uint8_t palette[GBN_PALETTE_SIZE], vram[GBN_VRAM_SIZE], oam[GBN_OAM_SIZE];
static uint8_t save_data[GBN_SAVE_MAX_SIZE];
static uint16_t pixels[GBN_SCREEN_WIDTH * GBN_SCREEN_HEIGHT];
static uint32_t rgb[GBN_SCREEN_WIDTH * GBN_SCREEN_HEIGHT];
static bool running = true, failed;
static uint16_t keys;
static struct {
	struct GbnStereo held;
	uint32_t next_when, fraction, rate, step, remainder;
	int16_t samples[256 * 2];
	unsigned size;
	bool started;
	uint64_t submitted;
} audio;

static void poll_input(void) {
	struct am_input input = am_input_read();
	if (input.quit) running = false;
	uint32_t b = input.buttons;
	uint16_t next = (b & AM_BUTTON_A ? 1 : 0) | (b & AM_BUTTON_B ? 2 : 0) |
		(b & AM_BUTTON_SELECT ? 4 : 0) | (b & AM_BUTTON_START ? 8 : 0) |
		(b & AM_BUTTON_RIGHT ? 16 : 0) | (b & AM_BUTTON_LEFT ? 32 : 0) |
		(b & AM_BUTTON_UP ? 64 : 0) | (b & AM_BUTTON_DOWN ? 128 : 0) |
		(b & AM_BUTTON_R ? 256 : 0) | (b & AM_BUTTON_L ? 512 : 0);
	if (next != keys) { keys = next; gbn_set_keys(&machine, keys); }
}

static bool flush_audio(void) {
	unsigned offset = 0;
	while (offset < audio.size && running) {
		size_t written = am_audio_write(audio.samples + offset * 2, audio.size - offset);
		if (written) { offset += (unsigned) written; audio.submitted += written; }
		else if (am_audio_queued() < am_capabilities()->audio_capacity) {
			fprintf(stderr, "Audio: %s\n", am_error()); failed = true; return false;
		} else { am_sleep_us(1000); poll_input(); }
	}
	audio.size = 0;
	return running;
}

static bool drain_audio(void) {
	if (!devices.audio.size) return true;
	struct GbnStereo samples[256];
	unsigned count;
	while ((count = gbn_audio_read(&machine, samples, 256)) != 0) {
		if (!am_capabilities()->audio) continue;
		for (unsigned i = 0; i < count; ++i) {
			struct GbnStereo sample = samples[i];
			if (!audio.started) { audio.started = true; audio.held = sample; audio.next_when = sample.when; }
			/* Zero-order hold using guest timestamps, including SOUNDBIAS
			 * sample-rate changes and timestamp wrap. Keep the fractional
			 * cycle remainder for devices that negotiate e.g. 44100 Hz. */
			while ((int32_t) (sample.when - audio.next_when) >= 0) {
				struct GbnStereo value = sample.when == audio.next_when ? sample : audio.held;
				audio.samples[audio.size * 2] = value.left;
				audio.samples[audio.size * 2 + 1] = value.right;
				if (++audio.size == 256 && !flush_audio()) return false;
				audio.next_when += audio.step;
				audio.fraction += audio.remainder;
				if (audio.fraction >= audio.rate) { audio.fraction -= audio.rate; ++audio.next_when; }
			}
			audio.held = sample;
		}
	}
	return true;
}

static bool present(void) {
	for (unsigned i = 0; i < GBN_SCREEN_WIDTH * GBN_SCREEN_HEIGHT; ++i) {
		unsigned p = pixels[i], r = p & 31, g = p >> 5 & 31, b = p >> 10 & 31;
		rgb[i] = ((r << 3 | r >> 2) << 16) | ((g << 3 | g >> 2) << 8) | (b << 3 | b >> 2);
	}
	if (am_video_present(rgb, GBN_SCREEN_WIDTH, GBN_SCREEN_HEIGHT, GBN_SCREEN_WIDTH)) return true;
	fprintf(stderr, "Video: %s\n", am_error()); failed = true; return false;
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	struct am_config config = {.title = "GBA next", .width = GBN_SCREEN_WIDTH, .height = GBN_SCREEN_HEIGHT,
		.video = true, .audio = GBN_PLAYER_AUDIO != 0, .audio_rate = 32768};
	if (!am_init(&config)) { fprintf(stderr, "AM init: %s\nUse GBN_PLAYER_AUDIO=0 if audio is unavailable.\n", am_error()); return 1; }
	if (!am_capabilities()->video || !am_capabilities()->buttons) { fputs("Display and buttons are required\n", stderr); am_shutdown(); return 1; }
	if (am_capabilities()->audio) {
		audio.rate = am_capabilities()->audio_rate;
		if (audio.rate < 8000 || audio.rate > 192000 || !am_capabilities()->audio_capacity) {
			fputs("Unsupported playback rate or queue\n", stderr); am_shutdown(); return 1;
		}
		audio.step = 16777216u / audio.rate; audio.remainder = 16777216u % audio.rate;
	}
#ifdef AM_ROM_OMIT_FF_TAIL
	memset(_rom_tail_start, 0xff, (uintptr_t) _rom_end - (uintptr_t) _rom_tail_start);
#endif
	uint32_t rom_size = (uint32_t) ((uintptr_t) _rom_end - (uintptr_t) _rom_start);
	gbn_init(&machine, ewram, iwram);
	bool ready = gbn_attach_rom(&machine, _rom_start, rom_size) &&
		gbn_attach_devices(&machine, &devices, palette, vram, oam) &&
		gbn_attach_pixels(&machine, pixels, GBN_SCREEN_WIDTH);
	gbn_use_builtin_bios(&machine);
	enum GbnSaveType save_type = gbn_detect_save(_rom_start, rom_size);
	if (save_type != GBN_SAVE_NONE) {
		memset(save_data, 255, sizeof(save_data));
		ready = ready && gbn_attach_save(&machine, &save, save_data, sizeof(save_data), save_type);
	}
	machine.cpu.cpsr = 0x1f;
	machine.cpu.r[13] = 0x03007f00;
	machine.cpu.banks[2].sp = 0x03007fa0;
	machine.cpu.banks[3].sp = 0x03007fe0;
	ready = ready && gbn_enter_arm(&machine, 0x08000000) == GBN_STEP;
#if GBN_RV32
	ready = ready && gbn_rv32_available();
	gbn_rv32_init(&backend, &machine);
#endif
	if (!ready) { fputs("Cannot initialize independent GBA core\n", stderr); am_shutdown(); return 1; }
	gbn_video_start(&machine, true);
	gbn_audio_start(&machine);
	printf("GBA next: ROM=%" PRIu32 " bytes; CPU=%s; audio=%u Hz; save=RAM\n", rom_size,
		GBN_RV32 ? "RV32 native" : "interpreter", audio.rate);
	puts("Arrows: move; Z/X: A/B; A/S: L/R; Enter: Start; Backspace: Select; Esc: quit");
	if (!present()) { am_shutdown(); return 1; }
	uint64_t epoch = am_uptime_us();
	uint32_t completed = 0, poll = 0;
	enum GbnStatus status = GBN_STEP;
	while (running) {
		if (!(poll++ & 63)) { poll_input(); if (!running) break; }
		status = gbn_service_events(&machine);
		if (!drain_audio()) break;
		if (status != GBN_STEP) break;
		if (devices.ppu.frames != completed) {
			completed = devices.ppu.frames;
			if (!present()) break;
			poll_input();
#if GBN_PLAYER_FRAMES > 0
			if (completed >= (uint32_t) GBN_PLAYER_FRAMES) break;
#endif
			uint64_t frame_us = (uint64_t) completed * 280896 * 1000000 / 16777216;
			uint64_t now = am_uptime_us(), target = epoch + frame_us;
			/* A slow simulator follows its own pace. Avoid a burst of
			 * catch-up frames after a host pause or window drag. */
			if (now > target && now - target > 100000) epoch = now - frame_us;
			while (running && now < target) {
				am_sleep_us(target - now > 2000 ? 2000 : target - now);
				poll_input(); now = am_uptime_us();
			}
		}
		if (!running) break;
		if (devices.halted) continue;
		uint32_t executed;
#if GBN_RV32
		status = gbn_rv32_run_batch(&backend, 256, &executed);
#else
		status = gbn_run_batch(&machine, 256, &executed);
#endif
		if (status != GBN_STEP && status != GBN_EVENT) break;
	}
	if (running && !failed && status == GBN_STEP) flush_audio();
	bool ok = !failed && (!running || status == GBN_STEP) && !devices.audio.dropped;
	printf("GBA next stop: %s; frames=%" PRIu32 "; status=%u; pc=%08" PRIx32 "; audio_submitted=%" PRIu64 "; audio_dropped=%" PRIu32 "\n",
		!running ? "window-closed" : ok ? "frame-limit" : "error", completed, (unsigned) status,
		machine.cpu.pc, audio.submitted, devices.audio.dropped);
	am_shutdown();
	return ok ? 0 : 2;
}
