/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// One flat packet buffer in place of the pinned NimBLE's mbuf chains. The fake
// host builds an ATT write's value in it, or collects a read response there.
// OS_MBUF_PKTLEN reports a uint16_t packet length, as in the SDK.
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OS_ENOMEM 1
#define OS_EINVAL 2

struct os_mbuf {
    uint16_t omp_len;
    uint16_t om_capacity;  // bytes the packet may hold, up to sizeof(om_databuf)
    uint8_t om_databuf[1024];
};

#define OS_MBUF_PKTLEN(__om) ((__om)->omp_len)

// Copies len bytes from offset off; non-zero when the packet is shorter.
int os_mbuf_copydata(const struct os_mbuf *m, int off, int len, void *dst);
// Appends data; OS_ENOMEM when it does not fit.
int os_mbuf_append(struct os_mbuf *m, const void *data, uint16_t len);

#ifdef __cplusplus
}
#endif
