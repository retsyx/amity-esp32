/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// BSD sockets on the host's own TCP stack. select goes to lwip_select, as
// lwIP's LWIP_COMPAT_SOCKETS names it, so a wait follows the scenario's clock;
// see platform_fakes.hpp. Every header that declares select comes first.
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

// macOS names the keepalive idle time TCP_KEEPALIVE.
#if defined(__APPLE__) && !defined(TCP_KEEPIDLE)
#define TCP_KEEPIDLE TCP_KEEPALIVE
#endif

int lwip_select(int maxfdp1, fd_set *readset, fd_set *writeset, fd_set *exceptset, struct timeval *timeout);
#define select(maxfdp1, readset, writeset, exceptset, timeout) \
    lwip_select(maxfdp1, readset, writeset, exceptset, timeout)
