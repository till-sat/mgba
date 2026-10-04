/* SPDX-License-Identifier: MPL-2.0 */
#ifndef AM_SIM_MEDIA_DEVICE_H
#define AM_SIM_MEDIA_DEVICE_H
#include <am.h>
#include <SDL.h>
#include "media.h"
#include <array>
#include <cstdio>

class AMMediaDevice {
public:
	~AMMediaDevice() { if (active) am_shutdown(); }
	explicit AMMediaDevice(const char* name) : title(name) {}
	void tick() {
		/* Keep the window responsive while the guest computes a frame. */
		if (active && SDL_GetTicks64() >= next_poll) {
			am_input_read();
			next_poll = SDL_GetTicks64() + 8;
		}
	}
	bool load(uint32_t offset, size_t len, uint8_t* bytes) {
		if (len != 4 || (offset & 3)) return false;
		uint32_t value;
		switch (offset) {
		case AM_SIM_ID: value = AM_SIM_MAGIC; break;
		case AM_SIM_STATUS: value = status; break;
		case AM_SIM_WIDTH: value = width; break;
		case AM_SIM_HEIGHT: value = height; break;
		case AM_SIM_QUIT: value = active && am_input_read().quit; break;
		case AM_SIM_TIME_LO: clock = am_uptime_us(); value = clock; break;
		case AM_SIM_TIME_HI: value = clock >> 32; break;
		case AM_SIM_AUDIO_RATE: value = active ? am_capabilities()->audio_rate : audio_rate; break;
		case AM_SIM_AUDIO_CAP: value = am_capabilities()->audio_capacity; break;
		case AM_SIM_AUDIO_QUEUED: value = am_audio_queued(); break;
		case AM_SIM_AUDIO_WRITTEN: value = audio_written; break;
		default: return false;
		}
		for (unsigned i = 0; i < 4; ++i) bytes[i] = value >> (8 * i);
		return true;
	}
	bool store(uint32_t offset, size_t len, const uint8_t* bytes) {
		if (len != 4 || (offset & 3)) return false;
		uint32_t value = 0;
		for (unsigned i = 0; i < 4; ++i) value |= uint32_t(bytes[i]) << (8 * i);
		if (offset >= AM_SIM_FB && offset < AM_SIM_FB + pixels.size() * 4) {
			pixels[(offset - AM_SIM_FB) / 4] = value;
			return true;
		}
		if (offset >= AM_SIM_PCM && offset < AM_SIM_PCM + samples.size() * 2) {
			size_t index = (offset - AM_SIM_PCM) / 2;
			samples[index] = static_cast<int16_t>(value & 0xffff);
			samples[index + 1] = static_cast<int16_t>(value >> 16);
			return true;
		}
		switch (offset) {
		case AM_SIM_CONTROL:
			if (active) am_shutdown();
			active = false;
			status = 0;
			next_poll = 0;
			audio_written = 0;
			if (!value) return true;
			if (value & ~(AM_SIM_VIDEO | AM_SIM_AUDIO) || ((value & AM_SIM_VIDEO) && !dimensions())) return fail("Invalid media configuration");
			{
				am_config config = { title, width, height, audio_rate,
				                     bool(value & AM_SIM_VIDEO), bool(value & AM_SIM_AUDIO) };
				if (!am_init(&config)) return fail(am_error());
			}
			active = true;
			status = value;
			return true;
		case AM_SIM_WIDTH: width = value; return true;
		case AM_SIM_HEIGHT: height = value; return true;
		case AM_SIM_PRESENT:
			if (!(status & AM_SIM_VIDEO) || !dimensions()) return fail("Invalid video frame");
			if (!am_video_present(pixels.data(), width, height, width)) return fail(am_error());
			return true;
		case AM_SIM_SLEEP_US:
			/* Bound each wait so a malformed guest cannot freeze the host UI. */
			am_sleep_us(value < 10000 ? value : 10000);
			am_input_read();
			return true;
		case AM_SIM_AUDIO_RATE: audio_rate = value; return true;
		case AM_SIM_AUDIO_SUBMIT: {
			audio_written = 0;
			if (!(status & AM_SIM_AUDIO) || value > AM_SIM_PCM_FRAMES) return fail("Invalid audio packet");
			/* Playback can drain the queue between calls. Sample space before
			 * writing so a full queue is not mistaken for an output failure. */
			bool space = am_audio_queued() < am_capabilities()->audio_capacity;
			audio_written = am_audio_write(samples.data(), value);
			if (value && !audio_written && space) return fail(am_error());
			return true;
		}
		default: return false;
		}
	}
private:
	const char* title;
	bool dimensions() const {
		return width && height && width <= AM_SIM_MAX_WIDTH && height <= AM_SIM_MAX_HEIGHT;
	}
	bool fail(const char* message) {
		std::fprintf(stderr, "AM media: %s\n", message);
		status = AM_SIM_ERROR;
		return true; /* Guest reports the device error, without a bus trap. */
	}
	bool active = false;
	uint32_t status = 0, width = 240, height = 160, audio_rate = 44100, audio_written = 0;
	uint64_t clock = 0, next_poll = 0;
	std::array<uint32_t, AM_SIM_MAX_WIDTH * AM_SIM_MAX_HEIGHT> pixels{};
	std::array<int16_t, AM_SIM_PCM_FRAMES * 2> samples{};
};

#endif
