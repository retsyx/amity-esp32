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
    Lock lock;
    if(arbiter.cancellation_pending())return arbiter.cleanup_complete(generation,now())==RadioResult::granted?ESP_OK:ESP_ERR_INVALID_STATE;
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
    if(fail_safe_armed())return ESP_ERR_INVALID_STATE;
    RadioResult result;
    {Lock lock;result=arbiter.request(inviter?RadioMode::consortium_inviter:RadioMode::consortium_candidate,now(),RadioArbiter::enrollment_ms,false);if(result==RadioResult::granted||result==RadioResult::cancelling)generation=arbiter.reservation().generation;}
    if(result!=RadioResult::granted&&result!=RadioResult::cancelling)return ESP_ERR_INVALID_STATE;
    // GAP cancellation is asynchronous. Do not advertise until the controller
    // has acknowledged it; this worker path has a bounded five-second budget.
    for(unsigned i=0;i<50;++i){const auto e=finish_cancellation(generation);if(e==ESP_OK)return ESP_OK;if(e!=ESP_ERR_TIMEOUT){release(generation);generation=0;return e;}vTaskDelay(pdMS_TO_TICKS(100));}
    release(generation);generation=0;return ESP_ERR_TIMEOUT;
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
