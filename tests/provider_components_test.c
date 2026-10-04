// SPDX-License-Identifier: MIT OR Apache-2.0
#include "provider_codec.h"
#include "provider_events.h"
#include "control_parse.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static void framing(void) {
    unsigned char bytes[33] = {7, 3, 0, 16};
    bytes[16] = 7;
    bytes[17] = 3;
    bytes[19] = 16;
    assert(provider_complete_batch(bytes, 16));
    assert(provider_complete_batch(bytes, 32));
    for (size_t size = 0; size < sizeof(bytes); ++size) {
        if (size != 16 && size != 32) assert(!provider_complete_batch(bytes, size));
    }
    bytes[19] = 0;
    assert(!provider_complete_batch(bytes, 32));
    bytes[19] = 17;
    assert(!provider_complete_batch(bytes, 32));
    bytes[19] = 16;
    bytes[17] = 4;
    assert(!provider_complete_batch(bytes, 32));
    /* Unaligned access and high bits must be portable to ARM. */
    write_be32(bytes + 1, UINT32_C(0xFEDCBA98));
    assert(!memcmp(bytes + 1, "\xfe\xdc\xba\x98", 4));
    assert(read_be32(bytes + 1) == UINT32_C(0xFEDCBA98));
    write_be16(bytes + 3, 0xFEDC);
    assert(read_be16(bytes + 3) == 0xFEDC);
}

static void events(void) {
    struct command_event event = {.sequence = 42, .state = 1, .has_level = 1, .level = 60};
    strcpy(event.endpoint, "FVB0123456789ABCDEF");
    char message[224];
    const char *expected = "{\"event\":\"command\",\"endpoint\":\"FVB0123456789ABCDEF\",\"state\":1,\"level\":60,\"sequence\":42}\n";
    assert(provider_event_format(&event, message, sizeof(message)) == (int)strlen(expected));
    assert(!strcmp(message, expected));
    /* A too-small destination must never yield a length unsafe for send(). */
    assert(provider_event_format(&event, message, strlen(expected)) == -1);
    assert(provider_event_format(&event, NULL, 0) == -1);
    event.has_level = 0;
    event.has_thermostat_timer = 1;
    event.thermostat_timer_mode = 255;
    event.thermostat_timer_previous = 3;
    assert(provider_event_format(&event, message, sizeof(message)) > 0);
    assert(strstr(message, "\"timer_action\":\"cancel\",\"timer_previous\":\"cold\""));

    struct provider_event_queue queue = {0};
    assert(!provider_event_pop(&queue, &event));
    for (unsigned i = 0; i < 100; ++i) {
        event.sequence = i;
        provider_event_push(&queue, &event);
    }
    /* Existing overflow policy: keep the newest 31 commands, in order. */
    assert(queue.dropped == 69);
    for (unsigned i = 69; i < 100; ++i) {
        assert(provider_event_pop(&queue, &event));
        assert(event.sequence == i);
        assert(!strcmp(event.endpoint, "FVB0123456789ABCDEF"));
    }
    assert(!provider_event_pop(&queue, &event));
}

int main(void) {
    uint32_t timestamp;
    unsigned schedule[4];
    assert(fvb_parse_timestamp("4294967295", &timestamp) && timestamp == UINT32_MAX);
    assert(!fvb_parse_timestamp("4294967296", &timestamp));
    assert(!fvb_parse_timestamp("99999999999999999999999999", &timestamp));
    assert(!fvb_parse_timestamp("-1", &timestamp));
    assert(!fvb_parse_timestamp("0", &timestamp));
    assert(!fvb_parse_timestamp("12garbage", &timestamp));
    assert(fvb_parse_schedule("40 36 100 200", schedule));
    assert(!fvb_parse_schedule("4294967336 36 100 200", schedule));
    assert(!fvb_parse_schedule("40 36 100 100", schedule));
    assert(!fvb_parse_schedule("40 36 100 10080", schedule));
    assert(!fvb_parse_schedule("40 36 100 200 extra", schedule));
    assert(!fvb_parse_schedule("40 36 -1 200", schedule));
    framing();
    events();
    puts("PASS provider codec, event formatting and bounded queue");
    return 0;
}
