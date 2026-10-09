/* SPDX-License-Identifier: MPL-2.0 */
#include "internal.h"
#include <string.h>

/* The sequencer and PCM clock share the machine timeline. Oscillators advance
 * analytically between observations; only the noise recurrence needs steps. */
static bool master(const struct Gbn* m) { return (m->devices->io[0x84 / 2] & 0x80) != 0; }
static unsigned interval(const struct Gbn* m) { return 512u >> (m->devices->io[0x88 / 2] >> 14); }

static unsigned period(const struct GbnPsgChannel* c, unsigned id) {
	if (id < 2) return 16u * (2048 - c->frequency);
	if (id == 2) return 8u * (2048 - c->frequency);
	unsigned ratio = c->noise & 7;
	return (32u * (ratio ? 2 * ratio : 1)) << (c->noise >> 4);
}

static unsigned nibble(const uint8_t* wave, unsigned index) {
	return wave[index / 2] >> ((index & 1) ? 0 : 4) & 15;
}

static void wave_rotate(struct Gbn* m, uint32_t steps) {
	struct GbnAudio* a = &m->devices->audio;
	uint16_t control = m->devices->io[0x70 / 2];
	unsigned length = control & 0x20 ? 64 : 32;
	uint8_t* wave = a->wave + (length == 64 || !(control & 0x40) ? 0 : 16);
	unsigned shift = steps % length;
	/* The GBA wave register is a rotating shift register, including the CPU
 * view of the two banks. Keep bytes in bus order, independent of host endian. */
	a->psg[2].sample = (uint8_t) nibble(wave, (steps - 1) % length);
	if (!shift) return;
	uint8_t next[32];
	for (unsigned i = 0; i < length / 2; ++i) {
		next[i] = (uint8_t) (nibble(wave, (2 * i + shift) % length) << 4 |
			nibble(wave, (2 * i + shift + 1) % length));
	}
	memcpy(wave, next, length / 2);
}

static void advance_channel(struct Gbn* m, unsigned id, uint32_t when) {
	struct GbnPsgChannel* c = &m->devices->audio.psg[id];
	int32_t elapsed = (int32_t) (when - c->updated);
	if (elapsed < 0) return; /* Wave trigger has a 24-cycle startup delay. */
	if (!master(m) || !c->enabled) { c->updated = when; return; }
	uint32_t total = c->remainder + (uint32_t) elapsed;
	unsigned cycles = period(c, id);
	uint32_t steps = total / cycles;
	c->remainder = total % cycles;
	c->updated = when;
	if (!steps) return;
	if (id < 2) c->phase = (uint8_t) ((c->phase + steps) & 7);
	else if (id == 2) wave_rotate(m, steps);
	else {
		uint16_t taps = c->noise & 8 ? 0x4040 : 0x4000;
		while (steps--) {
			unsigned feedback = (c->lfsr ^ (c->lfsr >> 1) ^ 1) & 1;
			c->lfsr = (uint16_t) ((c->lfsr >> 1 & ~taps) | (feedback ? taps : 0));
			c->sample = (uint8_t) feedback;
		}
	}
}

static void advance_all(struct Gbn* m, uint32_t when) {
	for (unsigned id = 0; id < 4; ++id) advance_channel(m, id, when);
}

static uint16_t mask(unsigned offset) {
	switch (offset) {
	case 0x60: return 0x007f;
	case 0x62: case 0x68: return 0xffc0;
	case 0x64: case 0x6c: case 0x74: return 0x4000;
	case 0x70: return 0x00e0;
	case 0x72: return 0xe000;
	case 0x78: return 0xff00;
	case 0x7c: return 0x40ff;
	case 0x80: return 0xff77;
	case 0x82: return 0x770f;
	case 0x84: return 0x0080;
	case 0x88: return 0xc3fe;
	default: return 0;
	}
}

bool gbn_audio_register(unsigned offset) { return offset >= 0x60 && offset < 0xa8; }

uint16_t gbn_audio_reg_read(const struct Gbn* m, unsigned offset) {
	const struct GbnAudio* a = &m->devices->audio;
	if (offset >= 0x90 && offset < 0xa0) {
		unsigned bank = master(m) ? !(m->devices->io[0x70 / 2] & 0x40) : 1;
		const struct GbnPsgChannel* c = &a->psg[2];
		unsigned base = bank * 16, length = 0, rotation = 0;
		int32_t elapsed = (int32_t) (m->now - c->updated);
		uint16_t control = m->devices->io[0x70 / 2];
		if (master(m) && c->enabled && elapsed >= 0 && ((control & 0x20) || bank == ((control >> 6) & 1))) {
			length = control & 0x20 ? 64 : 32;
			rotation = (c->remainder + (uint32_t) elapsed) / period(c, 2) % length;
			if (length == 64) base = 0;
		}
		unsigned pos = (bank * 16 + offset - 0x90 - base) * 2;
		uint16_t value = 0;
		for (unsigned n = 0; n < 4; ++n) {
			unsigned index = pos + n;
			if (length) index = (index + rotation) % length;
			value |= (uint16_t) (nibble(a->wave + base, index) << (n & 1 ? (n / 2) * 8 : 4 + (n / 2) * 8));
		}
		return value;
	}
	if (offset == 0x84) {
		uint16_t status = m->devices->io[offset / 2];
		for (unsigned i = 0; i < 4; ++i) if (a->psg[i].enabled) status |= (uint16_t) (1u << i);
		return status;
	}
	if (mask(offset)) return m->devices->io[offset / 2];
	return (uint16_t) gbn_open_bus(m);
}

static bool sweep_check(struct GbnPsgChannel* c, bool update) {
	unsigned shift = c->sweep & 7;
	int frequency = c->sweep_shadow;
	if (c->sweep & 8) { frequency -= frequency >> shift; c->swept_down = true; }
	else frequency += frequency >> shift;
	if (frequency >= 2048) { c->enabled = false; return false; }
	if (update && shift) c->sweep_shadow = c->frequency = (uint16_t) frequency;
	return true;
}

static void trigger(struct Gbn* m, unsigned id, uint8_t value) {
	struct GbnPsgChannel* c = &m->devices->audio.psg[id];
	bool extra_length = !(m->devices->audio.frame & 1);
	bool length_enable = (value & 0x40) != 0;
	if (!c->length_enable && length_enable && extra_length && c->length && !--c->length) c->enabled = false;
	c->length_enable = length_enable;
	if (!(value & 0x80)) return;
	c->enabled = c->dac;
	if (!c->length) {
		c->length = id == 2 ? 256 : 64;
		if (length_enable && extra_length) --c->length;
	}
	if (id != 2) {
		c->volume = c->envelope >> 4;
		c->envelope_ticks = c->envelope & 7;
	}
	if (!id) {
		c->sweep_shadow = c->frequency;
		c->sweep_ticks = (c->sweep >> 4) & 7;
		if (!c->sweep_ticks) c->sweep_ticks = 8;
		c->swept_down = false;
		if (c->sweep & 7) sweep_check(c, false);
	}
	if (id >= 2) {
		c->updated = m->now + (id == 2 ? 24 : 0);
		c->remainder = 0;
		if (id == 3) { c->lfsr = 0; c->sample = 0; }
	}
}

static void write_byte(struct Gbn* m, unsigned offset, uint8_t value) {
	struct GbnAudio* a = &m->devices->audio;
	uint16_t* io = m->devices->io;
	if (offset < 0x82 && !master(m)) return;
	unsigned id;
	struct GbnPsgChannel* c;
	switch (offset) {
	case 0x60:
		c = &a->psg[0];
		if (c->swept_down && !(value & 8)) c->enabled = false;
		c->sweep = value & 0x7f;
		break;
	case 0x62: case 0x68: case 0x78:
		id = offset == 0x62 ? 0 : offset == 0x68 ? 1 : 3;
		c = &a->psg[id]; c->length = 64 - (value & 63); c->duty = value >> 6;
		break;
	case 0x63: case 0x69: case 0x79:
		id = offset == 0x63 ? 0 : offset == 0x69 ? 1 : 3;
		c = &a->psg[id]; c->envelope = value; c->dac = (value & 0xf8) != 0;
		if (!c->dac) c->enabled = false;
		break;
	case 0x64: case 0x6c: case 0x74:
		id = offset == 0x64 ? 0 : offset == 0x6c ? 1 : 2;
		a->psg[id].frequency = (a->psg[id].frequency & 0x700) | value;
		break;
	case 0x65: case 0x6d: case 0x75:
		id = offset == 0x65 ? 0 : offset == 0x6d ? 1 : 2;
		a->psg[id].frequency = (a->psg[id].frequency & 255) | (value & 7) << 8;
		trigger(m, id, value);
		break;
	case 0x70:
		a->psg[2].dac = (value & 0x80) != 0;
		if (!a->psg[2].dac) a->psg[2].enabled = false;
		break;
	case 0x72: a->psg[2].length = 256 - value; break;
	case 0x73: a->psg[2].volume = value >> 5; break;
	case 0x7c: a->psg[3].noise = value; break;
	case 0x7d: trigger(m, 3, value); break;
	case 0x83:
		for (unsigned i = 0; i < 2; ++i) if (value & (8u << (4 * i))) {
			a->fifo[i].read = a->fifo[i].size = 0;
			a->fifo[i].sample = 0;
		}
		break;
	case 0x84:
		if (!(value & 0x80)) {
			memset(a->psg, 0, sizeof(a->psg));
			for (unsigned i = 0; i < 4; ++i) { a->psg[i].length = i == 2 ? 256 : 64; a->psg[i].updated = m->now; }
			for (unsigned i = 0x60 / 2; i <= 0x80 / 2; ++i) io[i] = 0;
			io[0x82 / 2] &= 0xff00;
		} else if (!master(m)) {
			a->frame = 7;
			for (unsigned i = 0; i < 4; ++i) a->psg[i].updated = m->now;
		}
		break;
	default: break;
	}
	unsigned aligned = offset & ~1u, shift = (offset & 1) * 8;
	uint16_t combined = (uint16_t) ((io[aligned / 2] & ~(255u << shift)) | (uint16_t) value << shift);
	io[aligned / 2] = combined & mask(aligned);
}

void gbn_audio_write(struct Gbn* m, unsigned offset, unsigned width, uint32_t value) {
	advance_all(m, m->now);
	struct GbnAudio* a = &m->devices->audio;
	if (offset >= 0xa0) {
		struct GbnFifo* f = &a->fifo[(offset - 0xa0) / 4];
		for (unsigned i = 0; i < width; ++i) if (f->size < 32) {
			f->data[(f->read + f->size) & 31] = (uint8_t) (value >> (8 * i));
			++f->size;
		}
		return;
	}
	if (offset >= 0x90) {
		unsigned bank = master(m) ? !(m->devices->io[0x70 / 2] & 0x40) : 1;
		for (unsigned i = 0; i < width; ++i) a->wave[bank * 16 + offset - 0x90 + i] = (uint8_t) (value >> (8 * i));
		return;
	}
	unsigned old_interval = interval(m);
	for (unsigned i = 0; i < width; ++i) write_byte(m, offset + i, (uint8_t) (value >> (8 * i)));
	if (a->running && old_interval != interval(m)) {
		unsigned cycles = interval(m);
		gbn_schedule_at(m, GBN_EVENT_AUDIO, (m->now & ~(cycles - 1u)) + cycles, 0x18);
	}
}

void gbn_audio_start(struct Gbn* m) {
	if (!m->devices || m->devices->audio.running) return;
	struct GbnAudio* a = &m->devices->audio;
	a->running = true;
	a->frame_when = m->now;
	gbn_schedule_at(m, GBN_EVENT_AUDIO_FRAME, m->now, 0x10);
	gbn_schedule_at(m, GBN_EVENT_AUDIO, m->now, 0x18);
}

unsigned gbn_audio_rate(const struct Gbn* m) { return m->devices ? 16777216u / interval(m) : 0; }

unsigned gbn_audio_read(struct Gbn* m, struct GbnStereo* output, unsigned capacity) {
	if (!m->devices || !output) return 0;
	struct GbnAudio* a = &m->devices->audio;
	unsigned count = a->size < capacity ? a->size : capacity;
	for (unsigned i = 0; i < count; ++i) output[i] = a->samples[(a->read + i) & (GBN_AUDIO_CAPACITY - 1)];
	a->read = (a->read + count) & (GBN_AUDIO_CAPACITY - 1);
	a->size -= count;
	return count;
}

void gbn_audio_frame(struct Gbn* m, uint32_t when) {
	struct GbnAudio* a = &m->devices->audio;
	advance_all(m, when);
	if (master(m)) {
		a->frame = (a->frame + 1) & 7;
		if (!(a->frame & 1)) for (unsigned i = 0; i < 4; ++i) {
			struct GbnPsgChannel* c = &a->psg[i];
			if (c->length_enable && c->length && !--c->length) c->enabled = false;
		}
		if (a->frame == 2 || a->frame == 6) {
			struct GbnPsgChannel* c = &a->psg[0];
			if ((c->sweep & 0x77) && c->sweep_ticks && !--c->sweep_ticks) {
				c->sweep_ticks = (c->sweep >> 4) & 7;
				if (!c->sweep_ticks) c->sweep_ticks = 8;
				if (c->sweep & 0x70) if (sweep_check(c, true) && (c->sweep & 7)) sweep_check(c, false);
			}
		}
		if (a->frame == 7) for (unsigned i = 0; i < 4; ++i) if (i != 2) {
			struct GbnPsgChannel* c = &a->psg[i];
			if (c->enabled && (c->envelope & 7) && c->envelope_ticks && !--c->envelope_ticks) {
				int volume = c->volume + (c->envelope & 8 ? 1 : -1);
				if (volume >= 0 && volume <= 15) c->volume = (uint8_t) volume;
				c->envelope_ticks = c->envelope & 7;
			}
		}
	}
	a->frame_when = when + 32768;
	gbn_schedule_at(m, GBN_EVENT_AUDIO_FRAME, a->frame_when, 0x10);
}

static int channel_sample(const struct GbnPsgChannel* c, unsigned id) {
	if (!c->enabled) return 0;
	if (id < 2) {
		static const uint8_t duties[] = {0x80, 0x81, 0xe1, 0x7e};
		return (duties[c->duty] >> c->phase & 1) * c->volume;
	}
	if (id == 3) return c->sample * c->volume;
	if (c->volume & 4) return c->sample * 3 / 4;
	return c->volume ? c->sample >> (c->volume - 1) : 0;
}

static int16_t pcm(int sample, unsigned bias) {
	sample += (int) bias;
	if (sample < 0) sample = 0;
	if (sample > 1023) sample = 1023;
	sample = (sample - (int) bias) * 48;
	if (sample < -32768) sample = -32768;
	if (sample > 32767) sample = 32767;
	return (int16_t) sample;
}

void gbn_audio_service(struct Gbn* m, uint32_t when) {
	struct GbnAudio* a = &m->devices->audio;
	const uint16_t* io = m->devices->io;
	advance_all(m, when);
	int left = 0, right = 0;
	if (master(m)) {
		unsigned routing = io[0x80 / 2] >> 8;
		for (unsigned i = 0; i < 4; ++i) {
			int sample = channel_sample(&a->psg[i], i);
			if (routing & (1u << i)) right += sample;
			if (routing & (16u << i)) left += sample;
		}
		unsigned shift = 4 - (io[0x82 / 2] & 3);
		left = left * 8 * (1 + (io[0x80 / 2] >> 4 & 7)) >> shift;
		right = right * 8 * (1 + (io[0x80 / 2] & 7)) >> shift;
		for (unsigned i = 0; i < 2; ++i) {
			int sample = a->fifo[i].sample * (io[0x82 / 2] & (4u << i) ? 4 : 2);
			if (io[0x82 / 2] & (0x100u << (4 * i))) right += sample;
			if (io[0x82 / 2] & (0x200u << (4 * i))) left += sample;
		}
	}
	if (a->size == GBN_AUDIO_CAPACITY) { a->read = (a->read + 1) & (GBN_AUDIO_CAPACITY - 1); --a->size; ++a->dropped; }
	unsigned slot = (a->read + a->size++) & (GBN_AUDIO_CAPACITY - 1);
	a->samples[slot].left = pcm(left, io[0x88 / 2] & 0x3ff);
	a->samples[slot].right = pcm(right, io[0x88 / 2] & 0x3ff);
	a->samples[slot].when = when;
	++a->produced;
	gbn_schedule_at(m, GBN_EVENT_AUDIO, when + interval(m), 0x18);
}

void gbn_audio_timer(struct Gbn* m, unsigned timer, uint32_t when) {
	if (timer > 1 || !master(m)) return;
	uint16_t control = m->devices->io[0x82 / 2];
	for (unsigned i = 0; i < 2; ++i) {
		if (!(control & (0x300u << (4 * i))) || ((control >> (10 + 4 * i)) & 1) != timer) continue;
		struct GbnFifo* f = &m->devices->audio.fifo[i];
		if (f->size) { f->sample = (int8_t) f->data[f->read]; f->read = (f->read + 1) & 31; --f->size; }
		else f->sample = 0;
		if (f->size <= 16) gbn_dma_fifo(m, i, when);
	}
}
