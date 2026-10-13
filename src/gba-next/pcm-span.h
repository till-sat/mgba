/* SPDX-License-Identifier: MPL-2.0 */
#ifndef GBN_PCM_SPAN_EXPERIMENT_H
#define GBN_PCM_SPAN_EXPERIMENT_H
#include <gba-next/core.h>

/* Explicit experimental entry points. Existing service/write/read/default
 * behavior is unchanged. Caller proves that FIFO output/routing/bias/power
 * do not change during the requested span and that no frontend consumes it.
 * The next PSG frame is always a hard boundary, even without PSG routing.
 * All false returns leave machine, audio and events completely untouched. */
bool gbn_audio_emit_constant_span(struct Gbn*, uint32_t first_when, uint32_t count);

/* Uses the real active AUDIO event as the first sample, emits through the
 * selected inclusive/exclusive boundary, and schedules the next PCM exactly
 * once. The caller owns device ordering: in normal priorities PCM (0x18)
 * is included at a Timer0 (0x20) boundary, but excluded at PSG frame (0x10).
 * No virtual timestamp is ever stored into event[AUDIO].when. */
bool gbn_audio_drain_constant_span(struct Gbn*, uint32_t through_when,
        bool inclusive, uint32_t* emitted);
#endif
