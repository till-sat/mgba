/* SPDX-License-Identifier: MPL-2.0 */

/* Unlike a cancelled clock, a detached clock retains its exact next sample
 * in events[AUDIO].when. Only the explicit pcm_detached flag gives an
 * inactive event this meaning. No virtual timestamp enters the real queue. */
void gbn_audio_timeline_restore(struct Gbn* m) {
    if (!m || !m->devices || !m->devices->audio.pcm_detached) return;
    struct GbnAudio* a = &m->devices->audio;
    uint32_t when = m->events[GBN_EVENT_AUDIO].when;
    bool tie = a->pcm_tie;
    a->pcm_detached = false;
    a->pcm_tie = false;
    (void) gbn_schedule_at(m, GBN_EVENT_AUDIO, when, 0x18);
    /* Cancelling then reinserting must not invert a pre-existing equal-key
     * winner. Once samples have been emitted, the ordinary reschedule would
     * have discarded this historical ownership. */
    int next = m->next_event;
    if (tie && next >= 0 && m->events[next].when == when &&
            m->events[next].priority == 0x18) m->next_event = GBN_EVENT_AUDIO;
}

void gbn_audio_timeline_queue(struct Gbn* m) {
    if (!m || !m->devices) return;
    struct GbnDevices* d = m->devices;
    struct GbnAudio* a = &d->audio;
    /* Compatibility is established on enable and invalidated by the
     * scheduling API before a custom event can disturb historical ties. */
    bool eligible = a->timeline && a->batched && a->running &&
        !(d->io[0x80 / 2] & 0xff00) &&
        m->events[GBN_EVENT_AUDIO_FRAME].active;
    if (!eligible) {
        gbn_audio_timeline_restore(m);
        return;
    }
    if (!a->pcm_detached && m->events[GBN_EVENT_AUDIO].active &&
            m->events[GBN_EVENT_AUDIO].priority == 0x18) {
        a->pcm_tie = m->next_event == GBN_EVENT_AUDIO;
        /* Mark detached only after our own cancel. Public cancellation of
         * an already detached clock must disable and cancel it normally. */
        gbn_cancel(m, GBN_EVENT_AUDIO);
        a->pcm_detached = true;
    }
}

void gbn_audio_timeline_drain(struct Gbn* m, uint32_t when, bool inclusive) {
    if (!m || !m->devices || !m->devices->audio.pcm_detached) return;
    struct GbnAudio* a = &m->devices->audio;
    uint32_t first = m->events[GBN_EVENT_AUDIO].when;
    int32_t distance = (int32_t) (when - first);
    if (distance < 0 || (!inclusive && !distance)) return;
    uint32_t count = 1 + ((uint32_t) distance - (inclusive ? 0 : 1)) / interval(m);
    if (gbn_audio_emit_constant_span(m, first, count)) {
        m->events[GBN_EVENT_AUDIO].when = first + count * interval(m);
        a->pcm_tie = false;
        return;
    }
    /* Exceptional/direct out-of-order API calls retain the ordinary queue.
     * Refuse to materialize across an unserviced device boundary. */
    gbn_audio_timeline_restore(m);
    while (m->next_event == GBN_EVENT_AUDIO) {
        uint32_t next = m->events[GBN_EVENT_AUDIO].when;
        int32_t remaining = (int32_t) (when - next);
        if (remaining < 0 || (!inclusive && !remaining)) break;
        gbn_audio_service(m, next);
    }
}

void gbn_audio_timeline_materialize(struct Gbn* m) {
    if (!m || !m->devices || !m->devices->audio.pcm_detached) return;
    uint32_t through = m->now;
    unsigned next = m->event_head;
    if (next < GBN_EVENT_COUNT) {
        const struct GbnEvent* event = &m->events[next];
        bool audio_first = 0x18 < event->priority ||
            (0x18 == event->priority && (GBN_EVENT_AUDIO < next ||
                (m->devices->audio.pcm_tie &&
                 m->events[GBN_EVENT_AUDIO].when == event->when)));
        uint32_t limit = event->when - (audio_first ? 0 : 1);
        if ((int32_t) (limit - through) < 0) through = limit;
    }
    gbn_audio_timeline_drain(m, through, true);
}

void gbn_audio_set_timeline(struct Gbn* m, bool enabled) {
    if (!m || !m->devices) return;
    gbn_audio_timeline_restore(m);
    if (enabled) for (unsigned id=0; id<GBN_EVENT_COUNT; ++id)
        if (m->events[id].active && !gbn_audio_regular_event(id,m->events[id].priority)) {
            enabled=false; break;
        }
    m->devices->audio.timeline = enabled;
    gbn_audio_timeline_queue(m);
}
