/* SPDX-License-Identifier: MPL-2.0 */
#include "pcm-span.h"

bool gbn_audio_emit_constant_span(struct Gbn* m, uint32_t first_when, uint32_t count) {
    if (!m || !m->devices) return false;
    struct GbnAudio* a = &m->devices->audio;
    const uint16_t* io = m->devices->io;
    const struct GbnEvent* frame = &m->events[GBN_EVENT_AUDIO_FRAME];
    /* This is precisely the deferred-PSG subset of gbn_audio_service().
     * No advance_all() is needed, including if psg_pending was already set. */
    if (!a->batched || !a->running || (io[0x80 / 2] & 0xff00) || !frame->active)
        return false;
    if (!count) return true;
    unsigned cycles = interval(m);
    uint64_t extent = (uint64_t) (count - 1) * cycles;
    if (extent > GBN_MAX_DELAY) return false;
    uint32_t last_when = first_when + (uint32_t) extent;
    if ((int32_t) (frame->when - first_when) <= 0 ||
            (int32_t) (frame->when - last_when) <= 0) return false;

    int left = 0, right = 0;
    if (master(m)) for (unsigned fifo = 0; fifo < 2; ++fifo) {
        int sample = a->fifo[fifo].sample * (io[0x82 / 2] & (4u << fifo) ? 4 : 2);
        if (io[0x82 / 2] & (0x100u << (4 * fifo))) right += sample;
        if (io[0x82 / 2] & (0x200u << (4 * fifo))) left += sample;
    }
    int16_t output_left = pcm(left, io[0x88 / 2] & 0x3ff);
    int16_t output_right = pcm(right, io[0x88 / 2] & 0x3ff);

    unsigned tail = (a->read + a->size) & (GBN_AUDIO_CAPACITY - 1);
    uint32_t excess = count > GBN_AUDIO_CAPACITY - a->size ?
        count - (GBN_AUDIO_CAPACITY - a->size) : 0;
    /* If a whole ring is overwritten, skipped early stores would themselves
     * be overwritten by these final stores. Every final stale/active slot is
     * identical, including read-index wrap and produced/dropped wrap. */
    uint32_t skip = count > GBN_AUDIO_CAPACITY ? count - GBN_AUDIO_CAPACITY : 0;
    uint32_t when = first_when + skip * cycles;
    tail = (tail + skip) & (GBN_AUDIO_CAPACITY - 1);
    for (uint32_t i = skip; i < count; ++i, when += cycles) {
        a->samples[tail] = (struct GbnStereo) {output_left, output_right, when};
        tail = (tail + 1) & (GBN_AUDIO_CAPACITY - 1);
    }
    a->read = (a->read + excess) & (GBN_AUDIO_CAPACITY - 1);
    a->size = excess ? GBN_AUDIO_CAPACITY : a->size + count;
    a->produced += count; a->dropped += excess;
    a->psg_pending = true;
    return true;
}

bool gbn_audio_drain_constant_span(struct Gbn* m, uint32_t through_when,
        bool inclusive, uint32_t* emitted) {
    if (emitted) *emitted = 0;
    if (!m || !m->devices) return false;
    const struct GbnEvent* event = &m->events[GBN_EVENT_AUDIO];
    if (!event->active || event->priority != 0x18) return false;
    uint32_t first = event->when;
    int32_t distance = (int32_t) (through_when - first);
    if (distance > (int32_t) GBN_MAX_DELAY || distance < -(int32_t) GBN_MAX_DELAY)
        return false;
    uint32_t count = distance < 0 || (!inclusive && !distance) ? 0 :
        1 + ((uint32_t) distance - (inclusive ? 0 : 1)) / interval(m);
    uint32_t next = first + count * interval(m);
    int32_t schedule_distance = (int32_t) (next - m->now);
    if (schedule_distance > (int32_t) GBN_MAX_DELAY ||
            schedule_distance < -(int32_t) GBN_MAX_DELAY) return false;
    if (!gbn_audio_emit_constant_span(m, first, count)) return false;
    if (count) {
        /* Prevalidated above: scheduling cannot fail after changing samples. */
        (void) gbn_schedule_at(m, GBN_EVENT_AUDIO, next, 0x18);
        if (emitted) *emitted = count;
    }
    return true;
}
