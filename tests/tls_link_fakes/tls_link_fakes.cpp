/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// The TLS link fakes: scripted Mbed TLS sessions and lwIP sockets over the
// wires a scenario scripts, the link clock, and the two credential functions
// tls_link.cpp calls.
#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "platform.hpp"
#include "scenario_main.hpp"
#include "tls_link_fakes.hpp"
#include "mbedtls/net_sockets.h"
#include "mbedtls/pk.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
// Last: its macros rename the socket calls.
#include "lwip/sockets.h"

uint64_t amity_fake::tls::clock_ms = 0;

namespace {

using amity_fake::tls::Wire;
using amity_fake::tls::clock_ms;
using amity_fake::tls::never;

[[noreturn]] void fail(const std::string &message) { amity_test::fail(__FILE__, __LINE__, message.c_str()); }

unsigned clock_reads = 0, jump_reads = 0;
uint64_t jump_to = 0;

struct Issued {
    amity::system::OctopusId id;
    std::string pin;
};
std::map<std::string, Issued> issued_certificates;
std::set<std::string> issued_keys;
unsigned issues = 0, binding_calls = 0;
std::multiset<std::string> failures;
std::set<const void *> contexts;
std::size_t certificate_allocations = 0;

std::map<int, Wire> &wires() {
    static std::map<int, Wire> all;
    return all;
}

// Calls made since the clock last moved: a link that keeps calling without
// time passing would spin forever on the target.
uint64_t guarded_clock = never;
unsigned long idle_calls = 0;
void guard(const char *call) {
    if (clock_ms != guarded_clock) {
        guarded_clock = clock_ms;
        idle_calls = 0;
    } else if (++idle_calls > 1000000) {
        fail(std::string("the link keeps calling ") + call + " without time passing");
    }
}

Wire &socket_wire(int fd, const char *call) {
    guard(call);
    const auto found = wires().find(fd);
    if (found == wires().end())
        fail(std::string(call) + " on socket " + std::to_string(fd) + ", which no scenario scripted");
    if (found->second.closes) fail(std::string(call) + " on a closed socket");
    return found->second;
}

void require(const void *context, const char *call) {
    if (!contexts.count(context)) fail(std::string(call) + " on a context that is not initialised");
}

Wire &session_wire(const mbedtls_ssl_context *ssl, const char *call) {
    require(ssl, call);
    if (!ssl->amity_config) fail(std::string(call) + " before mbedtls_ssl_setup");
    if (!ssl->amity_bio_set) fail(std::string(call) + " before mbedtls_ssl_set_bio");
    return socket_wire(*static_cast<const int *>(ssl->amity_bio), call);
}

bool take_failure(const char *function) {
    const auto found = failures.find(function);
    if (found == failures.end()) return false;
    failures.erase(found);
    return true;
}

uint64_t first_unread(const std::vector<amity_fake::tls::Byte> &bytes, size_t cursor) {
    return cursor < bytes.size() ? bytes[cursor].at : never;
}

// When the socket turns readable: decrypted bytes or raw bytes arrive, the
// handshake's input arrives, or a write that wanted to read can go on.
uint64_t readable(const Wire &wire) {
    auto at = std::min({first_unread(wire.incoming, wire.cursor), first_unread(wire.raw, wire.raw_cursor),
                        wire.readable_at});
    if (wire.write_wants_read) at = std::min(at, wire.writable_at);
    return at;
}

bool waiting(int step) { return step == MBEDTLS_ERR_SSL_WANT_READ || step == MBEDTLS_ERR_SSL_WANT_WRITE; }

// The verify calls of a completing handshake, over nodes that link the
// presented chain as Mbed TLS links a peer's certificates.
int verify_peer(mbedtls_ssl_context *ssl, Wire &wire) {
    auto calls = wire.verify_calls;
    if (calls.empty() && !wire.presented.empty()) calls.push_back({0, 0, wire.leaf_flags});
    auto certificates = wire.presented;
    std::vector<mbedtls_x509_crt> nodes(certificates.size());
    for (size_t i = 0; i < nodes.size(); ++i) {
        nodes[i].raw = {0x30, certificates[i].size(), reinterpret_cast<unsigned char *>(certificates[i].data())};
        nodes[i].next = i + 1 < nodes.size() ? &nodes[i + 1] : nullptr;
    }
    const auto *config = ssl->amity_config;
    uint32_t result = 0;
    for (const auto &call : calls) {
        if (call.node >= nodes.size()) fail("a verify call names a certificate the peer did not present");
        uint32_t flags = call.flags;
        if (config->amity_verify) {
            ++wire.verify_callbacks;
            const int error = config->amity_verify(config->amity_verify_context, &nodes[call.node], call.depth, &flags);
            if (error) return error;
        }
        result |= flags;
    }
    ssl->amity_verify_result = result;
    ssl->amity_handshaken = 1;
    return 0;
}

}  // namespace

// Scenario controls.

void amity_fake::tls::jump_clock(unsigned reads, uint64_t to) {
    clock_reads = 0;
    jump_reads = reads;
    jump_to = to;
}

Wire &amity_fake::tls::wire(int fd) { return wires()[fd]; }

std::string amity_fake::tls::issue(amity::system::OctopusId id, const std::string &pin, size_t size) {
    std::string der = "certificate " + std::to_string(++issues) + " for " + id.str();
    if (der.size() > size) fail("an issued certificate cannot be that small");
    der.resize(size, '.');
    issued_certificates[der] = {id, pin};
    return der;
}

std::string amity_fake::tls::unparsable() { return "unparsable " + std::to_string(++issues); }

std::string amity_fake::tls::private_key() {
    std::string key = "private key " + std::to_string(++issues);
    issued_keys.insert(key);
    return key;
}

unsigned amity_fake::tls::bindings() { return binding_calls; }

void amity_fake::tls::fail_next(const char *function) { failures.insert(function); }

std::size_t amity_fake::tls::pending_failures() { return failures.size(); }

std::size_t amity_fake::tls::live_contexts() { return contexts.size(); }

std::size_t amity_fake::tls::live_certificates() { return certificate_allocations; }

// The credential functions tls_link.cpp calls, defined in credentials.cpp.

uint64_t amity::consortium::platform::now_ms() {
    if (jump_reads && ++clock_reads == jump_reads) {
        clock_ms = jump_to;
        jump_reads = 0;
    }
    return clock_ms;
}

bool amity::consortium::platform::certificate_binding(const mbedtls_x509_crt &crt, model::OctopusId id,
                                                      std::string &pin) {
    ++binding_calls;
    if (!crt.raw.p) fail("certificate_binding got a certificate without DER bytes");
    const auto found = issued_certificates.find(std::string(reinterpret_cast<const char *>(crt.raw.p), crt.raw.len));
    if (found == issued_certificates.end() || found->second.id != id) return false;
    pin = found->second.pin;
    return true;
}

// Certificates and keys.

void mbedtls_x509_crt_init(mbedtls_x509_crt *crt) {
    *crt = mbedtls_x509_crt{};
    contexts.insert(crt);
}

void mbedtls_x509_crt_free(mbedtls_x509_crt *crt) {
    require(crt, "mbedtls_x509_crt_free");
    for (auto *node = crt; node;) {
        auto *next = node->next;
        if (node->raw.p) {
            delete[] node->raw.p;
            --certificate_allocations;
        }
        if (node != crt) {
            delete node;
            --certificate_allocations;
        }
        node = next;
    }
    *crt = mbedtls_x509_crt{};
    contexts.erase(crt);
}

int mbedtls_x509_crt_parse_der(mbedtls_x509_crt *chain, const unsigned char *buf, size_t buflen) {
    require(chain, "mbedtls_x509_crt_parse_der");
    if (!issued_certificates.count(std::string(reinterpret_cast<const char *>(buf), buflen)))
        return MBEDTLS_ERR_X509_INVALID_FORMAT;
    auto *node = chain;
    if (node->raw.p) {
        while (node->next) node = node->next;
        node->next = new mbedtls_x509_crt{};
        ++certificate_allocations;
        node = node->next;
    }
    node->raw = {0x30, buflen, new unsigned char[buflen]};
    ++certificate_allocations;
    std::memcpy(node->raw.p, buf, buflen);
    return 0;
}

void mbedtls_pk_init(mbedtls_pk_context *ctx) {
    *ctx = mbedtls_pk_context{};
    contexts.insert(ctx);
}

void mbedtls_pk_free(mbedtls_pk_context *ctx) {
    require(ctx, "mbedtls_pk_free");
    *ctx = mbedtls_pk_context{};
    contexts.erase(ctx);
}

int mbedtls_pk_parse_key(mbedtls_pk_context *ctx, const unsigned char *key, size_t keylen, const unsigned char *pwd,
                         size_t pwdlen) {
    require(ctx, "mbedtls_pk_parse_key");
    if (pwd || pwdlen) fail("Amity's private keys have no password");
    if (!issued_keys.count(std::string(reinterpret_cast<const char *>(key), keylen)))
        return MBEDTLS_ERR_PK_KEY_INVALID_FORMAT;
    ctx->amity_parsed = 1;
    return 0;
}

// Configurations.

void mbedtls_ssl_config_init(mbedtls_ssl_config *conf) {
    *conf = mbedtls_ssl_config{};
    conf->amity_initialized = 1;
    conf->amity_endpoint = conf->amity_transport = conf->amity_preset = -1;
    conf->amity_authmode = conf->amity_session_tickets = -1;
    contexts.insert(conf);
}

void mbedtls_ssl_config_free(mbedtls_ssl_config *conf) {
    require(conf, "mbedtls_ssl_config_free");
    *conf = mbedtls_ssl_config{};
    contexts.erase(conf);
}

int mbedtls_ssl_config_defaults(mbedtls_ssl_config *conf, int endpoint, int transport, int preset) {
    require(conf, "mbedtls_ssl_config_defaults");
    if (take_failure("mbedtls_ssl_config_defaults")) return MBEDTLS_ERR_SSL_BAD_INPUT_DATA;
    conf->amity_endpoint = endpoint;
    conf->amity_transport = transport;
    conf->amity_preset = preset;
    return 0;
}

void mbedtls_ssl_conf_min_tls_version(mbedtls_ssl_config *conf, mbedtls_ssl_protocol_version tls_version) {
    require(conf, "mbedtls_ssl_conf_min_tls_version");
    conf->amity_min_tls_version = tls_version;
}

void mbedtls_ssl_conf_max_tls_version(mbedtls_ssl_config *conf, mbedtls_ssl_protocol_version tls_version) {
    require(conf, "mbedtls_ssl_conf_max_tls_version");
    conf->amity_max_tls_version = tls_version;
}

void mbedtls_ssl_conf_ciphersuites(mbedtls_ssl_config *conf, const int *ciphersuites) {
    require(conf, "mbedtls_ssl_conf_ciphersuites");
    conf->amity_ciphersuites = ciphersuites;
}

void mbedtls_ssl_conf_authmode(mbedtls_ssl_config *conf, int authmode) {
    require(conf, "mbedtls_ssl_conf_authmode");
    conf->amity_authmode = authmode;
}

void mbedtls_ssl_conf_ca_chain(mbedtls_ssl_config *conf, mbedtls_x509_crt *ca_chain, mbedtls_x509_crl *ca_crl) {
    require(conf, "mbedtls_ssl_conf_ca_chain");
    conf->amity_ca_chain = ca_chain;
    conf->amity_ca_crl = ca_crl;
}

void mbedtls_ssl_conf_verify(mbedtls_ssl_config *conf, int (*f_vrfy)(void *, mbedtls_x509_crt *, int, uint32_t *),
                             void *p_vrfy) {
    require(conf, "mbedtls_ssl_conf_verify");
    conf->amity_verify = f_vrfy;
    conf->amity_verify_context = p_vrfy;
}

void mbedtls_ssl_conf_session_tickets(mbedtls_ssl_config *conf, int use_tickets) {
    require(conf, "mbedtls_ssl_conf_session_tickets");
    conf->amity_session_tickets = use_tickets;
}

int mbedtls_ssl_conf_own_cert(mbedtls_ssl_config *conf, mbedtls_x509_crt *own_cert, mbedtls_pk_context *pk_key) {
    require(conf, "mbedtls_ssl_conf_own_cert");
    if (take_failure("mbedtls_ssl_conf_own_cert")) return MBEDTLS_ERR_SSL_BAD_INPUT_DATA;
    conf->amity_own_cert = own_cert;
    conf->amity_own_key = pk_key;
    return 0;
}

// Sessions.

void mbedtls_ssl_init(mbedtls_ssl_context *ssl) {
    *ssl = mbedtls_ssl_context{};
    ssl->amity_initialized = 1;
    ssl->amity_verify_result = UINT32_MAX;
    contexts.insert(ssl);
}

void mbedtls_ssl_free(mbedtls_ssl_context *ssl) {
    require(ssl, "mbedtls_ssl_free");
    *ssl = mbedtls_ssl_context{};
    contexts.erase(ssl);
}

int mbedtls_ssl_setup(mbedtls_ssl_context *ssl, const mbedtls_ssl_config *conf) {
    require(ssl, "mbedtls_ssl_setup");
    require(conf, "mbedtls_ssl_setup");
    if (take_failure("mbedtls_ssl_setup")) return MBEDTLS_ERR_SSL_BAD_INPUT_DATA;
    ssl->amity_config = conf;
    return 0;
}

int mbedtls_ssl_set_hostname(mbedtls_ssl_context *ssl, const char *hostname) {
    require(ssl, "mbedtls_ssl_set_hostname");
    if (take_failure("mbedtls_ssl_set_hostname")) return MBEDTLS_ERR_SSL_BAD_INPUT_DATA;
    if (std::strlen(hostname) >= sizeof(ssl->amity_hostname)) fail("the fake holds shorter host names");
    std::strcpy(ssl->amity_hostname, hostname);
    ssl->amity_hostname_set = 1;
    return 0;
}

void mbedtls_ssl_set_bio(mbedtls_ssl_context *ssl, void *p_bio, mbedtls_ssl_send_t *f_send, mbedtls_ssl_recv_t *f_recv,
                         mbedtls_ssl_recv_timeout_t *f_recv_timeout) {
    require(ssl, "mbedtls_ssl_set_bio");
    ssl->amity_bio = p_bio;
    ssl->amity_send = f_send;
    ssl->amity_recv = f_recv;
    ssl->amity_recv_timeout = f_recv_timeout;
    ssl->amity_bio_set = 1;
}

int mbedtls_ssl_handshake(mbedtls_ssl_context *ssl) {
    auto &wire = session_wire(ssl, "mbedtls_ssl_handshake");
    ++wire.handshake_calls;
    wire.events.emplace_back("handshake");
    auto &steps = wire.handshake;
    if (steps.empty()) fail("a wire's handshake has no steps");
    const auto ready = [&](int step) {
        return step == MBEDTLS_ERR_SSL_WANT_READ ? clock_ms >= wire.readable_at : clock_ms >= wire.writable_at;
    };
    while (steps.size() > 1 && waiting(steps.front()) && ready(steps.front())) steps.erase(steps.begin());
    const int step = steps.front();
    if (waiting(step)) return step;
    if (steps.size() > 1) steps.erase(steps.begin());
    return step ? step : verify_peer(ssl, wire);
}

uint32_t mbedtls_ssl_get_verify_result(const mbedtls_ssl_context *ssl) {
    require(ssl, "mbedtls_ssl_get_verify_result");
    return ssl->amity_verify_result;
}

int mbedtls_ssl_read(mbedtls_ssl_context *ssl, unsigned char *buf, size_t len) {
    auto &wire = session_wire(ssl, "mbedtls_ssl_read");
    if (!ssl->amity_handshaken) fail("mbedtls_ssl_read before the handshake completed");
    wire.events.emplace_back("read");
    ++wire.reads;
    clock_ms += wire.read_elapsed;
    if (wire.read_error) return *wire.read_error;
    size_t size = 0;
    while (wire.cursor < wire.incoming.size() && wire.incoming[wire.cursor].at <= clock_ms && size < len)
        buf[size++] = wire.incoming[wire.cursor++].value;
    return size ? static_cast<int>(size) : MBEDTLS_ERR_SSL_WANT_READ;
}

int mbedtls_ssl_write(mbedtls_ssl_context *ssl, const unsigned char *buf, size_t len) {
    auto &wire = session_wire(ssl, "mbedtls_ssl_write");
    if (!ssl->amity_handshaken) fail("mbedtls_ssl_write before the handshake completed");
    wire.events.emplace_back("write");
    ++wire.write_calls;
    wire.maximum_write = std::max(wire.maximum_write, len);
    clock_ms += wire.write_elapsed;
    // Mbed TLS requires a write it told to wait to be retried with the same buffer.
    if (wire.blocked_buffer && (wire.blocked_buffer != buf || wire.blocked_size != len))
        fail("a write retried after WANT_READ or WANT_WRITE changed its buffer");
    if (clock_ms < wire.writable_at) {
        wire.blocked_buffer = buf;
        wire.blocked_size = len;
        return wire.write_wants_read ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_SSL_WANT_WRITE;
    }
    if (wire.write_error && wire.write_calls > wire.error_after) return *wire.write_error;
    wire.blocked_buffer = nullptr;
    const auto count = std::min(len, wire.partial_write);
    wire.written.insert(wire.written.end(), buf, buf + count);
    return static_cast<int>(count);
}

int mbedtls_ssl_check_pending(const mbedtls_ssl_context *ssl) {
    const auto &wire = session_wire(ssl, "mbedtls_ssl_check_pending");
    return first_unread(wire.incoming, wire.cursor) <= clock_ms;
}

int mbedtls_ssl_send_alert_message(mbedtls_ssl_context *ssl, unsigned char level, unsigned char message) {
    auto &wire = session_wire(ssl, "mbedtls_ssl_send_alert_message");
    if (level != MBEDTLS_SSL_ALERT_LEVEL_FATAL || message != MBEDTLS_SSL_ALERT_MSG_HANDSHAKE_FAILURE)
        fail("the link sent an alert other than a fatal handshake_failure");
    // As in the SDK, a pending record is flushed instead of creating another.
    if (ssl->MBEDTLS_PRIVATE(out_left)) {
        ++wire.flushes;
        wire.events.emplace_back("flush");
        if (clock_ms < wire.writable_at) return MBEDTLS_ERR_SSL_WANT_WRITE;
        ssl->MBEDTLS_PRIVATE(out_left) = 0;
        return 0;
    }
    ++wire.alerts;
    wire.events.emplace_back("alert");
    if (clock_ms < wire.writable_at) {
        ssl->MBEDTLS_PRIVATE(out_left) = 7;
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    }
    return 0;
}

// Sockets.

ssize_t lwip_send(int s, const void *dataptr, size_t size, int flags) {
    auto &wire = socket_wire(s, "send");
    wire.events.emplace_back("send");
    wire.send_flags.push_back(flags);
    if (!(flags & MSG_DONTWAIT)) fail("send without MSG_DONTWAIT can block for SO_SNDTIMEO");
    if (!dataptr) fail("send without data");
    long result = -EAGAIN;
    if (!wire.send_results.empty()) {
        result = wire.send_results.front();
        wire.send_results.pop_front();
    }
    if (result >= 0) return static_cast<ssize_t>(std::min(static_cast<size_t>(result), size));
    errno = static_cast<int>(-result);
    return -1;
}

ssize_t lwip_recv(int s, void *mem, size_t len, int flags) {
    auto &wire = socket_wire(s, "recv");
    wire.events.emplace_back("recv");
    wire.recv_flags.push_back(flags);
    wire.recv_sizes.push_back(len);
    if (!(flags & MSG_DONTWAIT)) fail("recv without MSG_DONTWAIT can block for SO_RCVTIMEO");
    size_t count = 0;
    auto *out = static_cast<uint8_t *>(mem);
    while (wire.raw_cursor < wire.raw.size() && wire.raw[wire.raw_cursor].at <= clock_ms && count < len)
        out[count++] = wire.raw[wire.raw_cursor++].value;
    if (count) return static_cast<ssize_t>(count);
    if (!wire.recv_error) return 0;
    errno = wire.recv_error;
    return -1;
}

int lwip_select(int maxfdp1, fd_set *readset, fd_set *writeset, fd_set *exceptset, struct timeval *timeout) {
    if (exceptset || !timeout || (readset == nullptr) == (writeset == nullptr))
        fail("a link's select waits in exactly one direction, with a timeout");
    const int fd = maxfdp1 - 1;
    auto *set = readset ? readset : writeset;
    if (fd < 0 || !FD_ISSET(fd, set)) fail("select's set does not hold its highest descriptor");
    for (int other = 0; other < fd; ++other)
        if (FD_ISSET(other, set)) fail("select waits for another socket");
    auto &wire = socket_wire(fd, "select");
    const bool write = writeset != nullptr;
    wire.events.emplace_back(write ? "select write" : "select read");
    if (timeout->tv_sec < 0 || timeout->tv_usec < 0) fail("select with a negative timeout");
    const uint64_t timeout_ms = uint64_t(timeout->tv_sec) * 1000 + uint64_t(timeout->tv_usec) / 1000;
    const uint64_t ready_at = write ? wire.writable_at : readable(wire);
    const uint64_t began = clock_ms;
    const bool ready = ready_at <= began + timeout_ms;
    clock_ms = ready ? std::max(began, ready_at) : began + timeout_ms;
    wire.selects.push_back({write, began, timeout_ms, clock_ms, ready});
    if (!ready) FD_CLR(fd, set);
    return ready ? 1 : 0;
}

int lwip_shutdown(int s, int how) {
    auto &wire = socket_wire(s, "shutdown");
    wire.events.emplace_back(how == SHUT_RD ? "shutdown read" : how == SHUT_WR ? "shutdown write" : "shutdown both");
    wire.shutdowns.push_back(how);
    return 0;
}

int lwip_setsockopt(int s, int level, int optname, const void *optval, socklen_t optlen) {
    auto &wire = socket_wire(s, "setsockopt");
    wire.events.emplace_back("setsockopt");
    long value = 0;
    if (optlen == sizeof(int)) {
        value = *static_cast<const int *>(optval);
    } else if (optlen == sizeof(struct timeval)) {
        const auto *duration = static_cast<const struct timeval *>(optval);
        value = long(duration->tv_sec) * 1000 + long(duration->tv_usec) / 1000;
    } else {
        fail("setsockopt with an option the fake does not know");
    }
    wire.options.push_back({level, optname, value});
    if (wire.failing_option == std::pair<int, int>(level, optname)) {
        errno = ENOPROTOOPT;
        return -1;
    }
    return 0;
}

int lwip_close(int s) {
    auto &wire = socket_wire(s, "close");
    wire.events.emplace_back("close");
    ++wire.closes;
    return 0;
}

int lwip_fcntl(int s, int cmd, int val) {
    auto &wire = socket_wire(s, "fcntl");
    if (cmd == F_GETFL) {
        wire.events.emplace_back("get_flags");
        return wire.fail_get_flags ? -1 : wire.status_flags;
    }
    if (cmd != F_SETFL) fail("fcntl with a command the fake does not know");
    wire.events.emplace_back("set_flags");
    if (wire.fail_set_flags) return -1;
    wire.set_flags = wire.status_flags = val;
    return 0;
}
