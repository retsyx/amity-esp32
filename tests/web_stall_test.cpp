/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// The HTTPS deadline guard, components/web/stall.cpp, built whole. The core
// fakes supply its timer on the scripted clock and a task per thread; HTTPD's
// private request state is built here. The test stands in for the SDK
// functions the linker routes through the guard, and native socket pairs show
// when an expiry shuts a session's socket down.
#include "stall.hpp"
#include "core_fakes.hpp"
#include "esp_httpd_priv.h"
#include "esp_timer.h"
#include "scenario_main.hpp"
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <sys/socket.h>
#include <unistd.h>

extern "C" {
esp_err_t __wrap_httpd_req_new(httpd_data *server, sock_db *session);
esp_err_t __wrap_httpd_uri(httpd_data *server);
esp_err_t __wrap_httpd_req_delete(httpd_data *server);
esp_err_t __real_httpd_req_new(httpd_data *server, sock_db *session);
esp_err_t __real_httpd_uri(httpd_data *server);
esp_err_t __real_httpd_req_delete(httpd_data *server);
}

namespace {

using web::stall::handshake_us;
using web::stall::head_us;
using web::stall::transfer_us;

// Transfers of known size get time at the minimum rate on top of the base:
// the 68 KB UI script about 9.2 s, a 2.5 MB unread upload about 161 s.
static_assert(transfer_us(0) == 5000000 && transfer_us(16384) == 6000000);
static_assert(transfer_us(68696) == 9192871 && transfer_us(2554212) == 160896728);

struct Pair {
    int fds[2];
    Pair() { CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0); }
    ~Pair() {
        close(fds[0]);
        close(fds[1]);
    }
    // The server's blocked receive ends once its socket is shut down.
    bool ended() {
        char byte;
        return recv(fds[0], &byte, 1, MSG_DONTWAIT) == 0;
    }
};

// An HTTPD server with its current request and that request's session.
struct Server {
    httpd_data data{};
    sock_db session{};
    Server() {
        data.hd_req.handle = &data;
        data.hd_req.aux = &data.hd_req_aux;
        data.hd_req_aux.sd = &session;
        session.handle = &data;
        session.fd = -1;
    }
    httpd_req_t *request() { return &data.hd_req; }
};

Server secure_server, other_server;
httpd_data *guarded = &secure_server.data;  // the server the SDK stand-ins expect deadlines for
std::string events;
bool fail_head = false, expire_head = false, expire_purge = false;
int head_socket = -1;
std::function<void()> handler_work;

// The guard's one timer.
amity_fake::Timer timer() {
    const auto timers = amity_fake::timers();
    CHECK(timers.size() == 1);
    return timers.front();
}
bool armed() { return !amity_fake::timers().empty() && timer().armed; }
bool armed_for(uint64_t timeout_us) {
    const auto current = timer();
    return current.armed && current.deadline_us == amity_fake::now_us() + int64_t(timeout_us);
}
// The esp_timer task runs an armed timer's callback at expiry.
void expire_now() {
    if (armed()) amity_fake::advance_us(timer().deadline_us - amity_fake::now_us());
}

void prepare_and_serve() {
    CHECK(web::stall::prepare() == ESP_OK);
    web::stall::serve(&secure_server.data);
}

}  // namespace

// The SDK parses a head, routes it, then runs the handler.
esp_err_t __real_httpd_uri(httpd_data *server) {
    // Handlers run without the head deadline.
    CHECK(!armed());
    CHECK(server == &secure_server.data || server == &other_server.data);
    events += "handler,";
    if (handler_work) handler_work();
    return ESP_OK;
}
esp_err_t __real_httpd_req_new(httpd_data *server, sock_db *session) {
    if (server == guarded)
        CHECK(armed_for(head_us) && session->fd == head_socket);
    else
        CHECK(!armed());
    events += "head,";
    if (fail_head) return ESP_FAIL;
    if (expire_head) {
        expire_now();
        return ESP_FAIL;
    }
    return __wrap_httpd_uri(server);
}
esp_err_t __real_httpd_req_delete(httpd_data *server) {
    const bool purge = server == guarded && server->hd_req_aux.remaining_len;
    CHECK(armed() == purge);
    if (purge) CHECK(armed_for(transfer_us(server->hd_req_aux.remaining_len)));
    events += purge ? "purge," : "delete,";
    if (expire_purge) {
        expire_now();
        return ESP_FAIL;
    }
    return ESP_OK;
}

namespace {

// Phases before the timer exists are unguarded rather than failing; a
// failed timer creation is reported, and the timer is created once.
void before_prepare() {
    web::stall::arm(3, 1);
    web::stall::disarm();
    CHECK(amity_fake::timers().empty());
    amity_fake::fail_timer_creations(1);
    CHECK(web::stall::prepare() == ESP_ERR_NO_MEM);
    CHECK(amity_fake::timers().empty());
    CHECK(web::stall::prepare() == ESP_OK);
    CHECK(web::stall::prepare() == ESP_OK);
    CHECK(timer().name == "https_stall" && !timer().armed && timer().period_us == 0);
}

void request_head() {
    // A request head gets 5 s from its first byte.
    CHECK(head_us == 5000000);
    prepare_and_serve();
    // A complete head is routed before the handler, and nothing stays armed.
    Pair pair;
    head_socket = pair.fds[0];
    sock_db session{};
    session.fd = pair.fds[0];
    CHECK(__wrap_httpd_req_new(&secure_server.data, &session) == ESP_OK && events == "head,handler,");
    CHECK(!armed() && !pair.ended());
    // A head that fails before it is routed ends its deadline too.
    events.clear();
    fail_head = true;
    CHECK(__wrap_httpd_req_new(&secure_server.data, &session) == ESP_FAIL && events == "head,");
    CHECK(!armed() && !pair.ended());
    fail_head = false;
    // A head still incomplete at its deadline ends the blocked receive.
    events.clear();
    expire_head = true;
    CHECK(__wrap_httpd_req_new(&secure_server.data, &session) == ESP_FAIL && events == "head,");
    CHECK(pair.ended() && !armed());
    CHECK(timer().starts == 3);
}

void unread_body() {
    prepare_and_serve();
    // Only an unread body is purged under a deadline.
    Pair pair;
    secure_server.session.fd = pair.fds[0];
    secure_server.data.hd_req_aux.remaining_len = 0;
    CHECK(__wrap_httpd_req_delete(&secure_server.data) == ESP_OK);
    secure_server.data.hd_req_aux.remaining_len = 4096;
    CHECK(__wrap_httpd_req_delete(&secure_server.data) == ESP_OK);
    CHECK(events == "delete,purge," && !armed() && !pair.ended());
    // A purge still running at its deadline ends the blocked receive.
    expire_purge = true;
    CHECK(__wrap_httpd_req_delete(&secure_server.data) == ESP_FAIL);
    CHECK(pair.ended() && !armed());
}

void handler_response() {
    prepare_and_serve();
    // A handler's response gets its size allowance, on the server task only.
    Pair pair;
    head_socket = pair.fds[0];
    sock_db session{};
    session.fd = pair.fds[0];
    secure_server.session.fd = pair.fds[0];
    bool responded = false;
    handler_work = [&] {
        // Only the HTTPS server's own responses are bounded.
        {
            web::stall::Response other(other_server.request(), 68696);
            CHECK(!armed());
        }
        {
            web::stall::Response bound(secure_server.request(), 68696);
            CHECK(armed_for(transfer_us(68696)));
            // A response streamed from another task, as logs are, is not bounded.
            std::thread([] { web::stall::Response streamed(secure_server.request(), 1); }).join();
            CHECK(armed_for(transfer_us(68696)));
        }
        CHECK(!armed() && !pair.ended());
        // A client that stops reading is cut off at the deadline.
        {
            web::stall::Response bound(secure_server.request(), 10);
            CHECK(armed_for(transfer_us(10)));
            expire_now();
            CHECK(pair.ended());
        }
        CHECK(!armed());
        responded = true;
    };
    CHECK(__wrap_httpd_req_new(&secure_server.data, &session) == ESP_OK && responded && events == "head,handler,");
    handler_work = nullptr;
    // Outside a handler, nothing is bounded.
    {
        web::stall::Response bound(secure_server.request(), 10);
        CHECK(!armed());
    }
}

void other_server_passes() {
    prepare_and_serve();
    // Another HTTPD server, such as HomeKit's, passes through unchanged.
    Pair pair;
    sock_db session{};
    session.fd = pair.fds[0];
    other_server.session.fd = pair.fds[0];
    other_server.data.hd_req_aux.remaining_len = 4096;
    handler_work = [] {
        web::stall::Response bound(other_server.request(), 68696);
        CHECK(!armed());
    };
    CHECK(__wrap_httpd_req_new(&other_server.data, &session) == ESP_OK);
    CHECK(__wrap_httpd_req_delete(&other_server.data) == ESP_OK);
    CHECK(events == "head,handler,delete," && !pair.ended());
    handler_work = nullptr;
    // Releasing another server keeps the HTTPS server guarded; releasing the
    // HTTPS server stops guarding it.
    web::stall::release(&other_server.data);
    head_socket = pair.fds[0];
    events.clear();
    CHECK(__wrap_httpd_req_new(&secure_server.data, &session) == ESP_OK && events == "head,handler,");
    web::stall::release(&secure_server.data);
    guarded = nullptr;
    secure_server.session.fd = pair.fds[0];
    secure_server.data.hd_req_aux.remaining_len = 4096;
    events.clear();
    CHECK(__wrap_httpd_req_new(&secure_server.data, &session) == ESP_OK);
    CHECK(__wrap_httpd_req_delete(&secure_server.data) == ESP_OK);
    CHECK(events == "head,handler,delete," && !pair.ended());
}

// The handshake deadline uses the same timer for the session's socket.
void handshake() {
    // A TLS handshake gets 5 s.
    CHECK(handshake_us == 5000000);
    prepare_and_serve();
    Pair pair;
    web::stall::arm(pair.fds[0], handshake_us);
    CHECK(armed_for(handshake_us));
    expire_now();
    CHECK(pair.ended());
    web::stall::disarm();
    CHECK(!armed());
    // Expiry after a disarm cannot reach a reused descriptor.
    Pair reused;
    expire_now();
    CHECK(!reused.ended());
}

}  // namespace

int main(int argc, char **argv) {
    return amity_test::run(argc, argv,
                           {{"before-prepare", before_prepare},
                            {"request-head", request_head},
                            {"unread-body", unread_body},
                            {"handler-response", handler_response},
                            {"other-server", other_server_passes},
                            {"handshake", handshake}});
}
