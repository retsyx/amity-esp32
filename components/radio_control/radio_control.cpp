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
#include <platform/ConnectivityManager.h>
#include <algorithm>
#include <array>
#include <cstddef>
extern "C" {
#include "host/ble_gap.h"
#include "nimble/hci_common.h"
}
namespace amity::radio {
namespace {
using namespace consortium;
RadioArbiter arbiter;
SemaphoreHandle_t mutex=nullptr;
uint64_t remote_generation=0;
uint64_t matter_disabled_generation=0;
bool peripheral_tracking_failed=false;
std::array<uint32_t,CONFIG_BT_NIMBLE_MAX_CONNECTIONS> peripheral_connections{};
portMUX_TYPE peripheral_mux=portMUX_INITIALIZER_UNLOCKED;
class PeripheralLock {public:PeripheralLock(){portENTER_CRITICAL_SAFE(&peripheral_mux);}~PeripheralLock(){portEXIT_CRITICAL_SAFE(&peripheral_mux);}};
uint64_t now(){return esp_timer_get_time()/1000;}
class Lock {public:Lock(){xSemaphoreTake(mutex,portMAX_DELAY);}~Lock(){xSemaphoreGive(mutex);}};
bool peripheral_active(){PeripheralLock lock;return peripheral_tracking_failed||std::any_of(peripheral_connections.begin(),peripheral_connections.end(),[](uint32_t handle){return handle!=0;});}
void peripheral_uncertain(){PeripheralLock lock;peripheral_tracking_failed=true;}
void matter_hci_event(const uint8_t* buffer){
    // VHCI delivers an ordered controller event stream. Observe physical links
    // before dispatch: NimBLE's GAP connect callback waits for remote-info
    // exchanges, and can arrive after advertising has already stopped.
    // The dedicated ISR-safe critical section protects only the bounded handle
    // table. It never contains application locks, SDK calls or HCI dispatch.
    const uint8_t code=buffer[0],length=buffer[1];
    const uint8_t* data=buffer+sizeof(ble_hci_ev);
    if(code==BLE_HCI_EVCODE_COMMAND_COMPLETE){
        if(length>=sizeof(ble_hci_ev_command_complete)&&
           (data[1]|uint16_t(data[2])<<8)==BLE_HCI_OP(BLE_HCI_OGF_CTLR_BASEBAND,BLE_HCI_OCF_CB_RESET)&&!data[3]){
            PeripheralLock lock;
            peripheral_connections.fill(0);
            peripheral_tracking_failed=false;
        }
        return;
    }
    bool connected=false;
    uint16_t handle=0;
    if(code==BLE_HCI_EVCODE_LE_META){
        if(!length)return;
        size_t required=0;
        if(data[0]==BLE_HCI_LE_SUBEV_CONN_COMPLETE)required=sizeof(ble_hci_ev_le_subev_conn_complete);
        else if(data[0]==BLE_HCI_LE_SUBEV_ENH_CONN_COMPLETE)required=offsetof(ble_hci_ev_le_subev_enh_conn_complete,mca)+1;
        else if(data[0]==BLE_HCI_LE_SUBEV_ENH_CONN_COMPLETE_V2)required=offsetof(ble_hci_ev_le_subev_enh_conn_complete,mca)+1+sizeof(uint8_t)+sizeof(uint16_t);
        else return;
        if(length<2){peripheral_uncertain();return;}
        if(data[1])return;
        if(length<required||data[4]>BLE_HCI_LE_CONN_COMPLETE_ROLE_SLAVE){peripheral_uncertain();return;}
        if(data[4]==BLE_HCI_LE_CONN_COMPLETE_ROLE_MASTER)return;
        handle=data[2]|uint16_t(data[3])<<8;
        connected=true;
    }else if(code==BLE_HCI_EVCODE_DISCONN_CMP){
        if(length<sizeof(ble_hci_ev_disconn_cmp)){peripheral_uncertain();return;}
        if(data[0])return;
        handle=data[1]|uint16_t(data[2])<<8;
    }else return;
    if(handle>BLE_HCI_LE_CONN_HANDLE_MAX){peripheral_uncertain();return;}
    const uint32_t encoded=uint32_t(handle)+1;
    PeripheralLock lock;
    if(connected){
        for(const auto slot:peripheral_connections)if(slot==encoded)return;
        for(auto& slot:peripheral_connections)if(!slot){slot=encoded;return;}
        peripheral_tracking_failed=true;
    }else{
        for(auto& slot:peripheral_connections)if(slot==encoded)slot=0;
    }
}
void release_idle_matter(uint64_t generation){
    Lock lock;
    const auto current=arbiter.reservation();
    if(current.owner!=RadioMode::matter_induction||current.generation!=generation||matter_disabled_generation!=generation)return;
    // A committed disable protects the initial scheduled-advertisement gap.
    // Physical cleanup, rather than IP commissioning state, owns the radio.
    const bool active=peripheral_active();
    arbiter.matter_ble_active(active);
    if(!active)arbiter.release(generation,now());
}
bool remote_request(uint32_t seconds,void*) {
    if(!mutex)return false;
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
        if(advertising||peripheral_active())return ESP_ERR_TIMEOUT;
        return arbiter.cleanup_complete(generation,now())==RadioResult::granted?ESP_OK:ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}
void worker(void*) {
    for(;;){
        RadioReservation reservation;bool cancelling;
        {Lock lock;reservation=arbiter.reservation();}
        if(reservation.owner==RadioMode::matter_induction){
            release_idle_matter(reservation.generation);
            Lock lock;reservation=arbiter.reservation();
        }
        // Expiry revokes the workflow through its GAP owner first. A logical
        // deadline alone cannot prove that controller cleanup has completed.
        const bool consortium_owner=reservation.owner==RadioMode::consortium_candidate||reservation.owner==RadioMode::consortium_inviter;
        const bool expired=reservation.deadline_ms&&now()>=reservation.deadline_ms;
        // The SDK controls Matter advertising. A lease deadline does not
        // prove that its advertiser and physical connections have stopped.
        const bool may_expire=reservation.owner!=RadioMode::matter_induction&&(!consortium_owner||!expired||runtime_cancel_radio_workflows());
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
    {Lock lock;result=arbiter.external_matter_will_open(now(),900000,peripheral_active());reservation=arbiter.reservation();if(result==RadioResult::granted||result==RadioResult::cancelling)matter_disabled_generation=0;}
    if(result==RadioResult::granted)return ble_remote_pause_background(true)==ESP_OK;
    if(result==RadioResult::cancelling)return finish_cancellation(reservation.generation)==ESP_OK;
    return false;
}
void matter_advertising_disabled(){if(!mutex)return;Lock lock;const auto current=arbiter.reservation();if(current.owner==RadioMode::matter_induction)matter_disabled_generation=current.generation;}
esp_err_t reserve_consortium(bool inviter,uint64_t& generation){
    generation=0;
    if(!mutex)return ESP_ERR_INVALID_STATE;
    uint64_t handoff_generation=0;
    RadioReservation matter;
    {Lock lock;matter=arbiter.reservation();}
    if(matter.owner==RadioMode::matter_induction){
        // SDK advertising changes use CHIP -> radio ordering. Never close an
        // incoming physical connection just to take over its radio workflow.
        esp_matter::lock::ScopedChipStackLock chip_lock(portMAX_DELAY);
        const auto cleanup_matches=[&](const RadioReservation& current){
            return matter_disabled_generation==matter.generation&&current.generation>matter.generation&&current.generation-matter.generation==1&&
                (current.owner==RadioMode::transition||current.owner==RadioMode::operational);
        };
        bool disable=true;
        {
            Lock lock;
            const auto current=arbiter.reservation();
            if(peripheral_active())return ESP_ERR_INVALID_STATE;
            if(current.owner!=RadioMode::matter_induction||current.generation!=matter.generation){
                if(!cleanup_matches(current))return ESP_ERR_INVALID_STATE;
                disable=false;
                if(current.owner==RadioMode::transition)handoff_generation=current.generation;
            }
        }
        if(disable){
            if(chip::DeviceLayer::ConnectivityMgr().SetBLEAdvertisingEnabled(false)!=CHIP_NO_ERROR)return ESP_ERR_INVALID_STATE;
            Lock lock;
            const auto current=arbiter.reservation();
            if(matter_disabled_generation!=matter.generation||peripheral_active())return ESP_ERR_INVALID_STATE;
            if(current.owner==RadioMode::matter_induction&&current.generation==matter.generation){
                arbiter.matter_ble_active(false);
                if(arbiter.release(matter.generation,now())!=RadioResult::cancelling)return ESP_ERR_INVALID_STATE;
                handoff_generation=arbiter.reservation().generation;
            }else{
                // The worker may already have consumed this exact disable.
                if(!cleanup_matches(current))return ESP_ERR_INVALID_STATE;
                if(current.owner==RadioMode::transition)handoff_generation=current.generation;
            }
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
    const CHIP_ERROR result=__real__ZN4chip11DeviceLayer8Internal14BLEManagerImpl22_SetAdvertisingEnabledEb(self,enabled);
    if(!enabled&&result==CHIP_NO_ERROR)amity::radio::matter_advertising_disabled();
    return result;
}

// The pinned ESP32-S3 VHCI path invokes this before either queuing an ordinary
// host event or releasing a command ACK. Preserve its buffer and ownership.
extern "C" int __real_ble_hs_hci_rx_evt(uint8_t*,void*);
extern "C" int __wrap_ble_hs_hci_rx_evt(uint8_t* event,void* context){
    amity::radio::matter_hci_event(event);
    return __real_ble_hs_hci_rx_evt(event,context);
}
