/*
 * Copyright 2026.
 * This file is part of Amity-esp32, licensed under GPL-3.0-or-later.
 */
#ifndef AMITY_HAP_INPUT_H
#define AMITY_HAP_INPUT_H

#include <limits.h>

/* Read one positive aid.iid pair without changing the input. Return 1 for a
 * pair, 0 at the end, and -1 for malformed input. Validate the entire list
 * before acting on any of its entries.
 */
static inline int hap_parse_characteristic_id(const char **cursor, int *aid, int *iid)
{
    if (!cursor || !*cursor || !aid || !iid) return -1;
    const char *p = *cursor;
    if (!*p) return 0;
    int values[2] = {0, 0};
    for (int part = 0; part < 2; ++part) {
        if (*p < '0' || *p > '9') return -1;
        do {
            const int digit = *p - '0';
            if (values[part] > (INT_MAX - digit) / 10) return -1;
            values[part] = values[part] * 10 + digit;
            ++p;
        } while (*p >= '0' && *p <= '9');
        if (!values[part]) return -1;
        if (!part) {
            if (*p != '.') return -1;
            ++p;
        }
    }
    if (*p && (*p != ',' || !p[1])) return -1;
    *aid = values[0];
    *iid = values[1];
    *cursor = *p ? p + 1 : p;
    return 1;
}

#endif
