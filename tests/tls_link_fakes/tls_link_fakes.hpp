/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// Scenario controls for the TLS link fakes: the link clock, the certificates
// and keys a scenario issues, and one scripted wire per socket descriptor,
// which holds the TLS session a link runs over that socket and the socket
// itself. Each scenario runs in its own process, so nothing here needs
// resetting. The fakes stop the scenario on calls no real link makes: socket
// calls on a descriptor without a wire or after close, a recv or send that
// could block, records before the handshake completed, a retried write with
// another buffer, an alert other than a fatal handshake_failure, and a link
// that keeps calling without time passing.
#include "mbedtls/ssl.h"
#include "amity/system/model.hpp"
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <fcntl.h>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace amity_fake::tls {

// Milliseconds on the link clock, which platform::now_ms() reads. A scenario
// sets it freely, backwards included; waits and slow calls advance it.
extern uint64_t clock_ms;
// The reads-th read of platform::now_ms() from now first sets the clock to
// to, as if the reading task were preempted just before it.
void jump_clock(unsigned reads, uint64_t to);

// A time that never comes.
constexpr uint64_t never = UINT64_MAX;

// A byte and the time it can first be read.
struct Byte {
    uint64_t at;
    uint8_t value;
};

// A call the handshake makes to the configured verify callback: the node of
// the presented chain, the depth reported, and the flags the standard
// verification gave that certificate.
struct VerifyCall {
    size_t node;
    int depth;
    uint32_t flags;
};

// A setsockopt call: an int option's value, or a timeval's milliseconds.
struct Option {
    int level, name;
    long value;
};

// A select call: the direction, when it began, its timeout, when it
// returned, and whether the socket was ready by then.
struct Select {
    bool write;
    uint64_t at, timeout_ms, woke;
    bool ready;
};

struct Wire {
    // The TLS session.
    // The handshake's steps, one per call: a WANT_READ or WANT_WRITE step
    // lasts until the socket turns readable at readable_at or writable at
    // writable_at, when the same call goes on to the next step. The last
    // step repeats. A step of 0 completes the handshake after the verify calls.
    std::vector<int> handshake{0};
    uint64_t readable_at = never;
    // The peer's certificate chain, leaf first, and the verify calls a
    // completing handshake makes. Without verify_calls it makes one, for the
    // leaf at depth 0 as a locally trusted certificate, with leaf_flags.
    std::vector<std::string> presented;
    std::vector<VerifyCall> verify_calls;
    uint32_t leaf_flags = 0;
    unsigned handshake_calls = 0, verify_callbacks = 0;
    // Decrypted application bytes, and how many were read. While read_error
    // is set every read returns it; each read takes read_elapsed.
    std::vector<Byte> incoming;
    size_t cursor = 0;
    unsigned reads = 0;
    std::optional<int> read_error;
    uint64_t read_elapsed = 0;
    // Records and alerts wait for the socket until writable_at. With
    // write_wants_read a blocked write reports WANT_READ instead, and the
    // socket turns readable at writable_at. A write accepts at most
    // partial_write bytes and takes write_elapsed; the writes after the first
    // error_after return write_error while it is set.
    uint64_t writable_at = 0;
    bool write_wants_read = false;
    size_t partial_write = 3;
    std::optional<int> write_error;
    unsigned error_after = 0;
    uint64_t write_elapsed = 0;
    unsigned write_calls = 0;
    size_t maximum_write = 0;
    std::vector<uint8_t> written;
    // The buffer of a write that was told to wait, which its retry must reuse.
    const unsigned char *blocked_buffer = nullptr;
    size_t blocked_size = 0;
    // Fatal alerts created, and flushes of a pending alert.
    unsigned alerts = 0, flushes = 0;

    // The TCP socket.
    int status_flags = O_RDWR;  // what F_GETFL reports
    bool fail_get_flags = false, fail_set_flags = false;
    std::optional<int> set_flags;  // the latest F_SETFL value
    std::optional<std::pair<int, int>> failing_option;  // the setsockopt level and name that fail
    std::vector<Option> options;
    // Bytes recv returns. Once none is ready, recv fails with recv_error, or
    // reports the end of the stream when it is 0.
    std::vector<Byte> raw;
    size_t raw_cursor = 0;
    int recv_error = EAGAIN;
    std::vector<size_t> recv_sizes;
    // What successive sends accept: a byte count, or -errno; -EAGAIN when empty.
    std::deque<long> send_results;
    std::vector<int> recv_flags, send_flags, shutdowns;
    unsigned closes = 0;
    std::vector<Select> selects;
    // The session and socket calls, in order: handshake, read, write, alert,
    // flush, get_flags, set_flags, setsockopt, select read, select write,
    // recv, send, shutdown read, shutdown write, shutdown both and close.
    std::vector<std::string> events;
};

// The wire of socket descriptor fd, created on first use.
Wire &wire(int fd);

// Certificate DER bytes that parse and that certificate_binding binds to id
// and pin, padded to size bytes.
std::string issue(amity::system::OctopusId id, const std::string &pin, size_t size = 300);
// Certificate bytes that do not parse.
std::string unparsable();
// A private key that parses.
std::string private_key();
// certificate_binding calls so far.
unsigned bindings();

// The next call of the named Mbed TLS configuration function fails:
// mbedtls_ssl_config_defaults, mbedtls_ssl_conf_own_cert, mbedtls_ssl_setup
// or mbedtls_ssl_set_hostname.
void fail_next(const char *function);
// Failures set by fail_next that no call has taken yet.
std::size_t pending_failures();

// Mbed TLS contexts initialised and not freed, and certificate allocations
// not freed.
std::size_t live_contexts();
std::size_t live_certificates();

}  // namespace amity_fake::tls
