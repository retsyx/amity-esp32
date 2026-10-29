// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#include "radio_control.hpp"
#include "ble_remote.h"
#include "amity/consortium/runtime.hpp"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <lib/core/CHIPError.h>
#include <esp_matter.h>
#include <app/server/Server.h>
#include <platform/ConnectivityManager.h>
extern "C" {
#include "host/ble_gap.h"
}
namespace amity::radio {
namespace {
using namespace consortium;
RadioArbiter arbiter;
SemaphoreHandle_t mutex=nullptr;
uint64_t remote_generation=0;
bool matter_active=false;
bool fail_safe_armed(){esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);return chip::Server::GetInstance().GetFailSafeContext().IsFailSafeArmed();}
uint64_t now(){return esp_timer_get_time()/1000;}
class Lock {public:Lock(){xSemaphoreTake(mutex,portMAX_DELAY);}~Lock(){xSemaphoreGive(mutex);}};
bool remote_request(uint32_t seconds,void*) {
    if(!mutex)return false;
    if(fail_safe_armed())return false;
    Lock lock;
    auto result=arbiter.request(RadioMode::remote_pairing,now(),seconds*1000,false);
    if(result!=RadioResult::granted)return false;
    remote_generation=arbiter.reservation().generation;return true;
}
void remote_done(void*){if(mutex){Lock lock;if(remote_generation){arbiter.release(remote_generation,now());remote_generation=0;}}}
esp_err_t finish_cancellation(uint64_t generation) {
    // This call runs outside the arbiter lock. The remote worker may be in a
    // completion callback, so no radio->remote->radio mutex cycle is possible.
    const auto cancelled=ble_remote_pause_background(true);
    if(cancelled!=ESP_OK)return cancelled;
    // A caller can exhaust its bounded handoff wait while the SDK is still
    // stopping advertising. The worker must prove that GAP cleanup too before
    // releasing the transition; query GAP outside the arbiter mutex.
    const bool advertising=ble_gap_adv_active();
    Lock lock;
    if(arbiter.cancellation_pending()){
        if(advertising)return ESP_ERR_TIMEOUT;
        return arbiter.cleanup_complete(generation,now())==RadioResult::granted?ESP_OK:ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}
void worker(void*) {
    for(;;){
        RadioReservation reservation;bool cancelling;
        {Lock lock;reservation=arbiter.reservation();}
        // Expiry revokes the workflow through its GAP owner first. A logical
        // deadline alone cannot prove that controller cleanup has completed.
        const bool consortium_owner=reservation.owner==RadioMode::consortium_candidate||reservation.owner==RadioMode::consortium_inviter;
        const bool expired=reservation.deadline_ms&&now()>=reservation.deadline_ms;
        const bool may_expire=!consortium_owner||!expired||runtime_cancel_radio_workflows();
        {Lock lock;
            if(may_expire&&arbiter.reservation().generation==reservation.generation)arbiter.expire(now());
            reservation=arbiter.reservation();cancelling=arbiter.cancellation_pending();
        }
        if(cancelling){
            finish_cancellation(reservation.generation);
            {Lock lock;reservation=arbiter.reservation();}
        }
        if(reservation.owner==RadioMode::operational)ble_remote_pause_background(false);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
}
esp_err_t start(){
    if(mutex)return ESP_ERR_INVALID_STATE;
    mutex=xSemaphoreCreateMutex();if(!mutex)return ESP_ERR_NO_MEM;
    ble_remote_set_radio_hooks(remote_request,remote_done,nullptr);
    runtime_radio_hooks({reserve_consortium,release});
    return xTaskCreate(worker,"radio_owner",3072,nullptr,4,nullptr)==pdPASS?ESP_OK:ESP_ERR_NO_MEM;
}
bool matter_will_advertise(){
    if(!mutex)return false;
    // Cancel through each workflow's owner before changing arbiter generation.
    // A callback can return busy while asynchronous controller cleanup finishes.
    if(!runtime_cancel_radio_workflows()||ble_remote_cancel_setup()!=ESP_OK)return false;
    RadioReservation reservation;RadioResult result;
    {Lock lock;result=arbiter.external_matter_will_open(now(),900000,matter_active);reservation=arbiter.reservation();}
    if(result==RadioResult::granted)return ble_remote_pause_background(true)==ESP_OK;
    if(result==RadioResult::cancelling)return finish_cancellation(reservation.generation)==ESP_OK;
    return false;
}
void matter_closed(){if(!mutex)return;Lock lock;auto r=arbiter.reservation();if(r.owner==RadioMode::matter_induction)arbiter.release(r.generation,now());}
void matter_connection(bool active){if(!mutex)return;Lock lock;matter_active=active;arbiter.matter_pase_active(active);}
RadioReservation status(){if(!mutex)return {};Lock lock;return arbiter.reservation();}
esp_err_t reserve_consortium(bool inviter,uint64_t& generation){
    generation=0;
    if(!mutex)return ESP_ERR_INVALID_STATE;
    uint64_t handoff_generation=0;
    {
        // SDK callbacks use CHIP -> radio ordering. Do not hold the radio
        // mutex while closing the SDK window or waiting for GAP cleanup.
        esp_matter::lock::ScopedChipStackLock chip_lock(portMAX_DELAY);
        auto& server=chip::Server::GetInstance();
        if(server.GetFailSafeContext().IsFailSafeArmed())return ESP_ERR_INVALID_STATE;
        RadioReservation matter;
        {
            Lock lock;
            matter=arbiter.reservation();
            if(matter.owner==RadioMode::matter_induction&&matter_active)return ESP_ERR_INVALID_STATE;
        }
        if(matter.owner==RadioMode::matter_induction){
            // The SDK connection count also covers a BLE connection whose
            // application-level Matter event has not reached us yet.
            if(chip::DeviceLayer::ConnectivityMgr().NumBLEConnections()!=0)return ESP_ERR_INVALID_STATE;
            auto& window=server.GetCommissioningWindowManager();
            if(window.IsCommissioningWindowOpen())window.CloseCommissioningWindow();
            Lock lock;
            const auto current=arbiter.reservation();
            if(current.owner!=RadioMode::matter_induction||current.generation!=matter.generation||matter_active)
                return ESP_ERR_INVALID_STATE;
            if(arbiter.release(matter.generation,now())!=RadioResult::cancelling)return ESP_ERR_INVALID_STATE;
            handoff_generation=arbiter.reservation().generation;
        }
    }
    // Share the original fifty-check/five-second cancellation budget between
    // the idle Matter handoff and the requested Amity radio workflow.
    unsigned checks=0;
    const auto wait_cleanup=[&](uint64_t expected,bool matter_handoff){
        while(checks++<50){
            {
                Lock lock;
                if(arbiter.reservation().generation!=expected)return ESP_ERR_INVALID_STATE;
            }
            const auto e=matter_handoff&&ble_gap_adv_active()?ESP_ERR_TIMEOUT:finish_cancellation(expected);
            if(e==ESP_OK)return ESP_OK;
            if(e!=ESP_ERR_TIMEOUT)return e;
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        return ESP_ERR_TIMEOUT;
    };
    if(handoff_generation){
        const auto e=wait_cleanup(handoff_generation,true);
        if(e!=ESP_OK){release(handoff_generation);return e;}
    }
    RadioResult result;
    {Lock lock;result=arbiter.request(inviter?RadioMode::consortium_inviter:RadioMode::consortium_candidate,now(),RadioArbiter::enrollment_ms,false);if(result==RadioResult::granted||result==RadioResult::cancelling)generation=arbiter.reservation().generation;}
    if(result!=RadioResult::granted&&result!=RadioResult::cancelling)return ESP_ERR_INVALID_STATE;
    const auto e=wait_cleanup(generation,false);
    if(e!=ESP_OK){release(generation);generation=0;}
    return e;
}
void release(uint64_t generation){if(!mutex)return;Lock lock;arbiter.release(generation,now());}
}

// The SDK calls this out-of-line method from CommissioningWindowManager. The
// linker wrapper gates even administrator-cluster requests synchronously,
// before _SetAdvertisingEnabled changes flags or schedules any GAP work.
extern "C" CHIP_ERROR __real__ZN4chip11DeviceLayer8Internal14BLEManagerImpl22_SetAdvertisingEnabledEb(void*,bool);
extern "C" CHIP_ERROR __wrap__ZN4chip11DeviceLayer8Internal14BLEManagerImpl22_SetAdvertisingEnabledEb(void* self,bool enabled){
    if(enabled&&!amity::radio::matter_will_advertise())return CHIP_ERROR_BUSY;
    return __real__ZN4chip11DeviceLayer8Internal14BLEManagerImpl22_SetAdvertisingEnabledEb(self,enabled);
}
