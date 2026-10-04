// SPDX-License-Identifier: MIT OR Apache-2.0
#ifndef FVB_CONTROL_PARSE_H
#define FVB_CONTROL_PARSE_H
#include <ctype.h>
#include <stdint.h>

/* Parse decimal protocol fields without scanf overflow or a dependency on
 * the width of unsigned long (32 bits on the router, often 64 on a host). */
static inline int fvb_parse_decimal(const char **cursor, uint32_t maximum,
                                    uint32_t *result) {
    const char *p = *cursor;
    uint32_t value = 0;
    if (*p < '0' || *p > '9') return 0;
    do {
        unsigned digit = (unsigned)(*p - '0');
        if (value > maximum / 10 ||
            (value == maximum / 10 && digit > maximum % 10)) return 0;
        value = value * 10 + digit;
        ++p;
    } while (*p >= '0' && *p <= '9');
    *cursor = p;
    *result = value;
    return 1;
}

static inline int fvb_parse_timestamp(const char *text, uint32_t *timestamp) {
    return fvb_parse_decimal(&text, UINT32_MAX, timestamp) &&
           *text == 0 && *timestamp != 0;
}

static inline int fvb_parse_schedule(const char *text, unsigned fields[4]) {
    for (unsigned i = 0; i < 4; ++i) {
        uint32_t value;
        if (i && !isspace((unsigned char)*text)) return 0;
        while (isspace((unsigned char)*text)) ++text;
        if (!fvb_parse_decimal(&text, i < 2 ? 56 : 10079, &value)) return 0;
        if (i < 2 && value < 16) return 0;
        fields[i] = value;
    }
    return *text == 0 && fields[2] != fields[3];
}
#endif
