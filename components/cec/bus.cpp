/* Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later */
#include "cec/bus.hpp"
#include "cec/scheduler.hpp"
#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "hal/gpio_ll.h"
#include "hal/timer_ll.h"
#include "soc/gpio_struct.h"
#include "soc/soc_caps.h"
#include <cstdlib>
#include <cstdio>
#include <new>

static_assert(SOC_GPTIMER_SUPPORTED && TIMER_LL_GPTIMERS_TOTAL >= 1);
static_assert(SOC_GPIO_PIN_COUNT <= 64);
#ifdef CONFIG_IDF_TARGET_ESP32S3
static_assert(TIMER_LL_GPTIMERS_TOTAL == 4, "Review target timer capabilities");
#endif

namespace cec {
constexpr unsigned tx_capacity = 8;
struct Bus::Impl {
    struct Runtime;
    static Runtime &runtime();
    static void *operator new(size_t size, const std::nothrow_t&) noexcept {
        return heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    static void operator delete(void *memory) { heap_caps_free(memory); }
    static void operator delete(void *memory, const std::nothrow_t&) { heap_caps_free(memory); }
    struct Work {
        Frame frame;
        unsigned attempts = 3;
        ExecutionFence fence{};
        TxCompletion *completion = nullptr;
    };
    struct AttemptResult {
        TxResult outcome = TxResult::Pending;
        TxDiagnostic diagnostic{};
        bool started = false;
    };
    Engine engine;
    gpio_num_t gpio = GPIO_NUM_NC;
    uint64_t pin_mask = 0;
    QueueHandle_t rx = nullptr, tx = nullptr, result = nullptr;
    StaticQueue_t rx_control{}, tx_control{}, result_control{};
    StaticSemaphore_t admission_control{};
    SemaphoreHandle_t admission = nullptr;
    unsigned tx_in_use = 0;
    uint8_t* rx_storage = nullptr;
    uint8_t* tx_storage = nullptr;
    uint8_t result_storage[sizeof(AttemptResult)]{};
    Statistics stats;
    bool started = false;
    bool attempt_pending = false;
    bool work_started = false;
    std::atomic<bool> active{false};
    Work work{};
    unsigned attempt = 0;
    uint64_t attempt_started_us = 0;
    void record_queue_depth();
    void service();
    bool reserve_slots(size_t count);
    void log_fifo_full(const Frame&) const;
    void log_tx_failure(const Work&, TxResult outcome, unsigned attempts, bool started,
                        const TxDiagnostic&) const;
    bool enqueue(const Work&);
    void free_slots(size_t count);
    static void complete_work(Impl& p, const Work& work, TxResult outcome);
    ~Impl();
};

// The singleton is internal .bss; every Impl is allocated in internal memory.
// Engines and queue storage never use PSRAM; the shared worker requests an
// INTERNAL stack and uses the matching IDF deletion API for cleanup.
struct Bus::Impl::Runtime {
    EngineBank engines;
    Impl *ports[EngineBank::capacity]{};
    portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
    StaticSemaphore_t lifecycle_control{};
    SemaphoreHandle_t lifecycle = xSemaphoreCreateMutexStatic(&lifecycle_control);
    gptimer_handle_t timer = nullptr;
    bool enabled = false, running = false;
    RuntimeStatistics stats;
    uint64_t previous_tick = 0;
    TaskHandle_t task = nullptr;

    static void run(void* context) {
        auto& r = *static_cast<Runtime*>(context);
        for (;;) {
            // Lifecycle protects task-side port pointers; no wire wait occurs
            // while held. ISR owns its independent short critical section.
            xSemaphoreTake(r.lifecycle, portMAX_DELAY);
            for (auto* p : r.ports) if (p && p->started) p->service();
            xSemaphoreGive(r.lifecycle);
            vTaskDelay(1);
        }
    }
    esp_err_t start_worker() {
        if (task) return ESP_OK;
        return xTaskCreateWithCaps(run, "cec_tx", 3072, this, 8, &task,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
    }

    static bool IRAM_ATTR tick(gptimer_handle_t, const gptimer_alarm_event_data_t *, void *arg);
    esp_err_t start_timer() {
        if (running) return ESP_OK;
        gptimer_config_t config{};
        config.clk_src = GPTIMER_CLK_SRC_DEFAULT;
        config.direction = GPTIMER_COUNT_UP;
        config.resolution_hz = 1000000;
        config.intr_priority = 3;
        config.flags.intr_shared = true;
        esp_err_t err = gptimer_new_timer(&config, &timer);
        if (err != ESP_OK) return err;
        gptimer_event_callbacks_t callbacks{}; callbacks.on_alarm = tick;
        if ((err = gptimer_register_event_callbacks(timer, &callbacks, this)) != ESP_OK) return err;
        gptimer_alarm_config_t alarm{};
        alarm.alarm_count = 50; alarm.reload_count = 0; alarm.flags.auto_reload_on_alarm = true;
        if ((err = gptimer_set_alarm_action(timer, &alarm)) != ESP_OK) return err;
        if ((err = gptimer_enable(timer)) != ESP_OK) return err;
        enabled = true;
        if ((err = gptimer_start(timer)) != ESP_OK) return err;
        running = true;
        portENTER_CRITICAL(&lock); stats.timer_instances = 1; portEXIT_CRITICAL(&lock);
        return ESP_OK;
    }
    void stop_timer() {
        if (running) gptimer_stop(timer);
        if (enabled) gptimer_disable(timer);
        if (timer) gptimer_del_timer(timer);
        running = enabled = false;
        timer = nullptr;
        portENTER_CRITICAL(&lock);
        previous_tick = 0;
        stats.timer_instances = 0;
        portEXIT_CRITICAL(&lock);
    }
    // The caller holds lifecycle from pin reservation through GPIO setup.
    esp_err_t attach(Impl &p) {
        esp_err_t err = ESP_ERR_INVALID_STATE;
        portENTER_CRITICAL(&lock);
        const int slot = engines.attach(p.engine, static_cast<uint8_t>(p.gpio));
        if (slot >= 0) { ports[slot] = &p; ++stats.active_ports; }
        portEXIT_CRITICAL(&lock);
        if (slot >= 0) {
            err = start_timer();
            if (err != ESP_OK) {
                portENTER_CRITICAL(&lock);
                ports[slot] = nullptr;
                engines.detach(p.engine);
                --stats.active_ports;
                portEXIT_CRITICAL(&lock);
                stop_timer();
            }
        }
        return err;
    }
    void detach(Impl &p) {
        if (!lifecycle) return;
        xSemaphoreTake(lifecycle, portMAX_DELAY);
        portENTER_CRITICAL(&lock);
        engines.detach(p.engine);
        for (auto &port : ports) if (port == &p) { port = nullptr; --stats.active_ports; }
        const bool empty = stats.active_ports == 0;
        portEXIT_CRITICAL(&lock);
        if (empty && task) { vTaskDeleteWithCaps(task); task = nullptr; }
        gpio_set_level(p.gpio, 1);
        if (empty) stop_timer();
        xSemaphoreGive(lifecycle);
    }
};
bool Bus::Impl::Runtime::tick(gptimer_handle_t, const gptimer_alarm_event_data_t *, void *arg) {
    auto &r = *static_cast<Runtime *>(arg);
    BaseType_t wake = pdFALSE;
    const uint64_t now = esp_timer_get_time();
    portENTER_CRITICAL_ISR(&r.lock);
    const uint64_t levels = uint64_t{GPIO.in} | (uint64_t{GPIO.in1.val} << 32);
    const uint64_t low = r.engines.tick(now, levels);
    // Apply outputs only after every engine has consumed the bank snapshot.
    for (auto *p : r.ports) if (p)
        gpio_ll_set_level(&GPIO, p->gpio, (low & p->pin_mask) == 0);
    for (auto *p : r.ports) if (p) {
        Frame frame;
        if (p->engine.take_received(frame)) {
            if (xQueueSendFromISR(p->rx, &frame, &wake) == pdTRUE) ++p->stats.received;
            else ++p->stats.rx_dropped;
        }
        const auto status = p->engine.take_result();
        if (status != TxResult::Pending) {
            // The existing one-result slot owns a complete numeric snapshot.
            // A later begin() or tick cannot overwrite the task's evidence.
            AttemptResult completed{status, p->engine.diagnostic(), p->engine.started()};
            xQueueSendFromISR(p->result, &completed, &wake);
            if (status == TxResult::ArbitrationLost) ++p->stats.arbitration_losses;
            if (status == TxResult::LineError) ++p->stats.line_errors;
        }
    }
    ++r.stats.ticks;
    if (r.previous_tick) {
        const uint64_t gap = now - r.previous_tick;
        const uint32_t bounded = gap > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(gap);
        if (bounded > r.stats.max_tick_gap_us) r.stats.max_tick_gap_us = bounded;
        if (gap > 200) ++r.stats.late_ticks;
    }
    r.previous_tick = now;
    const uint32_t duration = static_cast<uint32_t>(esp_timer_get_time() - now);
    if (duration > r.stats.max_isr_us) r.stats.max_isr_us = duration;
    portEXIT_CRITICAL_ISR(&r.lock);
    return wake == pdTRUE;
}
Bus::Impl::Runtime &Bus::Impl::runtime() { static Runtime shared; return shared; }
bool Bus::Impl::reserve_slots(size_t count) {
    auto& r = runtime();
    portENTER_CRITICAL(&r.lock);
    if (count > tx_capacity - tx_in_use) { portEXIT_CRITICAL(&r.lock); return false; }
    tx_in_use += count;
    portEXIT_CRITICAL(&r.lock);
    return true;
}
static void format_frame(const Frame& frame, char (&bytes)[16 * 3]) {
    for (size_t i = 0; i < frame.size; ++i)
        std::snprintf(bytes + 3 * i, sizeof(bytes) - 3 * i, "%02x%s",
                      unsigned(frame.data[i]), i + 1 == frame.size ? "" : " ");
}
void Bus::Impl::log_fifo_full(const Frame& frame) const {
    char bytes[16 * 3]{};
    format_frame(frame, bytes);
    ESP_LOGW("cec", "TX dropped: FIFO full (GPIO %d, %u bytes: %s)",
             int(gpio), unsigned(frame.size), bytes);
}
void Bus::Impl::log_tx_failure(const Work& work, TxResult outcome, unsigned attempts, bool started,
                              const TxDiagnostic& diagnostic) const {
    // Poll failures are ordinary address discovery. Data frames retain the
    // mechanical result, without interpreting their opcode or changing work.
    if (work.frame.size <= 1 || outcome == TxResult::Success) return;
    const char* result = "Unknown";
    switch (outcome) {
    case TxResult::Pending: return;
    case TxResult::Success: return;
    case TxResult::Nack: result = "Nack"; break;
    case TxResult::ArbitrationLost: result = "ArbitrationLost"; break;
    case TxResult::LineError: result = "LineError"; break;
    case TxResult::Timeout: result = "Timeout"; break;
    case TxResult::Cancelled: result = "Cancelled"; break;
    case TxResult::Expired: result = "Expired"; break;
    }
    char bytes[16 * 3]{};
    format_frame(work.frame, bytes);
    if (outcome == TxResult::LineError) {
        const char* reason = "None";
        switch (diagnostic.reason) {
        case LineErrorReason::None: break;
        case LineErrorReason::TickGap: reason = "TickGap"; break;
        case LineErrorReason::LowLine: reason = "LowLine"; break;
        case LineErrorReason::BeginRejected: reason = "BeginRejected"; break;
        }
        const int slot = diagnostic.slot;
        ESP_LOGW("cec", "TX failed (GPIO %d, %u bytes: %s, result %s, attempts %u, started %s, reason %s, at_us %llu, gap_us %llu, elapsed_us %u, slot %d, byte %d, bit %d, within_us %u, line %s)",
                 int(gpio), unsigned(work.frame.size), bytes, result, attempts, started ? "true" : "false", reason,
                 static_cast<unsigned long long>(diagnostic.at_us),
                 static_cast<unsigned long long>(diagnostic.tick_gap_us), unsigned(diagnostic.elapsed_us),
                 slot, slot < 0 ? slot : slot / 10, slot < 0 ? slot : slot % 10,
                 unsigned(diagnostic.within_us), diagnostic.line_high ? "high" : "low");
        return;
    }
    ESP_LOGW("cec", "TX failed (GPIO %d, %u bytes: %s, result %s, attempts %u, started %s)",
             int(gpio), unsigned(work.frame.size), bytes, result, attempts, started ? "true" : "false");
}
bool Bus::Impl::enqueue(const Work& work) {
    if (!started || !work.frame.size || work.frame.size > 16 || !work.attempts) return false;
    if (xSemaphoreTake(admission, 0) != pdTRUE) return false;
    const bool reserved = reserve_slots(1);
    // Initialize before the work becomes visible on another core. A rejected
    // tracker is never retained, so its previous terminal value is preserved.
    const auto previous = work.completion ? work.completion->result.load(std::memory_order_relaxed) : TxResult::Pending;
    if (reserved && work.completion) work.completion->result.store(TxResult::Pending, std::memory_order_relaxed);
    const bool queued = reserved && xQueueSend(tx, &work, 0) == pdTRUE;
    if (reserved && !queued) {
        if (work.completion) work.completion->result.store(previous, std::memory_order_relaxed);
        free_slots(1);
    }
    xSemaphoreGive(admission);
    if (queued) record_queue_depth();
    else log_fifo_full(work.frame);
    return queued;
}
void Bus::Impl::free_slots(size_t count) {
    auto& r = runtime();
    portENTER_CRITICAL(&r.lock);
    if (count > tx_in_use) std::abort();
    tx_in_use -= count;
    portEXIT_CRITICAL(&r.lock);
}
void Bus::Impl::complete_work(Impl& p, const Work& work, TxResult outcome) {
    p.free_slots(1);
    if (work.completion) work.completion->result.store(outcome, std::memory_order_release);
}
Bus::Impl::~Impl() {
    if (gpio != GPIO_NUM_NC) runtime().detach(*this);
    if (rx) vQueueDelete(rx);
    if (tx) vQueueDelete(tx);
    if (result) vQueueDelete(result);
    if (admission) vSemaphoreDelete(admission);
    heap_caps_free(rx_storage); heap_caps_free(tx_storage);
}
void Bus::Impl::service() {
    // Finishing an operation immediately advances this wire's FIFO. The loop
    // never waits for a wire edge or an ACK, so other ports remain independent.
    for (unsigned completed=0;completed<tx_capacity;++completed) {
        if (!active) {
            if (xQueueReceive(tx, &work, 0) != pdTRUE) return;
            active = true; attempt_pending = false; attempt = 0; work_started = false;
        }
        auto& r = runtime(); const uint64_t now = esp_timer_get_time();
        TxResult outcome = TxResult::Pending;
        TxDiagnostic diagnostic{};
        if (attempt_pending) {
            AttemptResult completed;
            if (xQueueReceive(result, &completed, 0) != pdTRUE) {
                if (now - attempt_started_us < 1600000) return;
                outcome = TxResult::Timeout;
                portENTER_CRITICAL(&r.lock);
                work_started = work_started || engine.started();
                portEXIT_CRITICAL(&r.lock);
            } else {
                outcome = completed.outcome;
                diagnostic = completed.diagnostic;
                work_started = work_started || completed.started;
            }
            attempt_pending = false;
        }
        const bool terminal = outcome == TxResult::Success || outcome == TxResult::Cancelled || outcome == TxResult::Expired;
        if (!terminal && attempt < work.attempts) {
            const auto fence = work.fence;
            if (fence.expires_at_us && now >= fence.expires_at_us) outcome = TxResult::Expired;
            else if (!fence_current(fence)) outcome = TxResult::Cancelled;
            else {
                xQueueReset(result);
                portENTER_CRITICAL(&r.lock);
                const bool begun = engine.begin(work.frame, now, attempt > 0, fence);
                if (begun && attempt) ++stats.retries;
                portEXIT_CRITICAL(&r.lock);
                if (begun) { ++attempt; attempt_pending = true; attempt_started_us = now; return; }
                outcome = !fence_current(fence) ? TxResult::Cancelled :
                    fence.expires_at_us && uint64_t(esp_timer_get_time()) >= fence.expires_at_us ? TxResult::Expired : TxResult::LineError;
                if (outcome == TxResult::LineError) {
                    diagnostic = {};
                    diagnostic.reason = LineErrorReason::BeginRejected;
                    diagnostic.at_us = esp_timer_get_time();
                    const uint64_t levels = uint64_t{GPIO.in} | (uint64_t{GPIO.in1.val} << 32);
                    diagnostic.line_high = (levels & pin_mask) != 0;
                }
            }
        }
        portENTER_CRITICAL(&r.lock);
        if (outcome == TxResult::Success) ++stats.transmitted;
        else if (outcome == TxResult::Cancelled || outcome == TxResult::Expired) ++stats.cancelled;
        else if (!(work.frame.size == 1 && outcome == TxResult::Nack)) ++stats.tx_errors;
        portEXIT_CRITICAL(&r.lock);
        log_tx_failure(work, outcome, attempt, work_started, diagnostic);
        complete_work(*this, work, outcome);
        active = false;
    }
}
void Bus::Impl::record_queue_depth() {
    const auto depth = uxQueueMessagesWaiting(tx);
    auto &r = runtime();
    portENTER_CRITICAL(&r.lock);
    if (depth > stats.tx_queue_high_water) stats.tx_queue_high_water = depth;
    portEXIT_CRITICAL(&r.lock);
}
Bus::Bus() : impl_(new (std::nothrow) Impl) {}
Bus::~Bus() = default;
void Bus::stop() { impl_.reset(); }
esp_err_t Bus::start(int gpio) { return start(gpio, QueueProfile::Legacy); }
esp_err_t Bus::start(int gpio, QueueProfile) {
    if (!impl_) impl_.reset(new (std::nothrow) Impl);
    if (!impl_) return ESP_ERR_NO_MEM;
    auto &p = *impl_;
    if (p.gpio != GPIO_NUM_NC) return ESP_ERR_INVALID_STATE;
    if (!GPIO_IS_VALID_OUTPUT_GPIO(gpio)) return ESP_ERR_INVALID_ARG;
    auto &r = Impl::runtime();
    if (!r.lifecycle || xSemaphoreTake(r.lifecycle, portMAX_DELAY) != pdTRUE) return ESP_ERR_NO_MEM;
    bool occupied = false;
    portENTER_CRITICAL(&r.lock);
    for (auto *port : r.ports) if (port && port->gpio == gpio) occupied = true;
    portEXIT_CRITICAL(&r.lock);
    if (occupied) { xSemaphoreGive(r.lifecycle); return ESP_ERR_INVALID_STATE; }
    p.gpio = static_cast<gpio_num_t>(gpio);
    p.pin_mask = uint64_t{1} << gpio;
    gpio_set_level(p.gpio, 1);
    const unsigned rx_depth = 32, tx_depth = tx_capacity;
    p.rx_storage = static_cast<uint8_t*>(heap_caps_malloc(rx_depth * sizeof(Frame), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    p.tx_storage = static_cast<uint8_t*>(heap_caps_malloc(tx_depth * sizeof(Impl::Work), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!p.rx_storage || !p.tx_storage) { xSemaphoreGive(r.lifecycle); return ESP_ERR_NO_MEM; }
    p.rx = xQueueCreateStatic(rx_depth, sizeof(Frame), p.rx_storage, &p.rx_control);
    p.tx = xQueueCreateStatic(tx_depth, sizeof(Impl::Work), p.tx_storage, &p.tx_control);
    p.result = xQueueCreateStatic(1, sizeof(Impl::AttemptResult), p.result_storage, &p.result_control);
    p.admission = xSemaphoreCreateMutexStatic(&p.admission_control);
    if (!p.rx || !p.tx || !p.result || !p.admission) {
        xSemaphoreGive(r.lifecycle); return ESP_ERR_NO_MEM;
    }
    gpio_config_t pin{};
    pin.pin_bit_mask = 1ULL << gpio;
    pin.mode = GPIO_MODE_INPUT_OUTPUT_OD;
    pin.pull_up_en = GPIO_PULLUP_DISABLE; pin.pull_down_en = GPIO_PULLDOWN_DISABLE;
    esp_err_t err = gpio_config(&pin);
    if (err != ESP_OK) { xSemaphoreGive(r.lifecycle); return err; }
    if ((err = r.attach(p)) != ESP_OK) { xSemaphoreGive(r.lifecycle); return err; }
    p.started = true;
    if (r.start_worker() != ESP_OK) {
        p.started = false; xSemaphoreGive(r.lifecycle); r.detach(p); return ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(r.lifecycle);
    return ESP_OK;
}
void Bus::set_address(uint8_t address) {
    if (!impl_) return;
    auto &r = Impl::runtime();
    portENTER_CRITICAL(&r.lock); impl_->engine.set_address(address); portEXIT_CRITICAL(&r.lock);
}
bool Bus::install_receive_policy(ReceivePolicy policy) {
    if (!impl_) return false;
    auto &r = Impl::runtime();
    portENTER_CRITICAL(&r.lock);
    const bool valid = impl_->engine.install_receive_policy(policy);
    portEXIT_CRITICAL(&r.lock);
    return valid;
}
bool Bus::send(const Frame &frame) {
    return send_guarded(frame, {});
}
static bool valid_fence_storage(ExecutionFence fence) {
    const auto valid = [](const std::atomic<uint32_t> *counter) {
        return !counter ||
            ((reinterpret_cast<uintptr_t>(counter) % alignof(std::atomic<uint32_t>)) == 0 &&
             esp_ptr_internal(counter) && esp_ptr_byte_accessible(counter));
    };
    return valid(fence.generation) && valid(fence.secondary_generation) && valid(fence.tertiary_generation);
}
bool Bus::send_guarded(const Frame &frame, ExecutionFence fence, unsigned attempts) {
    if (!impl_ || !valid_fence_storage(fence) || !fence_current(fence) || !attempts) return false;
    return impl_->enqueue({frame, attempts, fence});
}
bool Bus::send_async_guarded(const Frame &frame, ExecutionFence fence, TxCompletion &completion, unsigned attempts) {
    if (!impl_ || !valid_fence_storage(fence) || !fence_current(fence) || !attempts) return false;
    return impl_->enqueue({frame, attempts, fence, &completion});
}
bool Bus::receive(Frame &frame, uint32_t timeout_ms) {
    if (!impl_) return false;
    return impl_->started && xQueueReceive(impl_->rx, &frame, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}
Statistics Bus::statistics() const {
    if (!impl_) return {};
    auto &r = Impl::runtime();
    portENTER_CRITICAL(&r.lock); auto stats = impl_->stats; portEXIT_CRITICAL(&r.lock); return stats;
}
bool Bus::idle() const {
    if (!impl_) return true;
    auto &p = *impl_;
    if (!p.started) return true;
    auto &r = Impl::runtime();
    portENTER_CRITICAL(&r.lock);
    const bool idle = !p.engine.busy() && !p.tx_in_use;
    portEXIT_CRITICAL(&r.lock);
    return idle && !p.active && uxQueueMessagesWaiting(p.tx) == 0;
}
RuntimeStatistics Bus::runtime_statistics() {
    auto &r = Impl::runtime();
    portENTER_CRITICAL(&r.lock); auto stats = r.stats; portEXIT_CRITICAL(&r.lock); return stats;
}
}
