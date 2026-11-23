/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// The HTTP redirect server, components/web/redirect.cpp, built whole against
// the core fakes and native loopback TCP sockets. Its task runs on a thread,
// and the scripted clock decides when a client's deadline passes. Slow, idle
// and replaced clients, unread bodies, and stop and restart use real sockets.
#include "web.hpp"
#include "core_fakes.hpp"
#include "esp_heap_caps.h"
#include "scenario_main.hpp"
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <string>
#include <thread>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

using namespace std::chrono;

// The whole request head, from accept.
constexpr int64_t head_us = 3000000;
// Discarding an unread body after the reply.
constexpr int64_t linger_us = 1000000;

const std::string bad = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
std::string moved(const std::string &location) {
    return "HTTP/1.1 308 Permanent Redirect\r\nLocation: " + location +
           "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
}

uint16_t free_port() {
    const int fd = socket(AF_INET6, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    sockaddr_in6 address{};
    address.sin6_family = AF_INET6;
    address.sin6_addr = in6addr_any;
    CHECK(bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
    socklen_t size = sizeof(address);
    CHECK(getsockname(fd, reinterpret_cast<sockaddr *>(&address), &size) == 0);
    close(fd);
    return ntohs(address.sin6_port);
}
uint16_t port() {
    static const uint16_t value = free_port();
    return value;
}

int connect_to(bool v6 = false) {
    int fd;
    if (v6) {
        fd = socket(AF_INET6, SOCK_STREAM, 0);
        sockaddr_in6 address{};
        address.sin6_family = AF_INET6;
        address.sin6_addr = in6addr_loopback;
        address.sin6_port = htons(port());
        if (connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address))) {
            close(fd);
            return -1;
        }
    } else {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port());
        if (connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address))) {
            close(fd);
            return -1;
        }
    }
    timeval timeout{6, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    std::this_thread::sleep_for(milliseconds(50));  // accepted in arrival order
    return fd;
}
void put(int fd, const std::string &data) {
    CHECK(send(fd, data.data(), data.size(), 0) == static_cast<ssize_t>(data.size()));
}
// Everything the server sends before closing; a reset is not a close.
std::string response(int fd) {
    std::string data;
    char buffer[1024];
    for (;;) {
        const auto n = recv(fd, buffer, sizeof(buffer), 0);
        if (!n) return data;
        CHECK(n > 0);
        data.append(buffer, n);
    }
}
bool pending(int fd) {
    pollfd p{fd, POLLIN, 0};
    return poll(&p, 1, 0) == 0;
}
// Whether the server closed the connection without sending anything.
bool closed(int fd) {
    char byte;
    const auto n = recv(fd, &byte, 1, 0);
    return n == 0 || (n < 0 && errno == ECONNRESET);
}
// After the server's reply and FIN: whether the connection still ends
// cleanly, or the server has since reset it. Once the FIN has arrived, Linux
// recv() returns 0 even after a reset and records the reset as EPIPE, so the
// socket error is read first; macOS records ECONNRESET.
int socket_error(int fd) {
    int error = 0;
    socklen_t size = sizeof(error);
    CHECK(getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) == 0);
    return error;
}
bool ends_cleanly(int fd) {
    char byte;
    return socket_error(fd) == 0 && recv(fd, &byte, 1, MSG_DONTWAIT) == 0;
}
bool was_reset(int fd) {
    const int error = socket_error(fd);
    return error == ECONNRESET || error == EPIPE;
}
std::string ask(const std::string &request, bool v6 = false) {
    const int fd = connect_to(v6);
    CHECK(fd >= 0);
    put(fd, request);
    auto result = response(fd);
    close(fd);
    return result;
}

// Starts the redirect server and runs its task on a thread.
std::thread start() {
    const auto before = amity_fake::tasks().size();
    CHECK(web::start_redirect(port()) == ESP_OK);
    const auto tasks = amity_fake::tasks();
    CHECK(tasks.size() == before + 1);
    return amity_fake::start_task(tasks.back().handle);
}
void stop(std::thread &task) {
    CHECK(web::stop_redirect() == ESP_OK);
    task.join();
    CHECK(amity_fake::tasks().back().state == amity_fake::Task::State::deleted);
}

// The requested URL is unchanged apart from its scheme.
void location() {
    using web::redirect_location;
    CHECK(redirect_location("GET / HTTP/1.1\r\nHost: amity.local\r\n\r\n") == "https://amity.local/");
    CHECK(redirect_location("GET /api/status?view=all&x=%20 HTTP/1.1\r\nHost: amity.local\r\n\r\n") ==
          "https://amity.local/api/status?view=all&x=%20");
    CHECK(redirect_location("POST /a/b/ HTTP/1.0\r\nUser-Agent: x\r\nhOsT: \t Living-Room.local:8080 \r\n"
                            "Accept: */*\r\n\r\n") == "https://Living-Room.local:8080/a/b/");
    CHECK(redirect_location("GET / HTTP/1.1\nHost: [fe80::1]\n\n") == "https://[fe80::1]/");
    // An absolute-form target already names the host.
    CHECK(redirect_location("GET http://amity.local/x?y=1 HTTP/1.1\r\nHost: other.local\r\n\r\n") ==
          "https://amity.local/x?y=1");
    CHECK(redirect_location("GET HTTP://Amity.local/ HTTP/1.0\r\n\r\n") == "https://Amity.local/");
    // Malformed heads, missing or repeated hosts, and targets that are not
    // paths cannot be redirected.
    for (const char *head :
         {"GET / HTTP/1.1\r\n\r\n", "GET / HTTP/1.1\r\nHost:\r\n\r\n", "GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n",
          "GET / HTTP/1.1\r\nHost: a\r\n", "OPTIONS * HTTP/1.1\r\nHost: a\r\n\r\n",
          "GET https://a/ HTTP/1.1\r\nHost: a\r\n\r\n", "GET http:// HTTP/1.1\r\nHost: a\r\n\r\n",
          "GET /\r\nHost: a\r\n\r\n", "GET / SIP/2.0\r\nHost: a\r\n\r\n", "GET  / HTTP/1.1\r\nHost: a\r\n\r\n",
          "GET /\x01 HTTP/1.1\r\nHost: a\r\n\r\n", "GET / HTTP/1.1\r\nHost: a b\r\n\r\n",
          "GET / HTTP/1.1\r\nHost: evil/x?\r\n\r\n", "GET / HTTP/1.1\r\nHost: user@a\r\n\r\n",
          "GET / HTTP/1.1\r\nX: 1\r\n folded\r\nHost: a\r\n\r\n", "GET / HTTP/1.1\r\n: x\r\nHost: a\r\n\r\n",
          "\r\nGET / HTTP/1.1\r\nHost: a\r\n\r\n", ""})
        CHECK(redirect_location(head).empty());
}

// One server task, in PSRAM, serves the dual-stack listener.
void start_once() {
    CHECK(web::start_redirect(port()) == ESP_OK);
    CHECK(web::start_redirect(port()) == ESP_ERR_INVALID_STATE);
    const auto tasks = amity_fake::tasks();
    CHECK(tasks.size() == 1);
    CHECK(tasks[0].name == "http_redirect" && tasks[0].stack_bytes == 4096 && tasks[0].priority == 2);
    CHECK(tasks[0].with_caps && tasks[0].caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    auto task = amity_fake::start_task(tasks[0].handle);
    // IPv4 and IPv6 clients share the dual-stack listener.
    CHECK(ask("GET /a?b=1 HTTP/1.1\r\nHost: amity.local\r\n\r\n") == moved("https://amity.local/a?b=1"));
    CHECK(ask("HEAD / HTTP/1.1\r\nHost: amity.local\r\n\r\n", true) == moved("https://amity.local/"));
    CHECK(ask("GET / HTTP/1.1\r\n\r\n") == bad);
    stop(task);
}

// Without its task the server does not start, and the port stays free.
void task_failure() {
    amity_fake::fail_task_creations(1);
    CHECK(web::start_redirect(port()) == ESP_ERR_NO_MEM);
    CHECK(amity_fake::tasks().empty() && connect_to() < 0);
    auto task = start();
    CHECK(ask("GET /b HTTP/1.1\r\nHost: a\r\n\r\n") == moved("https://a/b"));
    stop(task);
}

// A head arriving in pieces is answered once complete.
void head_in_pieces() {
    auto task = start();
    const int fd = connect_to();
    put(fd, "GET /x HT");
    std::this_thread::sleep_for(milliseconds(100));
    put(fd, "TP/1.1\r\nHo");
    std::this_thread::sleep_for(milliseconds(100));
    CHECK(pending(fd));
    put(fd, "st: a\r\n\r\n");
    CHECK(response(fd) == moved("https://a/x"));
    close(fd);
    stop(task);
}

// An oversized head is rejected without waiting for its end. The scripted
// clock never reaches the head deadline, so only the size limit answers it.
void oversized_head() {
    auto task = start();
    CHECK(ask("GET /" + std::string(3000, 'a')) == bad);
    // A head just below the limit is still awaited.
    const int fd = connect_to();
    put(fd, "GET /" + std::string(2042, 'a'));
    std::this_thread::sleep_for(milliseconds(300));
    CHECK(pending(fd));
    put(fd, "a");
    CHECK(response(fd) == bad);
    close(fd);
    stop(task);
}

// An unread request body cannot reset the connection before the client has
// read the response.
void unread_body() {
    auto task = start();
    CHECK(ask("POST /api/login HTTP/1.1\r\nHost: amity.local\r\nContent-Length: 100000\r\n\r\n" +
              std::string(20000, 'x')) == moved("https://amity.local/api/login"));
    stop(task);
}

// After the reply, an unread body of any length is read and discarded: the
// connection stays open until the client closes it or the linger deadline
// passes, so further data from the client is not answered with a reset.
void drained_body() {
    auto task = start();
    const int fd = connect_to();
    put(fd, "POST /up HTTP/1.1\r\nHost: a\r\nContent-Length: 100000\r\n\r\n" + std::string(4096, 'x'));
    CHECK(response(fd) == moved("https://a/up"));
    std::this_thread::sleep_for(milliseconds(300));
    put(fd, "x");
    std::this_thread::sleep_for(milliseconds(300));
    CHECK(ends_cleanly(fd));
    // The linger deadline ends the connection; the next data is refused.
    amity_fake::advance_us(linger_us);
    std::this_thread::sleep_for(milliseconds(300));
    put(fd, "x");
    std::this_thread::sleep_for(milliseconds(300));
    CHECK(was_reset(fd));
    close(fd);
    stop(task);
}

// A client stalled mid-head does not delay another client, and is closed
// without a response when its head deadline passes.
void stalled_client() {
    auto task = start();
    const int slow = connect_to();
    put(slow, "GET / HTTP/1.1\r\nHost: a");
    const auto asked = steady_clock::now();
    CHECK(ask("GET /fast HTTP/1.1\r\nHost: a\r\n\r\n") == moved("https://a/fast"));
    CHECK(steady_clock::now() - asked < milliseconds(500) && pending(slow));
    amity_fake::advance_us(head_us - 1);
    std::this_thread::sleep_for(milliseconds(300));
    CHECK(pending(slow));
    amity_fake::advance_us(1);
    CHECK(closed(slow));
    close(slow);
    stop(task);
}

// With both slots held, a new client replaces the oldest unanswered one, and
// an answered client still lingering is replaced before an older unanswered one.
void replacement() {
    auto task = start();
    const int oldest = connect_to();
    amity_fake::advance_ms(1);
    const int newer = connect_to();
    amity_fake::advance_ms(1);
    const auto asked = steady_clock::now();
    CHECK(ask("GET /c HTTP/1.1\r\nHost: a\r\n\r\n") == moved("https://a/c"));
    CHECK(closed(oldest) && steady_clock::now() - asked < milliseconds(500) && pending(newer));
    amity_fake::advance_ms(1);
    const int answered = connect_to();
    put(answered, "GET /d HTTP/1.1\r\nHost: a\r\n\r\n");
    std::this_thread::sleep_for(milliseconds(100));
    amity_fake::advance_ms(1);
    const int next = connect_to();
    CHECK(pending(newer));
    put(next, "GET /e HTTP/1.1\r\nHost: a\r\n\r\n");
    CHECK(response(next) == moved("https://a/e"));
    CHECK(response(answered) == moved("https://a/d"));
    close(oldest);
    close(newer);
    close(answered);
    close(next);
    stop(task);
}

// Stop releases the port promptly, even while the server waits idle in
// select; a later start serves again.
void stop_and_restart() {
    auto task = start();
    CHECK(ask("GET /idle HTTP/1.1\r\nHost: a\r\n\r\n") == moved("https://a/idle"));
    std::this_thread::sleep_for(milliseconds(300));
    const auto started = steady_clock::now();
    CHECK(web::stop_redirect() == ESP_OK && steady_clock::now() - started < milliseconds(1000));
    task.join();
    CHECK(web::stop_redirect() == ESP_OK);
    CHECK(connect_to() < 0);
    task = start();
    CHECK(ask("GET /again HTTP/1.1\r\nHost: a\r\n\r\n") == moved("https://a/again"));
    stop(task);
    CHECK(connect_to() < 0);
    CHECK(amity_fake::tasks().size() == 2);
}

}  // namespace

int main(int argc, char **argv) {
    std::signal(SIGPIPE, SIG_IGN);
    return amity_test::run(argc, argv,
                           {{"location", location},
                            {"start-once", start_once},
                            {"task-failure", task_failure},
                            {"head-in-pieces", head_in_pieces},
                            {"oversized-head", oversized_head},
                            {"unread-body", unread_body},
                            {"drained-body", drained_body},
                            {"stalled-client", stalled_client},
                            {"replacement", replacement},
                            {"stop-and-restart", stop_and_restart}});
}
