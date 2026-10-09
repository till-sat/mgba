/* SPDX-License-Identifier: MPL-2.0 */
/* Bring-up frontend for the independent core. No mGBA library is linked. */
#include <gba-next/core.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef GBN_SDL
#include <SDL.h>

struct Display {
	SDL_Window* window;
	SDL_Renderer* renderer;
	SDL_Texture* texture;
	uint64_t epoch, frame_ticks;
	uint16_t keys;
	bool running, failed;
};

static bool open_display(struct Display* display, SDL_AudioDeviceID* audio) {
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) return false;
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
	display->window = SDL_CreateWindow("GBA next", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 720, 480, SDL_WINDOW_RESIZABLE);
	if (!display->window) return false;
	display->renderer = SDL_CreateRenderer(display->window, -1, SDL_RENDERER_ACCELERATED);
	if (!display->renderer) display->renderer = SDL_CreateRenderer(display->window, -1, SDL_RENDERER_SOFTWARE);
	if (!display->renderer || SDL_RenderSetLogicalSize(display->renderer, 240, 160)) return false;
	display->texture = SDL_CreateTexture(display->renderer, SDL_PIXELFORMAT_BGR555, SDL_TEXTUREACCESS_STREAMING, 240, 160);
	if (!display->texture) return false;
	SDL_AudioSpec desired = {0};
	desired.freq = 32768; desired.format = AUDIO_S16SYS; desired.channels = 2; desired.samples = 512;
	*audio = SDL_OpenAudioDevice(NULL, 0, &desired, NULL, 0);
	if (!*audio) return false;
	SDL_PauseAudioDevice(*audio, 0);
	display->epoch = SDL_GetPerformanceCounter();
	display->frame_ticks = SDL_GetPerformanceFrequency() * 280896 / 16777216;
	display->running = true;
	return true;
}

static void show_frame(struct Display* display, const uint16_t* pixels, uint32_t frame) {
	if (SDL_UpdateTexture(display->texture, NULL, pixels, 240 * (int) sizeof(*pixels)) ||
		SDL_RenderClear(display->renderer) || SDL_RenderCopy(display->renderer, display->texture, NULL, NULL)) {
		display->failed = true; display->running = false; return;
	}
	SDL_RenderPresent(display->renderer);
	uint64_t target = display->epoch + frame * display->frame_ticks;
	for (;;) {
		uint64_t now = SDL_GetPerformanceCounter();
		if (now >= target) break;
		uint64_t ms = (target - now) * 1000 / SDL_GetPerformanceFrequency();
		SDL_Delay((uint32_t) (ms > 10 ? 10 : ms ? ms : 1));
	}
	SDL_Event event;
	while (SDL_PollEvent(&event)) if (event.type == SDL_QUIT) display->running = false;
	const uint8_t* keys = SDL_GetKeyboardState(NULL);
	if (keys[SDL_SCANCODE_ESCAPE]) display->running = false;
	display->keys = (uint16_t) (keys[SDL_SCANCODE_Z] | keys[SDL_SCANCODE_X] << 1 | keys[SDL_SCANCODE_BACKSPACE] << 2 |
		keys[SDL_SCANCODE_RETURN] << 3 | keys[SDL_SCANCODE_RIGHT] << 4 | keys[SDL_SCANCODE_LEFT] << 5 |
		keys[SDL_SCANCODE_UP] << 6 | keys[SDL_SCANCODE_DOWN] << 7 | keys[SDL_SCANCODE_S] << 8 | keys[SDL_SCANCODE_A] << 9);
}
#endif

struct AudioSink {
	FILE* file;
	uint64_t inputs, nonzero;
	uint32_t crc, frames, next_when;
	struct GbnStereo held;
	bool started, failed;
#ifdef GBN_SDL
	SDL_AudioDeviceID device;
	int16_t playback[512];
	unsigned playback_frames;
#endif
};

struct Input { uint32_t frame; uint16_t keys; };

static struct Input* load_inputs(const char* path, unsigned* count) {
	FILE* file = fopen(path, "r");
	if (!file) { perror(path); return NULL; }
	struct Input* inputs = malloc(4096 * sizeof(*inputs));
	if (!inputs) { fclose(file); return NULL; }
	char line[256]; *count = 0;
	while (fgets(line, sizeof(line), file)) {
		char* start = line;
		while (*start == ' ' || *start == '\t') ++start;
		if (!*start || *start == '\n' || *start == '#') continue;
		char* end;
		errno = 0;
		unsigned long long frame = strtoull(start, &end, 10);
		if (errno || end == start || *start == '-' || frame > UINT32_MAX || (*end != ' ' && *end != '\t')) goto invalid;
		start = end;
		while (*start == ' ' || *start == '\t') ++start;
		errno = 0;
		unsigned long keys = strtoul(start, &end, 16);
		if (errno || end == start || *start == '-' || keys > 0x3ff) goto invalid;
		while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') ++end;
		if ((*end && *end != '#') || *count == 4096 || (*count && frame < inputs[*count - 1].frame)) goto invalid;
		inputs[(*count)++] = (struct Input) {(uint32_t) frame, (uint16_t) keys};
	}
	if (ferror(file)) goto invalid;
	fclose(file); return inputs;
invalid:
	fprintf(stderr, "Invalid input script: %s (decimal VBlank counter, hexadecimal pressed-key mask; ascending order, max 4096 records)\n", path);
	fclose(file); free(inputs); return NULL;
}

static bool write_frame(const char* path, const uint16_t* pixels) {
	FILE* file = fopen(path, "wb");
	if (!file) { perror(path); return false; }
	bool ok = fprintf(file, "P6\n240 160\n255\n") > 0;
	for (unsigned i = 0; i < GBN_SCREEN_WIDTH * GBN_SCREEN_HEIGHT && ok; ++i) {
		for (unsigned component = 0; component < 3; ++component) {
			unsigned value = pixels[i] >> (component * 5) & 31;
			if (fputc((int) (value << 3 | value >> 2), file) == EOF) { ok = false; break; }
		}
	}
	if (fclose(file)) ok = false;
	return ok;
}

static bool little(FILE* file, uint32_t value, unsigned width) {
	for (unsigned i = 0; i < width; ++i) if (fputc((int) (value >> (8 * i) & 255), file) == EOF) return false;
	return true;
}

static bool wav_header(FILE* file, uint32_t frames) {
	uint32_t bytes = frames * 4;
	return fwrite("RIFF", 1, 4, file) == 4 && little(file, 36 + bytes, 4) &&
		fwrite("WAVEfmt ", 1, 8, file) == 8 && little(file, 16, 4) && little(file, 1, 2) && little(file, 2, 2) &&
		little(file, 32768, 4) && little(file, 32768 * 4, 4) && little(file, 4, 2) && little(file, 16, 2) &&
		fwrite("data", 1, 4, file) == 4 && little(file, bytes, 4);
}

static void drain_audio(struct Gbn* m, struct AudioSink* sink) {
	struct GbnStereo samples[256];
	unsigned count;
	while ((count = gbn_audio_read(m, samples, 256)) != 0) for (unsigned i = 0; i < count; ++i) {
		struct GbnStereo sample = samples[i];
		++sink->inputs;
		if (sample.left || sample.right) ++sink->nonzero;
		for (unsigned byte = 0; byte < 4; ++byte) {
			uint16_t value = (uint16_t) (byte < 2 ? sample.left : sample.right);
			sink->crc ^= value >> (8 * (byte & 1)) & 255;
			for (unsigned bit = 0; bit < 8; ++bit) sink->crc = (sink->crc >> 1) ^ (0xedb88320u & (0u - (sink->crc & 1)));
		}
		if (sink->failed) continue;
#ifdef GBN_SDL
		if (!sink->file && !sink->device) continue;
#else
		if (!sink->file) continue;
#endif
		if (!sink->started) { sink->started = true; sink->held = sample; sink->next_when = sample.when; }
		/* Capture at 32768 Hz with a zero-order hold. Timestamps, rather than
		 * the current IO register, determine rate changes in buffered audio. */
		while ((int32_t) (sample.when - sink->next_when) >= 0) {
			struct GbnStereo output = sink->next_when == sample.when ? sample : sink->held;
			if (sink->file && (sink->frames >= (UINT32_MAX - 36) / 4 || !little(sink->file, (uint16_t) output.left, 2) ||
				!little(sink->file, (uint16_t) output.right, 2))) { sink->failed = true; break; }
#ifdef GBN_SDL
			if (sink->device) {
				sink->playback[sink->playback_frames * 2] = output.left;
				sink->playback[sink->playback_frames * 2 + 1] = output.right;
				if (++sink->playback_frames == 256) {
					if (SDL_QueueAudio(sink->device, sink->playback, sizeof(sink->playback))) sink->failed = true;
					sink->playback_frames = 0;
				}
			}
#endif
			++sink->frames;
			sink->next_when += 512;
		}
		sink->held = sample;
	}
}

static uint8_t* load_image(const char* path, uint32_t limit, uint32_t* size) {
	FILE* file = fopen(path, "rb");
	if (!file) { perror(path); return NULL; }
	if (fseek(file, 0, SEEK_END)) { fclose(file); return NULL; }
	long length = ftell(file);
	if (length <= 0 || (unsigned long) length > limit || fseek(file, 0, SEEK_SET)) {
		fprintf(stderr, "Invalid image size: %s\n", path); fclose(file); return NULL;
	}
	uint8_t* image = malloc((size_t) length);
	if (!image || fread(image, 1, (size_t) length, file) != (size_t) length) {
		fprintf(stderr, "Cannot read image: %s\n", path); free(image); fclose(file); return NULL;
	}
	fclose(file);
	*size = (uint32_t) length;
	return image;
}

static bool write_save(const char* path, const struct GbnSave* save) {
	size_t length = strlen(path);
	char* temporary = malloc(length + 5);
	if (!temporary) return false;
	memcpy(temporary, path, length); memcpy(temporary + length, ".tmp", 5);
	FILE* file = fopen(temporary, "wbx");
	if (!file) { perror(temporary); free(temporary); return false; }
	bool ok = fwrite(save->data, 1, save->size, file) == save->size;
	if (fclose(file)) ok = false;
	if (ok && rename(temporary, path)) { perror(path); ok = false; }
	if (!ok) remove(temporary);
	free(temporary);
	return ok;
}

int main(int argc, char** argv) {
	const char* rom_path = NULL;
	const char* bios_path = NULL;
	const char* wav_path = NULL;
	const char* save_path = NULL;
	const char* frame_path = NULL;
	const char* input_path = NULL;
	struct AudioSink audio = {.crc = UINT32_MAX};
	struct GbnSave save = {0};
	uint8_t* save_storage = NULL;
	uint64_t limit = 10000000;
	uint64_t cycle_limit = GBN_MAX_DELAY;
	uint64_t frame_limit = UINT32_MAX;
	bool display_requested = false, steps_set = false, cycles_set = false;
#ifdef GBN_SDL
	struct Display display = {0};
#endif
	for (int i = 1; i < argc; ++i) {
		if (!strcmp(argv[i], "--bios") && i + 1 < argc) bios_path = argv[++i];
		else if (!strcmp(argv[i], "--wav") && i + 1 < argc) wav_path = argv[++i];
		else if (!strcmp(argv[i], "--save") && i + 1 < argc) save_path = argv[++i];
		else if (!strcmp(argv[i], "--frame") && i + 1 < argc) frame_path = argv[++i];
		else if (!strcmp(argv[i], "--input") && i + 1 < argc) input_path = argv[++i];
		else if (!strcmp(argv[i], "--display")) display_requested = true;
		else if ((!strcmp(argv[i], "--steps") || !strcmp(argv[i], "--cycles") || !strcmp(argv[i], "--frames")) && i + 1 < argc) {
			bool cycles = !strcmp(argv[i], "--cycles");
			bool frames = !strcmp(argv[i], "--frames");
			char* end;
			const char* value = argv[++i];
			errno = 0;
			uint64_t parsed = strtoull(value, &end, 10);
			if (errno || *end || !*value || *value == '-' || !parsed) { fputs("Invalid execution limit\n", stderr); return 1; }
			if (cycles) { cycle_limit = parsed; cycles_set = true; } else if (frames && parsed <= UINT32_MAX) frame_limit = parsed;
			else if (frames) { fputs("Invalid frame limit\n", stderr); return 1; } else { limit = parsed; steps_set = true; }
		} else if (argv[i][0] != '-' && !rom_path) rom_path = argv[i];
		else {
			fprintf(stderr, "Usage: %s [--steps N] [--cycles N] [--frames N] [--bios FILE] [--wav FILE] [--save FILE] [--frame FILE.ppm] [--input FILE] ROM\n", argv[0]); return 1;
		}
	}
	if (!rom_path) { fprintf(stderr, "Usage: %s [options] ROM (use --frames, --frame, --input, --wav, --save)\n", argv[0]); return 1; }
	if (display_requested) {
#ifndef GBN_SDL
		fputs("Display requires a native build with GBN_DISPLAY=1\n", stderr); return 1;
#else
		if (!steps_set) limit = UINT64_MAX;
		if (!cycles_set) cycle_limit = UINT64_MAX;
#endif
	}
#ifndef GBN_SDL
	(void) steps_set; (void) cycles_set;
#endif
	uint32_t rom_size, bios_size = 0;
	uint8_t* rom = load_image(rom_path, GBN_ROM_MAX_SIZE, &rom_size);
	uint8_t* bios = bios_path ? load_image(bios_path, GBN_BIOS_SIZE, &bios_size) : NULL;
	struct Gbn* m = calloc(1, sizeof(*m));
	struct GbnDevices* devices = calloc(1, sizeof(*devices));
	uint8_t* ram = calloc(1, GBN_EWRAM_SIZE + GBN_IWRAM_SIZE);
	uint8_t* video = calloc(1, GBN_PALETTE_SIZE + GBN_VRAM_SIZE + GBN_OAM_SIZE);
	uint16_t* pixels = calloc(2 * GBN_SCREEN_WIDTH * GBN_SCREEN_HEIGHT, sizeof(*pixels));
	struct Input* inputs = NULL;
	unsigned input_count = 0, next_input = 0;
	uint32_t captured = 0;
	uint16_t replay_keys = 0;
	int result = 1;
	if (!rom || (bios_path && !bios) || !m || !ram || !devices || !video || !pixels) goto finish;
	if (input_path && !(inputs = load_inputs(input_path, &input_count))) goto finish;
	gbn_init(m, ram, ram + GBN_EWRAM_SIZE);
	if (!gbn_attach_rom(m, rom, rom_size)) goto finish;
	if (bios && !gbn_attach_bios(m, bios, bios_size)) { fputs("BIOS must be 16384 bytes\n", stderr); goto finish; }
	if (!bios) gbn_use_builtin_bios(m);
	if (!gbn_attach_devices(m, devices, video, video + GBN_PALETTE_SIZE, video + GBN_PALETTE_SIZE + GBN_VRAM_SIZE)) goto finish;
	if (!gbn_attach_pixels(m, pixels, GBN_SCREEN_WIDTH)) goto finish;
	enum GbnSaveType save_type = gbn_detect_save(rom, rom_size);
	if (save_type != GBN_SAVE_NONE) {
		save_storage = malloc(GBN_SAVE_MAX_SIZE);
		if (!save_storage) goto finish;
		memset(save_storage, 255, GBN_SAVE_MAX_SIZE);
		if (!gbn_attach_save(m, &save, save_storage, GBN_SAVE_MAX_SIZE, save_type)) goto finish;
	} else if (save_path) { fputs("ROM has no recognized save signature\n", stderr); goto finish; }
	if (save_path) {
		FILE* file = fopen(save_path, "rb");
		if (!file && errno != ENOENT) { perror(save_path); goto finish; }
		if (file) {
			if (fseek(file, 0, SEEK_END)) { fclose(file); goto finish; }
			long size = ftell(file);
			bool valid = size == (long) save.size || (save.type == GBN_SAVE_EEPROM && size == 8192) ||
				(save.type == GBN_SAVE_FLASH64 && size == 0x20000);
			if (!valid || fseek(file, 0, SEEK_SET) || fread(save.data, 1, (size_t) size, file) != (size_t) size) {
				fputs("Invalid save image\n", stderr); fclose(file); goto finish;
			}
			fclose(file);
			save.size = (uint32_t) size;
			if (save.type == GBN_SAVE_EEPROM && size == 8192) save.address_bits = 14;
			if (save.type == GBN_SAVE_FLASH64 && size == 0x20000) save.type = GBN_SAVE_FLASH128;
		}
	}
	/* Reset into supplied firmware, or bootstrap the independent BIOS service
	 * stack convention without executing a manufacturer boot animation. */
	m->cpu.cpsr = bios ? 0xd3 : 0x1f;
	if (!bios) {
		m->cpu.r[13] = 0x03007f00;
		m->cpu.banks[2].sp = 0x03007fa0;
		m->cpu.banks[3].sp = 0x03007fe0;
	}
	if (gbn_enter_arm(m, bios ? 0 : 0x08000000) != GBN_STEP) goto finish;
	gbn_video_start(m, !bios);
	gbn_audio_start(m);
	if (wav_path) {
		audio.file = fopen(wav_path, "wb");
		if (!audio.file) { perror(wav_path); goto finish; }
		if (!wav_header(audio.file, 0)) { fputs("Cannot write WAV header\n", stderr); goto finish; }
	}
#ifdef GBN_SDL
	if (display_requested && !open_display(&display, &audio.device)) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); goto finish; }
#endif
	struct { uint32_t pc, opcode, cpsr, cycle; } recent[32] = {{0}};
	uint64_t instructions = 0;
	uint64_t cycles = 0;
	uint32_t last_cycle = m->now;
	bool attempted = false;
	enum GbnStatus status = GBN_STEP;
	while (instructions < limit) {
		cycles += (uint32_t) (m->now - last_cycle); last_cycle = m->now;
		if (cycles >= cycle_limit) { status = GBN_DEADLINE; break; }
		attempted = false;
		if (next_input < input_count && inputs[next_input].frame <= devices->frames) {
			while (next_input < input_count && inputs[next_input].frame <= devices->frames) replay_keys = inputs[next_input++].keys;
#ifdef GBN_SDL
			gbn_set_keys(m, replay_keys | display.keys);
#else
			gbn_set_keys(m, replay_keys);
#endif
		}
		status = gbn_service_events(m);
		drain_audio(m, &audio);
		if (status != GBN_STEP) break;
		if (devices->ppu.frames != captured) {
			captured = devices->ppu.frames;
			memcpy(pixels + GBN_SCREEN_WIDTH * GBN_SCREEN_HEIGHT, pixels, GBN_SCREEN_WIDTH * GBN_SCREEN_HEIGHT * sizeof(*pixels));
#ifdef GBN_SDL
			if (display_requested) {
				show_frame(&display, pixels, captured);
				gbn_set_keys(m, replay_keys | display.keys);
				if (!display.running) break;
			}
#endif
		}
		if (captured >= frame_limit) break;
		if (audio.failed) { status = GBN_INVALID_ARGUMENT; break; }
		if (devices->halted) continue;
		unsigned slot = (unsigned) (instructions % 32);
		recent[slot].pc = m->cpu.pc;
		recent[slot].opcode = m->cpu.pipe[0];
		recent[slot].cpsr = m->cpu.cpsr;
		recent[slot].cycle = m->now;
		attempted = true;
		status = gbn_step(m);
		if (status != GBN_STEP) break;
		++instructions;
	}
	static const char* const names[] = {"instruction-limit", "event", "deadline", "unsupported-instruction",
		"unsupported-address", "unsupported-mode", "invalid-argument", "unsupported-device"};
	const char* stop = status == GBN_STEP && captured >= frame_limit ? "frame-limit" : names[status];
#ifdef GBN_SDL
	if (display_requested && !display.running) stop = display.failed ? "display-error" : "window-closed";
#endif
	printf("GBN stop=%s instructions=%" PRIu64 " cycles=%" PRIu32 " pc=%08" PRIx32
		" opcode=%08" PRIx32 " cpsr=%08" PRIx32 " rom_bytes=%" PRIu32 "\n",
		stop, instructions, m->now, m->cpu.pc, m->cpu.pipe[0], m->cpu.cpsr, rom_size);
	printf("devices vcount=%u dispstat=%04x dispcnt=%04x vblanks=%" PRIu32 " ie=%04x if=%04x ime=%u\n",
		devices->io[3], devices->io[2], devices->io[0], devices->frames,
		devices->io[0x100], devices->io[0x101], devices->io[0x104]);
	printf("audio rate=%u samples=%" PRIu32 " dropped=%" PRIu32 " pcm_crc=%08" PRIx32 " nonzero=%" PRIu64
		" status=%04x fifo_requests=%" PRIu32 "/%" PRIu32 "\n",
		gbn_audio_rate(m), devices->audio.produced, devices->audio.dropped, audio.crc ^ UINT32_MAX, audio.nonzero, devices->io[0x84 / 2],
		devices->audio.fifo[0].requests, devices->audio.fifo[1].requests);
	printf("save type=%u bytes=%" PRIu32 " dirty=%u busy=%u phase=%u\n", (unsigned) save.type, save.size, save.dirty, save.busy, save.phase);
	printf("video rendered_frames=%" PRIu32 " rendered_lines=%" PRIu32 " captured=%" PRIu32 " inputs=%u/%u\n", devices->ppu.frames, devices->ppu.lines, captured, next_input, input_count);
	for (unsigned r = 0; r < 15; ++r) printf("r%u=%08" PRIx32 "%c", r, m->cpu.r[r], r % 4 == 3 || r == 14 ? '\n' : ' ');
	if (status != GBN_STEP) {
		printf("stop_phase=%s\n", attempted ? "instruction" : "device-event");
		if (status == GBN_UNSUPPORTED_DEVICE && m->builtin_bios && m->cpu.pc == 0x20) printf("bios_service=%02" PRIx32 "\n", m->cpu.r[12]);
		if (!attempted && devices->active_dma >= 0) {
			struct GbnDma* dma = &devices->dma[devices->active_dma];
			printf("dma channel=%d source=%08" PRIx32 " dest=%08" PRIx32 " remaining=%" PRIu32 " control=%04x\n",
				devices->active_dma, dma->next_source, dma->next_dest, dma->remaining, dma->control);
		}
		uint64_t end = instructions + (attempted ? 1 : 0);
		uint64_t first = end > 32 ? end - 32 : 0;
		for (uint64_t n = first; n < end; ++n) {
			unsigned slot = (unsigned) (n % 32);
			printf("trace pc=%08" PRIx32 " opcode=%08" PRIx32 " cpsr=%08" PRIx32 " cycles=%" PRIu32 "\n",
				recent[slot].pc, recent[slot].opcode, recent[slot].cpsr, recent[slot].cycle);
		}
	}
	result = status == GBN_STEP ? 0 : 2;
finish:
#ifdef GBN_SDL
	if (display_requested) {
		if (display.failed) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); result = 1; }
		if (audio.device) SDL_CloseAudioDevice(audio.device);
		SDL_DestroyTexture(display.texture); SDL_DestroyRenderer(display.renderer); SDL_DestroyWindow(display.window); SDL_Quit();
	}
#endif
	if (frame_path && (!captured || !write_frame(frame_path, pixels + GBN_SCREEN_WIDTH * GBN_SCREEN_HEIGHT))) { fputs("No complete frame or frame output failed\n", stderr); result = 1; }
	if (save_path && save.dirty && !write_save(save_path, &save)) { fputs("Save output failed\n", stderr); result = 1; }
	if (audio.file) {
		if (fseek(audio.file, 0, SEEK_SET) || !wav_header(audio.file, audio.frames)) audio.failed = true;
		if (fclose(audio.file)) audio.failed = true;
		if (audio.failed) { fputs("WAV output failed\n", stderr); result = 1; }
	}
	free(inputs); free(pixels); free(save_storage); free(video); free(devices); free(ram); free(m); free(bios); free(rom);
	return result;
}
