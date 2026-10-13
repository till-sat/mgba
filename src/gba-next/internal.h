/* SPDX-License-Identifier: MPL-2.0 */
#ifndef GBA_NEXT_INTERNAL_H
#define GBA_NEXT_INTERNAL_H
#include <gba-next/core.h>
/* Relaxed timing (Makefile GBN_RV32_FAST); bits 32 and 64 also apply to the
 * interpreter. */
#ifndef GBN_RV32_FAST
#define GBN_RV32_FAST 0
#endif
struct GbnRv32;
/* PCM has no guest-visible side effects. A CPU-only interval may cross its
 * clock, provided sound writes first catch up and the event service retains
 * the ordinary chronological order at every other device boundary. */
static inline int gbn_cpu_event(const struct Gbn* m) {
    int id=m->next_event;
    if (id==(int)GBN_EVENT_AUDIO && m->devices && m->devices->audio.batched) {
        unsigned next=m->event_head;
        if(next==GBN_EVENT_AUDIO) next=m->event_next[next];
        return next<GBN_EVENT_COUNT?(int)next:-1;
    }
    return id;
}
void gbn_audio_service_span(struct Gbn*, uint32_t when);
/* The detached clock is valid for the built-in device schedule. Public
 * custom scheduling switches back before mutating the real queue, so its
 * historical equal-key ownership is never reconstructed from a partial list. */
static inline bool gbn_audio_regular_event(unsigned id, uint8_t priority) {
    static const uint8_t priorities[GBN_EVENT_COUNT] = {
        0,0,0,0,0,0x70,0x80,0x10,0x20,0x21,0x22,0x23,0x18,0x40,8,0
    };
    return id >= GBN_EVENT_SAVE && id < GBN_EVENT_COUNT && priority == priorities[id];
}
void gbn_audio_timeline_queue(struct Gbn*);
void gbn_audio_timeline_restore(struct Gbn*);
void gbn_audio_timeline_drain(struct Gbn*, uint32_t when, bool inclusive);
static inline void gbn_audio_catchup(struct Gbn* m) {
    if (m->devices && m->devices->audio.pcm_detached) {
        gbn_audio_timeline_materialize(m);
        return;
    }
    while(m->next_event==(int)GBN_EVENT_AUDIO &&
          (int32_t)(m->now-m->events[GBN_EVENT_AUDIO].when)>=0)
        gbn_audio_service_span(m,m->events[GBN_EVENT_AUDIO].when);
}

/* Called only after the shared instruction-boundary checks. Zero requests
 * the independent interpreter; successful blocks restore complete state.
 * Bit 31 requests one interpreter instruction after the completed prefix. */
unsigned gbn_rv32_execute(struct GbnRv32*, uint32_t cap);
enum GbnStatus gbn_device_read(const struct Gbn*, uint32_t address, unsigned width, struct GbnAccess*);
enum GbnStatus gbn_device_write(struct Gbn*, uint32_t address, unsigned width, uint32_t value, uint32_t* cycles);
bool gbn_device_writable(const struct Gbn*, uint32_t address, unsigned width);
uint32_t gbn_open_bus(const struct Gbn*);
void gbn_update_irq(struct Gbn*, uint32_t when);
void gbn_irq_at(struct Gbn*, uint16_t mask, uint32_t when);
bool gbn_bus_writable(const struct Gbn*, uint32_t address, unsigned width);
void gbn_dma_write(struct Gbn*, unsigned offset, uint16_t value);
void gbn_dma_trigger(struct Gbn*, unsigned timing, uint32_t when);
enum GbnStatus gbn_dma_service(struct Gbn*);
#if GBN_RV32_FAST & 262144
bool gbn_dma_now(struct Gbn*, uint32_t deadline);
#endif
void gbn_dma_fifo(struct Gbn*, unsigned fifo, uint32_t when);
uint16_t gbn_timer_read(const struct Gbn*, unsigned timer, uint32_t when);
void gbn_timer_write(struct Gbn*, unsigned timer, bool control, uint16_t value, uint16_t mask);
void gbn_timer_service(struct Gbn*, unsigned timer, uint32_t when);
#if GBN_RV32_FAST & 64
/* Relaxed timing: a timer that raises no IRQ and drives no cascade overflows
 * without events; its overflows up to until run here, in time order. */
void gbn_timer_catchup(struct Gbn*, uint32_t until);
/* Bounded by the next event, which must see the overflows before it. */
static inline void gbn_timer_catchup_now(struct Gbn* m) {
	int next = m->next_event;
	uint32_t until = m->now;
	if (next >= 0 && (int32_t) (until - m->events[next].when) > 0) until = m->events[next].when;
	gbn_timer_catchup(m, until);
}
#endif
bool gbn_audio_register(unsigned offset);
uint16_t gbn_audio_reg_read(const struct Gbn*, unsigned offset);
void gbn_audio_write(struct Gbn*, unsigned offset, unsigned width, uint32_t value);
void gbn_audio_service(struct Gbn*, uint32_t when);
void gbn_audio_frame(struct Gbn*, uint32_t when);
void gbn_audio_timer(struct Gbn*, unsigned timer, uint32_t when);
bool gbn_sio_register(unsigned offset);
enum GbnStatus gbn_sio_write(struct Gbn*, unsigned offset, unsigned width, uint32_t value);
enum GbnStatus gbn_sio_service(struct Gbn*, uint32_t when);
bool gbn_save_mapped(const struct Gbn*, uint32_t address, unsigned width);
uint32_t gbn_save_read(const struct Gbn*, uint32_t address, unsigned width);
void gbn_save_write(struct Gbn*, uint32_t address, unsigned width, uint32_t value);
void gbn_save_dma(struct Gbn*, uint32_t dest, unsigned width, uint32_t count);
unsigned gbn_save_wait(const struct Gbn*, uint32_t address);
void gbn_ppu_reference(struct Gbn*, unsigned offset);
enum GbnStatus gbn_ppu_line(struct Gbn*, unsigned y);
#if GBN_RV32_FAST & 32768
/* VRAM backing bytes [lo, hi) were written: mark their pages in this frame. */
static inline void gbn_vram_written(struct GbnDevices* d, uint32_t lo, uint32_t hi) {
	struct GbnPpuCache* c = d->ppu.cache;
	if (!c || hi <= lo) return;
	if (lo >= 0x10000) return;
	if (hi > 0x10000) hi = 0x10000;
	for (uint32_t p = lo >> 11; p <= (hi - 1) >> 11; ++p) c->dirty[d->frames & 3] |= 1u << p;
}
#endif
#if GBN_RV32_FAST & 2048
/* Draw the lines whose HBlank has passed (devices.c). */
enum GbnStatus gbn_ppu_catchup(struct Gbn*);
#if GBN_RV32_FAST & 65536
void gbn_hblank_observe(struct Gbn*); /* after DISPSTAT, IE or DMA control writes */
#endif
/* Before a write at address (width bytes, any region) that may change a line
 * not yet drawn: video memory, display registers, or DMA into them. */
static inline void gbn_ppu_before_write(struct Gbn* m, uint32_t address) {
	unsigned region = address >> 24;
#if GBN_RV32_FAST & 65536
	/* A merged line becomes due at its HBlank start, without an event. */
	struct GbnDevices* d = m->devices;
	if (d && d->hblank_merged && d->io[3] < 160 && d->ppu_due == d->io[3] &&
	    (int32_t) (m->now - (d->line_when + 1008)) >= 0 &&
	    ((region >= 5 && region <= 7) || (region == 4 && (address & 0xffffff) < 0x60)))
		d->ppu_due = (uint8_t) (d->io[3] + 1); /* io[3] = VCOUNT */
#endif
	if (m->devices && m->devices->ppu_drawn < m->devices->ppu_due &&
	    ((region >= 5 && region <= 7) || (region == 4 && (address & 0xffffff) < 0x60)))
		(void) gbn_ppu_catchup(m);
}
#endif
#endif
