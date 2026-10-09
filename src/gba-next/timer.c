/* SPDX-License-Identifier: MPL-2.0 */
#include "internal.h"

static unsigned shift(uint16_t control) {
	static const uint8_t shifts[] = {0, 6, 8, 10};
	return shifts[control & 3];
}

static bool cascade(const struct GbnTimer* t, unsigned id) { return id && (t->control & 4); }

uint16_t gbn_timer_read(const struct Gbn* m, unsigned id, uint32_t when) {
	const struct GbnTimer* t = &m->devices->timers[id];
	if (!(t->control & 0x80) || cascade(t, id)) return t->value;
	unsigned bits = shift(t->control);
	when &= ~((1u << bits) - 1);
	int32_t delta = (int32_t) (when - t->epoch);
	int32_t ticks = delta >= 0 ? delta >> bits : -(int32_t) ((0u - (uint32_t) delta) >> bits);
	int32_t value = t->value + ticks;
	if (value >= 0x10000) value = t->reload + (value - 0x10000) % (0x10000 - t->reload);
	return (uint16_t) value;
}

static void schedule_timer(struct Gbn* m, unsigned id) {
	struct GbnTimer* t = &m->devices->timers[id];
	gbn_cancel(m, GBN_EVENT_TIMER0 + id);
	if ((t->control & 0x80) && !cascade(t, id)) {
		gbn_schedule_at(m, GBN_EVENT_TIMER0 + id, t->epoch + ((0x10000u - t->value) << shift(t->control)), (uint8_t) (0x20 + id));
	}
}

void gbn_timer_write(struct Gbn* m, unsigned id, bool control, uint16_t value, uint16_t mask) {
	struct GbnTimer* t = &m->devices->timers[id];
	if (!control) { t->reload = (t->reload & ~mask) | (value & mask); return; }
	uint16_t old = t->control;
	t->value = gbn_timer_read(m, id, m->now);
	t->control = ((old & ~mask) | (value & mask)) & 0xc7;
	if (!(old & 0x80) && (t->control & 0x80)) t->value = t->reload;
	t->epoch = m->now & ~((1u << shift(t->control)) - 1);
	m->devices->io[(0x102 + 4 * id) / 2] = t->control;
	schedule_timer(m, id);
}

void gbn_timer_service(struct Gbn* m, unsigned id, uint32_t when) {
	struct GbnTimer* t = &m->devices->timers[id];
	t->value = t->reload;
	t->epoch = when;
	schedule_timer(m, id);
	if (t->control & 0x40) gbn_irq_at(m, (uint16_t) (8u << id), when);
	gbn_audio_timer(m, id, when);
	if (id < 3) {
		struct GbnTimer* next = &m->devices->timers[id + 1];
		if ((next->control & 0x84) == 0x84 && ++next->value == 0) gbn_timer_service(m, id + 1, when);
	}
}
