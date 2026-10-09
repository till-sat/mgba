#ifndef GBA_NEXT_EVENTS_TEST_H
#define GBA_NEXT_EVENTS_TEST_H
/* SPDX-License-Identifier: MPL-2.0 */
/* Independent full-event_oracle_scan oracle for the public event scheduling contract. */
#include <gba-next/core.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct Gbn event_oracle_machine;
static struct GbnEvent event_oracle_reference[GBN_EVENT_COUNT];
static int event_oracle_selected;
static uint32_t event_oracle_random_state = 0x4162a8b9u;
static unsigned event_oracle_checks, event_oracle_operations;
#define EVENT_ORACLE_CHECK(x) do { ++event_oracle_checks; if (!(x)) { \
    fprintf(stderr, "event queue failure line=%u operation=%u now=%08x next=%d want=%d\n", \
        (unsigned) __LINE__, event_oracle_operations, (unsigned) event_oracle_machine.now, event_oracle_machine.next_event, event_oracle_selected); \
    exit(1); } } while (0)
static uint32_t event_oracle_random_word(void) {
    event_oracle_random_state ^= event_oracle_random_state << 13; event_oracle_random_state ^= event_oracle_random_state >> 17; event_oracle_random_state ^= event_oracle_random_state << 5;
    return event_oracle_random_state;
}
static void event_oracle_scan(void) {
    event_oracle_selected = -1;
    for (unsigned i = 0; i < GBN_EVENT_COUNT; ++i) {
        if (!event_oracle_reference[i].active) continue;
        if (event_oracle_selected < 0 || (int32_t) (event_oracle_reference[i].when - event_oracle_reference[event_oracle_selected].when) < 0 ||
            (event_oracle_reference[i].when == event_oracle_reference[event_oracle_selected].when && event_oracle_reference[i].priority < event_oracle_reference[event_oracle_selected].priority)) event_oracle_selected = (int) i;
    }
}
static bool event_oracle_schedule(unsigned id, uint32_t when, uint8_t priority) {
    int32_t distance = (int32_t) (when - event_oracle_machine.now);
    if (id >= GBN_EVENT_COUNT || distance > (int32_t) GBN_MAX_DELAY || distance < -(int32_t) GBN_MAX_DELAY) return false;
    int old = event_oracle_selected;
    event_oracle_reference[id] = (struct GbnEvent) {when, priority, true};
    if (old < 0) event_oracle_selected = (int) id;
    else if (old == (int) id) event_oracle_scan();
    else if ((int32_t) (when - event_oracle_reference[old].when) < 0 ||
        (when == event_oracle_reference[old].when && priority < event_oracle_reference[old].priority)) event_oracle_selected = (int) id;
    return true;
}
static void event_oracle_cancel(unsigned id) {
    if (id >= GBN_EVENT_COUNT) return;
    event_oracle_reference[id].active = false;
    if (event_oracle_selected == (int) id) event_oracle_scan();
}
static void event_oracle_compare(void) {
    EVENT_ORACLE_CHECK(event_oracle_machine.next_event == event_oracle_selected);
    for (unsigned i = 0; i < GBN_EVENT_COUNT; ++i) {
        EVENT_ORACLE_CHECK(event_oracle_machine.events[i].when == event_oracle_reference[i].when);
        EVENT_ORACLE_CHECK(event_oracle_machine.events[i].priority == event_oracle_reference[i].priority);
        EVENT_ORACLE_CHECK(event_oracle_machine.events[i].active == event_oracle_reference[i].active);
    }
}
static void event_oracle_reset(uint32_t now) {
    gbn_init(&event_oracle_machine, NULL, NULL); event_oracle_machine.now = now;
    memset(event_oracle_reference, 0, sizeof(event_oracle_reference)); event_oracle_selected = -1;
}
static void event_oracle_add(unsigned id, uint32_t when, uint8_t priority) {
    ++event_oracle_operations;
    EVENT_ORACLE_CHECK(gbn_schedule_at(&event_oracle_machine, id, when, priority) == event_oracle_schedule(id, when, priority));
    event_oracle_compare();
}
static void event_oracle_remove_event(unsigned id) {
    ++event_oracle_operations; gbn_cancel(&event_oracle_machine, id); event_oracle_cancel(id); event_oracle_compare();
}
static void event_oracle_take(void) {
    ++event_oracle_operations;
    int want = -1; uint32_t lateness = 0;
    if (event_oracle_selected >= 0 && (int32_t) (event_oracle_machine.now - event_oracle_reference[event_oracle_selected].when) >= 0) {
        want = event_oracle_selected; lateness = event_oracle_machine.now - event_oracle_reference[want].when; event_oracle_cancel((unsigned) want);
    }
    uint32_t actual_lateness = 0xbadc0deu;
    EVENT_ORACLE_CHECK(gbn_take_event(&event_oracle_machine, &actual_lateness) == want);
    EVENT_ORACLE_CHECK(actual_lateness == (want < 0 ? 0xbadc0deu : lateness));
    event_oracle_compare();
}
static void check_event_oracle(void) {
    /* An equal non-head insertion retains the old winner; a later full
     * selection uses event ID. Keep these two observable rules distinct. */
    event_oracle_reset(0xfffffff0u);
    event_oracle_add(4, 2, 1); event_oracle_add(3, 2, 1); EVENT_ORACLE_CHECK(event_oracle_machine.next_event == 4);
    event_oracle_add(7, 2, 0); event_oracle_remove_event(7); EVENT_ORACLE_CHECK(event_oracle_machine.next_event == 3);
    event_oracle_add(3, 2, 1); EVENT_ORACLE_CHECK(event_oracle_machine.next_event == 3);
    event_oracle_machine.now = 2; event_oracle_take(); event_oracle_take(); event_oracle_take();
    /* Existing CPU fixtures event_oracle_reset the public event table directly. Reusing
     * a previously scheduled ID must not retain an unreachable self-link. */
    event_oracle_reset(0);
    event_oracle_add(0, 10, 0);
    event_oracle_machine.next_event = -1; memset(event_oracle_machine.events, 0, sizeof(event_oracle_machine.events));
    memset(event_oracle_reference, 0, sizeof(event_oracle_reference)); event_oracle_selected = -1;
    event_oracle_add(0, 20, 0); event_oracle_machine.now = 20; event_oracle_take();
    for (unsigned phase = 0; phase < 6; ++phase) {
        static const uint32_t starts[] = {0, 0xfffffff0u, 0x7ffffff0u};
        event_oracle_reset(starts[phase % 3]);
        for (unsigned trial = 0; trial < 24000; ++trial) {
            uint32_t value = event_oracle_random_word(); unsigned id = value >> 8 & 31;
            unsigned action = value % 6;
            if (action <= 2) {
                int32_t distance;
                if (phase < 3) distance = (int32_t) (event_oracle_random_word() % 4097) - 2048;
                else {
                    static const int32_t distances[] = {-(int32_t) GBN_MAX_DELAY, -1, 0, 1, (int32_t) GBN_MAX_DELAY};
                    distance = distances[event_oracle_random_word() % 5];
                }
                if (action == 2 && event_oracle_selected >= 0) id = (unsigned) event_oracle_selected;
                event_oracle_add(id, event_oracle_machine.now + (uint32_t) distance, (uint8_t) (event_oracle_random_word() % 8));
            } else if (action == 3) event_oracle_remove_event(id);
            else if (action == 4) event_oracle_take();
            else {
                struct Gbn snapshot = event_oracle_machine;
                EVENT_ORACLE_CHECK(!gbn_schedule_at(&event_oracle_machine, id, event_oracle_machine.now + GBN_MAX_DELAY + 1u, 0));
                EVENT_ORACLE_CHECK(!memcmp(&event_oracle_machine, &snapshot, sizeof(event_oracle_machine)));
                EVENT_ORACLE_CHECK(!gbn_schedule_at(&event_oracle_machine, id, event_oracle_machine.now - GBN_MAX_DELAY - 1u, 0));
                EVENT_ORACLE_CHECK(!memcmp(&event_oracle_machine, &snapshot, sizeof(event_oracle_machine)));
                EVENT_ORACLE_CHECK(!gbn_schedule(&event_oracle_machine, id, GBN_MAX_DELAY + 1u, 0));
                EVENT_ORACLE_CHECK(!memcmp(&event_oracle_machine, &snapshot, sizeof(event_oracle_machine)));
            }
            if (phase < 3 && trial % 17 == 0) {
                event_oracle_machine.now += event_oracle_random_word() % 32;
                while (event_oracle_selected >= 0 && (int32_t) (event_oracle_machine.now - event_oracle_reference[event_oracle_selected].when) >= 0) event_oracle_take();
            }
        }
    }
    printf("PASS event queue: %u public-state comparisons, %u schedule/cancel/take operations; full-scan oracle, ties, replacement, invalid calls and clock wrap\n", event_oracle_checks, event_oracle_operations);
}

#undef EVENT_ORACLE_CHECK
#endif
