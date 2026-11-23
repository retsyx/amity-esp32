/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// system_services.cpp, included whole so the scenarios reach its log ring,
// its allocation-failure records and its console hook: allocation-failure
// diagnostics, console timing, firmware uploads, firmware confirmation and
// delayed reboots. It builds against the core fakes, the HTTP server fake and
// strict seams for everything else.
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include <cstddef>
#include <cstdint>

// Three SDK calls are observed where the included file makes them: clock
// reads and task names are counted on their way to the core fakes, and free
// sizes come from the scenario. None allocates, because the allocation-failure
// hook makes all three and must not allocate.
namespace observed {
int64_t clock_time();
char *task_name(TaskHandle_t task);
size_t free_size(uint32_t caps);
}  // namespace observed
#define esp_timer_get_time observed::clock_time
#define pcTaskGetName observed::task_name
#define heap_caps_get_free_size observed::free_size
#include "../components/system_services/system_services.cpp"
#undef heap_caps_get_free_size
#undef pcTaskGetName
#undef esp_timer_get_time

#include "core_fakes.hpp"
#include "httpd_fake.hpp"
#include "scenario_main.hpp"
#include "seams.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace updates = system_services::updates;

// The allocation-failure hook runs inside a failing allocation, so it must
// not allocate.
std::atomic<bool> allocations_forbidden{false};
void *operator new(std::size_t size) {
    if (allocations_forbidden.load()) amity_test::fail(__FILE__, __LINE__, "the allocation-failure hook allocated");
    if (void *pointer = std::malloc(size ? size : 1)) return pointer;
    throw std::bad_alloc();
}
void operator delete(void *pointer) noexcept { std::free(pointer); }
void operator delete(void *pointer, std::size_t) noexcept { std::free(pointer); }

namespace {

// Whether a task holds the log ring's lock.
bool ring_locked() { return system_services::lock && xSemaphoreGetMutexHolder(system_services::lock); }

}  // namespace

namespace observed {

// Clock reads, with what the log ring and the console had seen at each.
struct ClockRead {
    size_t ring_used;
    bool ring_locked;
    unsigned forwards;
};
std::atomic<unsigned> clock_reads{0}, console_forwards{0};
std::array<ClockRead, 8> reads{};
// Added to every reading; a console call can move the clock back with it.
std::atomic<int64_t> clock_shift_us{0};
int64_t clock_time() {
    const unsigned read = clock_reads.fetch_add(1);
    if (read < reads.size()) reads[read] = {system_services::used, ring_locked(), console_forwards.load()};
    return amity_fake::now_us() + clock_shift_us.load();
}

// The task's name, in a buffer a scenario may overwrite after a failure to
// show that the hook kept a copy rather than the pointer.
std::atomic<unsigned> task_name_queries{0};
char task_names[64]{};
char *task_name(TaskHandle_t task) {
    ++task_name_queries;
    std::snprintf(task_names, sizeof(task_names), "%s", pcTaskGetName(task));
    return task_names;
}

// The free sizes a scenario reports; a call without them fails the scenario.
std::atomic<unsigned> free_size_queries{0};
size_t (*free_sizes)(uint32_t caps) = nullptr;
size_t free_size(uint32_t caps) {
    ++free_size_queries;
    if (!free_sizes) amity_test::fail(__FILE__, __LINE__, "heap_caps_get_free_size was called without free sizes");
    return free_sizes(caps);
}

}  // namespace observed

namespace {

constexpr uint32_t internal_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;

int quiet(const char *, va_list) { return 0; }

// Starts log capture, which forwards console output to console. The boot id
// comes from the entropy words 1, 2, 3 and 4.
void start_capture(vprintf_like_t console = quiet) {
    amity_seam::install<&bootloader_random_enable>([] {});
    amity_seam::install<&bootloader_random_disable>([] {});
    amity_seam::install<&esp_fill_random>([](void *bytes, size_t size) {
        CHECK(size == 16);
        const uint32_t entropy[] = {1, 2, 3, 4};
        std::memcpy(bytes, entropy, sizeof(entropy));
    });
    amity_seam::install<&amity::network_diagnostics::record_console_call>([](uint32_t, uint32_t) {});
    esp_log_set_vprintf(console);
    system_services::start_log_capture();
}

// What the log has recorded, as "<level letter>:<tag> <message>".
std::vector<std::string> lines() {
    std::vector<std::string> out;
    for (const auto &line : amity_fake::log_lines())
        out.push_back(std::string(1, "NEWIDV"[line.level]) + ":" + line.tag + " " + line.message);
    return out;
}

// The free sizes of the allocation-failure scenarios, by capabilities.
size_t typical_free_size(uint32_t caps) {
    if (caps == (MALLOC_CAP_DMA | MALLOC_CAP_8BIT)) return 111;
    if (caps == MALLOC_CAP_DMA) return 512;
    if (caps == internal_caps) return 12345;
    return 987654;
}

// Fails one heap_caps allocation, which calls the registered hook, with
// allocations forbidden throughout.
void fail_allocation(size_t bytes, uint32_t caps) {
    amity_fake::fail_allocations(1);
    allocations_forbidden = true;
    void *pointer = heap_caps_malloc(bytes, caps);
    allocations_forbidden = false;
    CHECK(!pointer);
}

// Runs body in a task of this name.
void in_task(const char *name, void (*body)()) {
    static void (*run)() = nullptr;
    run = body;
    TaskHandle_t task = nullptr;
    CHECK(xTaskCreate(
              [](void *) {
                  run();
                  vTaskDelete(nullptr);
              },
              name, 4096, nullptr, 5, &task) == pdPASS);
    amity_fake::run_task(task);
}

// Allocation-failure diagnostics.

// Log capture creates the ring's lock, routes console output through the
// ring, keeps the earlier console as the forward, registers the
// allocation-failure hook and draws the boot id from the bootloader entropy
// source while it is enabled.
void log_capture_start() {
    start_capture();
    const std::vector<std::string> entropy{"bootloader_random_enable", "esp_fill_random", "bootloader_random_disable"};
    CHECK(amity_seam::calls() == entropy);
    CHECK(std::string(system_services::boot_id()) == "00000001000000020000000300000004");
    CHECK(system_services::lock && system_services::original == quiet);
    const vprintf_like_t sink = esp_log_set_vprintf(nullptr);
    esp_log_set_vprintf(sink);
    CHECK(sink == system_services::capture);
    ESP_LOGI("test", "hello %d", 7);
    CHECK(system_services::logs() == "I (0) test: hello 7\n");
    CHECK(system_services::log_size() == 20);
    observed::free_sizes = typical_free_size;
    fail_allocation(64, MALLOC_CAP_DMA);
    CHECK(system_services::allocation_failure_count == 1);
}

// Without a lock for the ring, console output keeps its sink and the log
// reads as empty.
void log_capture_without_lock() {
    amity_fake::fail_queue_creations(1);
    start_capture();
    CHECK(!system_services::lock && !system_services::original);
    const vprintf_like_t sink = esp_log_set_vprintf(nullptr);
    esp_log_set_vprintf(sink);
    CHECK(sink == quiet);
    CHECK(system_services::logs().empty() && system_services::log_size() == 0);
    CHECK(!system_services::log_snapshot());
    CHECK(amity_fake::live_allocations() == 0);
}

// The hook records the failure's size, capabilities, function, task and time
// and the matching, DMA and internal free sizes, without allocating, logging
// or walking the heap. A failure inside the hook is counted, not recorded.
// The next poll logs both, before the firmware is ready; the one after logs
// nothing.
void allocation_hook() {
    start_capture();
    observed::free_sizes = [](uint32_t caps) {
        static bool nested = true;
        if (nested) {
            nested = false;
            amity_fake::fail_allocations(1);
            CHECK(!heap_caps_malloc(99, MALLOC_CAP_DMA));
        }
        return typical_free_size(caps);
    };
    amity_fake::advance_ms(777);
    in_task("group_link", [] { fail_allocation(512, MALLOC_CAP_DMA | MALLOC_CAP_8BIT); });
    CHECK(amity_fake::log_lines().empty());
    CHECK(observed::free_size_queries == 3 && observed::task_name_queries == 1);
    CHECK(system_services::allocation_failure_count == 1 && system_services::allocation_failure_suppressed == 1);
    amity_fake::advance_ms(222);
    system_services::log_memory_if_due();
    const std::vector<std::string> expected{
        "E:memory allocation_failed at_ms=777 bytes=512 caps=0x0000000c fn=heap_caps_malloc task=group_link "
        "matched_free=111 dma_free=512 internal_free=12345",
        "E:memory allocation_failed suppressed=1 reason=concurrent_or_full"};
    CHECK(lines() == expected);
    CHECK(amity_seam::count("heap_caps_get_minimum_free_size") == 0);
    system_services::log_memory_if_due();
    CHECK(lines() == expected);
}

// Eight records fit; a ninth failure is counted. The poll logs the eight in
// order, then the count, and the records' slots are reused after it.
void allocation_capacity() {
    start_capture();
    observed::free_sizes = typical_free_size;
    for (unsigned n = 0; n < 9; ++n) fail_allocation(100 + n, MALLOC_CAP_DMA);
    CHECK(system_services::allocation_failure_count == 8 && system_services::allocation_failure_suppressed == 1);
    system_services::log_memory_if_due();
    std::vector<std::string> expected;
    for (unsigned n = 0; n < 8; ++n)
        expected.push_back("E:memory allocation_failed at_ms=0 bytes=" + std::to_string(100 + n) +
                           " caps=0x00000008 fn=heap_caps_malloc task=main matched_free=512 dma_free=512 "
                           "internal_free=12345");
    expected.push_back("E:memory allocation_failed suppressed=1 reason=concurrent_or_full");
    CHECK(lines() == expected);
    CHECK(system_services::allocation_failure_count == 0 && system_services::allocation_failure_suppressed == 0);
    amity_fake::advance_ms(5);
    fail_allocation(200, internal_caps);
    system_services::log_memory_if_due();
    expected.push_back("E:memory allocation_failed at_ms=5 bytes=200 caps=0x00000804 fn=heap_caps_malloc task=main "
                       "matched_free=12345 dma_free=512 internal_free=12345");
    CHECK(lines() == expected);
}

// Function names keep 31 characters and task names 15, copied when the
// failure happens. The largest values still fit in a log line.
void allocation_names() {
    start_capture();
    observed::free_sizes = [](uint32_t) { return size_t(UINT32_MAX); };
    in_task(std::string(63, 't').c_str(), [] {
        char name[80];
        std::memset(name, 'f', sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
        allocations_forbidden = true;
        system_services::capture_allocation_failure(UINT32_MAX, UINT32_MAX, name);
        allocations_forbidden = false;
        std::memset(name, 'x', sizeof(name) - 1);
        std::strcpy(observed::task_names, "changed");
    });
    CHECK(observed::task_name_queries == 1);
    system_services::log_memory_if_due();
    const std::vector<std::string> expected{"E:memory allocation_failed at_ms=0 bytes=4294967295 caps=0xffffffff fn=" +
                                            std::string(31, 'f') + " task=" + std::string(15, 't') +
                                            " matched_free=4294967295 dma_free=4294967295 internal_free=4294967295"};
    CHECK(lines() == expected);
    CHECK(amity_fake::log_lines()[0].message.size() < 384);
}

// A failure in an interrupt is recorded as the ISR's, without asking for a
// task name.
void allocation_in_interrupt() {
    start_capture();
    observed::free_sizes = typical_free_size;
    in_task("group_link", [] {
        amity_fake::in_isr(true);
        fail_allocation(48, MALLOC_CAP_DMA);
        amity_fake::in_isr(false);
    });
    CHECK(observed::task_name_queries == 0);
    system_services::log_memory_if_due();
    const std::vector<std::string> expected{"E:memory allocation_failed at_ms=0 bytes=48 caps=0x00000008 "
                                            "fn=heap_caps_malloc task=ISR matched_free=512 dma_free=512 "
                                            "internal_free=12345"};
    CHECK(lines() == expected);
}

esp_partition_t running_partition{};

// Once the firmware is confirmed, a poll logs the internal and PSRAM free and
// minimum sizes from two of each query, then again every ten minutes.
void memory_totals() {
    start_capture();
    observed::free_sizes = typical_free_size;
    amity_seam::install<&heap_caps_get_minimum_free_size>([](uint32_t caps) { return size_t(caps == internal_caps ? 3 : 4); });
    system_services::log_memory_if_due();
    CHECK(amity_fake::log_lines().empty() && observed::free_size_queries == 0);
    amity_seam::install<&esp_ota_get_running_partition>([] { return &running_partition; });
    amity_seam::install<&esp_ota_get_state_partition>([](const esp_partition_t *, esp_ota_img_states_t *state) {
        *state = ESP_OTA_IMG_VALID;
        return ESP_OK;
    });
    CHECK(system_services::confirm_firmware() == ESP_OK && system_services::firmware_ready());
    amity_fake::advance_ms(1204000);
    system_services::log_memory_if_due();
    std::vector<std::string> expected{"I:memory uptime=1204s bytes: internal free=12345 min=3; psram free=987654 min=4"};
    CHECK(lines() == expected);
    CHECK(observed::free_size_queries == 2 && amity_seam::count("heap_caps_get_minimum_free_size") == 2);
    amity_fake::advance_ms(599999);
    system_services::log_memory_if_due();
    CHECK(lines() == expected && observed::free_size_queries == 2);
    amity_fake::advance_ms(1);
    system_services::log_memory_if_due();
    expected.push_back("I:memory uptime=1804s bytes: internal free=12345 min=3; psram free=987654 min=4");
    CHECK(lines() == expected);
    CHECK(observed::free_size_queries == 4 && amity_seam::count("heap_caps_get_minimum_free_size") == 4);
}

// Console timing.

struct Console {
    int result = 17;
    int64_t delay_us = 0, shift_us = 0;
    char forwarded[2048]{};
};
Console console;
struct Report {
    uint32_t began, duration;
    bool operator==(const Report &other) const { return began == other.began && duration == other.duration; }
};
std::vector<Report> reports;

// The console after the ring: it must run with the ring unlocked, and it
// takes delay_us of scripted time or moves the clock by shift_us.
int output(const char *format, va_list arguments) {
    CHECK(!ring_locked());
    ++observed::console_forwards;
    const int size = std::vsnprintf(console.forwarded, sizeof(console.forwarded), format, arguments);
    CHECK(size >= 0 && size < int(sizeof(console.forwarded)));
    if (console.delay_us) amity_fake::advance_us(console.delay_us);
    observed::clock_shift_us += console.shift_us;
    return console.result;
}

void start_console() {
    start_capture(output);
    amity_seam::install<&amity::network_diagnostics::record_console_call>([](uint32_t began, uint32_t duration) {
        CHECK(!ring_locked() && observed::console_forwards > reports.size());
        reports.push_back({began, duration});
    });
}

__attribute__((format(printf, 1, 2))) int emit(const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    const int result = system_services::capture(format, arguments);
    va_end(arguments);
    return result;
}

// The ring gets a copy of the arguments, so the console gets them intact and
// returns its own result. Timing starts after the ring is written and
// unlocked and covers only the console call, which is reported once.
void console_forwarding() {
    start_console();
    amity_fake::advance_us(1'234'567);
    console.delay_us = 1'500'900;
    CHECK(emit("key %u owner=%s", 3u, "member") == 17);
    CHECK(std::string(console.forwarded) == "key 3 owner=member");
    CHECK(system_services::logs() == "key 3 owner=member");
    CHECK(observed::console_forwards == 1 && observed::clock_reads == 2);
    CHECK(observed::reads[0].ring_used == 18 && !observed::reads[0].ring_locked && observed::reads[0].forwards == 0);
    CHECK(observed::reads[1].forwards == 1);
    CHECK((reports == std::vector<Report>{{1234, 1500}}));
}

// A line longer than the ring's line buffer keeps 383 bytes in the ring and
// reaches the console whole.
void console_long_line() {
    start_console();
    console.delay_us = 23'456;
    const std::string text(500, 'x');
    CHECK(emit("%s %d", text.c_str(), 41) == 17);
    CHECK(std::string(console.forwarded) == text + " 41");
    CHECK(system_services::log_size() == 383 && system_services::logs() == text.substr(0, 383));
    CHECK((reports == std::vector<Report>{{0, 23}}));
}

// A console error is returned as is.
void console_error() {
    start_console();
    console.result = -7;
    console.delay_us = 100'000;
    CHECK(emit("console error %d", 19) == -7);
    CHECK((reports == std::vector<Report>{{0, 100}}));
}

// Without a console, only the ring is written: no clock reads and no report.
void console_without_sink() {
    start_console();
    system_services::original = nullptr;
    console.delay_us = 9'000'000;
    CHECK(emit("ring only %d", 27) == 12);
    CHECK(system_services::logs() == "ring only 27");
    CHECK(observed::console_forwards == 0 && observed::clock_reads == 0 && reports.empty());
}

// Milliseconds since boot are reported in 32 bits: near the limit and past it.
void console_clock_wrap() {
    start_console();
    console.delay_us = 16'700;
    amity_fake::advance_us((int64_t(UINT32_MAX) - 5) * 1000 + 500);
    CHECK(emit("wrapped") == 17);
    amity_fake::advance_us((int64_t(UINT32_MAX) + 1 + 20) * 1000 - amity_fake::now_us());
    CHECK(emit("wrapped again") == 17);
    CHECK((reports == std::vector<Report>{{UINT32_MAX - 5, 16}, {20, 16}}));
}

// A console call that ends earlier than it began is reported as taking no time.
void console_clock_anomaly() {
    start_console();
    amity_fake::advance_ms(40);
    console.shift_us = -1'000;
    CHECK(emit("clock anomaly") == 17);
    CHECK((reports == std::vector<Report>{{40, 0}}));
}

// A line that finds the ring locked by another task waits for the lock: the
// ring gets it only after the other task releases the lock, 5 ms later, and
// the console call is timed from then.
std::mutex handoff;
std::condition_variable handed;
bool held = false, release = false, released = false;
size_t used_at_release = SIZE_MAX;

void console_contention() {
    start_console();
    console.delay_us = 2'000;
    std::thread holder([] {
        CHECK(xSemaphoreTake(system_services::lock, portMAX_DELAY) == pdTRUE);
        std::unique_lock<std::mutex> lock(handoff);
        held = true;
        handed.notify_all();
        handed.wait(lock, [] { return release; });
        CHECK(xSemaphoreGive(system_services::lock) == pdTRUE);
        released = true;
        handed.notify_all();
    });
    {
        std::unique_lock<std::mutex> lock(handoff);
        handed.wait(lock, [] { return held; });
    }
    CHECK(ring_locked());
    // The waiting line advances the clock; at 5 ms the holder releases the lock.
    esp_timer_create_args_t args{};
    args.name = "release";
    args.callback = [](void *) {
        used_at_release = system_services::used;
        std::unique_lock<std::mutex> lock(handoff);
        release = true;
        handed.notify_all();
        handed.wait(lock, [] { return released; });
    };
    esp_timer_handle_t timer = nullptr;
    CHECK(esp_timer_create(&args, &timer) == ESP_OK && esp_timer_start_once(timer, 5'000) == ESP_OK);
    CHECK(emit("contended %d", 7) == 17);
    bool waited;
    {
        std::lock_guard<std::mutex> lock(handoff);
        waited = release;
        release = true;
        handed.notify_all();
    }
    holder.join();
    CHECK(waited && used_at_release == 0 && !ring_locked());
    CHECK(system_services::logs() == "contended 7" && std::string(console.forwarded) == "contended 7");
    CHECK((reports == std::vector<Report>{{5, 2}}));
    CHECK(esp_timer_delete(timer) == ESP_OK);
}

// Firmware uploads.

esp_partition_t target_partition{};

struct Upload {
    std::string native;
    std::string manifest = "canonical-exact-manifest";
    // The signature validation accepts, in hex, and the version it reports.
    std::string signature_hex, version = "0.1.61";
    unsigned validations = 0, erases = 0, aborts = 0, failures = 0, starts = 0, finishes = 0;
    std::string written;
    std::optional<updates::ReleaseManifest> staged;
    std::vector<uint64_t> reader_timeouts, progress;
    // The reader construction that fails, counting from 1; 0 for none.
    unsigned failing_reader = 0;
    struct Reader {
        httpd_req_t *request;
        int64_t deadline;
        size_t number;
    };
    std::map<const web::BodyReader *, Reader> readers;
    // web::error's message and status, for each error answered.
    std::vector<std::pair<std::string, std::string>> errors;
    esp_err_t begin_result = ESP_OK, peer_begin_result = ESP_OK;
    // The offset of a block the receiver rejects.
    uint64_t rejected_block = UINT64_MAX;
};
Upload upload;

// An application image of this version with the running image's identity.
std::string image(const char *version = "0.1.61", size_t size = 5000) {
    std::string bytes(size, 'x');
    esp_image_header_t header{};
    header.magic = ESP_IMAGE_HEADER_MAGIC;
    header.chip_id = ESP_CHIP_ID_ESP32S3;
    const esp_image_segment_header_t segment{};
    esp_app_desc_t app = amity_fake::app_description();
    std::memset(app.version, 0, sizeof(app.version));
    std::memcpy(app.version, version, std::min(std::strlen(version), sizeof(app.version)));
    std::memcpy(bytes.data(), &header, sizeof(header));
    std::memcpy(bytes.data() + sizeof(header), &segment, sizeof(segment));
    std::memcpy(bytes.data() + sizeof(header) + sizeof(segment), &app, sizeof(app));
    return bytes;
}

// A bundle header declaring these lengths.
std::string bundle_header(uint64_t manifest, uint64_t signature, uint64_t image) {
    std::string out(updates::update_bundle_magic);
    const auto add = [&](uint64_t value, unsigned bytes) {
        for (unsigned n = bytes; n > 0; --n) out += char(value >> (8 * (n - 1)));
    };
    add(manifest, 4);
    add(signature, 4);
    add(image, 8);
    return out;
}

std::string bundle(const std::string &image_bytes, const std::string &signature = {}) {
    return bundle_header(upload.manifest.size(), signature.size(), image_bytes.size()) + upload.manifest + signature +
           image_bytes;
}

amity_fake::HttpRequest request(std::string file, size_t fragment = SIZE_MAX) {
    amity_fake::HttpRequest req(HTTP_POST, "/api/firmware", std::move(file));
    req.chunk = fragment;
    return req;
}

// Reads the body in fragments, as HTTPD does, ending with a failure at
// fail_at, and spending delay_us per receive once delay_from bytes are read.
void script(amity_fake::HttpRequest &req, size_t fragment, size_t fail_at, size_t delay_from = SIZE_MAX,
            int64_t delay_us = 0) {
    req.receive = [&req, fragment, fail_at, delay_from, delay_us](char *buffer, size_t length) {
        if (req.offset >= delay_from) amity_fake::advance_us(delay_us);
        if (req.offset >= req.body.size() || req.offset >= fail_at) return HTTPD_SOCK_ERR_FAIL;
        const size_t count = std::min({length, fragment, req.body.size() - req.offset, fail_at - req.offset});
        std::memcpy(buffer, req.body.data() + req.offset, count);
        req.offset += count;
        return int(count);
    };
}

size_t readers_alive() {
    return amity_seam::count("web::BodyReader::BodyReader") - amity_seam::count("web::BodyReader::~BodyReader");
}
bool readers_released() { return readers_alive() == 0; }

void reset_upload() {
    CHECK(readers_released());
    const std::string native = upload.native, manifest = upload.manifest;
    upload = Upload{};
    upload.native = native;
    upload.manifest = manifest;
}

// The running image, the update partition, readers with deadlines on the
// scripted clock, and the shared receiver: validation, cohort progress and
// staging. The receiver checks the application header before it erases the
// partition, then writes each block in order.
void install_upload() {
    auto &running = amity_fake::app_description();
    std::strcpy(running.version, "0.1.61");
    for (unsigned n = 0; n < 32; ++n) running.app_elf_sha256[n] = uint8_t(n + 1);
    target_partition.size = 0x3d0000;
    upload.native = image();
    amity_seam::install<&esp_ota_get_next_update_partition>([](const esp_partition_t *start) {
        CHECK(!start);
        return &target_partition;
    });
    amity_seam::install<&amity_seam::constructor<bool, web::BodyReader, httpd_req_t *, uint64_t>>(
        [](web::BodyReader &reader, httpd_req_t *request, uint64_t timeout_us) {
            upload.reader_timeouts.push_back(timeout_us);
            upload.readers[&reader] = {request, amity_fake::now_us() + int64_t(timeout_us),
                                       upload.reader_timeouts.size()};
            return upload.reader_timeouts.size() != upload.failing_reader;
        });
    // As the real reader: nothing at or after its deadline, including a
    // receive that ends past it. The first reader streams the image, when no
    // other reader may be left: on the device a metadata reader's deadline
    // would shut the connection.
    amity_seam::install<&web::BodyReader::receive>([](web::BodyReader &reader, char *buffer, size_t length) {
        const auto scope = upload.readers.at(&reader);
        if (scope.number == 1) CHECK(readers_alive() == 1);
        if (!reader.valid() || amity_fake::now_us() >= scope.deadline) return int(HTTPD_SOCK_ERR_TIMEOUT);
        const int received = httpd_req_recv(scope.request, buffer, length);
        return amity_fake::now_us() >= scope.deadline ? int(HTTPD_SOCK_ERR_TIMEOUT) : received;
    });
    amity_seam::install<&web::error>([](httpd_req_t *, const char *message, const char *status) {
        upload.errors.emplace_back(message, status);
        return ESP_FAIL;
    });
    amity_seam::install<&updates::validate_release>(
        [](std::string_view manifest, std::string_view signature_hex, updates::ReleaseManifest &out, std::string &error) {
            ++upload.validations;
            CHECK(upload.erases == 0);
            if (manifest != upload.manifest || signature_hex != upload.signature_hex) {
                error = "Publisher admission rejected";
                return ESP_FAIL;
            }
            updates::ReleaseManifest release;
            release.release_id = "test-release";
            release.image_bytes = upload.native.size();
            release.firmware_version = upload.version;
            out = release;
            return ESP_OK;
        });
    amity_seam::install<&updates::begin_system_upload>([](const updates::ReleaseManifest &release, std::string &error) {
        CHECK(release.release_id == "test-release" && release.image_bytes == upload.native.size());
        ++upload.starts;
        if (upload.begin_result != ESP_OK) error = "An update is already running";
        return upload.begin_result;
    });
    amity_seam::install<&updates::system_upload_progress>([](uint64_t bytes) {
        CHECK(bytes == upload.written.size());
        upload.progress.push_back(bytes);
    });
    amity_seam::install<&updates::finish_system_upload>([](std::string &) {
        CHECK(upload.written == upload.native);
        ++upload.finishes;
        return ESP_OK;
    });
    amity_seam::install<&updates::fail_system_upload>([](const std::string &) { ++upload.failures; });
    amity_seam::install<&updates::peer_begin>(
        [](const updates::ReleaseManifest &release, std::string_view signature_hex, std::string &error) {
            CHECK(release.release_id == "test-release" && signature_hex == upload.signature_hex);
            if (upload.peer_begin_result != ESP_OK) {
                error = "Cannot open the update partition";
                return upload.peer_begin_result;
            }
            upload.staged = release;
            return ESP_OK;
        });
    amity_seam::install<&updates::peer_chunk>(
        [](std::string_view release, uint64_t offset, std::string_view bytes, std::string &error) {
            // Exact 2 KiB blocks in order; only the last may be shorter.
            CHECK(upload.staged && release == "test-release" && offset == upload.written.size());
            CHECK(bytes.size() == 2048 || (bytes.size() < 2048 && offset + bytes.size() == upload.native.size()));
            if (offset == upload.rejected_block) {
                error = "Firmware block was rejected";
                return ESP_FAIL;
            }
            if (!offset) {
                esp_image_header_t header;
                esp_app_desc_t app;
                CHECK(bytes.size() >= sizeof(header) + sizeof(esp_image_segment_header_t) + sizeof(app));
                std::memcpy(&header, bytes.data(), sizeof(header));
                std::memcpy(&app, bytes.data() + sizeof(header) + sizeof(esp_image_segment_header_t), sizeof(app));
                const auto &running = amity_fake::app_description();
                if (header.magic != ESP_IMAGE_HEADER_MAGIC || header.chip_id != ESP_CHIP_ID_ESP32S3 ||
                    app.magic_word != ESP_APP_DESC_MAGIC_WORD ||
                    std::memcmp(app.project_name, running.project_name, sizeof(app.project_name)) ||
                    !std::memchr(app.version, 0, sizeof(app.version)) || upload.staged->firmware_version != app.version ||
                    std::memcmp(app.app_elf_sha256, running.app_elf_sha256, sizeof(app.app_elf_sha256))) {
                    error = "Application header differs from release";
                    return ESP_FAIL;
                }
                ++upload.erases;
            }
            upload.written.append(bytes);
            return ESP_OK;
        });
    amity_seam::install<&updates::peer_stage>([](std::string_view release, std::string &error) {
        CHECK(release == "test-release");
        if (upload.written != upload.native) {
            error = "Image digest differs from release";
            return ESP_FAIL;
        }
        return ESP_OK;
    });
    amity_seam::install<&updates::peer_abort>([](const std::string &) {
        if (upload.erases) ++upload.aborts;
    });
}

// The upload ended with one closing error of this status, sent nothing
// itself and released its readers.
bool rejected(amity_fake::HttpRequest &req, const char *status = "400 Bad Request") {
    const size_t before = upload.errors.size();
    return system_services::upload_firmware(&req) == ESP_FAIL && upload.errors.size() == before + 1 &&
           upload.errors.back().second == status && !req.response->head_sent && readers_released();
}

bool staged(amity_fake::HttpRequest &req) {
    return system_services::upload_firmware(&req) == ESP_OK && upload.errors.empty() && req.response->finished &&
           req.response->body == "{\"staged\":true}" && req.response->type == "application/json" && readers_released();
}

// A bundle read in fragments of any size is validated once, before the
// partition is erased, then staged byte for byte through the shared receiver
// with cohort start, progress and finish. Nothing selects it for boot.
void bundle_upload() {
    install_upload();
    for (size_t fragment : {1, 17, 31, 32, 255, 2048}) {
        reset_upload();
        auto req = request(bundle(upload.native), fragment);
        CHECK(staged(req));
        CHECK(upload.validations == 1 && upload.erases == 1 && upload.written == upload.native);
        CHECK(upload.starts == 1 && upload.finishes == 1 && upload.failures == 0);
        CHECK((upload.progress == std::vector<uint64_t>{2048, 4096, 5000}));
        CHECK(req.offset == req.body.size());
    }
    CHECK(amity_fake::tasks().empty());
}

// A bare application image is not an update file.
void raw_image() {
    install_upload();
    auto req = request(upload.native, 13);
    CHECK(rejected(req));
    CHECK(upload.validations == 0 && upload.erases == 0);
}

// Bad lengths, metadata, signatures and application headers are rejected
// before the partition is erased.
void bundle_corruptions() {
    install_upload();
    const char *lengths = "Update bundle lengths exceed the supported profile";
    const char *admission = "Publisher admission rejected", *header = "Application header differs from release";
    const char *expected[] = {lengths, lengths, lengths, lengths, admission, admission, header, header, header,
                              lengths, lengths};
    for (unsigned kind = 0; kind < 11; ++kind) {
        reset_upload();
        std::string manifest = upload.manifest, image_bytes = upload.native, signature;
        uint64_t manifest_bytes = manifest.size(), signature_bytes = 0, image_size = image_bytes.size();
        switch (kind) {
        case 0: manifest_bytes = 0; break;
        case 1: manifest_bytes = 8193; break;
        case 2: signature_bytes = 63; break;
        case 3: image_size = UINT64_MAX; break;
        case 4: manifest[0] = '!'; break;
        case 5: signature_bytes = 64; signature = std::string(64, 'z'); break;
        case 6: image_bytes[0] = 0; break;
        case 7: image_bytes[48] = '9'; break;
        case 8: image_bytes[176] = '!'; break;
        case 9: image_size = 0; break;
        case 10: image_size = 0x3d0001; break;
        }
        auto req = request(bundle_header(manifest_bytes, signature_bytes, image_size) + manifest + signature + image_bytes);
        CHECK(rejected(req));
        CHECK(upload.errors.back().first == expected[kind]);
        CHECK(upload.erases == 0 && upload.written.empty() && upload.finishes == 0);
    }
    CHECK(amity_fake::tasks().empty());
}

// A file shorter or longer than its header declares is rejected before
// validation.
void bundle_length() {
    install_upload();
    for (int delta : {-1, 1}) {
        reset_upload();
        std::string file = bundle(upload.native);
        if (delta < 0)
            file.pop_back();
        else
            file += '!';
        auto req = request(file);
        CHECK(rejected(req));
        CHECK(upload.validations == 0 && upload.erases == 0);
    }
}

// A receive failure in the header or metadata ends the upload before any erase.
void receive_failures() {
    install_upload();
    for (size_t fail_at : {size_t(2), size_t(33), size_t(50)}) {
        reset_upload();
        auto req = request(bundle(upload.native));
        script(req, SIZE_MAX, fail_at);
        CHECK(rejected(req));
        CHECK(upload.erases == 0);
    }
}

// A signed bundle hands validation its signature in hex.
void signed_bundle() {
    install_upload();
    const std::string signature(64, 'z');
    for (size_t n = 0; n < signature.size(); ++n) upload.signature_hex += "7a";
    auto req = request(bundle(upload.native, signature), 7);
    CHECK(staged(req));
    CHECK(upload.written == upload.native && upload.erases == 1);
}

// An image whose digest differs from its release fails after the erase; the
// receiver aborts, the cohort fails and the running image stays.
void image_digest() {
    install_upload();
    std::string corrupt = upload.native;
    corrupt.back() = '!';
    auto req = request(bundle(corrupt));
    CHECK(rejected(req));
    CHECK(upload.erases == 1 && upload.aborts == 1 && upload.failures == 1 && upload.finishes == 0);
    CHECK(amity_fake::tasks().empty());
}

// The header admits 1 to 8192 manifest bytes and a missing or 64-byte signature.
void header_bounds() {
    install_upload();
    for (uint64_t length : {0ull, 1ull, 8192ull, 8193ull, 0xffffffffull}) {
        for (uint64_t signature : {0ull, 1ull, 64ull, 65ull}) {
            updates::UpdateBundleHeader parsed;
            std::string error;
            const auto bytes = bundle_header(length, signature, upload.native.size());
            const bool good = updates::parse_update_bundle_header(
                bytes, updates::update_bundle_header_bytes + length + signature + upload.native.size(), parsed, error);
            CHECK(good == (length >= 1 && length <= 8192 && (signature == 0 || signature == 64)));
        }
    }
}

// Metadata that trickles in is cut off by the 30-second metadata deadline,
// before any erase; the whole upload has a 120-second deadline.
void metadata_trickle() {
    install_upload();
    auto req = request(bundle(upload.native));
    script(req, 1, SIZE_MAX, 0, 2'000'000);
    CHECK(rejected(req));
    CHECK(upload.erases == 0 && upload.errors.back().first == "Incomplete firmware header");
    CHECK((upload.reader_timeouts == std::vector<uint64_t>{120'000'000, 30'000'000}));
}

// An image that stalls after the erase is cut off by the whole-upload
// deadline; the receiver aborts and the cohort fails.
void image_stall() {
    install_upload();
    auto req = request(bundle(upload.native));
    script(req, 32, SIZE_MAX, updates::update_bundle_header_bytes + upload.manifest.size() + 2048, 130'000'000);
    CHECK(rejected(req));
    CHECK(upload.errors.back().first == "Firmware upload was interrupted; current firmware retained");
    CHECK(upload.erases == 1 && upload.aborts == 1 && upload.failures == 1 && upload.finishes == 0);
    CHECK(amity_fake::tasks().empty());
}

// Without a reader for the upload or for its metadata, nothing is read.
void reader_unavailable() {
    install_upload();
    for (unsigned failing : {1u, 2u}) {
        reset_upload();
        upload.failing_reader = failing;
        auto req = request(bundle(upload.native));
        CHECK(rejected(req, "503 Service Unavailable"));
        CHECK(upload.erases == 0 && req.offset == 0);
    }
}

// An older or unrecognized version needs the running version confirmed in a
// header; a version field without an end is rejected.
void downgrade() {
    install_upload();
    for (const char *version : {"0.1.60", "beta"}) {
        reset_upload();
        upload.native = image(version);
        upload.version = version;
        auto plain = request(bundle(upload.native));
        CHECK(rejected(plain, "409 Conflict"));
        CHECK(upload.errors.back().first.rfind("Firmware downgrade requires explicit confirmation", 0) == 0);
        auto wrong = request(bundle(upload.native));
        wrong.headers["X-Amity-Confirm-Version"] = "0.1.59";
        CHECK(rejected(wrong, "409 Conflict"));
        CHECK(upload.starts == 0 && upload.erases == 0);
        auto confirmed = request(bundle(upload.native));
        confirmed.headers["X-Amity-Confirm-Version"] = "0.1.61";
        upload.errors.clear();
        CHECK(staged(confirmed));
        CHECK(upload.written == upload.native);
    }
    reset_upload();
    upload.native = image(std::string(32, '1').c_str());
    auto endless = request(bundle(upload.native));
    CHECK(rejected(endless));
    CHECK(upload.errors.back().first == "Invalid firmware version field" && upload.starts == 0);
}

// Sizes outside the partition and its metadata, a cohort that cannot start,
// a receiver that cannot begin or rejects a block, a release for another
// image and an image shorter than its header are rejected.
void upload_rejections() {
    install_upload();
    const std::string file = bundle(upload.native);
    amity_seam::install<&esp_ota_get_next_update_partition>([](const esp_partition_t *) -> const esp_partition_t * {
        return nullptr;
    });
    auto no_partition = request(file);
    CHECK(rejected(no_partition));
    CHECK(upload.errors.back().first == "Invalid firmware size");
    install_upload();
    reset_upload();
    constexpr size_t header_size = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t);
    const uint64_t largest = uint64_t(target_partition.size) + updates::update_bundle_header_bytes + 8192 + 64;
    for (uint64_t length : {uint64_t(header_size - 1), largest + 1}) {
        auto req = request(file);
        req.content_len = length;
        CHECK(rejected(req));
        CHECK(upload.errors.back().first == "Invalid firmware size");
    }
    CHECK(amity_seam::count("web::BodyReader::BodyReader") == 0);
    for (uint64_t length : {uint64_t(header_size), largest}) {
        auto req = request(file);
        req.content_len = length;
        CHECK(rejected(req));
        CHECK(upload.errors.back().first == "Update bundle is truncated or has trailing data");
    }
    reset_upload();
    upload.begin_result = ESP_ERR_INVALID_STATE;
    auto busy = request(file);
    CHECK(rejected(busy, "409 Conflict"));
    CHECK(upload.errors.back().first == "An update is already running");
    CHECK(amity_seam::count("system_services::updates::peer_begin") == 0);
    reset_upload();
    upload.peer_begin_result = ESP_FAIL;
    auto closed = request(file);
    CHECK(rejected(closed));
    CHECK(upload.errors.back().first == "Cannot open the update partition");
    CHECK(upload.failures == 1 && upload.erases == 0);
    reset_upload();
    upload.rejected_block = 2048;
    auto refused_block = request(file);
    CHECK(rejected(refused_block));
    CHECK(upload.errors.back().first == "Firmware block was rejected");
    CHECK(upload.erases == 1 && upload.aborts == 1 && upload.failures == 1 && upload.finishes == 0);
    reset_upload();
    const std::string native = upload.native;
    upload.native = image("0.1.61", 6000);
    auto other = request(bundle(native));
    CHECK(rejected(other));
    CHECK(upload.errors.back().first == "This is not a valid Amity update file.");
    reset_upload();
    upload.manifest = std::string(300, 'm');
    upload.native = std::string(100, 'x');
    auto short_image = request(bundle(upload.native));
    CHECK(rejected(short_image));
    CHECK(upload.errors.back().first == "Incomplete firmware header" && upload.starts == 0);
}

// A bundle made by tools/update_bundle.py: the header parser finds its image,
// and an upload stages exactly the manifest and image the tool wrote.
void python_bundle() {
    std::ifstream stream(AMITY_PYTHON_UPDATE_BUNDLE, std::ios::binary);
    static std::string file;
    file.assign(std::istreambuf_iterator<char>(stream), {});
    static updates::UpdateBundleHeader parsed;
    std::string error;
    CHECK(file.size() > updates::update_bundle_header_bytes);
    CHECK(updates::parse_update_bundle_header(std::string_view(file).substr(0, updates::update_bundle_header_bytes),
                                              file.size(), parsed, error));
    CHECK(static_cast<unsigned char>(file[parsed.image_offset()]) == ESP_IMAGE_HEADER_MAGIC);
    install_upload();
    amity_seam::install<&updates::validate_release>(
        [](std::string_view manifest, std::string_view signature_hex, updates::ReleaseManifest &out, std::string &) {
            CHECK(manifest == std::string_view(file).substr(updates::update_bundle_header_bytes, parsed.manifest_bytes));
            CHECK(signature_hex.empty());
            out.release_id = "test-release";
            out.image_bytes = parsed.image_bytes;
            return ESP_OK;
        });
    upload.native = file.substr(parsed.image_offset());
    amity_seam::install<&updates::peer_chunk>(
        [](std::string_view, uint64_t offset, std::string_view bytes, std::string &) {
            CHECK(offset == upload.written.size());
            upload.written.append(bytes);
            return ESP_OK;
        });
    auto req = request(file);
    req.headers["X-Amity-Confirm-Version"] = amity_fake::app_description().version;
    CHECK(staged(req));
    CHECK(upload.written == upload.native);
}

// Firmware confirmation and reboots.

// A first boot pending verification is marked valid before the firmware is
// ready; a failed mark leaves it unready and can be retried.
void confirm_pending_image() {
    static esp_err_t mark = ESP_FAIL;
    amity_seam::install<&esp_ota_get_running_partition>([] { return &running_partition; });
    amity_seam::install<&esp_ota_get_state_partition>([](const esp_partition_t *partition, esp_ota_img_states_t *state) {
        CHECK(partition == &running_partition);
        *state = ESP_OTA_IMG_PENDING_VERIFY;
        return ESP_OK;
    });
    amity_seam::install<&esp_ota_mark_app_valid_cancel_rollback>([] { return mark; });
    CHECK(system_services::confirm_firmware() == ESP_FAIL && !system_services::firmware_ready());
    mark = ESP_OK;
    CHECK(system_services::confirm_firmware() == ESP_OK && system_services::firmware_ready());
    CHECK(amity_seam::count("esp_ota_mark_app_valid_cancel_rollback") == 2);
}

// A confirmed image, an image whose state is unknown and a missing running
// partition need no mark.
void confirm_confirmed_image() {
    static esp_err_t query = ESP_OK;
    static const esp_partition_t *running = &running_partition;
    amity_seam::install<&esp_ota_get_running_partition>([] { return running; });
    amity_seam::install<&esp_ota_get_state_partition>([](const esp_partition_t *, esp_ota_img_states_t *state) {
        *state = ESP_OTA_IMG_VALID;
        return query;
    });
    CHECK(system_services::confirm_firmware() == ESP_OK && system_services::firmware_ready());
    query = ESP_ERR_NOT_FOUND;
    CHECK(system_services::confirm_firmware() == ESP_OK);
    running = nullptr;
    CHECK(system_services::confirm_firmware() == ESP_OK);
    CHECK(amity_seam::count("esp_ota_get_state_partition") == 2);
    CHECK(amity_seam::count("esp_ota_mark_app_valid_cancel_rollback") == 0);
}

// A reboot comes from its own task, by default 1.5 seconds later.
void reboot_later() {
    system_services::reboot_later();
    system_services::reboot_later(250);
    const auto tasks = amity_fake::tasks();
    CHECK(tasks.size() == 2);
    for (const auto &task : tasks) CHECK(task.name == "restart" && task.stack_bytes == 2048 && task.priority == 3);
    CHECK(reinterpret_cast<uintptr_t>(tasks[0].argument) == 1500 && reinterpret_cast<uintptr_t>(tasks[1].argument) == 250);
    for (const auto &[task, expected_ms] : {std::pair{tasks[0].handle, 1500}, std::pair{tasks[1].handle, 1750}}) {
        bool restarted = false;
        try {
            amity_fake::run_task(task);
        } catch (const amity_fake::Restart &) {
            restarted = true;
        }
        CHECK(restarted && amity_fake::now_us() == int64_t(expected_ms) * 1000);
    }
}

// The log ring keeps the newest log_capacity bytes, read back in order.
void log_ring() {
    CHECK(system_services::logs().empty() && system_services::log_size() == 0);
    start_capture();
    std::string written;
    for (unsigned n = 0; written.size() <= amity::consortium::log_capacity + 400; ++n) {
        const std::string line = std::to_string(n) + std::string(200, char('a' + n % 26)) + "\n";
        emit("%s", line.c_str());
        written += line;
    }
    CHECK(system_services::log_size() == amity::consortium::log_capacity);
    CHECK(system_services::logs() == written.substr(written.size() - amity::consortium::log_capacity));
}

}  // namespace

int main(int argc, char **argv) {
    return amity_test::run(argc, argv,
                           {{"log-capture-start", log_capture_start},
                            {"log-capture-without-lock", log_capture_without_lock},
                            {"allocation-hook", allocation_hook},
                            {"allocation-capacity", allocation_capacity},
                            {"allocation-names", allocation_names},
                            {"allocation-in-interrupt", allocation_in_interrupt},
                            {"memory-totals", memory_totals},
                            {"console-forwarding", console_forwarding},
                            {"console-long-line", console_long_line},
                            {"console-error", console_error},
                            {"console-without-sink", console_without_sink},
                            {"console-clock-wrap", console_clock_wrap},
                            {"console-clock-anomaly", console_clock_anomaly},
                            {"console-contention", console_contention},
                            {"bundle-upload", bundle_upload},
                            {"raw-image", raw_image},
                            {"bundle-corruptions", bundle_corruptions},
                            {"bundle-length", bundle_length},
                            {"receive-failures", receive_failures},
                            {"signed-bundle", signed_bundle},
                            {"image-digest", image_digest},
                            {"header-bounds", header_bounds},
                            {"metadata-trickle", metadata_trickle},
                            {"image-stall", image_stall},
                            {"reader-unavailable", reader_unavailable},
                            {"downgrade", downgrade},
                            {"upload-rejections", upload_rejections},
                            {"python-bundle", python_bundle},
                            {"confirm-pending-image", confirm_pending_image},
                            {"confirm-confirmed-image", confirm_confirmed_image},
                            {"reboot-later", reboot_later},
                            {"log-ring", log_ring}});
}
