/* Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later */
#include "cec/bus.hpp"
#include "cec/scheduler.hpp"
#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
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
#include <new>

static_assert(SOC_GPTIMER_SUPPORTED && TIMER_LL_GPTIMERS_TOTAL >= 1);
static_assert(SOC_GPIO_PIN_COUNT <= 64);
#ifdef CONFIG_IDF_TARGET_ESP32S3
static_assert(TIMER_LL_GPTIMERS_TOTAL == 4, "Review target timer capabilities");
#endif

namespace cec {
constexpr unsigned nominal_tx_capacity = 8, maximum_tx_capacity = 9;
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
        SemaphoreHandle_t completed = nullptr;
        unsigned attempts = 3;
        ExecutionFence fence{};
        TxResult *outcome = nullptr;
        uint32_t operation_token = 0;
        uint8_t batch_slot = maximum_tx_capacity, batch_index = 0;
        bool requires_previous_start = false;
    };
    struct BatchState { bool used = false; uint8_t remaining = 0; bool started[maximum_tx_capacity]{}; };
    Engine engine;
    gpio_num_t gpio = GPIO_NUM_NC;
    uint64_t pin_mask = 0;
    QueueHandle_t rx = nullptr, tx = nullptr, result = nullptr;
    QueueHandle_t completions = nullptr;
    SemaphoreHandle_t completion_slots = nullptr;
    StaticQueue_t completions_control{};
    StaticSemaphore_t completion_slots_control{};
    uint8_t completion_storage[maximum_tx_capacity * sizeof(Completion)]{};
    SemaphoreHandle_t gate = nullptr;
    TaskHandle_t task = nullptr;
    StaticQueue_t rx_control{}, tx_control{}, result_control{};
    StaticSemaphore_t gate_control{};
    StaticSemaphore_t admission_control{};
    SemaphoreHandle_t admission = nullptr;
    StaticSemaphore_t space_available_control{};
    SemaphoreHandle_t space_available = nullptr;
    unsigned tx_in_use = 0;
    BatchState batches[maximum_tx_capacity]{}; // Dependency/completion metadata only, never frames.
    uint8_t* rx_storage = nullptr;
    uint8_t* tx_storage = nullptr;
    uint8_t result_storage[sizeof(TxResult)]{};
    Statistics stats;
    bool started = false;
    bool native = false, native_started = false;
    bool native_work_started = false;
    std::atomic<bool> native_active{false};
    Work native_work{};
    unsigned native_attempt = 0;
    uint64_t native_started_us = 0;
    TxResult transmit(const Frame &frame, unsigned attempts, ExecutionFence fence = {}, bool* started_out = nullptr);
    void record_queue_depth();
    void service_native();
    bool reserve_slots(size_t count, uint16_t overflow_mask);
    bool wait_reserve_slot(bool allow_overflow, uint32_t timeout_ms);
    void free_slots(size_t count);
    bool dependency_ready(const Work& work);
    static void complete_work(Impl& p, const Work& work, TxResult outcome, bool started);
    static void run(void *arg) {
        auto *p = static_cast<Impl *>(arg);
        Work work;
        // Separate workers keep arbitration/timeout on one wire from blocking
        // the other wires. Consolidating them needs separate qualification.
        for (;;) if (xQueueReceive(p->tx, &work, portMAX_DELAY) == pdTRUE) {
            bool started = false;
            const auto outcome = p->dependency_ready(work) ?
                p->transmit(work.frame, work.attempts, work.fence, &started) : TxResult::Cancelled;
            complete_work(*p, work, outcome, started);
        }
    }
    ~Impl();
};

// The singleton is internal .bss; every Impl is allocated in internal memory.
// Engines and queue storage never use PSRAM; worker stacks request INTERNAL
// explicitly and use the matching IDF deletion API for cross-core cleanup.
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
    TaskHandle_t native_task = nullptr;

    static void run_native(void* context) {
        auto& r = *static_cast<Runtime*>(context);
        for (;;) {
            // Lifecycle protects task-side port pointers; no wire wait occurs
            // while held. ISR owns its independent short critical section.
            xSemaphoreTake(r.lifecycle, portMAX_DELAY);
            for (auto* p : r.ports) if (p && p->started && p->native) p->service_native();
            xSemaphoreGive(r.lifecycle);
            vTaskDelay(1);
        }
    }
    esp_err_t start_native_worker() {
        if (native_task) return ESP_OK;
        return xTaskCreateWithCaps(run_native, "cec_native", 3072, this, 8, &native_task,
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
        bool native_remaining = false;
        for (auto* port : ports) if (port && port->native) native_remaining = true;
        portEXIT_CRITICAL(&lock);
        if (!native_remaining && native_task) { vTaskDeleteWithCaps(native_task); native_task = nullptr; }
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
        ReceiveObservation observation;
        if (p->engine.take_observation(observation)) {
            if (xQueueSendFromISR(p->rx, &observation, &wake) == pdTRUE) ++p->stats.received;
            else ++p->stats.rx_dropped;
        }
        const auto status = p->engine.take_result();
        if (status != TxResult::Pending) {
            // One result slot is reserved for each in-flight operation.
            xQueueSendFromISR(p->result, &status, &wake);
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
bool Bus::Impl::reserve_slots(size_t count, uint16_t overflow_mask) {
    auto& r = runtime();
    portENTER_CRITICAL(&r.lock);
    auto proposed = tx_in_use;
    for (size_t i = 0; i < count; ++i) {
        const auto limit = (overflow_mask & (uint16_t{1} << i)) ? maximum_tx_capacity : nominal_tx_capacity;
        if (proposed >= limit) { portEXIT_CRITICAL(&r.lock); return false; }
        ++proposed;
    }
    tx_in_use = proposed;
    portEXIT_CRITICAL(&r.lock);
    return true;
}
bool Bus::Impl::wait_reserve_slot(bool allow_overflow, uint32_t timeout_ms) {
    const auto start = uint64_t(esp_timer_get_time());
    const auto duration = uint64_t(timeout_ms) * 1000;
    for (;;) {
        if (reserve_slots(1, allow_overflow ? 1 : 0)) return true;
        if (allow_overflow || !timeout_ms) return false;
        const auto elapsed = uint64_t(esp_timer_get_time()) - start;
        if (elapsed >= duration) return false;
        const auto remaining_ms = static_cast<uint32_t>((duration - elapsed + 999) / 1000);
        const auto ticks = pdMS_TO_TICKS(remaining_ms);
        if (!ticks) return false;
        xSemaphoreTake(space_available, ticks);
    }
}
void Bus::Impl::free_slots(size_t count) {
    auto& r = runtime();
    portENTER_CRITICAL(&r.lock);
    if (count > tx_in_use) std::abort();
    tx_in_use -= count;
    portEXIT_CRITICAL(&r.lock);
    xSemaphoreGive(space_available);
}
bool Bus::Impl::dependency_ready(const Work& work) {
    if (!work.requires_previous_start) return true;
    auto& r = runtime();
    portENTER_CRITICAL(&r.lock);
    const bool ready = work.batch_slot < maximum_tx_capacity && work.batch_index > 0 &&
        batches[work.batch_slot].used && batches[work.batch_slot].started[work.batch_index - 1];
    portEXIT_CRITICAL(&r.lock);
    return ready;
}
void Bus::Impl::complete_work(Impl& p, const Work& work, TxResult outcome, bool started) {
    if (work.batch_slot < maximum_tx_capacity) {
        auto& r = runtime();
        portENTER_CRITICAL(&r.lock);
        auto& batch = p.batches[work.batch_slot];
        batch.started[work.batch_index] = started;
        if (!--batch.remaining) batch.used = false;
        portEXIT_CRITICAL(&r.lock);
    }
    if (work.outcome) *work.outcome = outcome;
    if (work.operation_token) {
        const Completion completion{work.operation_token, outcome, started};
        if (xQueueSend(p.completions, &completion, 0) != pdTRUE) std::abort();
    }
    p.free_slots(1);
    if (work.completed) xSemaphoreGive(work.completed);
}
Bus::Impl::~Impl() {
    if (task) vTaskDeleteWithCaps(task);
    if (gpio != GPIO_NUM_NC) runtime().detach(*this);
    if (rx) vQueueDelete(rx);
    if (tx) vQueueDelete(tx);
    if (result) vQueueDelete(result);
    if (completions) vQueueDelete(completions);
    if (completion_slots) vSemaphoreDelete(completion_slots);
    if (gate) vSemaphoreDelete(gate);
    if (admission) vSemaphoreDelete(admission);
    if (space_available) vSemaphoreDelete(space_available);
    heap_caps_free(rx_storage); heap_caps_free(tx_storage);
}
void Bus::Impl::service_native() {
    if (!native_active) {
        if (!uxQueueMessagesWaiting(tx) || xSemaphoreTake(gate, 0) != pdTRUE) return;
        if (xQueueReceive(tx, &native_work, 0) != pdTRUE) { xSemaphoreGive(gate); return; }
        native_active = true; native_started = false; native_attempt = 0; native_work_started = false;
    }
    auto& r = runtime(); const uint64_t now = esp_timer_get_time();
    TxResult outcome = TxResult::Pending;
    if (!native_attempt && !dependency_ready(native_work)) outcome = TxResult::Cancelled;
    if (native_started) {
        if (xQueueReceive(result, &outcome, 0) != pdTRUE) {
            if (now - native_started_us < 1600000) return;
            outcome = TxResult::Timeout;
        }
        portENTER_CRITICAL(&r.lock);
        native_work_started = native_work_started || engine.started();
        portEXIT_CRITICAL(&r.lock);
        native_started = false;
    }
    const bool terminal = outcome == TxResult::Success || outcome == TxResult::Cancelled || outcome == TxResult::Expired;
    if (!terminal && native_attempt < native_work.attempts) {
        const auto fence = native_work.fence;
        if (fence.expires_at_us && now >= fence.expires_at_us) outcome = TxResult::Expired;
        else if (!fence_current(fence)) outcome = TxResult::Cancelled;
        else {
            xQueueReset(result);
            portENTER_CRITICAL(&r.lock);
            const bool begun = engine.begin(native_work.frame, now, native_attempt > 0, fence);
            if (begun && native_attempt) ++stats.retries;
            portEXIT_CRITICAL(&r.lock);
            if (begun) { ++native_attempt; native_started = true; native_started_us = now; return; }
            outcome = !fence_current(fence) ? TxResult::Cancelled :
                fence.expires_at_us && uint64_t(esp_timer_get_time()) >= fence.expires_at_us ? TxResult::Expired : TxResult::LineError;
        }
    }
    portENTER_CRITICAL(&r.lock);
    if (outcome == TxResult::Success) ++stats.transmitted;
    else if (outcome == TxResult::Cancelled || outcome == TxResult::Expired) ++stats.cancelled;
    else if (!(native_work.frame.size == 1 && outcome == TxResult::Nack)) ++stats.tx_errors;
    portEXIT_CRITICAL(&r.lock);
    complete_work(*this, native_work, outcome, native_work_started);
    native_active = false; xSemaphoreGive(gate);
}
TxResult Bus::Impl::transmit(const Frame &frame, unsigned attempts, ExecutionFence fence, bool* started_out) {
    if (started_out) *started_out = false;
    if (!started || !attempts || !frame.size || frame.size > 16) return TxResult::LineError;
    if (xSemaphoreTake(gate, pdMS_TO_TICKS(5000)) != pdTRUE) return TxResult::Timeout;
    auto &r = runtime();
    TxResult status = TxResult::Timeout;
    for (unsigned attempt = 0; attempt < attempts; ++attempt) {
        if (fence.expires_at_us && uint64_t(esp_timer_get_time()) >= fence.expires_at_us) { status = TxResult::Expired; break; }
        if (!fence_current(fence)) { status = TxResult::Cancelled; break; }
        xQueueReset(result);
        portENTER_CRITICAL(&r.lock);
        const bool begun = engine.begin(frame, esp_timer_get_time(), attempt > 0, fence);
        if (begun && attempt) ++stats.retries;
        portEXIT_CRITICAL(&r.lock);
        if (!begun) {
            if (!fence_current(fence)) status = TxResult::Cancelled;
            else if (fence.expires_at_us && uint64_t(esp_timer_get_time()) >= fence.expires_at_us) status = TxResult::Expired;
            break;
        }
        if (xQueueReceive(result, &status, pdMS_TO_TICKS(1600)) != pdTRUE) status = TxResult::Timeout;
        if (started_out) {
            portENTER_CRITICAL(&r.lock);
            *started_out = *started_out || engine.started();
            portEXIT_CRITICAL(&r.lock);
        }
        if (status == TxResult::Success || status == TxResult::Cancelled || status == TxResult::Expired) break;
    }
    portENTER_CRITICAL(&r.lock);
    if (status == TxResult::Success) ++stats.transmitted;
    else if (status == TxResult::Cancelled || status == TxResult::Expired) ++stats.cancelled;
    else if (!(frame.size == 1 && status == TxResult::Nack)) ++stats.tx_errors;
    portEXIT_CRITICAL(&r.lock);
    xSemaphoreGive(gate);
    return status;
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
esp_err_t Bus::start(int gpio, QueueProfile profile) {
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
    p.native = profile == QueueProfile::Native;
    gpio_set_level(p.gpio, 1);
    const unsigned rx_depth = p.native ? 16 : 32, tx_depth = maximum_tx_capacity;
    p.rx_storage = static_cast<uint8_t*>(heap_caps_malloc(rx_depth * sizeof(ReceiveObservation), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    p.tx_storage = static_cast<uint8_t*>(heap_caps_malloc(tx_depth * sizeof(Impl::Work), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!p.rx_storage || !p.tx_storage) { xSemaphoreGive(r.lifecycle); return ESP_ERR_NO_MEM; }
    p.rx = xQueueCreateStatic(rx_depth, sizeof(ReceiveObservation), p.rx_storage, &p.rx_control);
    p.tx = xQueueCreateStatic(tx_depth, sizeof(Impl::Work), p.tx_storage, &p.tx_control);
    p.result = xQueueCreateStatic(1, sizeof(TxResult), p.result_storage, &p.result_control);
    p.gate = xSemaphoreCreateMutexStatic(&p.gate_control);
    p.admission = xSemaphoreCreateMutexStatic(&p.admission_control);
    p.space_available = xSemaphoreCreateBinaryStatic(&p.space_available_control);
    p.completions = xQueueCreateStatic(maximum_tx_capacity, sizeof(Completion), p.completion_storage, &p.completions_control);
    p.completion_slots = xSemaphoreCreateCountingStatic(maximum_tx_capacity, maximum_tx_capacity, &p.completion_slots_control);
    if (!p.rx || !p.tx || !p.result || !p.gate || !p.admission || !p.space_available || !p.completions || !p.completion_slots) {
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
    if ((p.native ? r.start_native_worker() :
        xTaskCreateWithCaps(Impl::run, "cec_tx", 3072, &p, 8, &p.task,
                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM) != ESP_OK) {
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
bool Bus::send(const Frame &frame, uint32_t timeout_ms) {
    return send_guarded(frame, {}, timeout_ms);
}
static bool valid_fence_storage(ExecutionFence fence) {
    const auto valid = [](const std::atomic<uint32_t> *counter) {
        return !counter ||
            ((reinterpret_cast<uintptr_t>(counter) % alignof(std::atomic<uint32_t>)) == 0 &&
             esp_ptr_internal(counter) && esp_ptr_byte_accessible(counter));
    };
    return valid(fence.generation) && valid(fence.secondary_generation);
}
bool Bus::send_guarded(const Frame &frame, ExecutionFence fence, uint32_t timeout_ms, unsigned attempts, bool allow_overflow) {
    if (!impl_) return false;
    if (!valid_fence_storage(fence) || !fence_current(fence) || !attempts) return false;
    if (!impl_->started || !frame.size || frame.size > 16 || xSemaphoreTake(impl_->admission, allow_overflow ? 0 : pdMS_TO_TICKS(timeout_ms)) != pdTRUE) return false;
    const Impl::Work work{frame, nullptr, attempts, fence};
    const bool reserved = impl_->wait_reserve_slot(allow_overflow, timeout_ms);
    const bool queued = reserved && xQueueSend(impl_->tx, &work, 0) == pdTRUE;
    if (reserved && !queued) impl_->free_slots(1);
    xSemaphoreGive(impl_->admission);
    if (queued) impl_->record_queue_depth();
    return queued;
}
bool Bus::send_and_wait(const Frame &frame, uint32_t timeout_ms, unsigned attempts) {
    return send_and_wait_guarded(frame, {}, timeout_ms, attempts);
}
bool Bus::send_and_wait_guarded(const Frame &frame, ExecutionFence fence, uint32_t timeout_ms, unsigned attempts, TxResult* result, bool allow_overflow) {
    if (result) *result = TxResult::Pending;
    if (!impl_) return false;
    if (!impl_->started || !frame.size || frame.size > 16 || !attempts) return false;
    if (!valid_fence_storage(fence) || !fence_current(fence)) return false;
    StaticSemaphore_t completion_control{};
    const auto completed = xSemaphoreCreateBinaryStatic(&completion_control);
    if (!completed) return false;
    TxResult outcome = TxResult::Pending;
    const Impl::Work work{frame, completed, attempts, fence, &outcome};
    bool queued = false;
    if (xSemaphoreTake(impl_->admission, allow_overflow ? 0 : pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
        const bool reserved = impl_->wait_reserve_slot(allow_overflow, timeout_ms);
        queued = reserved && xQueueSend(impl_->tx, &work, 0) == pdTRUE;
        if (reserved && !queued) impl_->free_slots(1);
        xSemaphoreGive(impl_->admission);
    }
    // Keep stack-owned completion storage alive until admitted work finishes.
    if (queued) { impl_->record_queue_depth(); xSemaphoreTake(completed, portMAX_DELAY); }
    if (result) *result = outcome;
    vSemaphoreDelete(completed);
    return queued && outcome != TxResult::Cancelled && outcome != TxResult::Expired;
}
TxResult Bus::transmit(const Frame &frame, unsigned attempts) { return transmit_guarded(frame, {}, attempts); }
TxResult Bus::transmit_guarded(const Frame &frame, ExecutionFence fence, unsigned attempts) {
    if (!impl_ || !impl_->started || !frame.size || frame.size > 16 || !attempts || !valid_fence_storage(fence)) return TxResult::LineError;
    if (!fence_current(fence)) return TxResult::Cancelled;
    if (fence.expires_at_us && uint64_t(esp_timer_get_time()) >= fence.expires_at_us) return TxResult::Expired;
    TxResult outcome = TxResult::Pending;
    send_and_wait_guarded(frame, fence, 5000, attempts, &outcome);
    return outcome == TxResult::Pending ? TxResult::Timeout : outcome;
}
bool Bus::submit_guarded(const Frame &frame, ExecutionFence fence, uint32_t token, unsigned attempts, bool allow_overflow) {
    const Submission submission{frame, fence, token, attempts, false, allow_overflow};
    return submit_guarded_batch(&submission, 1);
}
bool Bus::submit_guarded_batch(const Submission* batch, size_t count) {
    if (!impl_ || !impl_->started || !batch || !count || count > maximum_tx_capacity) return false;
    for (size_t i = 0; i < count; ++i) {
        const auto& item = batch[i];
        if (!i && item.requires_previous_start) return false;
        if (!item.operation_token || !item.attempts || !item.frame.size || item.frame.size > 16 ||
            !valid_fence_storage(item.fence) || !fence_current(item.fence) ||
            (item.fence.expires_at_us && uint64_t(esp_timer_get_time()) >= item.fence.expires_at_us)) return false;
    }
    if (xSemaphoreTake(impl_->admission, 0) != pdTRUE) return false;
    uint16_t overflow_mask = 0;
    for (size_t i = 0; i < count; ++i) if (batch[i].allow_overflow) overflow_mask |= uint16_t{1} << i;
    if (!impl_->reserve_slots(count, overflow_mask)) { xSemaphoreGive(impl_->admission); return false; }
    size_t reserved = 0;
    while (reserved < count && xSemaphoreTake(impl_->completion_slots, 0) == pdTRUE) ++reserved;
    if (reserved != count) {
        while (reserved--) xSemaphoreGive(impl_->completion_slots);
        impl_->free_slots(count);
        xSemaphoreGive(impl_->admission); return false;
    }
    uint8_t batch_slot = maximum_tx_capacity;
    bool dependent = false;
    for (size_t i = 1; i < count; ++i) dependent = dependent || batch[i].requires_previous_start;
    if (dependent) {
        auto& r = Impl::runtime();
        portENTER_CRITICAL(&r.lock);
        for (uint8_t i = 0; i < maximum_tx_capacity; ++i) if (!impl_->batches[i].used) {
            batch_slot = i; impl_->batches[i] = {}; impl_->batches[i].used = true;
            impl_->batches[i].remaining = static_cast<uint8_t>(count); break;
        }
        portEXIT_CRITICAL(&r.lock);
        if (batch_slot == maximum_tx_capacity) {
            for (size_t i = 0; i < count; ++i) xSemaphoreGive(impl_->completion_slots);
            impl_->free_slots(count);
            xSemaphoreGive(impl_->admission); return false;
        }
    }
    // All producers use admission; consumers only make more space. Once the
    // entire batch and its completion slots fit, every FIFO insertion succeeds.
    for (size_t i = 0; i < count; ++i) {
        const auto& item = batch[i];
        const Impl::Work work{item.frame, nullptr, item.attempts, item.fence, nullptr, item.operation_token,
            batch_slot, static_cast<uint8_t>(i), item.requires_previous_start};
        if (xQueueSend(impl_->tx, &work, 0) != pdTRUE) std::abort();
    }
    xSemaphoreGive(impl_->admission);
    impl_->record_queue_depth();
    return true;
}
bool Bus::take_completion(Completion &completion) {
    if (!impl_) return false;
    if (!impl_->started || xQueueReceive(impl_->completions, &completion, 0) != pdTRUE) return false;
    xSemaphoreGive(impl_->completion_slots);
    return true;
}
bool Bus::receive(Frame &frame, uint32_t timeout_ms) {
    ReceiveObservation observation;
    if (!receive_observation(observation, timeout_ms)) return false;
    frame = observation.frame;
    return true;
}
bool Bus::receive_observation(ReceiveObservation &observation, uint32_t timeout_ms) {
    if (!impl_) return false;
    return impl_->started && xQueueReceive(impl_->rx, &observation, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
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
    const bool idle = !p.engine.busy();
    portEXIT_CRITICAL(&r.lock);
    return idle && !p.native_active && uxQueueMessagesWaiting(p.tx) == 0;
}
RuntimeStatistics Bus::runtime_statistics() {
    auto &r = Impl::runtime();
    portENTER_CRITICAL(&r.lock); auto stats = r.stats; portEXIT_CRITICAL(&r.lock); return stats;
}
}
