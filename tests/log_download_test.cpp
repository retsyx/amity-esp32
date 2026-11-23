/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// log_download.cpp, included whole so the scenarios can wait for its worker,
// with the real system_services.cpp linked for the log ring and its
// snapshots. It builds against the core fakes, the HTTP server fake and strict
// seams for the runtime, device settings and web.
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include <sys/types.h>

// Four calls are observed where the included file makes them. Each forwards
// to the shared fakes unless the scenario scripts a failure, and the last two
// run the scenario's hook first.
namespace observed {
BaseType_t queue_send(QueueHandle_t queue, const void *item, TickType_t wait, BaseType_t position);
esp_err_t async_begin(httpd_req_t *request, httpd_req_t **out);
esp_err_t async_complete(httpd_req_t *request);
esp_err_t send_chunk(httpd_req_t *request, const char *bytes, ssize_t size);
}  // namespace observed
#define xQueueGenericSend observed::queue_send
#define httpd_req_async_handler_begin observed::async_begin
#define httpd_req_async_handler_complete observed::async_complete
#define httpd_resp_send_chunk observed::send_chunk
#include "../components/system_services/log_download.cpp"
#undef httpd_resp_send_chunk
#undef httpd_req_async_handler_complete
#undef httpd_req_async_handler_begin
#undef xQueueGenericSend

#include "bootloader_random.h"
#include "core_fakes.hpp"
#include "esp_random.h"
#include "httpd_fake.hpp"
#include "network_diagnostics.hpp"
#include "scenario_main.hpp"
#include "seams.hpp"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <tuple>
#include <unistd.h>
#include <vector>

namespace observed {

std::atomic<unsigned> failing_sends{0}, failing_begins{0};
// Runs before each chunk with its bytes, on the sending thread; a result
// other than ESP_OK is the send's, and nothing is sent.
std::function<esp_err_t(std::string_view)> on_chunk;
// Runs when an asynchronous request completes, before the fake completes it.
std::function<void()> on_complete;

BaseType_t queue_send(QueueHandle_t queue, const void *item, TickType_t wait, BaseType_t position) {
    if (failing_sends.load()) {
        --failing_sends;
        return errQUEUE_FULL;
    }
    return xQueueGenericSend(queue, item, wait, position);
}
esp_err_t async_begin(httpd_req_t *request, httpd_req_t **out) {
    if (failing_begins.load()) {
        --failing_begins;
        return ESP_ERR_NO_MEM;
    }
    return httpd_req_async_handler_begin(request, out);
}
esp_err_t async_complete(httpd_req_t *request) {
    if (on_complete) on_complete();
    return httpd_req_async_handler_complete(request);
}
esp_err_t send_chunk(httpd_req_t *request, const char *bytes, ssize_t size) {
    if (on_chunk && bytes && size > 0) {
        const esp_err_t result = on_chunk(std::string_view(bytes, size_t(size)));
        if (result != ESP_OK) return result;
    }
    return httpd_resp_send_chunk(request, bytes, size);
}

}  // namespace observed

namespace {

namespace c = amity::consortium;
namespace model = amity::system;

template <class Id> Id id(unsigned n) {
    char text[33];
    std::snprintf(text, sizeof(text), "%032x", n);
    return *Id::parse(text);
}

const model::OctopusId local = id<model::OctopusId>(1);
const std::string boot = "00000001000000020000000300000004";

int quiet(const char *, va_list) { return 0; }

int call(vprintf_like_t sink, const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    const int result = sink(format, arguments);
    va_end(arguments);
    return result;
}

// Appends text to the log ring through the console hook, a line buffer at a time.
void write_log(std::string_view text) {
    const vprintf_like_t sink = esp_log_set_vprintf(nullptr);
    esp_log_set_vprintf(sink);
    for (size_t offset = 0; offset < text.size(); offset += 383) {
        const int count = int(std::min<size_t>(383, text.size() - offset));
        call(sink, "%.*s", count, text.data() + offset);
    }
}

// The ring's lock, the only FreeRTOS mutex these files create.
SemaphoreHandle_t ring_lock() {
    for (const auto &queue : amity_fake::queues())
        if (queue.kind == amity_fake::Queue::Kind::mutex) return queue.handle;
    amity_test::fail(__FILE__, __LINE__, "the log ring has no lock");
}

// The members the runtime reports, and what each one's log pull does.
struct Member {
    unsigned id, boot;
    const char *hostname;
    bool enrolled, provisional;
};
const Member members[] = {
    {2, 12, "bedroom", true, false},   // its whole log
    {3, 13, "office", true, false},    // part of its log, then no answer in time
    {4, 0, "offline", true, false},    // unreachable
    {5, 15, "candidate", true, true},  // provisional
    {6, 16, "kitchen", true, false},   // no logs
    {7, 17, "garage", false, false},   // not enrolled
    {8, 18, "attic", true, false},     // busy
    {9, 19, "porch", true, false},     // no answer in time, before any log
    {1, 11, "itself", true, false},    // this device
};

struct Errors {
    std::mutex mutex;
    // web::error's request, message and status, for each error answered.
    std::vector<std::tuple<httpd_req_t *, std::string, std::string>> answered;
};
Errors errors;

bool main_device = true;
std::mutex pull_mutex;
std::condition_variable pull_changed;
std::vector<unsigned> pulled;
bool block_pull = false, pull_waiting = false, release_pull = false;

// The request was last answered with this error and 503, and sent nothing else.
bool refused(amity_fake::HttpRequest &request, const char *message) {
    std::lock_guard<std::mutex> lock(errors.mutex);
    for (auto error = errors.answered.rbegin(); error != errors.answered.rend(); ++error) {
        const auto &[answered, text, status] = *error;
        if (answered == &request) return text == message && status == "503 Service Unavailable" && !request.response->sends;
    }
    return false;
}

// A download that must be refused while another one is admitted.
void nested_download(const char *message) {
    amity_fake::HttpRequest other;
    CHECK(system_services::download_logs(&other, local) == ESP_FAIL);
    CHECK(refused(other, message));
}

// Seams for the runtime, device settings and web; log capture with the boot
// id from the entropy words 1, 2, 3 and 4; and FreeRTOS waits that block, as
// the download worker runs on its own thread.
void start() {
    amity_fake::waits(amity_fake::Waits::block);
    amity_seam::install<&bootloader_random_enable>([] {});
    amity_seam::install<&bootloader_random_disable>([] {});
    amity_seam::install<&esp_fill_random>([](void *bytes, size_t size) {
        const uint32_t entropy[] = {1, 2, 3, 4};
        CHECK(size == sizeof(entropy));
        std::memcpy(bytes, entropy, sizeof(entropy));
    });
    amity_seam::install<&amity::network_diagnostics::record_console_call>([](uint32_t, uint32_t) {});
    esp_log_set_vprintf(quiet);
    system_services::start_log_capture();
    amity_seam::install<&device_settings::hostname>([] { return "living-room"; });
    amity_seam::install<&web::error>([](httpd_req_t *request, const char *message, const char *status) {
        std::lock_guard<std::mutex> lock(errors.mutex);
        errors.answered.emplace_back(request, message, status);
        return ESP_FAIL;
    });
    amity_seam::install<&c::runtime_status>([] {
        c::RuntimeStatus status;
        status.coordinator = main_device;
        return status;
    });
    amity_seam::install<&c::runtime_peers>([] {
        std::vector<c::RuntimePeerStatus> peers;
        for (const auto &member : members) {
            c::RuntimePeerStatus peer;
            peer.id = id<model::OctopusId>(member.id);
            if (member.boot) peer.boot_id = id<model::BootId>(member.boot);
            peer.hostname = member.hostname;
            peer.enrolled = member.enrolled;
            peer.provisional = member.provisional;
            peers.push_back(peer);
        }
        return peers;
    });
    // No local snapshot is held while a member's log is pulled.
    amity_seam::install<&c::runtime_pull_logs>(
        [](model::OctopusId owner, const std::function<bool(std::string_view)> &consume) -> esp_err_t {
            CHECK(amity_fake::live_allocations() == 0);
            unsigned number = 0;
            for (const auto &member : members)
                if (id<model::OctopusId>(member.id) == owner) number = member.id;
            {
                std::unique_lock<std::mutex> lock(pull_mutex);
                pulled.push_back(number);
                if (block_pull) {
                    pull_waiting = true;
                    pull_changed.notify_all();
                    pull_changed.wait(lock, [] { return release_pull; });
                    block_pull = false;
                }
            }
            switch (number) {
            case 2: {
                const std::string chunk(c::log_chunk_bytes, 'A');
                for (size_t sent = 0; sent < c::log_capacity; sent += chunk.size())
                    if (!consume(chunk)) return ESP_FAIL;
                return ESP_OK;
            }
            case 3: return consume("partial-secondary\n") ? ESP_ERR_TIMEOUT : ESP_FAIL;
            case 4: return ESP_ERR_INVALID_STATE;
            case 6: return ESP_OK;
            case 8: return ESP_ERR_NO_MEM;
            case 9: return ESP_ERR_TIMEOUT;
            }
            amity_test::fail(__FILE__, __LINE__, "a log pull from a member that is never pulled");
        });
    // Every completion leaves the HTTP task free to refuse another download.
    observed::on_complete = [] { nested_download("A log download is already in progress."); };
}

// The one worker, which a successful download created.
TaskHandle_t worker_task() {
    const auto tasks = amity_fake::tasks();
    CHECK(tasks.size() == 1);
    const auto &task = tasks[0];
    CHECK(task.name == "log_download" && task.stack_bytes == 8192 && !task.argument && task.priority == 2);
    CHECK(task.with_caps && task.caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    return task.handle;
}

// Waits until no download is admitted.
void drain() {
    std::unique_lock<std::mutex> lock(system_services::download_mutex);
    system_services::download_finished.wait(lock, [] { return !system_services::active_request; });
}

// Stops downloads and waits for the worker to delete itself.
void stop(std::thread &worker) {
    system_services::stop_log_downloads();
    worker.join();
    CHECK(amity_fake::tasks()[0].state == amity_fake::Task::State::deleted);
    CHECK(amity_fake::queues().back().deleted && amity_fake::pending_async_requests() == 0);
    CHECK(amity_fake::live_allocations() == 0);
}

// A connected socket pair; the download's end is first.
struct Socket {
    int ends[2];
    Socket() { CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, ends) == 0); }
    ~Socket() {
        close(ends[0]);
        close(ends[1]);
    }
    // Whether the download's end was shut down; waits for it when wait is set.
    bool shut(bool wait = false) {
        pollfd ready{ends[1], POLLIN, 0};
        if (poll(&ready, 1, wait ? -1 : 0) != 1) return false;
        char byte;
        return recv(ends[1], &byte, 1, MSG_DONTWAIT) == 0;
    }
};

std::string section(const char *role, const std::string &hostname, unsigned device, const std::string &boot_text) {
    return std::string("\n===== ") + role + ": " + hostname + " =====\nDevice: " + id<model::OctopusId>(device).str() +
           "\nBoot: " + boot_text + "\n\n";
}
std::string member_section(unsigned number) {
    for (const auto &member : members)
        if (member.id == number)
            return section("Amity", member.hostname, number, member.boot ? id<model::BootId>(member.boot).str() : "unknown");
    return {};
}
// What the members contribute to a download from the main.
std::string member_logs() {
    return member_section(2) + std::string(c::log_capacity, 'A') + member_section(3) + "partial-secondary\n" +
           "\n[Log collection interrupted; some logs may be missing.]\n" + member_section(4) +
           "[Logs unavailable: the device could not be reached or is not ready.]\n" + member_section(6) +
           "[No logs captured.]\n" + member_section(8) + "[Logs unavailable: insufficient memory or the device is busy.]\n" +
           member_section(9) + "[Logs unavailable: the device did not respond in time.]\n";
}
const std::vector<unsigned> pulled_members{2, 3, 4, 6, 8, 9};

// Setup failures: no queue, no worker, no asynchronous request, or a full
// queue. Each is refused, leaves no request owned, and the next download
// succeeds.
void setup_failures() {
    start();
    const char *message = "Cannot start log download.";
    amity_fake::fail_queue_creations(1);
    amity_fake::HttpRequest no_queue;
    CHECK(system_services::download_logs(&no_queue, local) == ESP_FAIL && refused(no_queue, message));
    CHECK(amity_fake::queues().size() == 1 && amity_fake::tasks().empty());
    amity_fake::fail_task_creations(1);
    amity_fake::HttpRequest no_worker;
    CHECK(system_services::download_logs(&no_worker, local) == ESP_FAIL && refused(no_worker, message));
    CHECK(amity_fake::queues().back().deleted && amity_fake::tasks().empty() && !system_services::download_task);
    observed::failing_begins = 1;
    amity_fake::HttpRequest no_async;
    CHECK(system_services::download_logs(&no_async, local) == ESP_FAIL && refused(no_async, message));
    observed::failing_sends = 1;
    Socket socket;
    amity_fake::HttpRequest full;
    full.socket = socket.ends[0];
    CHECK(system_services::download_logs(&full, local) == ESP_FAIL && refused(full, message));
    CHECK(!socket.shut());
    for (const auto *request : {&no_queue, &no_worker, &no_async, &full}) CHECK(!request->response->sends);
    CHECK(!system_services::active_request && system_services::active_socket < 0);
    CHECK(amity_fake::pending_async_requests() == 0);
    const auto queue = amity_fake::queues().back();
    CHECK(!queue.deleted && queue.length == 1 && queue.item_size == sizeof(system_services::Download));
    main_device = false;
    amity_fake::HttpRequest retry;
    CHECK(system_services::download_logs(&retry, local) == ESP_OK);
    std::thread worker = amity_fake::start_task(worker_task());
    drain();
    CHECK(retry.response->finished && retry.response->body == section("Amity", "living-room", 1, boot) + "[No logs captured.]\n");
    stop(worker);
}

// A snapshot copies the ring, oldest first, into one PSRAM allocation made
// outside the ring's lock. The allocation holds the whole ring, however little
// the ring holds, since a line can arrive before the lock is taken. Later
// writes do not change the copy, it is freed with its last owner, and a failed
// allocation gives no snapshot.
void snapshot() {
    start();
    write_log("old\nnew\n");
    static size_t failed_bytes = 0;
    static uint32_t failed_caps = 0;
    static bool failed_locked = true;
    CHECK(heap_caps_register_failed_alloc_callback([](size_t bytes, uint32_t caps, const char *) {
              failed_bytes = bytes;
              failed_caps = caps;
              failed_locked = xSemaphoreGetMutexHolder(ring_lock()) != nullptr;
          }) == ESP_OK);
    amity_fake::fail_allocations(1);
    CHECK(!system_services::log_snapshot());
    CHECK(failed_bytes == c::log_capacity && failed_caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) && !failed_locked);
    CHECK(amity_fake::live_allocations() == 0);
    {
        const auto copy = system_services::log_snapshot();
        CHECK(copy && copy.size == 8 && std::string(copy.bytes.get(), copy.size) == "old\nnew\n");
        CHECK(amity_fake::allocation_caps(copy.bytes.get()) == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        CHECK(amity_fake::live_allocations() == 1);
        write_log("changed");
        CHECK(std::string(copy.bytes.get(), copy.size) == "old\nnew\n");
    }
    CHECK(amity_fake::live_allocations() == 0);
    std::string written = "old\nnew\nchanged";
    for (unsigned n = 0; written.size() < c::log_capacity + 5000; ++n) {
        const std::string line = std::to_string(n) + std::string(300, char('a' + n % 26)) + "\n";
        write_log(line);
        written += line;
    }
    {
        const auto copy = system_services::log_snapshot();
        CHECK(copy.size == c::log_capacity);
        CHECK(std::string(copy.bytes.get(), copy.size) == written.substr(written.size() - c::log_capacity));
    }
    CHECK(amity_fake::live_allocations() == 0);
}

// The main's download: its hostname names the file; the local log comes as
// the snapshot taken before it streamed, in log chunks, though the full ring
// moves on as its first chunk goes out; then each enrolled, admitted member's
// log or the reason it is missing. Another download is refused while this one
// streams, and the connection stays open.
void main_download() {
    start();
    std::string written;
    for (unsigned n = 0; written.size() < c::log_capacity + 1000; ++n) {
        const std::string line = "line " + std::to_string(n) + " " + std::string(n % 50, 'L') + "\n";
        write_log(line);
        written += line;
    }
    const std::string local_log = written.substr(written.size() - c::log_capacity);
    static std::vector<size_t> chunks;
    static size_t largest_live = 0;
    observed::on_chunk = [](std::string_view bytes) {
        chunks.push_back(bytes.size());
        largest_live = std::max(largest_live, amity_fake::live_allocations());
        if (chunks.size() == 1) nested_download("A log download is already in progress.");
        if (chunks.size() == 2) write_log("changed ring");
        return ESP_OK;
    };
    Socket socket;
    amity_fake::HttpRequest download;
    download.socket = socket.ends[0];
    CHECK(system_services::download_logs(&download, local) == ESP_OK);
    std::thread worker = amity_fake::start_task(worker_task());
    drain();
    const auto &response = *download.response;
    CHECK(response.finished && response.chunked && response.type == "text/plain; charset=utf-8");
    CHECK(response.header("Content-Disposition") == "attachment; filename=living-room.log");
    CHECK(response.header("Cache-Control") == "no-store");
    CHECK(response.body == section("Main Amity", "living-room", 1, boot) + local_log + member_logs());
    CHECK(chunks.size() > 7);
    CHECK(std::vector<size_t>(chunks.begin() + 1, chunks.begin() + 7) == std::vector<size_t>(6, c::log_chunk_bytes));
    CHECK(std::all_of(chunks.begin(), chunks.end(), [](size_t size) { return size <= c::log_chunk_bytes; }));
    CHECK(largest_live == 1 && pulled == pulled_members);
    CHECK(!socket.shut());
    stop(worker);
}

// A failed send while the local log streams shuts the connection without
// pulling any member log.
void http_failure() {
    start();
    write_log(std::string(10000, 'L'));
    static unsigned sends = 0;
    observed::on_chunk = [](std::string_view) { return ++sends == 3 ? ESP_FAIL : ESP_OK; };
    Socket socket;
    amity_fake::HttpRequest download;
    download.socket = socket.ends[0];
    CHECK(system_services::download_logs(&download, local) == ESP_OK);
    std::thread worker = amity_fake::start_task(worker_task());
    drain();
    CHECK(!download.response->finished && sends == 3 && pulled.empty());
    CHECK(socket.shut() && system_services::active_socket < 0);
    stop(worker);
}

// A failed send while a member's log streams ends the download there.
void member_send_failure() {
    start();
    static unsigned sends = 0;
    observed::on_chunk = [](std::string_view) { return ++sends == 5 ? ESP_FAIL : ESP_OK; };
    Socket socket;
    amity_fake::HttpRequest download;
    download.socket = socket.ends[0];
    CHECK(system_services::download_logs(&download, local) == ESP_OK);
    std::thread worker = amity_fake::start_task(worker_task());
    drain();
    CHECK(!download.response->finished && sends == 5 && (pulled == std::vector<unsigned>{2}));
    CHECK(download.response->body ==
          section("Main Amity", "living-room", 1, boot) + "[No logs captured.]\n" + member_section(2) +
              std::string(c::log_chunk_bytes, 'A'));
    CHECK(socket.shut());
    stop(worker);
}

// A member downloads only its own log.
void member_download() {
    start();
    main_device = false;
    amity_fake::HttpRequest download;
    CHECK(system_services::download_logs(&download, local) == ESP_OK);
    std::thread worker = amity_fake::start_task(worker_task());
    drain();
    CHECK(download.response->finished &&
          download.response->header("Content-Disposition") == "attachment; filename=living-room.log");
    CHECK(download.response->body == section("Amity", "living-room", 1, boot) + "[No logs captured.]\n");
    CHECK(pulled.empty() && amity_seam::count("amity::consortium::runtime_peers") == 0);
    stop(worker);
}

// Without memory for the local snapshot, the download says so and still
// collects the members' logs.
void local_memory() {
    start();
    write_log("local\n");
    amity_seam::install<&heap_caps_get_free_size>([](uint32_t) { return size_t(0); });
    amity_fake::fail_allocations(1);
    amity_fake::HttpRequest download;
    CHECK(system_services::download_logs(&download, local) == ESP_OK);
    std::thread worker = amity_fake::start_task(worker_task());
    drain();
    CHECK(download.response->finished);
    CHECK(download.response->body == section("Main Amity", "living-room", 1, boot) +
                                         "[Local logs unavailable: insufficient memory.]\n" + member_logs());
    CHECK(pulled == pulled_members);
    stop(worker);
}

// A slow member keeps the one worker busy after the handler returned, and
// other downloads are refused. Stopping shuts the connection, refuses new
// downloads and waits for the worker to finish and delete itself.
void slow_member() {
    start();
    block_pull = true;
    Socket socket;
    amity_fake::HttpRequest slow;
    slow.socket = socket.ends[0];
    CHECK(system_services::download_logs(&slow, local) == ESP_OK);
    std::thread worker = amity_fake::start_task(worker_task());
    {
        std::unique_lock<std::mutex> lock(pull_mutex);
        pull_changed.wait(lock, [] { return pull_waiting; });
    }
    CHECK(!slow.response->finished);
    nested_download("A log download is already in progress.");
    CHECK(amity_fake::tasks().size() == 1);
    observed::on_complete = [] { nested_download("Administration is stopping."); };
    std::atomic<bool> stopped{false};
    std::thread stopper([&] {
        system_services::stop_log_downloads();
        stopped = true;
    });
    CHECK(socket.shut(true));
    CHECK(!stopped);
    nested_download("Administration is stopping.");
    {
        std::lock_guard<std::mutex> lock(pull_mutex);
        release_pull = true;
        pull_changed.notify_all();
    }
    stopper.join();
    worker.join();
    CHECK(stopped && amity_fake::tasks()[0].state == amity_fake::Task::State::deleted);
    CHECK((pulled == std::vector<unsigned>{2}) && !slow.response->finished);
    CHECK(amity_fake::queues().back().deleted && amity_fake::pending_async_requests() == 0);
    CHECK(amity_fake::live_allocations() == 0 && !system_services::download_task);
    system_services::stop_log_downloads();
    CHECK(!system_services::download_queue);
}

// Stopping while a refused download still completes its request waits for
// that request before it stops the worker.
void stop_during_refusal() {
    start();
    observed::failing_begins = 1;
    amity_fake::HttpRequest no_async;
    CHECK(system_services::download_logs(&no_async, local) == ESP_FAIL);
    static std::thread stopper;
    static std::atomic<bool> stopped{false};
    observed::on_complete = [] {
        stopper = std::thread([] {
            system_services::stop_log_downloads();
            stopped = true;
        });
        while (!system_services::stopping.load()) std::this_thread::yield();
        // The stop holds the lock until it waits.
        std::lock_guard<std::mutex> lock(system_services::download_mutex);
        CHECK(uxQueueMessagesWaiting(system_services::download_queue) == 0 && !stopped);
    };
    observed::failing_sends = 1;
    amity_fake::HttpRequest full;
    CHECK(system_services::download_logs(&full, local) == ESP_FAIL);
    CHECK(refused(full, "Cannot start log download."));
    std::thread worker = amity_fake::start_task(worker_task());
    stopper.join();
    worker.join();
    CHECK(stopped && amity_fake::tasks()[0].state == amity_fake::Task::State::deleted);
    CHECK(amity_fake::queues().back().deleted && amity_fake::pending_async_requests() == 0);
    nested_download("Administration is stopping.");
}

}  // namespace

int main(int argc, char **argv) {
    return amity_test::run(argc, argv,
                           {{"setup-failures", setup_failures},
                            {"snapshot", snapshot},
                            {"main-download", main_download},
                            {"http-failure", http_failure},
                            {"member-send-failure", member_send_failure},
                            {"member-download", member_download},
                            {"local-memory", local_memory},
                            {"slow-member", slow_member},
                            {"stop-during-refusal", stop_during_refusal}});
}
