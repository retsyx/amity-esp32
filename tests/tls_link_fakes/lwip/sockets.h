/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// lwIP's BSD socket calls on scripted sockets, one amity_fake::tls wire per
// descriptor; any other descriptor fails the scenario. The host headers give
// the types and option names. The calls are routed to lwip_* functions by
// macros, as lwIP's own LWIP_COMPAT_SOCKETS does, after the host headers have
// declared their own versions, so no header included later may call these
// names. Test sources include this header last.
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

// macOS names the keepalive idle time TCP_KEEPALIVE.
#if !defined(TCP_KEEPIDLE) && defined(TCP_KEEPALIVE)
#define TCP_KEEPIDLE TCP_KEEPALIVE
#endif

#ifdef __cplusplus
extern "C" {
#endif

ssize_t lwip_send(int s, const void *dataptr, size_t size, int flags);
ssize_t lwip_recv(int s, void *mem, size_t len, int flags);
int lwip_select(int maxfdp1, fd_set *readset, fd_set *writeset, fd_set *exceptset, struct timeval *timeout);
int lwip_shutdown(int s, int how);
int lwip_setsockopt(int s, int level, int optname, const void *optval, socklen_t optlen);
int lwip_close(int s);
int lwip_fcntl(int s, int cmd, int val);

#ifdef __cplusplus
}
#endif

// TlsLink has a member named send, so only a four-argument send is lwIP's.
#define AMITY_LWIP_SEND_PICK(_1, _2, _3, _4, _5, _6, name, ...) name
#define send(...) AMITY_LWIP_SEND_PICK(__VA_ARGS__, send, send, lwip_send, send, send, send, send)(__VA_ARGS__)
#define recv(s, mem, len, flags) lwip_recv(s, mem, len, flags)
#define select(maxfdp1, readset, writeset, exceptset, timeout) \
    lwip_select(maxfdp1, readset, writeset, exceptset, timeout)
#define shutdown(s, how) lwip_shutdown(s, how)
#define setsockopt(s, level, optname, opval, optlen) lwip_setsockopt(s, level, optname, opval, optlen)
#define close(s) lwip_close(s)
#define fcntl(s, cmd, val) lwip_fcntl(s, cmd, val)
