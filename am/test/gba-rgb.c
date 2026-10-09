/* SPDX-License-Identifier: MPL-2.0 */
#include <mgba/core/core.h>
#include <mgba/gba/core.h>
#include <mgba/gb/core.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/io.h>
#include <mgba/internal/gba/renderers/video-software.h>
#include <mgba-util/audio-buffer.h>
#include <mgba-util/vfs.h>

enum { WIDTH = 240, HEIGHT = 160, NATIVE_STRIDE = 248, RGB_STRIDE = 251 };
static unsigned checks;
static uint32_t randomState = 0x183D249A;

static uint32_t randomValue(void) {
	randomState ^= randomState << 13;
	randomState ^= randomState >> 17;
	randomState ^= randomState << 5;
	return randomState;
}

static void require(bool condition, const char* label) {
	if (!condition) { fprintf(stderr, "FAIL: %s\n", label); exit(1); }
	++checks;
}

static void quiet(struct mLogger* logger, int category, enum mLogLevel level, const char* format, va_list args) {
	(void) logger; (void) category; (void) level; (void) format; (void) args;
}

static struct mCore* openCore(const void* rom, size_t size, mColor* pixels) {
	struct mCore* core = GBACoreCreate();
	require(core && core->init(core), "core init");
	mCoreInitConfig(core, NULL);
	struct VFile* vf = VFileFromConstMemory(rom, size);
	require(vf && core->loadROM(core, vf), "load ROM");
	core->setVideoBuffer(core, pixels, NATIVE_STRIDE);
	core->rtc.override = RTC_FIXED;
	core->rtc.value = 946684800;
	core->reset(core);
	core->setKeys(core, 0);
	return core;
}

static void closeCore(struct mCore* core) {
	GBACoreSetVideoRGBBuffer(core, NULL, 0);
	mCoreConfigDeinit(&core->config);
	core->deinit(core);
}

static void compare(const mColor* pixels, const uint32_t* rgb) {
	for (unsigned y = 0; y < HEIGHT; ++y) {
		for (unsigned x = 0; x < WIDTH; ++x) {
			uint32_t expected = mColorConvert(pixels[y * NATIVE_STRIDE + x], mCOLOR_NATIVE, mCOLOR_RGB8);
			if (rgb[y * RGB_STRIDE + x] != expected) {
				fprintf(stderr, "pixel %u,%u: %08lx != %08lx\n", x, y,
				        (unsigned long) rgb[y * RGB_STRIDE + x], (unsigned long) expected);
				require(false, "full 32-bit RGB pixel");
			}
		}
		for (unsigned x = WIDTH; x < RGB_STRIDE; ++x) require(rgb[y * RGB_STRIDE + x] == 0xDEADBEEF, "RGB row padding");
	}
	require(rgb[HEIGHT * RGB_STRIDE] == 0xDEADBEEF, "RGB end guard");
	++checks;
}

static uint32_t* allocRGB(void) {
	uint32_t* result = malloc((RGB_STRIDE * HEIGHT + 1) * sizeof(*result));
	require(result != NULL, "RGB allocation");
	for (unsigned i = 0; i <= RGB_STRIDE * HEIGHT; ++i) result[i] = 0xDEADBEEF;
	return result;
}

static void synthetic(void) {
	uint32_t rom[64] = {0xEAFFFFFE};
	mColor* pixels = calloc(NATIVE_STRIDE * HEIGHT, sizeof(*pixels));
	uint32_t* rgb = allocRGB();
	struct mCore* core = openCore(rom, sizeof(rom), pixels);
	struct GBA* gba = core->board;
	struct GBAVideoRenderer* renderer = gba->video.renderer;
	require(GBACoreSetVideoRGBBuffer(core, rgb, RGB_STRIDE), "attach");
	compare(pixels, rgb);
	require(!GBACoreSetVideoRGBBuffer(core, rgb, WIDTH - 1), "short stride rejects");
	require(!GBACoreSetVideoRGBBuffer(NULL, rgb, RGB_STRIDE), "null core rejects");
	struct mCore* gb = GBCoreCreate();
	require(!GBACoreSetVideoRGBBuffer(gb, rgb, RGB_STRIDE), "GB core rejects");
	free(gb);

	/* Exercise all source bits, including renderer flags in the high byte,
	 * through the public pixel restore path and native pixel read interface. */
	mColor* restored = malloc(NATIVE_STRIDE * HEIGHT * sizeof(*restored));
	for (unsigned i = 0; i < NATIVE_STRIDE * HEIGHT; ++i) restored[i] = randomValue();
	core->putPixels(core, restored, NATIVE_STRIDE);
	const void* output;
	size_t stride;
	core->getPixels(core, &output, &stride);
	require(output == pixels && stride == NATIVE_STRIDE, "native pixel interface preserved");
	for (unsigned y = 0; y < HEIGHT; ++y)
		require(!memcmp(pixels + y * NATIVE_STRIDE, restored + y * NATIVE_STRIDE, WIDTH * sizeof(*pixels)), "native restore bits");
	compare(pixels, rgb);
	core->setVideoBuffer(core, restored, NATIVE_STRIDE);
	compare(restored, rgb);
	core->setVideoBuffer(core, pixels, NATIVE_STRIDE);

	for (unsigned i = 0; i < GBA_SIZE_VRAM; i += 2) core->busWrite16(core, GBA_BASE_VRAM + i, randomValue());
	for (unsigned i = 0; i < GBA_SIZE_PALETTE_RAM; i += 2) core->busWrite16(core, GBA_BASE_PALETTE_RAM + i, randomValue());
	for (unsigned i = 0; i < GBA_SIZE_OAM; i += 8) core->busWrite16(core, GBA_BASE_OAM + i, 0x0200);
	for (unsigned mode = 0; mode <= 5; ++mode) {
		for (unsigned stereo = 0; stereo < 2; ++stereo) {
			for (unsigned effect = 0; effect < 4; ++effect) {
				renderer->writeVideoRegister(renderer, GBA_REG_DISPCNT, mode | 0x0F00);
				renderer->writeVideoRegister(renderer, GBA_REG_STEREOCNT, stereo);
				renderer->writeVideoRegister(renderer, GBA_REG_BLDCNT, 0x3F3F | (effect << 6));
				renderer->writeVideoRegister(renderer, GBA_REG_BLDALPHA, 0x0808);
				renderer->writeVideoRegister(renderer, GBA_REG_BLDY, 7);
				for (unsigned repeat = 0; repeat < 2; ++repeat) {
					for (unsigned y = 0; y < HEIGHT; ++y) renderer->drawScanline(renderer, y);
					renderer->finishFrame(renderer);
					compare(pixels, rgb);
				}
			}
		}
	}
	renderer->writeVideoRegister(renderer, GBA_REG_DISPCNT, 0x80);
	for (unsigned y = 0; y < HEIGHT; ++y) renderer->drawScanline(renderer, y);
	compare(pixels, rgb);
	require(GBACoreSetVideoRGBBuffer(core, NULL, 0), "detach");
	uint32_t* detached = malloc((RGB_STRIDE * HEIGHT + 1) * sizeof(*rgb));
	memcpy(detached, rgb, (RGB_STRIDE * HEIGHT + 1) * sizeof(*rgb));
	core->putPixels(core, restored, NATIVE_STRIDE);
	require(!memcmp(detached, rgb, (RGB_STRIDE * HEIGHT + 1) * sizeof(*rgb)), "detached buffer untouched");
	require(GBACoreSetVideoRGBBuffer(core, rgb, RGB_STRIDE), "reattach");
	compare(pixels, rgb);
	core->reset(core);
	compare(pixels, rgb);
	closeCore(core);
	free(detached); free(restored); free(rgb); free(pixels);
	printf("PASS: scanline RGB modes 0-5, blending, stereo, repeated/blank rows, flags, stride, pixel restore, attach/detach and reset\n");
}

#ifndef AM_BAREMETAL
static void romCheck(const char* path, unsigned frames) {
	FILE* file = fopen(path, "rb");
	require(file != NULL, "open ROM");
	fseek(file, 0, SEEK_END);
	size_t size = ftell(file);
	rewind(file);
	void* rom = malloc(size);
	require(rom && fread(rom, 1, size, file) == size, "read ROM");
	fclose(file);
	mColor* videos[2] = {calloc(NATIVE_STRIDE * HEIGHT, sizeof(mColor)), calloc(NATIVE_STRIDE * HEIGHT, sizeof(mColor))};
	struct mCore* cores[2] = {openCore(rom, size, videos[0]), openCore(rom, size, videos[1])};
	uint32_t* rgb = allocRGB();
	require(GBACoreSetVideoRGBBuffer(cores[1], rgb, RGB_STRIDE), "ROM mirror attach");
	size_t stateSize = cores[0]->stateSize(cores[0]);
	void* states[2] = {calloc(1, stateSize), calloc(1, stateSize)};
	for (unsigned frame = 0; frame < frames; ++frame) {
		for (unsigned i = 0; i < 2; ++i) {
			cores[i]->runFrame(cores[i]);
			require(cores[i]->saveState(cores[i], states[i]), "save state");
		}
		require(!memcmp(states[0], states[1], stateSize), "frame serialized state");
		require(!memcmp(videos[0], videos[1], NATIVE_STRIDE * HEIGHT * sizeof(mColor)), "frame native pixels");
		compare(videos[0], rgb);
		struct mAudioBuffer* a = cores[0]->getAudioBuffer(cores[0]);
		struct mAudioBuffer* b = cores[1]->getAudioBuffer(cores[1]);
		require(mAudioBufferAvailable(a) == mAudioBufferAvailable(b), "PCM count");
		while (mAudioBufferAvailable(a)) {
			int16_t pcm[2][512];
			size_t count = mAudioBufferRead(a, pcm[0], 256);
			require(mAudioBufferRead(b, pcm[1], 256) == count && !memcmp(pcm[0], pcm[1], count * 2 * sizeof(int16_t)), "PCM bytes");
		}
		if (frame == frames / 2) {
			for (unsigned i = 0; i < 2; ++i) require(cores[i]->loadState(cores[i], states[i]), "reload state");
		}
	}
	printf("PASS: %s: %u complete frames, native and RGB pixels, PCM and serialized states\n", path, frames);
	for (unsigned i = 0; i < 2; ++i) { closeCore(cores[i]); free(videos[i]); free(states[i]); }
	free(rgb); free(rom);
}
#endif

int main(int argc, char** argv) {
	struct mLogger logger = {.log = quiet};
	mLogSetDefaultLogger(&logger);
	synthetic();
#ifndef AM_BAREMETAL
	for (int i = 1; i < argc; ++i) romCheck(argv[i], 150);
#else
	(void) argc; (void) argv;
#endif
	printf("PASS: %u RGB/state checks\n", checks);
	return 0;
}
