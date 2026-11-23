/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// The HTTPS server's startup and pre-handshake policy, components/web/web.cpp,
// included whole. Credentials, the TLS identity, the stall guard and the SDK's
// HTTPS server are strict seams, so the call journal shows their order. The
// handshake wrapper sets its TCP option on native sockets.
#include "web.cpp"
#include "core_fakes.hpp"
#include "httpd_fake.hpp"
#include "scenario_main.hpp"
#include "seams.hpp"
#include <cerrno>
#include <string>
#include <vector>

esp_err_t web::tls_identity(const std::string &hostname, std::string &certificate, std::string &private_key) {
    return AMITY_SEAM(web::tls_identity, hostname, certificate, private_key);
}
esp_err_t web::stall::prepare() { return AMITY_SEAM(web::stall::prepare); }
void web::stall::serve(httpd_handle_t server) { AMITY_SEAM(web::stall::serve, server); }
void web::stall::release(httpd_handle_t server) { AMITY_SEAM(web::stall::release, server); }
void web::stall::arm(int socket, uint64_t timeout_us) { AMITY_SEAM(web::stall::arm, socket, timeout_us); }
void web::stall::disarm() { AMITY_SEAM(web::stall::disarm); }
esp_err_t httpd_ssl_start(httpd_handle_t *handle, httpd_ssl_config_t *config) {
    return AMITY_SEAM(httpd_ssl_start, handle, config);
}
esp_err_t httpd_ssl_stop(httpd_handle_t handle) { return AMITY_SEAM(httpd_ssl_stop, handle); }
int __real_esp_tls_server_session_create(esp_tls_cfg_server_t *cfg, int socket, esp_tls_t *tls) {
    return AMITY_SEAM(__real_esp_tls_server_session_create, cfg, socket, tls);
}

namespace {

using Calls = std::vector<std::string>;

int server_object;
httpd_handle_t const server = &server_object;
httpd_ssl_config_t captured;
// A session the SDK would own; only its address is passed on.
alignas(16) char session[64];
esp_tls_t *const tls = reinterpret_cast<esp_tls_t *>(session);

// Installs a successful startup: each step checks its arguments.
void startup(esp_err_t credentials = ESP_OK, esp_err_t identity = ESP_OK, esp_err_t prepare = ESP_OK,
             esp_err_t start = ESP_OK) {
    amity_seam::install<&web::credentials_init>([credentials](const std::string &authentication) {
        CHECK(authentication == "fixture-authentication");
        return credentials;
    });
    amity_seam::install<&web::tls_identity>(
        [identity](const std::string &hostname, std::string &certificate, std::string &private_key) {
            CHECK(hostname == "amity-test");
            certificate = "fixture-certificate";
            private_key = "fixture-private-key";
            return identity;
        });
    amity_seam::install<&web::stall::prepare>([prepare] { return prepare; });
    amity_seam::install<&httpd_ssl_start>([start](httpd_handle_t *handle, httpd_ssl_config_t *config) {
        captured = *config;
        if (start == ESP_OK) *handle = server;
        return start;
    });
    amity_seam::install<&web::stall::serve>([](httpd_handle_t handle) { CHECK(handle == server); });
}

// The TCP_NODELAY option of a socket.
int nodelay(int socket) {
    int value = -1;
    socklen_t size = sizeof(value);
    CHECK(getsockopt(socket, IPPROTO_TCP, TCP_NODELAY, &value, &size) == 0);
    CHECK(size == sizeof(value));
    return value;
}
struct Socket {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    Socket() { CHECK(fd >= 0); }
    ~Socket() { ::close(fd); }
};

std::vector<amity_fake::LogLine> warnings() {
    std::vector<amity_fake::LogLine> found;
    for (const auto &line : amity_fake::log_lines())
        if (line.level == ESP_LOG_WARN) found.push_back(line);
    return found;
}

// Credentials, then the identity and the deadline timer, come before the
// server can accept a connection; the guard serves it once it has started.
void start_order() {
    startup();
    httpd_handle_t handle = nullptr;
    CHECK(web::start(handle, "amity-test", "fixture-authentication") == ESP_OK);
    CHECK(handle == server);
    CHECK(amity_seam::calls() == Calls({"web::credentials_init", "web::tls_identity", "web::stall::prepare",
                                        "httpd_ssl_start", "web::stall::serve"}));
}

// The HTTPS configuration, field by field.
void start_configuration() {
    startup();
    httpd_handle_t handle = nullptr;
    CHECK(web::start(handle, "amity-test", "fixture-authentication") == ESP_OK);
    CHECK(!captured.httpd.open_fn && captured.httpd.uri_match_fn == httpd_uri_match_wildcard);
    CHECK(captured.httpd.max_uri_handlers == 32 && captured.httpd.stack_size == 12288);
    CHECK(captured.httpd.max_open_sockets == 8 && captured.httpd.lru_purge_enable);
    CHECK(captured.httpd.recv_wait_timeout == 15 && captured.httpd.send_wait_timeout == 15);
    CHECK(captured.transport_mode == HTTPD_SSL_TRANSPORT_SECURE && captured.port_secure == 443);
    CHECK(!captured.session_tickets);
    // The certificate and key the identity left, with their terminating NULs.
    CHECK(web::certificate == "fixture-certificate" && web::private_key == "fixture-private-key");
    CHECK(captured.servercert == reinterpret_cast<const uint8_t *>(web::certificate.c_str()));
    CHECK(captured.servercert_len == web::certificate.size() + 1);
    CHECK(captured.prvtkey_pem == reinterpret_cast<const uint8_t *>(web::private_key.c_str()));
    CHECK(captured.prvtkey_len == web::private_key.size() + 1);
}

// A failed step is returned, and nothing after it runs.
void start_failures() {
    httpd_handle_t handle = nullptr;
    startup(37);
    CHECK(web::start(handle, "amity-test", "fixture-authentication") == 37);
    CHECK(!handle && amity_seam::calls() == Calls({"web::credentials_init"}));
    amity_seam::clear_calls();
    startup(ESP_OK, 41);
    CHECK(web::start(handle, "amity-test", "fixture-authentication") == 41);
    CHECK(!handle && amity_seam::calls() == Calls({"web::credentials_init", "web::tls_identity"}));
    // Without the deadline timer, the server does not start.
    amity_seam::clear_calls();
    startup(ESP_OK, ESP_OK, 45);
    CHECK(web::start(handle, "amity-test", "fixture-authentication") == 45);
    CHECK(!handle &&
          amity_seam::calls() == Calls({"web::credentials_init", "web::tls_identity", "web::stall::prepare"}));
    // A server that did not start is not guarded.
    amity_seam::clear_calls();
    startup(ESP_OK, ESP_OK, ESP_OK, 43);
    CHECK(web::start(handle, "amity-test", "fixture-authentication") == 43);
    CHECK(!handle && amity_seam::calls() == Calls({"web::credentials_init", "web::tls_identity",
                                                   "web::stall::prepare", "httpd_ssl_start"}));
}

// Each accepted socket gets TCP_NODELAY before the SDK's handshake, which
// runs under the handshake deadline for that socket with the arguments
// unchanged, and its result, success or failure, is returned.
void handshake() {
    esp_tls_cfg_server_t cfg{};
    Socket first, second;
    CHECK(nodelay(first.fd) == 0 && nodelay(second.fd) == 0);
    int armed_socket = -2;
    amity_seam::install<&web::stall::arm>([&](int socket, uint64_t timeout_us) {
        CHECK(armed_socket == -2 && timeout_us == web::stall::handshake_us);
        armed_socket = socket;
    });
    amity_seam::install<&web::stall::disarm>([&] { armed_socket = -2; });
    int expected_socket = -1, result = 0;
    amity_seam::install<&__real_esp_tls_server_session_create>([&](esp_tls_cfg_server_t *c, int socket, esp_tls_t *t) {
        CHECK(c == &cfg && t == tls && socket == expected_socket);
        CHECK(nodelay(socket) != 0 && armed_socket == socket);
        return result;
    });
    const Calls one = {"web::stall::arm", "__real_esp_tls_server_session_create", "web::stall::disarm"};
    expected_socket = first.fd;
    CHECK(__wrap_esp_tls_server_session_create(&cfg, first.fd, tls) == 0);
    CHECK(nodelay(first.fd) != 0 && nodelay(second.fd) == 0 && armed_socket == -2);
    CHECK(amity_seam::calls() == one);
    amity_seam::clear_calls();
    expected_socket = second.fd;
    result = -0x7780;
    CHECK(__wrap_esp_tls_server_session_create(&cfg, second.fd, tls) == -0x7780);
    CHECK(nodelay(second.fd) != 0 && armed_socket == -2 && amity_seam::calls() == one);
    CHECK(warnings().empty());
}

// A socket option failure is reported once, and the SDK still runs the
// handshake with the same arguments; even an arbitrary failure is returned.
void handshake_option_failure() {
    esp_tls_cfg_server_t cfg{};
    int armed_socket = -2;
    amity_seam::install<&web::stall::arm>([&](int socket, uint64_t) { armed_socket = socket; });
    amity_seam::install<&web::stall::disarm>([&] { armed_socket = -2; });
    amity_seam::install<&__real_esp_tls_server_session_create>([&](esp_tls_cfg_server_t *c, int socket, esp_tls_t *t) {
        CHECK(c == &cfg && t == tls && socket == -1 && armed_socket == -1);
        CHECK(warnings().size() == 1);
        return -1234;
    });
    CHECK(__wrap_esp_tls_server_session_create(&cfg, -1, tls) == -1234);
    CHECK(armed_socket == -2);
    CHECK(amity_seam::calls() ==
          Calls({"web::stall::arm", "__real_esp_tls_server_session_create", "web::stall::disarm"}));
    const auto lines = warnings();
    CHECK(lines.size() == 1 && lines[0].tag == "web");
    CHECK(lines[0].message == "Could not enable TCP_NODELAY on HTTPS socket: errno " + std::to_string(EBADF));
}

// Stopping releases the guard only once the server has stopped.
void stop() {
    esp_err_t stopped = 47;
    amity_seam::install<&httpd_ssl_stop>([&](httpd_handle_t handle) {
        CHECK(handle == server);
        return stopped;
    });
    amity_seam::install<&web::stall::release>([](httpd_handle_t handle) { CHECK(handle == server); });
    CHECK(web::stop(server) == 47);
    CHECK(amity_seam::calls() == Calls({"httpd_ssl_stop"}));
    stopped = ESP_OK;
    CHECK(web::stop(server) == ESP_OK);
    CHECK(amity_seam::calls() == Calls({"httpd_ssl_stop", "httpd_ssl_stop", "web::stall::release"}));
}

esp_err_t handler(httpd_req_t *) { return ESP_OK; }

// A route registers its handler and context, and a refusal is returned.
void route() {
    int context;
    CHECK(web::route(server, "/api/status", HTTP_GET, handler, &context) == ESP_OK);
    CHECK(web::route(server, "/api/status", HTTP_POST, handler) == ESP_OK);
    CHECK(web::route(server, "/api/status", HTTP_GET, handler) == ESP_ERR_HTTPD_HANDLER_EXISTS);
    const auto handlers = amity_fake::uri_handlers();
    CHECK(handlers.size() == 2);
    CHECK(handlers[0].server == server && handlers[0].uri == "/api/status" && handlers[0].method == HTTP_GET);
    CHECK(handlers[0].handler == handler && handlers[0].user_ctx == &context);
    CHECK(handlers[1].method == HTTP_POST && handlers[1].user_ctx == nullptr);
}

}  // namespace

int main(int argc, char **argv) {
    return amity_test::run(argc, argv,
                           {{"start-order", start_order},
                            {"start-configuration", start_configuration},
                            {"start-failures", start_failures},
                            {"handshake", handshake},
                            {"handshake-option-failure", handshake_option_failure},
                            {"stop", stop},
                            {"route", route}});
}
