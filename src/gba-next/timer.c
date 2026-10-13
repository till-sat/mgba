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

#if GBN_RV32_FAST & 64
/* Only the sound FIFOs observe these overflows (reads compute the value). */
static bool batched(const struct GbnDevices* d, unsigned id) {
	const struct GbnTimer* t = &d->timers[id];
	return (t->control & 0xc0) == 0x80 && !cascade(t, id) &&
		(id == 3 || (d->timers[id + 1].control & 0x84) != 0x84);
}
/* The earliest pending batched overflow of the machine last caught up, so
 * that the frequent calls with nothing due return at once. Every timer
 * change goes through schedule_timer, which forgets it. */
static const struct Gbn* due_machine;
static uint32_t due_when;
/* Next overflow of each batched timer (bit in due_live) while due_machine
 * is current: overflows below keep them exact without a rescan. */
static uint32_t due_next[4];
static unsigned due_live;
#endif

static void schedule_timer(struct Gbn* m, unsigned id) {
	struct GbnTimer* t = &m->devices->timers[id];
	gbn_cancel(m, GBN_EVENT_TIMER0 + id);
#if GBN_RV32_FAST & 64
	due_machine = 0;
	if (batched(m->devices, id)) return;
#endif
	if ((t->control & 0x80) && !cascade(t, id)) {
		gbn_schedule_at(m, GBN_EVENT_TIMER0 + id, t->epoch + ((0x10000u - t->value) << shift(t->control)), (uint8_t) (0x20 + id));
	}
}

void gbn_timer_write(struct Gbn* m, unsigned id, bool control, uint16_t value, uint16_t mask) {
	struct GbnTimer* t = &m->devices->timers[id];
#if GBN_RV32_FAST & 64
	gbn_timer_catchup_now(m);
#endif
	if (!control) { t->reload = (t->reload & ~mask) | (value & mask); return; }
	uint16_t old = t->control;
	t->value = gbn_timer_read(m, id, m->now);
	t->control = ((old & ~mask) | (value & mask)) & 0xc7;
	if (!(old & 0x80) && (t->control & 0x80)) t->value = t->reload;
	t->epoch = m->now & ~((1u << shift(t->control)) - 1);
	m->devices->io[(0x102 + 4 * id) / 2] = t->control;
	schedule_timer(m, id);
#if GBN_RV32_FAST & 64
	/* A new cascade below changes whether the previous timer is batched. */
	if (id) schedule_timer(m, id - 1);
#endif
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

#if GBN_RV32_FAST & 64
void gbn_timer_catchup(struct Gbn* m, uint32_t until) {
	struct GbnDevices* d = m->devices;
	if (!d || (due_machine == m && (int32_t) (due_when - until) > 0)) return;
	uint32_t* next = due_next;
	unsigned live = due_live;
	if (due_machine != m) {
		live = 0;
		for (unsigned id = 0; id < 4; ++id) if (batched(d, id)) {
			const struct GbnTimer* t = &d->timers[id];
			next[id] = t->epoch + ((0x10000u - t->value) << shift(t->control));
			live |= 1u << id;
		}
		due_live = live;
	}
	for (;;) {
		int best = -1;
		uint32_t when = until + GBN_MAX_DELAY;
		for (unsigned id = 0; id < 4; ++id)
			if ((live >> id & 1) && (best < 0 || (int32_t) (next[id] - when) < 0)) { best = (int) id; when = next[id]; }
		if (best < 0 || (int32_t) (when - until) > 0) { due_machine = m; due_when = when; return; }
		/* gbn_timer_service without its event, IRQ and cascade: batched
		 * timers have none of them. */
		struct GbnTimer* t = &d->timers[best];
		t->value = t->reload;
		t->epoch = when;
		gbn_audio_timer(m, (unsigned) best, when);
		next[best] = when + ((0x10000u - t->reload) << shift(t->control));
	}
}
#endif
