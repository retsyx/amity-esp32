/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// The administration web server, built from the whole web_admin.cpp: starting
// and stopping it, the public readiness reply, the embedded assets and where
// the API checks the session. request.cpp sends the responses through the
// HTTP server fake; every other firmware file it calls is a strict seam.
#ifdef __clang__
#pragma clang diagnostic push
// web_admin.cpp declares the assets in its anonymous namespace, and the
// embedded files define them outside C++, as below.
#pragma clang diagnostic ignored "-Wundefined-internal"
#endif
#include "../components/web_admin/web_admin.cpp"
#ifdef __clang__
#pragma clang diagnostic pop
#endif
#include "core_fakes.hpp"
#include "httpd_fake.hpp"
#include "seams.hpp"
#include "stall.hpp"
#include <algorithm>
#include <initializer_list>
#include <iterator>
#include <string>
#include <vector>

// ESP-IDF embeds each compressed asset between _binary_<file>_start and
// _binary_<file>_end labels. These payloads take their place.
#ifdef __APPLE__
#define ASSET_SECTION "__DATA,__const"
#else
#define ASSET_SECTION ".rodata"
#endif
#define ASSET(file, bytes)                                                                         \
    ".globl _binary_" file "_start\n.globl _binary_" file "_end\n_binary_" file "_start:\n.ascii \"" \
    bytes "\"\n_binary_" file "_end:\n"
asm(".pushsection " ASSET_SECTION "\n" ASSET("index_html_gz", "index") ASSET("app_js_gz", "app")
        ASSET("system_js_gz", "system") ASSET("style_css_gz", "style") ASSET("favicon_svg_gz", "favicon")
            ".popsection\n");

// Only the HTTPS server task arms a response deadline.
web::stall::Response::Response(httpd_req_t *, size_t) { (void)armed_; }
web::stall::Response::~Response() {}

namespace {

using amity_fake::HttpRequest;
namespace consortium = amity::consortium;
namespace model = amity::system;
using Document = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;

const char *const boot = "11111111111111111111111111111111";
const char *const device = "0123456789abcdef0123456789abcdef";
const char *const group = "fedcba9876543210fedcba9876543210";

amity::SystemControl control;

// The handle web::start gives the administration server.
int server_storage;
const httpd_handle_t running = &server_storage;

struct Route {
    std::string path;
    httpd_method_t method;
    web::Handler handler;
    bool operator==(const Route &other) const {
        return path == other.path && method == other.method && handler == other.handler;
    }
};
// Routes registered, including one that failed.
std::vector<Route> routes;
bool redirect_open = false;
// What the scripted server calls return.
esp_err_t post_route_result = ESP_OK, redirect_start_result = ESP_OK, redirect_stop_result = ESP_OK,
          server_stop_result = ESP_OK;

const std::initializer_list<const char *> lifecycle = {"web::start", "web::route", "web::start_redirect",
                                                       "web::stop_redirect", "system_services::stop_log_downloads",
                                                       "web::stop"};

// The calls to the named seams, in order.
std::vector<std::string> calls_to(std::initializer_list<const char *> names) {
    std::vector<std::string> out;
    for (const auto &call : amity_seam::calls())
        if (std::find(names.begin(), names.end(), call) != names.end()) out.push_back(call);
    return out;
}

std::vector<Route> administration_routes() {
    std::vector<Route> out;
    for (const char *path : {"/", "/app.js", "/system.js", "/style.css", "/favicon.svg"})
        out.push_back({path, HTTP_GET, web_admin::asset});
    out.push_back({"/api/*", HTTP_GET, web_admin::api});
    out.push_back({"/api/*", HTTP_POST, web_admin::api});
    return out;
}

// A ready original main, as runtime_status reports it.
consortium::RuntimeStatus coordinator() {
    consortium::RuntimeStatus role;
    role.initialized = role.credential_ready = true;
    role.coordinator = role.authority_ready = true;
    role.phase = "idle";
    return role;
}

// Scripts runtime_status with role, the server and redirect calls, and the
// facts the readiness reply reads.
void script(const consortium::RuntimeStatus &role) {
    amity_seam::install<&consortium::runtime_status>([role] { return role; });
    amity_seam::install<&amity::HubService::set_system_identity>(
        [](amity::HubService &, model::ConsortiumId, model::OctopusId) {});
    control.set_identity(*model::ConsortiumId::parse(group), *model::OctopusId::parse(device));
    amity_seam::install<&web::start>([](httpd_handle_t &handle, const std::string &hostname, const std::string &auth) {
        CHECK(hostname == "test.local" && auth == "private");
        handle = running;
        return ESP_OK;
    });
    amity_seam::install<&web::route>(
        [](httpd_handle_t handle, const char *path, httpd_method_t method, web::Handler handler, void *context) {
            CHECK(handle == running && !context);
            routes.push_back({path, method, handler});
            return std::string(path) == "/api/*" && method == HTTP_POST ? post_route_result : ESP_OK;
        });
    amity_seam::install<&web::start_redirect>([](uint16_t port) {
        // The redirect opens only once every route is registered and the UI enabled.
        CHECK(port == 80 && web_admin::server == running && routes == administration_routes());
        CHECK(web_admin::ui_enabled.load());
        redirect_open = redirect_start_result == ESP_OK;
        return redirect_start_result;
    });
    // Handlers, including accepted ones, are revoked before any socket closes.
    amity_seam::install<&web::stop_redirect>([] {
        CHECK(web_admin::ui_revoked.load() && !web_admin::ui_enabled.load());
        if (redirect_stop_result == ESP_OK) redirect_open = false;
        return redirect_stop_result;
    });
    amity_seam::install<&system_services::stop_log_downloads>(
        [] { CHECK(web_admin::ui_revoked.load() && !web_admin::ui_enabled.load()); });
    amity_seam::install<&web::stop>([](httpd_handle_t handle) {
        CHECK(handle == running && web_admin::ui_revoked.load() && !web_admin::ui_enabled.load());
        return server_stop_result;
    });
    amity_seam::install<&system_services::boot_id>([] { return boot; });
    amity_seam::install<&device_settings::hostname>([] { return "current-name"; });
    amity_seam::install<&system_services::firmware_ready>([] { return true; });
}

// Starts administration on a ready original main.
void serve() {
    script(coordinator());
    CHECK(web_admin::start(control, "test", "private") == ESP_OK);
    amity_seam::clear_calls();
}

// Whether the response grants any cross-origin access.
bool cors(const HttpRequest &request) {
    for (const char *header : {"Access-Control-Allow-Origin", "Access-Control-Allow-Credentials",
                               "Access-Control-Allow-Methods", "Access-Control-Allow-Headers"})
        if (request.response->has_header(header)) return true;
    return false;
}

// Whether the request was answered once, with web::error's response and status.
bool rejected(const HttpRequest &request, const char *status) {
    const auto &response = *request.response;
    return response.finished && response.sends == 1 && response.status == status &&
           response.type == "application/json" && response.body.rfind("{\"error\":\"", 0) == 0 &&
           response.header("Connection") == "close";
}

// Whether the request was answered once with status and body.
bool answered(const HttpRequest &request, const char *status, const std::string &body) {
    const auto &response = *request.response;
    return response.finished && response.sends == 1 && response.status == status && response.body == body;
}

std::vector<std::string> keys(const cJSON *object) {
    std::vector<std::string> out;
    for (const cJSON *field = object ? object->child : nullptr; field; field = field->next)
        out.push_back(field->string);
    std::sort(out.begin(), out.end());
    return out;
}

std::string text(const cJSON *object, const char *key) {
    const cJSON *field = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(field) ? field->valuestring : "";
}

// GET /api/ready answers anyone, across origins, with exactly the device ID,
// boot ID, hostname and readiness. It reads nothing else: no session, body,
// clock or administration state.
void ready() {
    serve();
    for (bool firmware_ready : {false, true}) {
        amity_seam::install<&system_services::firmware_ready>([firmware_ready] { return firmware_ready; });
        amity_seam::clear_calls();
        HttpRequest request(HTTP_GET, "/api/ready");
        CHECK(web_admin::api(&request) == ESP_OK);
        const auto &response = *request.response;
        CHECK(response.finished && response.sends == 1 && response.status == "200 OK");
        CHECK(response.type == "application/json" && response.header("Cache-Control") == "no-store");
        CHECK(response.header("Access-Control-Allow-Origin") == "*");
        CHECK(!response.has_header("Access-Control-Allow-Credentials") &&
              !response.has_header("Access-Control-Allow-Methods"));
        CHECK((amity_seam::calls() == std::vector<std::string>{"system_services::boot_id", "device_settings::hostname",
                                                               "system_services::firmware_ready"}));
        CHECK(amity_fake::clock_reads() == 0 && amity_fake::timers().empty() && request.receive_calls == 0);
        Document body(cJSON_Parse(response.body.c_str()), cJSON_Delete);
        CHECK((keys(body.get()) == std::vector<std::string>{"boot_id", "device_id", "hostname", "ready"}));
        CHECK(text(body.get(), "device_id") == device && text(body.get(), "boot_id") == boot);
        CHECK(text(body.get(), "hostname") == "current-name");
        const cJSON *readiness = cJSON_GetObjectItemCaseSensitive(body.get(), "ready");
        CHECK(cJSON_IsBool(readiness) && bool(cJSON_IsTrue(readiness)) == firmware_ready);
    }
}

// Only GET of exactly /api/ready is public. Other methods and paths, query
// strings included, need a session, and a GET body is refused. No refusal
// grants cross-origin access.
void public_boundary() {
    serve();
    amity_seam::install<&web::authenticated>([](httpd_req_t *) { return false; });
    for (int method : {HTTP_POST, HTTP_OPTIONS}) {
        HttpRequest request(method, "/api/ready");
        CHECK(web_admin::api(&request) == ESP_FAIL && rejected(request, "401 Unauthorized") && !cors(request));
    }
    for (const char *path : {"/api/config", "/api/activity-editor/status", "/api/ready?cache=1"}) {
        HttpRequest request(HTTP_GET, path);
        CHECK(web_admin::api(&request) == ESP_FAIL && rejected(request, "401 Unauthorized") && !cors(request));
    }
    HttpRequest with_body(HTTP_GET, "/api/ready", "x");
    CHECK(web_admin::api(&with_body) == ESP_FAIL && rejected(with_body, "400 Bad Request") && !cors(with_body));
    CHECK(with_body.receive_calls == 0 && amity_fake::clock_reads() == 0);
    // Each refusal for want of a session checked it once, and nothing else ran.
    CHECK((amity_seam::calls() == std::vector<std::string>(5, "web::authenticated")));
}

// GET /api/setup tells the login page, without a session, whether a password
// is needed and which firmware runs. Unlike /api/ready, other origins cannot
// read it.
void setup_status() {
    const uint8_t digest[] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef};
    std::copy(std::begin(digest), std::end(digest), amity_fake::app_description().app_elf_sha256);
    std::snprintf(amity_fake::app_description().version, sizeof(amity_fake::app_description().version), "1.2.3");
    serve();
    amity_seam::install<&web::setup_required>([] { return true; });
    amity_seam::clear_calls();
    HttpRequest request(HTTP_GET, "/api/setup");
    CHECK(web_admin::api(&request) == ESP_OK);
    const auto &response = *request.response;
    CHECK(response.finished && response.sends == 1 && response.status == "200 OK");
    CHECK(response.type == "application/json" && !cors(request));
    CHECK((amity_seam::calls() == std::vector<std::string>{"web::setup_required", "device_settings::hostname",
                                                           "system_services::boot_id",
                                                           "system_services::firmware_ready"}));
    Document body(cJSON_Parse(response.body.c_str()), cJSON_Delete);
    CHECK((keys(body.get()) == std::vector<std::string>{"boot_id", "hostname", "image_elf_sha256", "ready", "required",
                                                        "update_manifest_required", "version"}));
    CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(body.get(), "required")));
    CHECK(text(body.get(), "hostname") == "current-name" && text(body.get(), "version") == "1.2.3");
    CHECK(text(body.get(), "image_elf_sha256") == "0123456789abcdef" + std::string(48, '0'));
    CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(body.get(), "update_manifest_required")));
    CHECK(text(body.get(), "boot_id") == boot && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(body.get(), "ready")));
}

// Before administration starts, the API and the assets refuse every request
// without reading anything.
void disabled() {
    script(coordinator());
    amity_seam::clear_calls();
    HttpRequest request(HTTP_GET, "/api/ready"), page(HTTP_GET, "/");
    CHECK(web_admin::api(&request) == ESP_FAIL && rejected(request, "403 Forbidden") && !cors(request));
    CHECK(web_admin::asset(&page) == ESP_FAIL && rejected(page, "403 Forbidden") && !cors(page));
    CHECK(amity_seam::calls().empty() && amity_fake::clock_reads() == 0);
}

// Each asset is served gzip-encoded as its type, unsniffed, revalidated on
// every load, and with a policy that allows HTTPS probes of other .local
// names but no foreign scripts, styles, objects or framing. A GET body is
// refused.
void asset_headers() {
    serve();
    const std::string policy =
        "default-src 'self'; connect-src 'self' https://*.local; script-src 'self'; style-src 'self'; "
        "object-src 'none'; base-uri 'none'; frame-ancestors 'none'";
    struct Asset {
        const char *uri, *type, *body;
    };
    for (const Asset &asset :
         {Asset{"/", "text/html; charset=utf-8", "index"}, Asset{"/app.js", "text/javascript", "app"},
          Asset{"/system.js", "text/javascript", "system"}, Asset{"/style.css", "text/css", "style"},
          Asset{"/favicon.svg", "image/svg+xml", "favicon"}}) {
        HttpRequest request(HTTP_GET, asset.uri);
        CHECK(web_admin::asset(&request) == ESP_OK && answered(request, "200 OK", asset.body));
        const auto &response = *request.response;
        CHECK(response.type == asset.type && response.header("Content-Encoding") == "gzip");
        CHECK(response.header("X-Content-Type-Options") == "nosniff" && response.header("Cache-Control") == "no-cache");
        CHECK(response.header("Content-Security-Policy") == policy && !cors(request));
    }
    HttpRequest with_body(HTTP_GET, "/", "x");
    CHECK(web_admin::asset(&with_body) == ESP_FAIL && rejected(with_body, "400 Bad Request"));
    CHECK(with_body.receive_calls == 0 && amity_seam::calls().empty());
}

// The ETag is the first eight bytes of the firmware's ELF digest. A matching
// If-None-Match, strong, weak, in a list or *, is answered 304 without a body
// or encoding; any other tag gets the whole asset.
void asset_revalidation() {
    const uint8_t digest[] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef, 0xff};
    std::copy(std::begin(digest), std::end(digest), amity_fake::app_description().app_elf_sha256);
    serve();
    const std::string tag = "\"0123456789abcdef\"";
    {
        HttpRequest request(HTTP_GET, "/app.js");
        CHECK(web_admin::asset(&request) == ESP_OK && answered(request, "200 OK", "app"));
        CHECK(request.response->header("ETag") == tag && request.response->header("Content-Encoding") == "gzip");
    }
    for (const char *cached :
         {"\"0123456789abcdef\"", "W/\"0123456789abcdef\"", "\"older\", \"0123456789abcdef\"", "*"}) {
        HttpRequest request(HTTP_GET, "/app.js");
        request.headers["If-None-Match"] = cached;
        CHECK(web_admin::asset(&request) == ESP_OK && answered(request, "304 Not Modified", ""));
        CHECK(request.response->header("ETag") == tag && !request.response->has_header("Content-Encoding"));
    }
    HttpRequest changed(HTTP_GET, "/style.css");
    changed.headers["If-None-Match"] = "\"fedcba9876543210\"";
    CHECK(web_admin::asset(&changed) == ESP_OK && answered(changed, "200 OK", "style"));
    CHECK(changed.response->header("ETag") == tag && changed.response->header("Content-Encoding") == "gzip");
}

// start() reads role and refuses it before opening anything.
void refuses(const consortium::RuntimeStatus &role) {
    script(role);
    amity_seam::clear_calls();
    CHECK(web_admin::start(control, "test", "private") == ESP_ERR_INVALID_STATE);
    CHECK((amity_seam::calls() == std::vector<std::string>{"amity::consortium::runtime_status"}));
    CHECK(routes.empty() && !web_admin::server && !web_admin::ui_enabled.load());
}

// The API and the assets refuse requests, without a downstream effect.
void closed() {
    amity_seam::clear_calls();
    HttpRequest request(HTTP_GET, "/api/ready"), page(HTTP_GET, "/");
    CHECK(web_admin::api(&request) == ESP_FAIL && rejected(request, "403 Forbidden") && !cors(request));
    CHECK(web_admin::asset(&page) == ESP_FAIL && rejected(page, "403 Forbidden"));
    CHECK(amity_seam::calls().empty());
}

// Only a ready original main serves administration. Each refusing condition
// refuses on its own, as well as in the role runtime_status reports.
void member() {
    auto role = coordinator();
    role.coordinator = role.authority_ready = false;
    role.member = true;
    refuses(role);
    role = coordinator();
    role.member = true;
    refuses(role);
    closed();
}

void prepared() {
    auto role = coordinator();
    role.coordinator = role.authority_ready = false;
    role.member = true;
    role.phase = "finishing_enrollment";
    refuses(role);
    role = coordinator();
    role.member = true;
    role.phase = "finishing_enrollment";
    refuses(role);
    closed();
}

void recovery_required() {
    consortium::RuntimeStatus role;
    role.initialized = role.member = true;
    role.phase = "recovery_required";
    refuses(role);
    role = coordinator();
    role.phase = "recovery_required";
    refuses(role);
    closed();
}

void unclaimed() {
    refuses(consortium::RuntimeStatus{});
    auto role = coordinator();
    role.coordinator = false;
    refuses(role);
    closed();
}

void awaiting_setup() {
    consortium::RuntimeStatus role;
    role.initialized = role.credential_ready = true;
    role.phase = "awaiting_setup";
    refuses(role);
    role = coordinator();
    role.authority_ready = false;
    refuses(role);
    closed();
}

// A stop before any start has nothing to close and blocks every later start.
void stop_before_start() {
    script(coordinator());
    CHECK(web_admin::stop() == ESP_OK);
    CHECK((calls_to(lifecycle) == std::vector<std::string>{"web::stop_redirect"}));
    CHECK(web_admin::start(control, "test", "private") == ESP_ERR_INVALID_STATE);
    CHECK((calls_to(lifecycle) == std::vector<std::string>{"web::stop_redirect"}));
    closed();
}

// Starts and serves as a ready original main, then stops: once with the
// scripted results, and again after they clear.
void serve_and_stop(esp_err_t redirect_start, esp_err_t redirect_stop, esp_err_t server_stop) {
    redirect_start_result = redirect_start;
    script(coordinator());
    // A redirect error is returned, and the routed server is kept.
    CHECK(web_admin::start(control, "test", "private") == redirect_start);
    CHECK(routes == administration_routes() && web_admin::server == running && web_admin::ui_enabled.load());
    std::vector<std::string> started{"web::start"};
    started.insert(started.end(), 7, "web::route");
    started.push_back("web::start_redirect");
    CHECK(calls_to(lifecycle) == started && redirect_open == (redirect_start == ESP_OK));
    CHECK(web_admin::start(control, "test", "private") == ESP_ERR_INVALID_STATE && calls_to(lifecycle) == started);
    HttpRequest request(HTTP_GET, "/api/ready"), page(HTTP_GET, "/");
    CHECK(web_admin::api(&request) == ESP_OK && request.response->status == "200 OK");
    CHECK(web_admin::asset(&page) == ESP_OK && answered(page, "200 OK", "index"));

    // Stopping revokes the handlers, closes the redirect, then stops log
    // downloads and the server. A redirect error is reported ahead of a
    // server error, and a server that fails to stop is kept for a retry.
    redirect_stop_result = redirect_stop;
    server_stop_result = server_stop;
    amity_seam::clear_calls();
    CHECK(web_admin::stop() == (redirect_stop != ESP_OK ? redirect_stop : server_stop));
    CHECK(!web_admin::ui_enabled.load() && web_admin::ui_revoked.load());
    CHECK((calls_to(lifecycle) ==
           std::vector<std::string>{"web::stop_redirect", "system_services::stop_log_downloads", "web::stop"}));
    CHECK(redirect_open == (redirect_start == ESP_OK && redirect_stop != ESP_OK));
    CHECK((web_admin::server == running) == (server_stop != ESP_OK));
    // Requests already accepted are refused, and administration never starts again.
    closed();
    CHECK(web_admin::start(control, "test", "private") == ESP_ERR_INVALID_STATE && calls_to(lifecycle).empty());

    redirect_stop_result = server_stop_result = ESP_OK;
    amity_seam::clear_calls();
    CHECK(web_admin::stop() == ESP_OK && web_admin::stop() == ESP_OK);
    CHECK(!web_admin::server && !redirect_open);
    if (server_stop != ESP_OK)
        CHECK((calls_to(lifecycle) == std::vector<std::string>{"web::stop_redirect",
                                                               "system_services::stop_log_downloads", "web::stop",
                                                               "web::stop_redirect"}));
    else
        CHECK((calls_to(lifecycle) == std::vector<std::string>{"web::stop_redirect", "web::stop_redirect"}));
}

void live_stop() { serve_and_stop(ESP_OK, ESP_OK, ESP_OK); }
void stop_error() { serve_and_stop(ESP_OK, ESP_OK, ESP_ERR_NOT_FINISHED); }
void redirect_error() { serve_and_stop(ESP_ERR_NO_MEM, ESP_OK, ESP_OK); }
void redirect_stop_error() { serve_and_stop(ESP_OK, ESP_ERR_TIMEOUT, ESP_OK); }
void both_stop_errors() { serve_and_stop(ESP_OK, ESP_ERR_TIMEOUT, ESP_ERR_NOT_FINISHED); }

// A server that cannot route the API returns the error without enabling the
// UI or opening the redirect.
void route_error() {
    post_route_result = ESP_ERR_HTTPD_HANDLERS_FULL;
    script(coordinator());
    CHECK(web_admin::start(control, "test", "private") == ESP_ERR_HTTPD_HANDLERS_FULL);
    CHECK(routes == administration_routes() && !web_admin::ui_enabled.load());
    CHECK(amity_seam::count("web::start_redirect") == 0 && !redirect_open);
    closed();
}

// The gates an authenticated API request passes before its handler.
void authenticated_requests(bool passive) {
    amity_seam::install<&web::authenticated>([](httpd_req_t *) { return true; });
    amity_seam::install<&device_settings::reset_pending>([] { return false; });
    amity_seam::install<&web_admin::configuration_is_passive>([passive] { return passive; });
    amity_seam::install<&web_admin::activity_editor_request>(
        [](amity::SystemControl &hub, httpd_req_t *, const std::string &, esp_err_t &) {
            CHECK(&hub == &control);
            return false;
        });
    amity_seam::install<&web_admin::admin_request>(
        [](httpd_req_t *, const std::string &, esp_err_t &) { return false; });
    amity_seam::install<&system_services::upload_firmware>([](httpd_req_t *request) {
        return web::json(request, "{\"ok\":true}");
    });
}

// A firmware upload without a session is refused before anything reads it.
void firmware_unauthenticated() {
    serve();
    authenticated_requests(false);
    amity_seam::install<&web::authenticated>([](httpd_req_t *) { return false; });
    HttpRequest upload(HTTP_POST, "/api/firmware", "image");
    CHECK(web_admin::api(&upload) == ESP_FAIL && rejected(upload, "401 Unauthorized"));
    CHECK(amity_seam::count("system_services::upload_firmware") == 0 && upload.receive_calls == 0);
}

// Firmware uploads only through POST.
void firmware_get() {
    serve();
    authenticated_requests(false);
    HttpRequest request(HTTP_GET, "/api/firmware");
    CHECK(web_admin::api(&request) == ESP_FAIL && rejected(request, "404 Not Found"));
    CHECK(amity_seam::count("system_services::upload_firmware") == 0);
}

// An authenticated POST uploads firmware, also while configuration is passive
// and other changes are refused.
void firmware_upload() {
    serve();
    for (bool passive : {false, true}) {
        authenticated_requests(passive);
        amity_seam::clear_calls();
        HttpRequest upload(HTTP_POST, "/api/firmware", "image");
        CHECK(web_admin::api(&upload) == ESP_OK && answered(upload, "200 OK", "{\"ok\":true}"));
        CHECK((amity_seam::calls() ==
               std::vector<std::string>{"web::authenticated", "device_settings::reset_pending",
                                        "web_admin::configuration_is_passive", "web_admin::activity_editor_request",
                                        "web_admin::admin_request", "system_services::upload_firmware"}));
    }
    amity_seam::clear_calls();
    HttpRequest change(HTTP_POST, "/api/matter/close");
    CHECK(web_admin::api(&change) == ESP_FAIL && rejected(change, "403 Forbidden"));
    CHECK((amity_seam::calls() == std::vector<std::string>{"web::authenticated", "device_settings::reset_pending",
                                                           "web_admin::configuration_is_passive"}));
}

// The harness consumes what the scenarios script: start() reads the scripted
// role once and routes the handle web::start returned, each embedded asset
// spans exactly its payload, and a request's scripted header and the scripted
// ELF digest reach the asset handler.
void sanity() {
    const uint8_t digest[] = {0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10};
    std::copy(std::begin(digest), std::end(digest), amity_fake::app_description().app_elf_sha256);
    script(coordinator());
    amity_seam::clear_calls();
    CHECK(web_admin::start(control, "test", "private") == ESP_OK);
    CHECK(amity_seam::count("amity::consortium::runtime_status") == 1 && amity_seam::count("web::start") == 1);
    CHECK(routes.size() == 7 && web_admin::server == running && redirect_open);
    struct Span {
        const uint8_t *begin, *end;
        const char *payload;
    };
    for (const Span &span : {Span{web_admin::html_start, web_admin::html_end, "index"},
                             Span{web_admin::js_start, web_admin::js_end, "app"},
                             Span{web_admin::system_js_start, web_admin::system_js_end, "system"},
                             Span{web_admin::css_start, web_admin::css_end, "style"},
                             Span{web_admin::favicon_start, web_admin::favicon_end, "favicon"}})
        CHECK(std::string(reinterpret_cast<const char *>(span.begin), size_t(span.end - span.begin)) == span.payload);
    HttpRequest page(HTTP_GET, "/favicon.svg");
    page.headers["If-None-Match"] = "\"fedcba9876543210\"";
    CHECK(web_admin::asset(&page) == ESP_OK && answered(page, "304 Not Modified", ""));
}

// A request that reaches a dependency the scenario did not script fails the
// test; CTest expects this scenario's failure message.
void strict() {
    serve();
    amity_seam::install<&web::authenticated>([](httpd_req_t *) { return true; });
    HttpRequest request(HTTP_GET, "/api/config");
    web_admin::api(&request);
    CHECK(false);
}

}  // namespace

int main(int argc, char **argv) {
    return amity_test::run(argc, argv,
                           {{"ready", ready},
                            {"public-boundary", public_boundary},
                            {"setup-status", setup_status},
                            {"disabled", disabled},
                            {"asset-headers", asset_headers},
                            {"asset-revalidation", asset_revalidation},
                            {"member", member},
                            {"prepared", prepared},
                            {"recovery_required", recovery_required},
                            {"unclaimed", unclaimed},
                            {"awaiting_setup", awaiting_setup},
                            {"stop-before-start", stop_before_start},
                            {"live-stop", live_stop},
                            {"stop-error", stop_error},
                            {"redirect-error", redirect_error},
                            {"redirect-stop-error", redirect_stop_error},
                            {"both-stop-errors", both_stop_errors},
                            {"route-error", route_error},
                            {"firmware-unauthenticated", firmware_unauthenticated},
                            {"firmware-get", firmware_get},
                            {"firmware-upload", firmware_upload},
                            {"sanity", sanity},
                            {"strict", strict}});
}
