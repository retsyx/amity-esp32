// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#include "network_diagnostics.hpp"
#include <atomic>
#include "esp_attr.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_private/wifi.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "sdkconfig.h"
#if defined(__XTENSA__) && CONFIG_STDATOMIC_S32C1I_SPIRAM_WORKAROUND
#include "xtensa/config/core-isa.h"
#endif

namespace amity::network_diagnostics {
namespace {
static_assert(sizeof(std::atomic<uint32_t>)==4&&alignof(std::atomic<uint32_t>)>=4);
static_assert(sizeof(std::atomic<int32_t>)==4&&alignof(std::atomic<int32_t>)>=4);
#if defined(__XTENSA__) && CONFIG_STDATOMIC_S32C1I_SPIRAM_WORKAROUND
static_assert(XCHAL_HAVE_S32C1I==1);
// The trait is false for arbitrary addresses under the SDK's PSRAM workaround.
// esp_stdatomic.h dispatches these explicitly DRAM_ATTR 32-bit objects to its
// hardware __atomic_s32c1i_* helpers before any external-RAM critical section.
// Their CAS may retry; they are lock-free, not wait-free. Verify final placement
// in the target ELF: this claim does not apply to atomics in PSRAM.
#else
static_assert(std::atomic<uint32_t>::is_always_lock_free);
static_assert(std::atomic<int32_t>::is_always_lock_free);
#endif
constexpr auto relaxed=std::memory_order_relaxed;
constexpr uint32_t start_event=1,connected_event=2,registered=1;
struct PathStats {
    std::atomic<uint32_t> attempts{0},ok{0},no_mem{0},other{0};
    std::atomic<uint32_t> last_call_ms{0},last_ok_ms{0},last_error_ms{0},last_duration_ms{0};
    std::atomic<int32_t> last_error_code{0};
};
DRAM_ATTR PathStats tx_stats,rx_stats;
DRAM_ATTR std::atomic<uint32_t> done_ok{0},done_fail{0},done_last{0},done_last_ok{0},done_last_fail{0};
DRAM_ATTR std::atomic<uint32_t> started{0},event_registered{0},pending{0},lifecycle{0},attempts{0};
DRAM_ATTR std::atomic<int32_t> event_error{ESP_ERR_INVALID_STATE},registration_error{ESP_ERR_INVALID_STATE};
DRAM_ATTR std::atomic<uint32_t> power_generation{UINT32_MAX};
DRAM_ATTR std::atomic<int32_t> power_mode{-1},power_error{ESP_ERR_INVALID_STATE};
DRAM_ATTR std::atomic<uint32_t> console_calls{0},console_elapsed{0},console_slow{0},console_last_began{0},console_last_duration{0};

uint32_t now_ms(){return static_cast<uint32_t>(esp_timer_get_time()/1000);}
uint32_t begin(PathStats& stats){
    const auto now=now_ms();stats.attempts.fetch_add(1,relaxed);stats.last_call_ms.store(now,relaxed);return now;
}
void finish(PathStats& stats,uint32_t began,esp_err_t result){
    const auto now=now_ms();stats.last_duration_ms.store(now-began,relaxed);
    if(result==ESP_OK){stats.ok.fetch_add(1,relaxed);stats.last_ok_ms.store(now,relaxed);}
    else{
        (result==ESP_ERR_NO_MEM?stats.no_mem:stats.other).fetch_add(1,relaxed);
        stats.last_error_ms.store(now,relaxed);
        stats.last_error_code.store(result,relaxed);
    }
}
PathSnapshot read(const PathStats& stats){
    return {stats.attempts.load(relaxed),stats.ok.load(relaxed),stats.no_mem.load(relaxed),stats.other.load(relaxed),
            stats.last_call_ms.load(relaxed),stats.last_ok_ms.load(relaxed),stats.last_error_ms.load(relaxed),stats.last_duration_ms.load(relaxed),stats.last_error_code.load(relaxed)};
}
void tx_done(uint8_t, uint8_t*, uint16_t*, bool success){
    // The SDK owns/reuses these buffers. Do not inspect or retain either pointer.
    const auto now=now_ms();done_last.store(now,relaxed);
    if(success){done_ok.fetch_add(1,relaxed);done_last_ok.store(now,relaxed);}
    else{done_fail.fetch_add(1,relaxed);done_last_fail.store(now,relaxed);}
}
void wifi_event(void*,esp_event_base_t,int32_t event,void*){
    if(event==WIFI_EVENT_STA_START){
        // The pinned setter can accept a callback after init but before start.
        // Re-establish it and read power saving after this actual start event.
        lifecycle.fetch_add(2,relaxed);lifecycle.fetch_and(~registered,relaxed);
        pending.fetch_or(start_event,relaxed);
    }
    else if(event==WIFI_EVENT_STA_CONNECTED)pending.fetch_or(connected_event,relaxed);
    else if(event==WIFI_EVENT_STA_STOP){
        // Change generation before clearing registration so an in-flight SDK
        // call cannot publish success after this driver instance has stopped.
        lifecycle.fetch_add(2,relaxed);lifecycle.fetch_and(~registered,relaxed);pending.store(0,relaxed);
    }
}
void register_tx_done(){
    auto generation=lifecycle.load(relaxed);
    if(generation&registered)return;
    attempts.fetch_add(1,relaxed);
    // Own the SDK's single replaceable TxDone callback slot. The pinned SDK
    // and application have no other registrant; there is no getter/chaining.
    // Any future user must coordinate ownership here rather than overwrite it.
    const auto error=esp_wifi_set_tx_done_cb(tx_done);registration_error.store(error,relaxed);
    if(error!=ESP_OK||!lifecycle.compare_exchange_strong(generation,generation|registered,relaxed))return;
    wifi_ps_type_t mode=WIFI_PS_NONE;
    const auto readback=esp_wifi_get_ps(&mode);
    power_mode.store(readback==ESP_OK?static_cast<int32_t>(mode):-1,relaxed);power_error.store(readback,relaxed);
    power_generation.store(generation|registered,std::memory_order_release);
}
} // namespace

void start(){
    if(started.exchange(1,relaxed))return;
    const auto error=esp_event_handler_register(WIFI_EVENT,ESP_EVENT_ANY_ID,wifi_event,nullptr);
    event_error.store(error,relaxed);
    if(error!=ESP_OK)return;
    event_registered.store(1,relaxed);
    register_tx_done();
}
Snapshot snapshot(){
    Snapshot result;result.tx=read(tx_stats);result.rx=read(rx_stats);
    result.tx_done_ok=done_ok.load(relaxed);result.tx_done_fail=done_fail.load(relaxed);result.tx_done_last_ms=done_last.load(relaxed);
    result.tx_done_last_ok_ms=done_last_ok.load(relaxed);result.tx_done_last_fail_ms=done_last_fail.load(relaxed);
    const auto generation=lifecycle.load(relaxed);result.tx_done_registered=generation&registered;
    result.registration_attempts=attempts.load(relaxed);result.registration_pending=pending.load(relaxed);
    result.registration_error=registration_error.load(relaxed);result.event_registered=event_registered.load(relaxed);result.event_error=event_error.load(relaxed);
    result.power_save_checked=result.tx_done_registered&&power_generation.load(std::memory_order_acquire)==generation;
    if(result.power_save_checked){result.power_save_mode=power_mode.load(relaxed);result.power_save_error=power_error.load(relaxed);}
    else result.power_save_error=ESP_ERR_INVALID_STATE;
    result.console_calls=console_calls.load(relaxed);result.console_elapsed_ms=console_elapsed.load(relaxed);result.console_slow_calls=console_slow.load(relaxed);
    result.console_last_slow_began_ms=console_last_began.load(relaxed);result.console_last_slow_duration_ms=console_last_duration.load(relaxed);
    result.now_ms=now_ms();return result;
}
void record_console_call(uint32_t began_ms,uint32_t duration_ms){
    console_calls.fetch_add(1,relaxed);console_elapsed.fetch_add(duration_ms,relaxed);
    if(duration_ms>=100){
        console_slow.fetch_add(1,relaxed);console_last_began.store(began_ms,relaxed);console_last_duration.store(duration_ms,relaxed);
    }
}
void poll(){
    // Start and this single background caller own SDK registration. A lifecycle
    // event arriving during startup stays pending; no started event is lost.
    const auto events=pending.exchange(0,relaxed);
    if((events&start_event)||((events&connected_event)&&registration_error.load(relaxed)==ESP_ERR_WIFI_NOT_STARTED))register_tx_done();
    const auto current=snapshot();
    static Snapshot previous;
    static bool reported=false;
    if(!reported||current.event_registered!=previous.event_registered||current.event_error!=previous.event_error||
       current.tx_done_registered!=previous.tx_done_registered||current.registration_attempts!=previous.registration_attempts||
       current.registration_error!=previous.registration_error||current.power_save_checked!=previous.power_save_checked||
       current.power_save_mode!=previous.power_save_mode||current.power_save_error!=previous.power_save_error){
        ESP_LOGI("network_diag","status event=%u event_err=%d tx_done=%u reg_err=%d attempts=%u ps_checked=%u ps_mode=%d ps_err=%d",
                 static_cast<unsigned>(current.event_registered),static_cast<int>(current.event_error),static_cast<unsigned>(current.tx_done_registered),
                 static_cast<int>(current.registration_error),static_cast<unsigned>(current.registration_attempts),static_cast<unsigned>(current.power_save_checked),
                 static_cast<int>(current.power_save_mode),static_cast<int>(current.power_save_error));
    }
    const auto tx_nomem=current.tx.no_mem-previous.tx.no_mem,tx_other=current.tx.other-previous.tx.other;
    const auto rx_nomem=current.rx.no_mem-previous.rx.no_mem,rx_other=current.rx.other-previous.rx.other;
    const auto failed=current.tx_done_fail-previous.tx_done_fail;
    if(tx_nomem||tx_other||rx_nomem||rx_other||failed)
        ESP_LOGW("network_diag","errors tx_nomem=%u tx_other=%u rx_nomem=%u rx_other=%u tx_done_fail=%u tx_at=%u rx_at=%u done_at=%u tx_rc=%d rx_rc=%d",
                 static_cast<unsigned>(tx_nomem),static_cast<unsigned>(tx_other),static_cast<unsigned>(rx_nomem),static_cast<unsigned>(rx_other),
                 static_cast<unsigned>(failed),static_cast<unsigned>(current.tx.last_error_ms),static_cast<unsigned>(current.rx.last_error_ms),static_cast<unsigned>(current.tx_done_last_fail_ms),
                 static_cast<int>(current.tx.last_error_code),static_cast<int>(current.rx.last_error_code));
    previous=current;reported=true;
}
} // namespace amity::network_diagnostics

extern "C" esp_err_t __real_esp_wifi_internal_tx(wifi_interface_t,void*,uint16_t);
extern "C" esp_err_t __wrap_esp_wifi_internal_tx(wifi_interface_t interface,void* buffer,uint16_t length){
    using namespace amity::network_diagnostics;
    const auto began=begin(tx_stats);const auto result=__real_esp_wifi_internal_tx(interface,buffer,length);finish(tx_stats,began,result);return result;
}
extern "C" esp_err_t __real_esp_netif_receive(esp_netif_t*,void*,size_t,void*);
extern "C" esp_err_t __wrap_esp_netif_receive(esp_netif_t* interface,void* buffer,size_t length,void* driver_buffer){
    using namespace amity::network_diagnostics;
    const auto began=begin(rx_stats);const auto result=__real_esp_netif_receive(interface,buffer,length,driver_buffer);finish(rx_stats,began,result);return result;
}
