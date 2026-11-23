/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// The consortium TLS link, built from the whole of tls_link.cpp over scripted
// Mbed TLS sessions and sockets: setup, the lazy handshake and the server's
// rejection of a missing client certificate, bounded framed sends, and the
// nonblocking receive state machine with its deadlines and diagnostics.
#include "platform.hpp"
#include "core_fakes.hpp"
#include "scenario_main.hpp"
#include "tls_link_fakes.hpp"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl_ciphersuites.h"
#include <algorithm>
#include <cstdio>
#include <initializer_list>
#include <map>
#include <string>
#include <tuple>
#include <vector>
// Last: its macros rename the socket calls.
#include "lwip/sockets.h"

namespace {

namespace tls = amity_fake::tls;
namespace model = amity::system;
using amity::consortium::frame;
using amity::consortium::max_frame_bytes;
using amity::consortium::platform::ConnectionAdmission;
using amity::consortium::platform::Identity;
using amity::consortium::platform::Peer;
using amity::consortium::platform::TlsLink;
using amity::consortium::platform::TlsReceiveDiagnostic;
using Result = TlsLink::ReceiveResult;
using Cause = TlsReceiveDiagnostic::Cause;
using tls::clock_ms;

model::OctopusId octopus(unsigned n) {
    char text[33];
    std::snprintf(text, sizeof(text), "%032x", n);
    return *model::OctopusId::parse(text);
}

// Roster peers by number, each with an issued certificate.
const Peer &roster(unsigned n) {
    static std::map<unsigned, Peer> peers;
    auto found = peers.find(n);
    if (found == peers.end()) {
        const std::string pin = "pin " + std::to_string(n);
        found = peers.emplace(n, Peer{octopus(n), tls::issue(octopus(n), pin), pin}).first;
    }
    return found->second;
}
const Peer &first() { return roster(1); }

const Identity &local() {
    static const Identity identity{octopus(9), tls::issue(octopus(9), "local pin"), tls::private_key(), {}};
    return identity;
}

// A certificate issued to an Amity outside every roster.
const std::string &stranger() {
    static const std::string der = tls::issue(octopus(7), "pin 7");
    return der;
}

int next_socket = 40;

// A link on its own admission pool and scripted socket.
struct Connection {
    int fd = next_socket++;
    tls::Wire &wire = tls::wire(fd);
    ConnectionAdmission pool;
    TlsLink link{pool};

    // Sets the link up as a server for the first roster peer, or as a client
    // that expects it. The peer presents its certificate in the handshake.
    bool setup(bool server = true) {
        wire.presented = {first().der};
        return link.setup(local(), {first()}, server, fd, server ? model::OctopusId{} : first().id);
    }
    void establish(bool server = true) {
        CHECK(setup(server));
        CHECK(link.handshake());
    }
};

// Queues a frame whose length prefix arrives at prefix_at and body at body_at.
void append(tls::Wire &wire, const std::string &body, uint64_t prefix_at = 0, uint64_t body_at = 0,
            bool bulk = false) {
    const auto bytes = frame(body, bulk);
    for (size_t i = 0; i < bytes.size(); ++i) wire.incoming.push_back({i < 4 ? prefix_at : body_at, bytes[i]});
}

std::vector<std::string> events_since(const tls::Wire &wire, size_t mark) {
    return {wire.events.begin() + static_cast<std::ptrdiff_t>(mark), wire.events.end()};
}

// The DER bytes of each certificate in a chain.
std::vector<std::string> chain(const mbedtls_x509_crt &crt) {
    std::vector<std::string> out;
    for (const auto *node = &crt; node && node->raw.p; node = node->next)
        out.emplace_back(reinterpret_cast<const char *>(node->raw.p), node->raw.len);
    return out;
}

std::vector<int> suites(const mbedtls_ssl_config &config) {
    std::vector<int> out;
    for (const int *suite = config.amity_ciphersuites; suite && *suite; ++suite) out.push_back(*suite);
    return out;
}

using Setting = std::tuple<int, int, long>;
std::vector<Setting> settings(const tls::Wire &wire) {
    std::vector<Setting> out;
    for (const auto &option : wire.options) out.emplace_back(option.level, option.name, option.value);
    std::sort(out.begin(), out.end());
    return out;
}

// Whether a setup timing line of area and stage carries every field.
bool timing(const std::string &area, const std::string &stage, std::initializer_list<std::string> fields) {
    for (const auto &line : amity_fake::log_lines()) {
        if (line.message.rfind("area=" + area + " stage=" + stage + " ", 0) != 0) continue;
        const std::string padded = " " + line.message + " ";
        bool all = true;
        for (const auto &field : fields) all = all && padded.find(" " + field + " ") != std::string::npos;
        if (all) return true;
    }
    return false;
}

// The handshake is lazy and runs once. A failed verification, and a fatal
// read or write, leave the link unusable without another handshake.
void lazy_handshake_and_transport_health() {
    std::string body;
    {
        Connection c;
        CHECK(c.setup());
        CHECK(!c.link.established() && c.link.usable());
        // Polling and expiring never start the blocking handshake.
        CHECK(c.link.poll_receive(body) == Result::failed && c.wire.handshake_calls == 0 && c.wire.reads == 0);
        CHECK(c.link.expire_receive() == Result::failed && c.wire.handshake_calls == 0);
        append(c.wire, "first response");
        CHECK(c.link.send("first request") && c.link.receive(body) && body == "first response" &&
              c.link.established() && c.wire.handshake_calls == 1);
        append(c.wire, "second response");
        CHECK(c.link.send("second request") && c.link.receive(body) && body == "second response" &&
              c.link.handshake() && c.wire.handshake_calls == 1);
        auto sent = frame("first request");
        const auto second = frame("second request");
        sent.insert(sent.end(), second.begin(), second.end());
        CHECK(c.wire.written == sent);
    }
    {
        // A receive can be the first exchange.
        Connection c;
        CHECK(c.setup());
        append(c.wire, "unprompted");
        CHECK(c.link.receive(body) && body == "unprompted" && c.wire.handshake_calls == 1);
    }
    {
        // The standard verification failed although the callback bound the peer.
        Connection rejected;
        CHECK(rejected.setup());
        rejected.wire.leaf_flags = MBEDTLS_X509_BADCERT_NOT_TRUSTED;
        CHECK(!rejected.link.send("request") && !rejected.link.usable() && rejected.wire.handshake_calls == 1 &&
              rejected.wire.write_calls == 0);
        rejected.wire.leaf_flags = 0;
        CHECK(!rejected.link.send("request") && rejected.wire.handshake_calls == 1 && rejected.wire.write_calls == 0);
    }
    for (int error : {0, -99}) {
        Connection broken;
        broken.establish();
        broken.wire.read_error = error;
        CHECK(broken.link.poll_receive(body) == Result::failed && !broken.link.usable() && !broken.link.established());
        const auto before = broken.wire.reads;
        broken.wire.read_error.reset();
        CHECK(!broken.link.receive(body) && !broken.link.send("later") && broken.wire.reads == before &&
              broken.wire.write_calls == 0 && broken.wire.handshake_calls == 1);
    }
    {
        Connection write;
        write.establish();
        write.wire.write_error = -99;
        CHECK(!write.link.send("request") && !write.link.usable());
        write.wire.write_error.reset();
        const auto before = write.wire.write_calls;
        CHECK(!write.link.send("later") && write.wire.write_calls == before && write.wire.handshake_calls == 1);
    }
    {
        // A payload no frame can carry neither opens nor breaks the transport.
        Connection invalid;
        CHECK(invalid.setup());
        CHECK(!invalid.link.send("") && invalid.link.usable() && invalid.wire.handshake_calls == 0 &&
              invalid.wire.write_calls == 0);
    }
}

// poll_receive never waits and never exposes a partial frame, so the link
// worker keeps serving its other work while a frame arrives.
void ipc_during_partial_frame() {
    Connection c;
    c.establish();
    append(c.wire, "complete owner observation", 0, 1200);
    std::string body;
    bool admitted = false, processed = false, complete = false;
    uint64_t admitted_at = 0;
    for (unsigned pass = 0; pass < 65; ++pass) {
        const auto before = clock_ms;
        const auto result = c.link.poll_receive(body);
        CHECK(clock_ms == before);
        CHECK(result != Result::failed);
        if (clock_ms == 20) {
            admitted = true;
            admitted_at = clock_ms;
        }
        // The worker pumps already admitted work on every turn.
        if (admitted && !processed) {
            CHECK(clock_ms - admitted_at < 1000);
            processed = true;
        }
        if (result == Result::frame) {
            CHECK(clock_ms == 1200 && body == "complete owner observation");
            complete = true;
            break;
        }
        CHECK(body.empty());
        clock_ms += 20;
    }
    CHECK(admitted && processed && complete && c.wire.selects.empty());
}

// Fragments assemble into one frame, and a poll never consumes the next
// frame. A poll reads at most 16 times. The blocking wrapper resumes the
// partial frame a poll left.
void fragments_and_frame_boundaries() {
    std::string body;
    {
        clock_ms = 0;
        Connection c;
        c.establish();
        const auto bytes = frame("fragmented");
        for (size_t i = 0; i < bytes.size(); ++i) c.wire.incoming.push_back({i * 10, bytes[i]});
        for (size_t i = 0; i < bytes.size(); ++i) {
            clock_ms = i * 10;
            CHECK(c.link.poll_receive(body) == (i + 1 == bytes.size() ? Result::frame : Result::pending));
        }
        CHECK(body == "fragmented");
    }
    {
        Connection pair;
        pair.establish();
        append(pair.wire, "first");
        append(pair.wire, "second");
        CHECK(pair.link.poll_receive(body) == Result::frame && body == "first" && pair.wire.cursor == 9);
        CHECK(pair.link.poll_receive(body) == Result::frame && body == "second" && pair.wire.cursor == 19);
    }
    {
        Connection maximum;
        maximum.establish();
        append(maximum.wire, std::string(max_frame_bytes, 'x'));
        CHECK(maximum.link.poll_receive(body) == Result::pending && maximum.wire.reads == 16);
        CHECK(maximum.link.poll_receive(body) == Result::frame && body.size() == max_frame_bytes);
    }
    // Switching from polls to the wrapper continues the same frame; a partial
    // prefix or body is never taken for the next frame's prefix.
    for (size_t available : {size_t{2}, size_t{6}}) {
        clock_ms = 0;
        Connection handover;
        handover.establish();
        const auto first_frame = frame("handover");
        for (size_t i = 0; i < first_frame.size(); ++i)
            handover.wire.incoming.push_back({i < available ? uint64_t{0} : uint64_t{100}, first_frame[i]});
        append(handover.wire, "following", 100, 100);
        CHECK(handover.link.poll_receive(body) == Result::pending && handover.wire.cursor == available);
        CHECK(handover.link.receive(body, 500) && body == "handover" && clock_ms == 100);
        CHECK(handover.link.poll_receive(body) == Result::frame && body == "following");
    }
}

// The 2 s progress and 10 s total frame deadlines, invalid lengths, the end
// of the stream and fatal reads; WANT_WRITE on a read only defers it.
void terminal_deadlines_and_errors() {
    std::string body;
    {
        clock_ms = 0;
        Connection idle;
        idle.establish();
        clock_ms = 20000;
        CHECK(idle.link.poll_receive(body) == Result::pending);
    }
    {
        clock_ms = 0;
        Connection progress;
        progress.establish();
        append(progress.wire, "later", 0, 3000);
        CHECK(progress.link.poll_receive(body) == Result::pending);
        clock_ms = 1999;
        CHECK(progress.link.poll_receive(body) == Result::pending);
        clock_ms = 2000;
        CHECK(progress.link.poll_receive(body) == Result::expired && progress.link.usable() && body.empty());
        clock_ms = 3000;
        CHECK(progress.link.poll_receive(body) == Result::pending && body.empty() && progress.link.usable());
    }
    {
        clock_ms = 0;
        Connection total;
        total.establish();
        const auto bytes = frame(std::string(20, 'x'));
        for (size_t i = 0; i < bytes.size(); ++i) total.wire.incoming.push_back({i < 4 ? 0 : (i - 3) * 1000, bytes[i]});
        for (clock_ms = 0; clock_ms < 10000; clock_ms += 1000) CHECK(total.link.poll_receive(body) == Result::pending);
        clock_ms = 9999;
        CHECK(total.link.poll_receive(body) == Result::pending);
        clock_ms = 10000;
        CHECK(total.link.poll_receive(body) == Result::expired && total.link.usable());
    }
    for (uint32_t length : {0u, static_cast<uint32_t>(max_frame_bytes + 1)}) {
        clock_ms = 0;
        Connection invalid;
        invalid.establish();
        for (unsigned shift : {24, 16, 8, 0})
            invalid.wire.incoming.push_back({0, static_cast<uint8_t>(length >> shift)});
        CHECK(invalid.link.poll_receive(body) == Result::failed &&
              invalid.link.receive_diagnostic().cause == Cause::invalid_length);
        invalid.wire.incoming.clear();
        invalid.wire.cursor = 0;
        append(invalid.wire, "valid later");
        CHECK(invalid.link.poll_receive(body) == Result::failed);
    }
    for (int error : {0, -99}) {
        Connection broken;
        broken.establish();
        broken.wire.read_error = error;
        CHECK(broken.link.poll_receive(body) == Result::failed);
    }
    {
        Connection write;
        write.establish();
        write.wire.read_error = MBEDTLS_ERR_SSL_WANT_WRITE;
        CHECK(write.link.poll_receive(body) == Result::pending);
        write.wire.read_error.reset();
        append(write.wire, "after want");
        CHECK(write.link.poll_receive(body) == Result::frame);
    }
}

// The bounded blocking receive and send wrappers wait on the socket. An
// expired wrapper keeps the partial frame and the healthy transport.
void blocking_wrappers() {
    std::string body;
    {
        clock_ms = 0;
        Connection c;
        c.establish();
        append(c.wire, "hello", 0, 1200);
        CHECK(c.link.receive(body, 2000) && clock_ms == 1200 && body == "hello");
        CHECK(!c.wire.selects.empty());
    }
    for (size_t available : {size_t{0}, size_t{2}, size_t{6}}) {
        clock_ms = 0;
        Connection expired;
        expired.establish();
        const auto bytes = frame("late hello");
        for (size_t i = 0; i < bytes.size(); ++i)
            expired.wire.incoming.push_back({i < available ? uint64_t{0} : uint64_t{1200}, bytes[i]});
        append(expired.wire, "following", 1200, 1200);
        CHECK(!expired.link.receive(body, 1000) && clock_ms == 1000 && expired.link.usable() &&
              expired.link.established());
        const auto timeout = expired.link.receive_diagnostic();
        CHECK(timeout.cause == Cause::wrapper_timeout && timeout.bytes == available &&
              timeout.expected == (available >= 4 ? bytes.size() : 4));
        CHECK(expired.link.receive(body, 1000) && clock_ms == 1200 && body == "late hello" &&
              expired.wire.handshake_calls == 1);
        CHECK(expired.link.receive_diagnostic().cause == Cause::none);
        CHECK(expired.link.poll_receive(body) == Result::frame && body == "following");
    }
    {
        clock_ms = 0;
        Connection sender;
        sender.establish();
        sender.wire.writable_at = 1200;
        CHECK(sender.link.send("hello") && clock_ms == 1200 && sender.wire.written == frame("hello"));
        CHECK(!sender.wire.selects.empty());
    }
    {
        // A write still pending at the three-second budget breaks the link.
        clock_ms = 0;
        Connection blocked;
        blocked.establish();
        blocked.wire.writable_at = 3100;
        CHECK(!blocked.link.send("late") && clock_ms == 3000 && blocked.wire.written.empty() &&
              !blocked.link.usable());
        // It polled the socket for writing every 100 ms at most.
        CHECK(blocked.wire.selects.size() == 30);
        for (const auto &wait : blocked.wire.selects) CHECK(wait.write && wait.timeout_ms <= 100 && !wait.ready);
    }
    {
        // Decrypted bytes already buffered need no wait.
        clock_ms = 0;
        Connection buffered;
        buffered.establish();
        append(buffered.wire, std::string(max_frame_bytes, 'b'));
        CHECK(buffered.link.receive(body, 1000) && body.size() == max_frame_bytes && buffered.wire.reads > 16);
        CHECK(buffered.wire.selects.empty() && clock_ms == 0);
    }
}

// The socket callbacks never block: an empty socket or a full send buffer
// asks Mbed TLS to retry, other errors fail, and bytes pass through.
void socket_callbacks() {
    Connection c;
    CHECK(c.setup());
    auto &ssl = c.link.ssl;
    CHECK(ssl.amity_bio == &c.link.socket && ssl.amity_send && ssl.amity_recv && !ssl.amity_recv_timeout);
    unsigned char bytes[8] = {};
    CHECK(ssl.amity_recv(ssl.amity_bio, bytes, sizeof(bytes)) == MBEDTLS_ERR_SSL_WANT_READ &&
          c.wire.recv_flags.back() == MSG_DONTWAIT && c.wire.recv_sizes.back() == sizeof(bytes));
    CHECK(ssl.amity_send(ssl.amity_bio, bytes, sizeof(bytes)) == MBEDTLS_ERR_SSL_WANT_WRITE &&
          c.wire.send_flags.back() == MSG_DONTWAIT);
    c.wire.recv_error = EINTR;
    c.wire.send_results = {-EINTR};
    CHECK(ssl.amity_recv(ssl.amity_bio, bytes, sizeof(bytes)) == MBEDTLS_ERR_SSL_WANT_READ);
    CHECK(ssl.amity_send(ssl.amity_bio, bytes, sizeof(bytes)) == MBEDTLS_ERR_SSL_WANT_WRITE);
    c.wire.raw = {{0, 'a'}, {0, 'b'}};
    c.wire.send_results = {5};
    CHECK(ssl.amity_recv(ssl.amity_bio, bytes, sizeof(bytes)) == 2 && bytes[0] == 'a' && bytes[1] == 'b');
    CHECK(ssl.amity_send(ssl.amity_bio, bytes, sizeof(bytes)) == 5);
    c.wire.recv_error = ECONNRESET;
    c.wire.send_results = {-EPIPE};
    CHECK(ssl.amity_recv(ssl.amity_bio, bytes, sizeof(bytes)) == MBEDTLS_ERR_NET_RECV_FAILED);
    CHECK(ssl.amity_send(ssl.amity_bio, bytes, sizeof(bytes)) == MBEDTLS_ERR_NET_SEND_FAILED);
    c.wire.recv_error = 0;
    CHECK(ssl.amity_recv(ssl.amity_bio, bytes, sizeof(bytes)) == 0);
    CHECK(c.wire.recv_flags.size() == 5 && c.wire.send_flags.size() == 4);
}

// Ordinary writes use records of at most 512 bytes within a three-second
// budget that progress does not renew, and retry a waiting write unchanged.
void bounded_application_writes() {
    for (size_t size : {size_t{1}, size_t{508}, size_t{509}, max_frame_bytes}) {
        clock_ms = 0;
        Connection c;
        c.establish();
        c.wire.partial_write = 97;
        c.wire.writable_at = 30;
        const std::string body(size, 'x');
        CHECK(c.link.send(body) && c.wire.written == frame(body));
        CHECK(c.wire.maximum_write <= 512 && clock_ms == 30);
    }
    {
        clock_ms = 0;
        Connection want;
        want.establish();
        want.wire.write_wants_read = true;
        want.wire.writable_at = 20;
        CHECK(want.link.send(std::string(600, 'x')) && want.wire.maximum_write == 512 && clock_ms == 20);
        for (const auto &wait : want.wire.selects) CHECK(!wait.write);
    }
    for (int error : {0, -99}) {
        Connection failed;
        failed.establish();
        failed.wire.write_error = error;
        failed.wire.error_after = 1;
        failed.wire.partial_write = 17;
        const std::string body(1000, 'x');
        const auto bytes = frame(body);
        CHECK(!failed.link.send(body) && failed.wire.write_calls == 2 &&
              failed.wire.written == std::vector<uint8_t>(bytes.begin(), bytes.begin() + 17));
        CHECK(!failed.link.usable());
    }
    {
        clock_ms = 0;
        Connection slow;
        slow.establish();
        slow.wire.write_elapsed = 20;
        CHECK(!slow.link.send(std::string(max_frame_bytes, 'x')) && clock_ms == 3000 && slow.wire.write_calls == 150);
    }
    for (const std::string &body : {std::string{}, std::string(max_frame_bytes + 1, 'x')}) {
        Connection invalid;
        invalid.establish();
        CHECK(!invalid.link.send(body) && invalid.wire.write_calls == 0 && invalid.link.usable());
    }
}

// Bulk frames take 30 s progress and 60 s total receive budgets, a 30 s send
// budget and 2048-byte records; ordinary frames keep theirs, before and
// after a bulk frame.
void bulk_transfers() {
    std::string body;
    {
        clock_ms = 0;
        Connection bulk;
        bulk.establish();
        append(bulk.wire, "firmware payload", 0, 2500, true);
        append(bulk.wire, "ordinary after bulk", 2500, 5001);
        CHECK(bulk.link.poll_receive(body) == Result::pending && bulk.link.receiving_bulk());
        clock_ms = 2499;
        CHECK(bulk.link.poll_receive(body) == Result::pending && bulk.link.usable());
        clock_ms = 2500;
        CHECK(bulk.link.poll_receive(body) == Result::frame && body == "firmware payload" &&
              bulk.link.received_bulk() && !bulk.link.receiving_bulk());
        CHECK(bulk.link.poll_receive(body) == Result::pending && !bulk.link.receiving_bulk());
        clock_ms = 4500;
        CHECK(bulk.link.poll_receive(body) == Result::expired &&
              bulk.link.receive_diagnostic().cause == Cause::progress_timeout && bulk.link.usable());
        clock_ms = 5001;
        CHECK(bulk.link.poll_receive(body) == Result::pending && body.empty() && bulk.link.usable());
        append(bulk.wire, "ordinary recovered", 5001, 5001);
        CHECK(bulk.link.poll_receive(body) == Result::frame && !bulk.link.received_bulk() &&
              body == "ordinary recovered");
    }
    {
        clock_ms = 0;
        Connection stalled;
        stalled.establish();
        append(stalled.wire, "late bulk", 0, 31000, true);
        append(stalled.wire, "fresh ordinary", 31000, 31000);
        CHECK(stalled.link.poll_receive(body) == Result::pending && stalled.link.receiving_bulk());
        clock_ms = 29999;
        CHECK(stalled.link.poll_receive(body) == Result::pending);
        clock_ms = 30000;
        CHECK(stalled.link.poll_receive(body) == Result::expired && stalled.link.usable() &&
              stalled.link.receive_diagnostic().cause == Cause::progress_timeout &&
              stalled.link.receive_diagnostic().progress_ms == 30000);
        clock_ms = 31000;
        CHECK(stalled.link.poll_receive(body) == Result::pending && body.empty() && stalled.link.usable());
        CHECK(stalled.link.poll_receive(body) == Result::frame && body == "fresh ordinary" &&
              !stalled.link.received_bulk());
    }
    {
        clock_ms = 0;
        Connection total;
        total.establish();
        const auto slow = frame(std::string(100, 'x'), true);
        for (size_t i = 0; i < slow.size(); ++i) total.wire.incoming.push_back({i < 4 ? 0 : (i - 3) * 1000, slow[i]});
        append(total.wire, "after bulk total", 101000, 101000);
        for (clock_ms = 0; clock_ms < 60000; clock_ms += 1000) CHECK(total.link.poll_receive(body) == Result::pending);
        clock_ms = 59999;
        CHECK(total.link.poll_receive(body) == Result::pending);
        clock_ms = 60000;
        CHECK(total.link.poll_receive(body) == Result::expired && total.link.usable() &&
              total.link.receive_diagnostic().cause == Cause::frame_timeout &&
              total.link.receive_diagnostic().total_ms == 60000);
        clock_ms = 101000;
        CHECK(total.link.poll_receive(body) == Result::pending && body.empty());
        CHECK(total.link.poll_receive(body) == Result::frame && body == "after bulk total" &&
              !total.link.received_bulk());
    }
    for (bool read_want : {false, true}) {
        clock_ms = 0;
        Connection sender;
        sender.establish();
        sender.wire.write_wants_read = read_want;
        sender.wire.writable_at = 4000;
        CHECK(sender.link.send("bulk send", true) && clock_ms == 4000 &&
              sender.wire.written == frame("bulk send", true) && sender.wire.maximum_write <= 512);
    }
    {
        clock_ms = 0;
        Connection blocked;
        blocked.establish();
        blocked.wire.writable_at = 31000;
        CHECK(!blocked.link.send("late bulk send", true) && clock_ms == 30000 && blocked.wire.written.empty() &&
              !blocked.link.usable());
    }
    {
        clock_ms = 0;
        Connection slow_sender;
        slow_sender.establish();
        slow_sender.wire.partial_write = 1;
        slow_sender.wire.write_elapsed = 10;
        CHECK(!slow_sender.link.send(std::string(max_frame_bytes, 'x'), true) && clock_ms == 30000 &&
              slow_sender.wire.write_calls == 3000);
    }
    {
        clock_ms = 0;
        Connection ordinary;
        ordinary.establish();
        ordinary.wire.writable_at = 4000;
        CHECK(!ordinary.link.send("ordinary remains bounded") && clock_ms == 3000 && !ordinary.link.usable());
    }
    // A 4779-byte frame takes 3 bulk records or 10 ordinary ones.
    for (bool bulk : {false, true}) {
        Connection sender;
        sender.establish();
        sender.wire.partial_write = max_frame_bytes;
        const std::string body(4775, 'x');
        CHECK(sender.link.send(body, bulk) && sender.wire.written == frame(body, bulk));
        CHECK(sender.wire.maximum_write == (bulk ? 2048u : 512u) && sender.wire.write_calls == (bulk ? 3u : 10u));
    }
    {
        clock_ms = 0;
        Connection bounded_bulk;
        bounded_bulk.establish();
        bounded_bulk.wire.partial_write = 97;
        bounded_bulk.wire.writable_at = 4000;
        bounded_bulk.wire.write_wants_read = true;
        CHECK(bounded_bulk.link.send(std::string(max_frame_bytes, 'x'), true) &&
              bounded_bulk.wire.maximum_write == 2048 &&
              bounded_bulk.wire.written == frame(std::string(max_frame_bytes, 'x'), true));
    }
}

// A failed or expired receive records its cause, code, bytes received and
// expected, progress and total time, the gap between polls and the slowest read.
void terminal_receive_diagnostics() {
    std::string body;
    {
        clock_ms = 0;
        Connection starved;
        starved.establish();
        append(starved.wire, "buffered body", 0, 100);
        CHECK(starved.link.poll_receive(body) == Result::pending);
        clock_ms = 2300;
        CHECK(starved.link.poll_receive(body) == Result::expired && starved.link.usable());
        const auto captured = starved.link.receive_diagnostic();
        CHECK(captured.cause == Cause::progress_timeout && captured.bytes == 4 && captured.expected == 17 &&
              captured.progress_ms == 2300 && captured.total_ms == 2300 && captured.poll_gap_ms == 2300 &&
              captured.read_ms == 0);
        clock_ms = 9900;
        CHECK(starved.link.poll_receive(body) == Result::pending &&
              starved.link.receive_diagnostic().progress_ms == captured.progress_ms && starved.link.usable());
    }
    {
        // The task is preempted just before it feeds bytes it read in time.
        clock_ms = 0;
        Connection preempted;
        preempted.establish();
        append(preempted.wire, "body after preemption", 0, 1);
        CHECK(preempted.link.poll_receive(body) == Result::pending);
        clock_ms = 1;
        tls::jump_clock(5, 2100);
        CHECK(preempted.link.poll_receive(body) == Result::expired && preempted.link.usable() &&
              preempted.link.receive_diagnostic().cause == Cause::progress_timeout);
    }
    {
        // Time spent inside the SDK's read is not a gap between polls.
        clock_ms = 0;
        Connection blocked;
        blocked.establish();
        append(blocked.wire, "blocked inside SDK");
        blocked.wire.read_elapsed = 2100;
        CHECK(blocked.link.poll_receive(body) == Result::expired && blocked.link.usable() &&
              blocked.link.receive_diagnostic().cause == Cause::progress_timeout &&
              blocked.link.receive_diagnostic().read_ms == 2100 && blocked.link.receive_diagnostic().poll_gap_ms == 0);
    }
    {
        clock_ms = 0;
        Connection wrapper;
        wrapper.establish();
        CHECK(!wrapper.link.receive(body, 100) && wrapper.link.usable() &&
              wrapper.link.receive_diagnostic().cause == Cause::wrapper_timeout &&
              wrapper.link.receive_diagnostic().bytes == 0);
        // A later fatal read replaces the application timeout.
        wrapper.wire.read_error = -123;
        CHECK(wrapper.link.poll_receive(body) == Result::failed &&
              wrapper.link.receive_diagnostic().cause == Cause::tls_error &&
              wrapper.link.receive_diagnostic().code == -123);
    }
    {
        // A fatal read mid-frame records the bytes it had and expected.
        clock_ms = 0;
        Connection partial;
        partial.establish();
        append(partial.wire, "cut short", 0, 3000);
        CHECK(partial.link.poll_receive(body) == Result::pending);
        partial.wire.read_error = -123;
        CHECK(partial.link.poll_receive(body) == Result::failed &&
              partial.link.receive_diagnostic().cause == Cause::tls_error &&
              partial.link.receive_diagnostic().bytes == 4 && partial.link.receive_diagnostic().expected == 13);
    }
    {
        // Idle polls before a frame begins are no gap in it.
        clock_ms = 0;
        Connection idle;
        idle.establish();
        append(idle.wire, "arrives later", 2500, 9000);
        CHECK(idle.link.poll_receive(body) == Result::pending);
        clock_ms = 2500;
        CHECK(idle.link.poll_receive(body) == Result::pending);
        clock_ms = 4500;
        CHECK(idle.link.poll_receive(body) == Result::expired &&
              idle.link.receive_diagnostic().poll_gap_ms == 2000 && idle.link.receive_diagnostic().progress_ms == 2000);
    }
    {
        // Each frame's diagnostics start afresh.
        clock_ms = 0;
        Connection again;
        again.establish();
        append(again.wire, "slow read");
        again.wire.read_elapsed = 500;
        CHECK(again.link.poll_receive(body) == Result::frame && body == "slow read");
        again.wire.read_elapsed = 0;
        append(again.wire, "next", clock_ms, clock_ms + 5000);
        CHECK(again.link.poll_receive(body) == Result::pending);
        clock_ms += 2000;
        CHECK(again.link.poll_receive(body) == Result::expired && again.link.receive_diagnostic().read_ms == 0 &&
              again.link.receive_diagnostic().poll_gap_ms == 2000);
    }
    {
        Connection eof;
        eof.establish();
        eof.wire.read_error = 0;
        CHECK(eof.link.poll_receive(body) == Result::failed && eof.link.receive_diagnostic().cause == Cause::tls_eof &&
              eof.link.receive_diagnostic().code == 0);
    }
    {
        Connection fatal;
        fatal.establish();
        fatal.wire.read_error = -123;
        CHECK(fatal.link.poll_receive(body) == Result::failed &&
              fatal.link.receive_diagnostic().cause == Cause::tls_error &&
              fatal.link.receive_diagnostic().code == -123);
    }
    {
        clock_ms = 0;
        Connection invalid;
        invalid.establish();
        for (unsigned i = 0; i < 4; ++i) invalid.wire.incoming.push_back({0, 0});
        CHECK(invalid.link.poll_receive(body) == Result::failed &&
              invalid.link.receive_diagnostic().cause == Cause::invalid_length &&
              invalid.link.receive_diagnostic().bytes == 4);
    }
    {
        clock_ms = 100;
        Connection reversed;
        reversed.establish();
        append(reversed.wire, "later", 100, 3000);
        CHECK(reversed.link.poll_receive(body) == Result::pending);
        clock_ms = 99;
        CHECK(reversed.link.poll_receive(body) == Result::expired && reversed.link.usable() &&
              reversed.link.receive_diagnostic().cause == Cause::clock_reversed);
    }
    {
        clock_ms = 0;
        Connection total;
        total.establish();
        const auto bytes = frame(std::string(20, 'x'));
        for (size_t i = 0; i < bytes.size(); ++i) total.wire.incoming.push_back({i * 1000, bytes[i]});
        for (clock_ms = 0; clock_ms < 10000; clock_ms += 1000) CHECK(total.link.poll_receive(body) == Result::pending);
        CHECK(total.link.poll_receive(body) == Result::expired && total.link.usable() &&
              total.link.receive_diagnostic().cause == Cause::frame_timeout &&
              total.link.receive_diagnostic().total_ms == 10000);
    }
}

// An expired frame is drained to its last byte on the same TLS session, and
// the next frame arrives intact.
void expired_frames_drain() {
    // The 512- and 582-byte observation from the bench, and partial prefixes.
    for (size_t available : {size_t{1}, size_t{2}, size_t{3}, size_t{4}, size_t{512}}) {
        clock_ms = 0;
        Connection c;
        c.establish();
        std::string body;
        const auto expired = frame(std::string(578, 'x'));
        for (size_t i = 0; i < expired.size(); ++i)
            c.wire.incoming.push_back({i < available ? uint64_t{0} : uint64_t{3100}, expired[i]});
        append(c.wire, "fresh round", 3100, 3100);
        CHECK(c.link.poll_receive(body) == Result::pending && c.wire.cursor == available);
        clock_ms = 2000;
        const auto read_before = c.wire.reads;
        CHECK(c.link.poll_receive(body) == Result::expired && c.wire.reads == read_before && body.empty() &&
              c.link.established());
        const auto diagnostic = c.link.receive_diagnostic();
        CHECK(diagnostic.expired() && !diagnostic.fatal() && diagnostic.bytes == available &&
              diagnostic.progress_ms == 2000);
        clock_ms = 2500;
        CHECK(c.link.poll_receive(body) == Result::pending && body.empty() && c.link.usable());
        clock_ms = 3100;
        CHECK(c.link.poll_receive(body) == Result::pending && body.empty() && c.wire.cursor == expired.size() &&
              c.link.usable());
        CHECK(c.link.receive_diagnostic().progress_ms == 2000);
        CHECK(c.link.poll_receive(body) == Result::frame && body == "fresh round" && c.wire.handshake_calls == 1 &&
              c.link.receive_diagnostic().cause == Cause::none);
    }
    std::string body;
    {
        // An outer budget can expire before a record yields a prefix.
        clock_ms = 0;
        Connection encrypted;
        encrypted.establish();
        append(encrypted.wire, "old encrypted frame", 3000, 3000);
        append(encrypted.wire, "after encrypted", 3000, 3000);
        CHECK(encrypted.link.expire_receive() == Result::expired && encrypted.link.usable() &&
              encrypted.link.receive_diagnostic().cause == Cause::operation_timeout);
        CHECK(encrypted.link.expire_receive() == Result::pending);
        clock_ms = 3000;
        CHECK(encrypted.link.poll_receive(body) == Result::pending && body.empty());
        CHECK(encrypted.link.poll_receive(body) == Result::frame && body == "after encrypted");
    }
    {
        // Slow continuous progress does not extend the total deadline.
        clock_ms = 0;
        Connection total;
        total.establish();
        const auto slow = frame(std::string(20, 'x'));
        for (size_t i = 0; i < slow.size(); ++i) total.wire.incoming.push_back({i < 4 ? 0 : (i - 3) * 1000, slow[i]});
        append(total.wire, "after total", 21000, 21000);
        for (clock_ms = 0; clock_ms < 10000; clock_ms += 1000) CHECK(total.link.poll_receive(body) == Result::pending);
        CHECK(total.link.poll_receive(body) == Result::expired && total.link.receive_diagnostic().total_ms == 10000);
        clock_ms = 21000;
        CHECK(total.link.poll_receive(body) == Result::pending && body.empty() && total.link.usable());
        CHECK(total.link.poll_receive(body) == Result::frame && body == "after total");
    }
    // A fatal read while draining, and an invalid late prefix, break the link.
    for (int error : {0, -99}) {
        clock_ms = 0;
        Connection broken;
        broken.establish();
        append(broken.wire, "late", 0, 3000);
        CHECK(broken.link.poll_receive(body) == Result::pending);
        clock_ms = 2000;
        CHECK(broken.link.poll_receive(body) == Result::expired);
        broken.wire.read_error = error;
        CHECK(broken.link.poll_receive(body) == Result::failed && !broken.link.usable() &&
              broken.link.receive_diagnostic().fatal());
    }
    {
        clock_ms = 0;
        Connection invalid;
        invalid.establish();
        invalid.wire.incoming = {{0, 0}, {0, 0}, {3000, 0}, {3000, 0}};
        CHECK(invalid.link.poll_receive(body) == Result::pending);
        clock_ms = 2000;
        CHECK(invalid.link.poll_receive(body) == Result::expired && invalid.link.usable());
        clock_ms = 3000;
        CHECK(invalid.link.poll_receive(body) == Result::failed && !invalid.link.usable() &&
              invalid.link.receive_diagnostic().cause == Cause::invalid_length);
    }
    {
        // A blocking caller returns at the expiry. Later waits drain, keeping
        // the expiry's evidence rather than a wrapper timeout.
        clock_ms = 0;
        Connection wrapper;
        wrapper.establish();
        append(wrapper.wire, "too late", 0, 5000);
        append(wrapper.wire, "resumed", 5000, 5000);
        CHECK(!wrapper.link.receive(body, 3000) && clock_ms == 2000 && wrapper.link.usable() &&
              wrapper.link.receive_diagnostic().expired());
        CHECK(!wrapper.link.receive(body, 1000) && clock_ms == 3000 &&
              wrapper.link.receive_diagnostic().progress_ms == 2000);
        CHECK(!wrapper.wire.selects.empty());
        clock_ms = 5000;
        CHECK(wrapper.link.receive(body, 1000) && body == "resumed" && wrapper.wire.handshake_calls == 1);
    }
    {
        // Draining a maximum frame stays bounded per poll.
        clock_ms = 0;
        Connection maximum;
        maximum.establish();
        append(maximum.wire, std::string(max_frame_bytes, 'x'), 0, 3000);
        append(maximum.wire, "after maximum", 3000, 3000);
        CHECK(maximum.link.poll_receive(body) == Result::pending);
        clock_ms = 2000;
        CHECK(maximum.link.poll_receive(body) == Result::expired);
        clock_ms = 3000;
        bool resumed = false;
        for (unsigned turn = 0; turn < 4; ++turn) {
            const auto before = maximum.wire.reads;
            const auto result = maximum.link.poll_receive(body);
            CHECK(maximum.wire.reads - before <= 16);
            CHECK(result != Result::expired && result != Result::failed);
            if (result == Result::frame) {
                CHECK(body == "after maximum");
                resumed = true;
                break;
            }
        }
        CHECK(resumed && maximum.link.usable() && maximum.wire.selects.empty());
    }
}

// A server rejects a client without a certificate with exactly one fatal
// handshake_failure alert, then half-closes and drains at most 4 KiB without
// blocking, within 250 ms.
void missing_client_certificate() {
    Connection c;
    CHECK(c.setup());
    const auto mark = c.wire.events.size();
    c.wire.handshake = {MBEDTLS_ERR_SSL_NO_CLIENT_CERTIFICATE};
    for (unsigned i = 0; i < 9000; ++i) c.wire.raw.push_back({0, static_cast<uint8_t>(i)});
    CHECK(!c.link.handshake() && !c.link.usable());
    CHECK(c.wire.handshake_calls == 1 && c.wire.alerts == 1 && c.wire.flushes == 0);
    CHECK(c.wire.shutdowns == std::vector<int>{SHUT_WR});
    CHECK(c.wire.raw_cursor == 4096 && c.wire.set_flags == (O_RDWR | O_NONBLOCK) && clock_ms <= 250);
    for (const auto size : c.wire.recv_sizes) CHECK(size <= 256);
    std::vector<std::string> expected{"handshake", "get_flags", "set_flags", "alert", "shutdown write"};
    expected.insert(expected.end(), 16, "recv");
    CHECK(events_since(c.wire, mark) == expected);
    CHECK(timing("tls_handshake", "reject_alert", {"result=failed", "code=-29824"}));
    CHECK(timing("tls_handshake", "handshake", {"result=failed", "count=1", "code=-29824"}));
}

// An alert the SDK already queued is flushed, not replaced, and an empty
// socket ends the drain at the 250 ms deadline.
void queued_alert_flushed() {
    Connection c;
    CHECK(c.setup());
    c.wire.handshake = {MBEDTLS_ERR_X509_CERT_VERIFY_FAILED};
    c.link.ssl.MBEDTLS_PRIVATE(out_left) = 7;
    CHECK(!c.link.handshake() && c.wire.alerts == 0 && c.wire.flushes == 1 && !c.link.ssl.MBEDTLS_PRIVATE(out_left));
    CHECK(clock_ms == 250 && c.wire.raw_cursor == 0);
    CHECK(c.wire.shutdowns == std::vector<int>{SHUT_WR});
    for (const auto &wait : c.wire.selects) CHECK(!wait.write && wait.timeout_ms <= 100);
}

// A fatal alert that cannot be written keeps the rejection within 250 ms,
// without resuming the handshake. One written late leaves the drain the rest
// of the same 250 ms.
void blocked_alert() {
    {
        Connection c;
        CHECK(c.setup());
        c.wire.handshake = {MBEDTLS_ERR_SSL_NO_CLIENT_CERTIFICATE};
        c.wire.writable_at = tls::never;
        CHECK(!c.link.handshake() && c.wire.handshake_calls == 1 && clock_ms <= 250);
        CHECK(c.wire.alerts == 1 && c.wire.flushes >= 1 && c.wire.shutdowns == std::vector<int>{SHUT_WR});
        for (const auto &wait : c.wire.selects) CHECK(wait.write && wait.timeout_ms <= 100);
    }
    {
        clock_ms = 0;
        Connection c;
        CHECK(c.setup());
        c.wire.handshake = {MBEDTLS_ERR_SSL_NO_CLIENT_CERTIFICATE};
        c.wire.writable_at = 120;
        for (unsigned i = 0; i < 100; ++i) c.wire.raw.push_back({200, 0});
        for (unsigned i = 0; i < 100; ++i) c.wire.raw.push_back({300, 0});
        CHECK(!c.link.handshake() && c.wire.alerts == 1 && c.wire.flushes == 3);
        CHECK(clock_ms == 250 && c.wire.raw_cursor == 100);
    }
}

// The rejection drain stops at the end of the stream and on a socket error,
// whatever errno an earlier call left, and waits after an interrupted recv.
void rejection_drain_ends() {
    {
        Connection c;
        CHECK(c.setup());
        c.wire.handshake = {MBEDTLS_ERR_SSL_NO_CLIENT_CERTIFICATE};
        for (unsigned i = 0; i < 300; ++i) c.wire.raw.push_back({0, 0});
        c.wire.recv_error = 0;
        errno = EAGAIN;
        CHECK(!c.link.handshake() && c.wire.raw_cursor == 300 && c.wire.recv_sizes.size() == 3);
        CHECK(c.wire.selects.empty() && clock_ms == 0);
    }
    {
        Connection c;
        CHECK(c.setup());
        c.wire.handshake = {MBEDTLS_ERR_SSL_NO_CLIENT_CERTIFICATE};
        c.wire.raw.push_back({0, 0});
        c.wire.recv_error = ECONNRESET;
        CHECK(!c.link.handshake() && c.wire.raw_cursor == 1 && c.wire.recv_sizes.size() == 2);
        CHECK(c.wire.selects.empty() && clock_ms == 0);
    }
    {
        Connection c;
        CHECK(c.setup());
        c.wire.handshake = {MBEDTLS_ERR_SSL_NO_CLIENT_CERTIFICATE};
        c.wire.recv_error = EINTR;
        CHECK(!c.link.handshake() && clock_ms == 250 && c.wire.selects.size() == 3);
        for (const auto &wait : c.wire.selects) CHECK(!wait.write);
    }
}

// Only a server rejects: a client's failed handshake sends nothing.
void client_does_not_reject() {
    Connection c;
    CHECK(c.setup(false));
    const auto mark = c.wire.events.size();
    c.wire.handshake = {MBEDTLS_ERR_SSL_NO_CLIENT_CERTIFICATE};
    CHECK(!c.link.handshake() && c.wire.alerts == 0 && c.wire.shutdowns.empty() && !c.wire.set_flags);
    CHECK(events_since(c.wire, mark) == std::vector<std::string>{"handshake"});
}

// A socket that cannot be made nonblocking gets no rejection, which could block.
void unsettable_socket() {
    {
        Connection c;
        CHECK(c.setup());
        const auto mark = c.wire.events.size();
        c.wire.handshake = {MBEDTLS_ERR_SSL_NO_CLIENT_CERTIFICATE};
        c.wire.fail_get_flags = true;
        CHECK(!c.link.handshake() && c.wire.alerts == 0 && c.wire.shutdowns.empty() && clock_ms == 0);
        CHECK(events_since(c.wire, mark) == (std::vector<std::string>{"handshake", "get_flags"}));
    }
    {
        Connection c;
        CHECK(c.setup());
        const auto mark = c.wire.events.size();
        c.wire.handshake = {MBEDTLS_ERR_SSL_NO_CLIENT_CERTIFICATE};
        c.wire.fail_set_flags = true;
        CHECK(!c.link.handshake() && c.wire.alerts == 0 && c.wire.shutdowns.empty() && clock_ms == 0);
        CHECK(events_since(c.wire, mark) == (std::vector<std::string>{"handshake", "get_flags", "set_flags"}));
    }
}

// A valid handshake authenticates the roster peer once and is not repeated.
void valid_handshake_once() {
    Connection c;
    CHECK(c.setup());
    CHECK(c.link.handshake() && c.wire.alerts == 0 && c.wire.shutdowns.empty());
    CHECK(c.link.handshake() && c.wire.handshake_calls == 1);
    CHECK(c.link.established() && c.link.authenticated == first().id);
    CHECK(timing("tls_handshake", "handshake", {"result=ok", "count=1", "code=0", "peer=" + first().id.str()}));
    CHECK(timing("tls_handshake", "verify_callback", {"result=ok", "count=1", "metric=sum"}));
}

// A handshake that authenticated no roster peer fails: one that presented
// another Amity's certificate, and one that verified none.
void unauthenticated_handshake() {
    {
        Connection c;
        CHECK(c.setup());
        c.wire.presented = {stranger()};
        CHECK(!c.link.handshake() && !c.link.usable() && !c.link.authenticated);
    }
    {
        Connection c;
        CHECK(c.setup());
        c.wire.presented.clear();
        CHECK(!c.link.handshake() && !c.link.usable() && !c.link.authenticated && c.wire.verify_callbacks == 0);
    }
}

// Verification flags the callback does not excuse fail the handshake.
void bad_verify_flags() {
    Connection c;
    CHECK(c.setup());
    c.wire.leaf_flags = MBEDTLS_X509_BADCERT_NOT_TRUSTED;
    CHECK(!c.link.handshake() && !c.link.usable());
    CHECK(c.link.authenticated == first().id && c.link.ssl.amity_verify_result == MBEDTLS_X509_BADCERT_NOT_TRUSTED);
}

// The verify callback accepts only the exact roster leaf, alone and at depth
// 0, and on a client only the peer it expects. It excuses dates only for that leaf.
void verify_callback() {
    {
        Connection c;
        CHECK(c.setup());
        c.wire.leaf_flags = MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE;
        CHECK(c.link.handshake() && c.link.authenticated == first().id && c.link.ssl.amity_verify_result == 0);
        CHECK(c.link.timing_verify_calls == 1);
    }
    {
        Connection c;
        CHECK(c.setup());
        c.wire.presented = {stranger()};
        c.wire.leaf_flags = MBEDTLS_X509_BADCERT_EXPIRED;
        CHECK(!c.link.handshake() &&
              c.link.ssl.amity_verify_result == (MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_OTHER));
    }
    {
        // A client expecting the first peer rejects the second, a roster peer too.
        Connection c;
        CHECK(c.link.setup(local(), {first(), roster(2)}, false, c.fd, first().id));
        c.wire.presented = {roster(2).der};
        CHECK(!c.link.handshake() && c.link.authenticated == first().id &&
              c.link.ssl.amity_verify_result == MBEDTLS_X509_BADCERT_OTHER);
    }
    {
        Connection c;
        CHECK(c.link.setup(local(), {first(), roster(2)}, true, c.fd));
        c.wire.presented = {roster(2).der};
        CHECK(c.link.handshake() && c.link.authenticated == roster(2).id);
    }
    {
        // The roster leaf sent with another certificate.
        Connection c;
        CHECK(c.setup());
        c.wire.presented = {first().der, stranger()};
        CHECK(!c.link.handshake() && !c.link.authenticated &&
              c.link.ssl.amity_verify_result == MBEDTLS_X509_BADCERT_OTHER);
    }
    {
        // A roster certificate verified above another leaf authenticates nothing.
        Connection c;
        CHECK(c.setup());
        c.wire.presented = {stranger(), first().der};
        c.wire.verify_calls = {{1, 1, 0}, {0, 0, 0}};
        CHECK(!c.link.handshake() && !c.link.authenticated && c.wire.verify_callbacks == 2);
        CHECK(c.link.timing_verify_calls == 2);
    }
}

// Setup configures TLS 1.2 with one suite, a required client certificate,
// the roster's exact certificates as trust anchors and the local identity,
// without session tickets; it sets the socket options and the nonblocking
// callbacks, and a configured link refuses another setup.
void setup_configuration() {
    {
        Connection c;
        const auto bindings = tls::bindings();
        CHECK(c.link.setup(local(), {first(), roster(2)}, true, c.fd));
        CHECK(tls::bindings() == bindings + 2);
        const auto &config = c.link.config;
        CHECK(c.link.socket == c.fd && c.link.server && c.link.usable() && !c.link.established());
        CHECK(config.amity_endpoint == MBEDTLS_SSL_IS_SERVER && config.amity_preset == MBEDTLS_SSL_PRESET_DEFAULT &&
              config.amity_transport == MBEDTLS_SSL_TRANSPORT_STREAM);
        CHECK(config.amity_min_tls_version == MBEDTLS_SSL_VERSION_TLS1_2 &&
              config.amity_max_tls_version == MBEDTLS_SSL_VERSION_TLS1_2);
        CHECK(suites(config) == std::vector<int>{MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256});
        CHECK(config.amity_authmode == MBEDTLS_SSL_VERIFY_REQUIRED);
        CHECK(config.amity_ca_chain == &c.link.anchors && !config.amity_ca_crl);
        CHECK(chain(c.link.anchors) == (std::vector<std::string>{first().der, roster(2).der}));
        CHECK(config.amity_verify == &TlsLink::verify && config.amity_verify_context == &c.link);
        CHECK(config.amity_own_cert == &c.link.own && chain(c.link.own) == std::vector<std::string>{local().der});
        CHECK(config.amity_own_key == &c.link.key && c.link.key.amity_parsed);
        CHECK(config.amity_session_tickets == MBEDTLS_SSL_SESSION_TICKETS_DISABLED);
        CHECK(c.link.ssl.amity_config == &config && !c.link.ssl.amity_hostname_set);
        CHECK(c.link.ssl.amity_bio == &c.link.socket && c.link.ssl.amity_send && c.link.ssl.amity_recv &&
              !c.link.ssl.amity_recv_timeout);
        std::vector<Setting> expected{{IPPROTO_TCP, TCP_NODELAY, 1},  {SOL_SOCKET, SO_KEEPALIVE, 1},
                                      {IPPROTO_TCP, TCP_KEEPIDLE, 10}, {IPPROTO_TCP, TCP_KEEPINTVL, 3},
                                      {IPPROTO_TCP, TCP_KEEPCNT, 3},   {SOL_SOCKET, SO_RCVTIMEO, 100},
                                      {SOL_SOCKET, SO_SNDTIMEO, 100}};
        std::sort(expected.begin(), expected.end());
        CHECK(settings(c.wire) == expected);
        CHECK(c.link.allowed.size() == 2 && c.link.allowed[1].id == roster(2).id && !c.link.authenticated);
        CHECK(timing("tls_setup", "total", {"result=ok"}));
        CHECK(timing("tls_setup", "peer_validation", {"result=ok", "count=2", "metric=sum"}));
        CHECK(timing("tls_setup", "anchor_parse", {"result=ok", "count=2", "metric=sum"}));
        // Another setup changes nothing, not even the socket.
        const auto events = c.wire.events.size();
        CHECK(!c.link.setup(local(), {first()}, true, c.fd + 1000));
        CHECK(c.link.socket == c.fd && c.link.usable() && c.wire.events.size() == events);
    }
    {
        // A client names the server it expects.
        Connection c;
        CHECK(c.setup(false));
        CHECK(c.link.config.amity_endpoint == MBEDTLS_SSL_IS_CLIENT && !c.link.server &&
              c.link.authenticated == first().id);
        CHECK(c.link.ssl.amity_hostname_set &&
              std::string(c.link.ssl.amity_hostname) == "o-" + first().id.str() + ".amity.invalid");
        CHECK(c.link.handshake() && c.link.authenticated == first().id);
    }
    {
        // The roster may be full.
        Connection c;
        c.wire.presented = {roster(3).der};
        CHECK(c.link.setup(local(), {first(), roster(2), roster(3)}, true, c.fd) && c.link.handshake());
    }
}

// Setup refuses an empty or oversized roster, credentials that do not parse
// or bind, a client without an expected peer, and any failure to configure
// Mbed TLS or the socket. A refused link cannot handshake.
void setup_refusals() {
    const auto refused = [](Connection &c, bool result) {
        CHECK(!result && !c.link.usable() && !c.link.handshake() && c.wire.handshake_calls == 0);
    };
    {
        // The link owns its socket from the start, even when refusing.
        Connection c;
        refused(c, c.link.setup(local(), {}, true, c.fd));
        CHECK(c.link.socket == c.fd);
        CHECK(!c.link.setup(local(), {first()}, true, c.fd + 1000) && c.link.socket == c.fd && !c.link.usable());
    }
    {
        Connection c;
        refused(c, c.link.setup(local(), {first(), roster(2), roster(3), roster(4)}, true, c.fd));
    }
    {
        Connection c;
        Identity broken = local();
        broken.der = tls::unparsable();
        refused(c, c.link.setup(broken, {first()}, true, c.fd));
        CHECK(timing("tls_setup", "own_credentials", {"result=failed"}));
    }
    {
        Connection c;
        Identity broken = local();
        broken.key_der = "not a key";
        refused(c, c.link.setup(broken, {first()}, true, c.fd));
    }
    {
        Connection c;
        Peer broken = first();
        broken.der = tls::unparsable();
        const auto bindings = tls::bindings();
        refused(c, c.link.setup(local(), {broken}, true, c.fd));
        CHECK(tls::bindings() == bindings);
    }
    {
        // A roster certificate above 1 KiB, even one that would bind.
        Connection c;
        const Peer large{octopus(5), tls::issue(octopus(5), "pin 5", 1025), "pin 5"};
        const auto bindings = tls::bindings();
        refused(c, c.link.setup(local(), {large}, true, c.fd));
        CHECK(tls::bindings() == bindings);
    }
    {
        Connection c;
        const Peer edge{octopus(6), tls::issue(octopus(6), "pin 6", 1024), "pin 6"};
        c.wire.presented = {edge.der};
        CHECK(c.link.setup(local(), {edge}, true, c.fd) && c.link.handshake() && c.link.authenticated == edge.id);
    }
    {
        // A certificate that binds to another Amity, or to another pin.
        Connection c;
        Peer other = first();
        other.id = octopus(8);
        refused(c, c.link.setup(local(), {other}, true, c.fd));
        CHECK(timing("tls_setup", "peer_validation", {"result=failed"}));
    }
    {
        Connection c;
        Peer other = first();
        other.pin = "another pin";
        refused(c, c.link.setup(local(), {other}, true, c.fd));
    }
    {
        // An empty pin is no substitute for a binding.
        Connection c;
        Peer other = first();
        other.id = octopus(8);
        other.pin.clear();
        refused(c, c.link.setup(local(), {other}, true, c.fd));
    }
    {
        Connection c;
        Peer other = roster(2);
        other.pin = "another pin";
        refused(c, c.link.setup(local(), {first(), other}, true, c.fd));
    }
    for (const char *function : {"mbedtls_ssl_config_defaults", "mbedtls_ssl_conf_own_cert", "mbedtls_ssl_setup"}) {
        Connection c;
        tls::fail_next(function);
        refused(c, c.setup());
        CHECK(tls::pending_failures() == 0);
    }
    {
        Connection c;
        tls::fail_next("mbedtls_ssl_set_hostname");
        refused(c, c.setup(false));
        CHECK(tls::pending_failures() == 0);
    }
    {
        Connection c;
        refused(c, c.link.setup(local(), {first()}, false, c.fd, {}));
    }
    for (const auto &option : {std::pair<int, int>{IPPROTO_TCP, TCP_NODELAY}, {SOL_SOCKET, SO_KEEPALIVE},
                               {IPPROTO_TCP, TCP_KEEPIDLE}, {IPPROTO_TCP, TCP_KEEPINTVL}, {IPPROTO_TCP, TCP_KEEPCNT}}) {
        Connection c;
        c.wire.failing_option = option;
        refused(c, c.setup());
    }
    // The receive and send timeouts are best effort.
    for (const auto &option : {std::pair<int, int>{SOL_SOCKET, SO_RCVTIMEO}, {SOL_SOCKET, SO_SNDTIMEO}}) {
        Connection c;
        c.wire.failing_option = option;
        CHECK(c.setup() && c.link.usable() && c.link.handshake());
    }
}

// A waiting handshake polls the socket in the direction Mbed TLS asks for, at
// most 100 ms at a time, and fails at its 10 s deadline without a rejection.
void handshake_deadline() {
    {
        clock_ms = 0;
        Connection c;
        CHECK(c.setup());
        c.wire.handshake = {MBEDTLS_ERR_SSL_WANT_WRITE, MBEDTLS_ERR_SSL_WANT_READ, 0};
        c.wire.writable_at = 150;
        c.wire.readable_at = 420;
        CHECK(c.link.handshake() && clock_ms == 420 && c.link.authenticated == first().id);
        const auto &waits = c.wire.selects;
        CHECK(waits.size() == 5 && waits[0].write && waits[1].write && waits[1].ready && waits[1].woke == 150);
        for (size_t i = 2; i < waits.size(); ++i) CHECK(!waits[i].write);
        CHECK(waits.back().ready && waits.back().woke == 420);
        for (const auto &wait : waits) CHECK(wait.timeout_ms <= 100);
    }
    {
        clock_ms = 0;
        Connection c;
        CHECK(c.setup());
        const auto mark = c.wire.events.size();
        c.wire.handshake = {MBEDTLS_ERR_SSL_WANT_READ};
        CHECK(!c.link.handshake() && clock_ms == 10000 && !c.link.usable());
        CHECK(c.wire.handshake_calls == 100 && c.wire.selects.size() == 100);
        for (const auto &event : events_since(c.wire, mark)) CHECK(event == "handshake" || event == "select read");
        const auto timeout = "code=" + std::to_string(MBEDTLS_ERR_SSL_TIMEOUT);
        CHECK(timing("tls_handshake", "handshake", {"result=failed", "count=100", "wait_ms=10000", timeout}));
    }
}

// Each pool admits one pending handshake. Authentication frees its slot, a
// failed handshake keeps it, and a destroyed link releases it.
void admission_slots() {
    ConnectionAdmission pool;
    std::vector<int> fds;
    for (unsigned i = 0; i < 5; ++i) {
        fds.push_back(next_socket++);
        tls::wire(fds.back()).presented = {first().der};
    }
    {
        TlsLink authenticated(pool), waiting(pool);
        CHECK(authenticated.setup(local(), {first()}, true, fds[0]));
        CHECK(!waiting.setup(local(), {first()}, true, fds[1]) && !waiting.usable());
        CHECK(authenticated.handshake());
        TlsLink failing(pool);
        CHECK(failing.setup(local(), {first()}, true, fds[2]));
        tls::wire(fds[2]).handshake = {MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE};
        CHECK(!failing.handshake());
        TlsLink refused(pool);
        CHECK(!refused.setup(local(), {first()}, true, fds[3]));
    }
    TlsLink later(pool);
    CHECK(later.setup(local(), {first()}, true, fds[4]) && later.handshake());
}

// A link shuts its socket down both ways and closes it once, frees every
// Mbed TLS context, and leaves a socket it never owned alone.
void teardown() {
    int owned = 0, refused = 0, unused = 0;
    {
        Connection c;
        owned = c.fd;
        c.establish();
    }
    const auto &wire = tls::wire(owned);
    CHECK(wire.shutdowns == std::vector<int>{SHUT_RDWR} && wire.closes == 1);
    CHECK(events_since(wire, wire.events.size() - 2) == (std::vector<std::string>{"shutdown both", "close"}));
    {
        Connection c;
        refused = c.fd;
        CHECK(!c.link.setup(local(), {}, true, c.fd));
    }
    CHECK(tls::wire(refused).closes == 1);
    {
        Connection c;
        unused = c.fd;
    }
    CHECK(tls::wire(unused).events.empty());
    CHECK(tls::live_contexts() == 0 && tls::live_certificates() == 0);
}

}  // namespace

int main(int argc, char **argv) {
    return amity_test::run(argc, argv,
                           {{"lazy-handshake-and-transport-health", lazy_handshake_and_transport_health},
                            {"ipc-during-partial-frame", ipc_during_partial_frame},
                            {"fragments-and-frame-boundaries", fragments_and_frame_boundaries},
                            {"terminal-deadlines-and-errors", terminal_deadlines_and_errors},
                            {"blocking-wrappers", blocking_wrappers},
                            {"socket-callbacks", socket_callbacks},
                            {"bounded-application-writes", bounded_application_writes},
                            {"bulk-transfers", bulk_transfers},
                            {"terminal-receive-diagnostics", terminal_receive_diagnostics},
                            {"expired-frames-drain", expired_frames_drain},
                            {"missing-client-certificate", missing_client_certificate},
                            {"queued-alert-flushed", queued_alert_flushed},
                            {"blocked-alert", blocked_alert},
                            {"rejection-drain-ends", rejection_drain_ends},
                            {"client-does-not-reject", client_does_not_reject},
                            {"unsettable-socket", unsettable_socket},
                            {"valid-handshake-once", valid_handshake_once},
                            {"unauthenticated-handshake", unauthenticated_handshake},
                            {"bad-verify-flags", bad_verify_flags},
                            {"verify-callback", verify_callback},
                            {"setup-configuration", setup_configuration},
                            {"setup-refusals", setup_refusals},
                            {"handshake-deadline", handshake_deadline},
                            {"admission-slots", admission_slots},
                            {"teardown", teardown}});
}
