#!/usr/bin/env python3
"""Actual radio-owner handoff function plus real arbiter; SDK/GAP are seams."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
from consortium_gatt_callback_test import function

ROOT = Path(__file__).resolve().parents[1]
PREFIX = r'''
#include "amity/consortium/radio.hpp"
#include <cassert>
#include <algorithm>
#include <array>
#include <cstddef>
#include <map>
#include <vector>
#include <cstdint>
#include <functional>
#include <iostream>
#include <stdexcept>
using namespace amity::consortium;
using esp_err_t=int;using CHIP_ERROR=int;constexpr CHIP_ERROR CHIP_NO_ERROR=0,CHIP_ERROR_BUSY=19;
constexpr int ESP_OK=0,ESP_ERR_INVALID_STATE=1,ESP_ERR_TIMEOUT=2;
constexpr unsigned portMAX_DELAY=0xffffffff;
RadioArbiter arbiter;
void* mutex=reinterpret_cast<void*>(1);
// These IP-state markers deliberately have no accessors in the radio SDK seam.
bool fail_safe=false,fail_safe_busy=false,pase_session=false,window_open=false,advertising=false;
uint64_t matter_disabled_generation=0,remote_generation=0;
bool desired_advertising=false,sdk_advertising_call=false;int advertising_error=0;
bool peripheral_tracking_failed=false;
std::array<uint32_t,3> peripheral_connections{};
struct portMUX_TYPE {unsigned depth=0;};
portMUX_TYPE peripheral_mux{};
unsigned physical_depth=0,physical_entries=0;
void portENTER_CRITICAL_SAFE(portMUX_TYPE* mux){assert(mux==&peripheral_mux&&physical_depth==0&&mux->depth==0);++physical_depth;++physical_entries;++mux->depth;}
void portEXIT_CRITICAL_SAFE(portMUX_TYPE* mux){assert(mux==&peripheral_mux&&physical_depth==1&&mux->depth==1);--physical_depth;--mux->depth;}
// Public HCI wire types are an SDK seam; tests submit literal byte payloads.
constexpr uint8_t BLE_HCI_EVCODE_DISCONN_CMP=0x05,BLE_HCI_EVCODE_COMMAND_COMPLETE=0x0e,BLE_HCI_EVCODE_LE_META=0x3e;
constexpr uint8_t BLE_HCI_LE_SUBEV_CONN_COMPLETE=1,BLE_HCI_LE_SUBEV_ENH_CONN_COMPLETE=0x0a,BLE_HCI_LE_SUBEV_ENH_CONN_COMPLETE_V2=0x29;
constexpr uint8_t BLE_HCI_LE_CONN_COMPLETE_ROLE_MASTER=0,BLE_HCI_LE_CONN_COMPLETE_ROLE_SLAVE=1;
constexpr uint8_t BLE_GAP_ROLE_MASTER=0,BLE_GAP_ROLE_SLAVE=1;
constexpr uint16_t BLE_HCI_LE_CONN_HANDLE_MAX=0x0eff;
constexpr uint16_t BLE_HCI_OGF_CTLR_BASEBAND=3,BLE_HCI_OCF_CB_RESET=3;
#define BLE_HCI_OP(ogf,ocf) ((ocf)|((ogf)<<10))
struct __attribute__((packed)) ble_hci_ev {uint8_t opcode,length;};
struct __attribute__((packed)) ble_hci_ev_command_complete {uint8_t num_packets;uint16_t opcode;uint8_t status;};
struct __attribute__((packed)) ble_hci_ev_disconn_cmp {uint8_t status;uint16_t conn_handle;uint8_t reason;};
struct __attribute__((packed)) ble_hci_ev_le_subev_conn_complete {uint8_t subev_code,status;uint16_t conn_handle;uint8_t role,rest[14];};
struct __attribute__((packed)) ble_hci_ev_le_subev_enh_conn_complete {uint8_t subev_code,status;uint16_t conn_handle;uint8_t role,rest[25],mca;};
static_assert(sizeof(ble_hci_ev_le_subev_conn_complete)==19&&sizeof(ble_hci_ev_le_subev_enh_conn_complete)==31);
std::map<uint16_t,uint8_t> gap_roles;
std::function<void(uint8_t*,void*)> on_dispatch;
unsigned dispatches=0;
uint8_t* expected_event=nullptr;
void* expected_context=reinterpret_cast<void*>(0x1234);
extern unsigned radio_depth;
int __real_ble_hs_hci_rx_evt(uint8_t* buffer,void* context){assert(radio_depth==0&&physical_depth==0&&context==expected_context&&buffer==expected_event);++dispatches;if(on_dispatch)on_dispatch(buffer,context);return 73;}
unsigned chip_depth=0,radio_depth=0,disables=0,pauses=0,delays=0,resumes=0;
uint64_t clock_ms=0,advert_stop_at=0,cleanup_ready_at=0;
bool background_paused=false;
std::function<void()> on_disable,on_disabled,on_delay;
extern "C" CHIP_ERROR __wrap__ZN4chip11DeviceLayer8Internal14BLEManagerImpl22_SetAdvertisingEnabledEb(void*,bool);
extern "C" CHIP_ERROR __real__ZN4chip11DeviceLayer8Internal14BLEManagerImpl22_SetAdvertisingEnabledEb(void*,bool enabled){assert(chip_depth&&radio_depth==0&&physical_depth==0);if(advertising_error)return advertising_error;desired_advertising=enabled;if(!enabled){++disables;if(!advert_stop_at)advertising=false;if(on_disable)on_disable();}return CHIP_NO_ERROR;}
class Lock {public:Lock(){assert(physical_depth==0);++radio_depth;}~Lock(){--radio_depth;}};
uint64_t now(){return clock_ms;}
namespace esp_matter::lock {
class ScopedChipStackLock {public:explicit ScopedChipStackLock(unsigned){assert(radio_depth==0&&physical_depth==0);++chip_depth;}~ScopedChipStackLock(){--chip_depth;}};
}
namespace chip::DeviceLayer {
struct Connectivity {
 CHIP_ERROR SetBLEAdvertisingEnabled(bool enabled){const bool previous=sdk_advertising_call;sdk_advertising_call=true;const auto result=__wrap__ZN4chip11DeviceLayer8Internal14BLEManagerImpl22_SetAdvertisingEnabledEb(nullptr,enabled);sdk_advertising_call=previous;if(!enabled&&on_disabled)on_disabled();return result;}
};
Connectivity& ConnectivityMgr(){static Connectivity value;return value;}
}
bool ble_gap_adv_active(){assert(chip_depth==0||sdk_advertising_call);return advertising;}
esp_err_t ble_remote_pause_background(bool paused){assert(chip_depth==0||sdk_advertising_call);background_paused=paused;if(!paused){++resumes;return ESP_OK;}++pauses;return clock_ms<cleanup_ready_at?ESP_ERR_TIMEOUT:ESP_OK;}
bool runtime_cancel_radio_workflows(){return true;}
esp_err_t ble_remote_cancel_setup(){return ESP_OK;}
unsigned pdMS_TO_TICKS(unsigned value){return value;}
void vTaskDelay(unsigned ms){assert(chip_depth==0&&radio_depth==0);clock_ms+=ms;++delays;if(advert_stop_at&&clock_ms>=advert_stop_at)advertising=false;if(on_delay)on_delay();}
void release(uint64_t);
'''

SUITE = r'''
void check(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
void reset(){arbiter=RadioArbiter{};fail_safe=fail_safe_busy=pase_session=window_open=advertising=background_paused=false;matter_disabled_generation=remote_generation=0;desired_advertising=sdk_advertising_call=false;advertising_error=0;peripheral_tracking_failed=false;peripheral_connections.fill(0);check(physical_depth==0&&peripheral_mux.depth==0,"physical critical section leaked");physical_entries=0;gap_roles.clear();on_dispatch={};dispatches=0;expected_event=nullptr;chip_depth=radio_depth=disables=pauses=delays=resumes=0;clock_ms=advert_stop_at=cleanup_ready_at=0;on_disable={};on_disabled={};on_delay={};}
void hci_event(uint8_t code,std::vector<uint8_t> payload){
 std::vector<uint8_t> packet{code,static_cast<uint8_t>(payload.size())};packet.insert(packet.end(),payload.begin(),payload.end());
 const auto original=packet;const unsigned previous=dispatches;expected_event=packet.data();
 check(__wrap_ble_hs_hci_rx_evt(packet.data(),expected_context)==73,"wrapper changed dispatch return");
 check(packet==original&&dispatches==previous+1,"wrapper changed buffer/dispatch ownership");
}
void gap_connect(uint16_t handle,uint8_t role=BLE_GAP_ROLE_SLAVE,uint8_t status=0,uint8_t subevent=BLE_HCI_LE_SUBEV_CONN_COMPLETE){
 if(!status)gap_roles[handle]=role;
 std::vector<uint8_t> payload(subevent==BLE_HCI_LE_SUBEV_CONN_COMPLETE?19:subevent==BLE_HCI_LE_SUBEV_ENH_CONN_COMPLETE?31:34);
 payload[0]=subevent;payload[1]=status;payload[2]=handle&0xff;payload[3]=handle>>8;payload[4]=role;
 hci_event(BLE_HCI_EVCODE_LE_META,payload);
}
void gap_disconnect(uint16_t handle,uint8_t status=0){if(!status)gap_roles.erase(handle);hci_event(BLE_HCI_EVCODE_DISCONN_CMP,{status,static_cast<uint8_t>(handle),static_cast<uint8_t>(handle>>8),0x13});}
void controller_reset(uint8_t status=0){hci_event(BLE_HCI_EVCODE_COMMAND_COMPLETE,{1,3,0x0c,status});if(!status)gap_roles.clear();}
void disable_matter(){esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);check(chip::DeviceLayer::ConnectivityMgr().SetBLEAdvertisingEnabled(false)==CHIP_NO_ERROR,"could not commit Matter BLE disable");}
void worker_once(){struct StopWorker {};on_delay=[](){throw StopWorker{};};try{worker(nullptr);}catch(const StopWorker&){}on_delay={};}
void idle_matter(){check(arbiter.external_matter_will_open(now(),300000,false)==RadioResult::granted,"could not open idle Matter");window_open=advertising=desired_advertising=true;}
void idle_handoff(){
 for(bool inviter:{false,true}){
  reset();idle_matter();const auto previous=arbiter.reservation();advert_stop_at=300;uint64_t admitted=99;
  check(reserve_consortium(inviter,admitted)==ESP_OK,"idle Matter did not hand off");
  auto current=arbiter.reservation();check(disables==1&&window_open&&!advertising&&!desired_advertising,"Matter BLE was not disabled and drained independently of its IP window");
  check(current.owner==(inviter?RadioMode::consortium_inviter:RadioMode::consortium_candidate)&&current.generation==admitted&&admitted>previous.generation,"wrong requested radio owner");
  check(clock_ms==300&&delays==3,"handoff did not wait for asynchronous advertisement stop");
  disable_matter();check(arbiter.reservation().generation==admitted&&arbiter.reservation().owner==current.owner,"queued old Matter disable released new owner");
 }
}
void active_denied(){
 for(uint16_t count=1;count<=3;++count){
  reset();idle_matter();for(uint16_t handle=1;handle<=count;++handle)gap_connect(handle);
  const auto previous=arbiter.reservation();uint64_t admitted=99;
  check(reserve_consortium(false,admitted)==ESP_ERR_INVALID_STATE,"active Matter BLE was interrupted");
  check(admitted==0&&disables==0&&window_open&&advertising&&arbiter.reservation().generation==previous.generation,"denied handoff changed radio/window");
  check(!remote_request(20,nullptr),"remote scan interrupted physical Matter BLE");
 }
}
void other_busy(){
 check(arbiter.request(RadioMode::remote_pairing,now(),30000,false)==RadioResult::granted,"remote did not reserve");
 auto previous=arbiter.reservation();uint64_t admitted=99;
 check(reserve_consortium(true,admitted)==ESP_ERR_INVALID_STATE&&admitted==0&&disables==0,"busy unrelated owner was preempted");
 check(arbiter.reservation().generation==previous.generation&&arbiter.reservation().owner==RadioMode::remote_pairing,"busy owner state changed");
}
void replace_owner(){auto current=arbiter.reservation();arbiter.release(current.generation,now());current=arbiter.reservation();arbiter.cleanup_complete(current.generation,now());check(arbiter.request(RadioMode::remote_pairing,now(),30000,false)==RadioResult::granted,"replacement owner failed");}
void stale_disable(){
 idle_matter();on_disable=replace_owner;uint64_t admitted=99;
 check(reserve_consortium(false,admitted)==ESP_ERR_INVALID_STATE&&admitted==0,"stale disable reservation admitted");
 check(arbiter.reservation().owner==RadioMode::remote_pairing,"handoff released replacement generation");
}
void stale_cleanup(){
 idle_matter();advert_stop_at=300;on_delay=[](){on_delay={};replace_owner();};uint64_t admitted=99;
 check(reserve_consortium(false,admitted)==ESP_ERR_INVALID_STATE&&admitted==0,"stale cleanup reservation admitted");
 check(arbiter.reservation().owner==RadioMode::remote_pairing,"cleanup released replacement generation");
}
void bounded_timeout(){
 idle_matter();advert_stop_at=6000;uint64_t admitted=99;
 check(reserve_consortium(false,admitted)==ESP_ERR_TIMEOUT&&admitted==0,"stuck advertiser did not fail bounded handoff");
 check(delays==50&&clock_ms==5000&&arbiter.reservation().owner!=RadioMode::consortium_candidate,"cancellation budget changed or request bypassed cleanup");
}
void late_cleanup(){
 for(const bool finishing_release:{false,true}){
  reset();
  if(finishing_release){
   check(arbiter.request(RadioMode::remote_pairing,now(),30000,false)==RadioResult::granted,"could not reserve remote pairing");
   check(arbiter.release(arbiter.reservation().generation,now())==RadioResult::cancelling,"could not release remote pairing");
  }else{
   arbiter.remote_reconnect_running(true);
   check(arbiter.request(RadioMode::consortium_candidate,now(),30000,false)==RadioResult::cancelling,"could not begin setup handoff");
  }
  const auto cancelled=arbiter.reservation();cleanup_ready_at=6000;
  struct StopWorker {};
  on_delay=[&](){
   if(clock_ms<=cleanup_ready_at)check(arbiter.reservation().owner==RadioMode::transition,"unconfirmed cleanup released radio");
   if(clock_ms>cleanup_ready_at)throw StopWorker{};
  };
  try{worker(nullptr);}catch(const StopWorker&){}
  on_delay={};
  check(arbiter.reservation().owner==RadioMode::operational&&arbiter.reservation().deadline_ms==0,"late confirmed cleanup stranded radio or revived expired setup");
  check(arbiter.remote_may_reconnect()&&!background_paused&&resumes==1,"worker did not resume remote reconnect after late cleanup");
  check(!arbiter.accepts_callback(cancelled.generation),"expired setup callback regained authority");
  check(arbiter.cleanup_complete(cancelled.generation,now())==RadioResult::stale,"late duplicate cleanup was accepted");
  for(const auto requested:{RadioMode::remote_pairing,RadioMode::consortium_candidate,RadioMode::matter_induction}){
   check(arbiter.request(requested,now(),30000,false)==RadioResult::granted,"fresh setup remained blocked after cleanup");
   const auto fresh=arbiter.reservation();
   check(fresh.generation>cancelled.generation&&arbiter.cleanup_complete(cancelled.generation,now())==RadioResult::stale&&arbiter.reservation().generation==fresh.generation&&arbiter.reservation().owner==requested,"stale cleanup changed fresh setup");
   check(arbiter.release(fresh.generation,now())==RadioResult::cancelling,"fresh setup could not release");
   check(arbiter.cleanup_complete(arbiter.reservation().generation,now())==RadioResult::granted,"fresh cleanup could not complete");
  }
 }
}
void late_cleanup_result(){
 arbiter.remote_reconnect_running(true);
 check(arbiter.request(RadioMode::consortium_candidate,now(),30000,false)==RadioResult::cancelling,"could not begin setup handoff");
 const auto generation=arbiter.reservation().generation;clock_ms=RadioArbiter::cancel_ms;
 check(arbiter.cleanup_complete(generation,now())==RadioResult::timed_out,"expired handoff reported success");
 check(arbiter.reservation().owner==RadioMode::operational,"expired handoff did not consume confirmed cleanup");
}
void late_advertising_cleanup(){
 bounded_timeout();
 const auto cancelled=arbiter.reservation();
 check(advertising&&cancelled.owner==RadioMode::transition,"timed-out handoff lost pending advertiser cleanup");
 struct StopWorker {};
 on_delay=[&](){
  if(clock_ms<=advert_stop_at)check(arbiter.reservation().owner==RadioMode::transition&&background_paused,"worker released radio before advertiser stopped");
  if(clock_ms>advert_stop_at)throw StopWorker{};
 };
 try{worker(nullptr);}catch(const StopWorker&){}
 on_delay={};
 check(!advertising&&arbiter.reservation().owner==RadioMode::operational&&!background_paused,"worker did not release radio after advertiser stopped");
 check(!arbiter.accepts_callback(cancelled.generation),"timed-out advertiser handoff regained setup authority");
 uint64_t fresh=0;check(reserve_consortium(false,fresh)==ESP_OK&&fresh>cancelled.generation,"new admission could not start after confirmed advertiser cleanup");
}
void worker_cleanup_can_finish_while_handoff_disables(){
 for(bool completed:{false,true}){
  reset();idle_matter();const auto previous=arbiter.reservation();
  on_disabled=[&](){
   on_disabled={};release_idle_matter(previous.generation);
   check(arbiter.reservation().owner==RadioMode::transition,"worker did not consume committed disable");
   if(completed)check(arbiter.cleanup_complete(arbiter.reservation().generation,now())==RadioResult::granted,"worker cleanup failed");
  };
  uint64_t admitted=0;check(reserve_consortium(false,admitted)==ESP_OK&&admitted>previous.generation,"worker progress made a valid Matter handoff fail");
  check(arbiter.reservation().owner==RadioMode::consortium_candidate&&window_open,"handoff changed IP window or selected wrong owner");
 }
}
void ordinary(){uint64_t admitted=99;check(reserve_consortium(false,admitted)==ESP_OK&&admitted&&disables==0&&delays==0&&arbiter.reservation().owner==RadioMode::consortium_candidate,"ordinary existing admission changed");}
void disabled_before_disconnect(){
 idle_matter();gap_connect(1);
 window_open=advertising=false;disable_matter();
 check(arbiter.reservation().owner==RadioMode::matter_induction,"BLE disable released a live Matter connection");
 clock_ms=60;gap_disconnect(1);
 struct StopWorker {};
 on_delay=[](){throw StopWorker{};};
 try{worker(nullptr);}catch(const StopWorker&){}
 on_delay={};
 check(arbiter.reservation().owner==RadioMode::operational,"completed commissioning stranded the Matter radio reservation");
 check(arbiter.request(RadioMode::remote_pairing,now(),20000,false)==RadioResult::granted,"remote scan remains blocked after commissioning");
}
void disconnect_before_disable(){
 idle_matter();gap_connect(1);gap_disconnect(1);worker_once();
 check(arbiter.reservation().owner==RadioMode::matter_induction,"disconnect revoked advertising that was still enabled");
 window_open=advertising=false;disable_matter();worker_once();
 check(arbiter.reservation().owner==RadioMode::operational,"disconnect-before-disable stranded the radio");
}
void physical_links_outlive_unrelated_events(){
 idle_matter();gap_connect(11);gap_connect(12);gap_connect(13,BLE_GAP_ROLE_MASTER);
 window_open=advertising=false;disable_matter();
 // Unrelated LE connection-update events are not a physical connection change.
 for(unsigned count=0;count<3;++count)hci_event(BLE_HCI_EVCODE_LE_META,{3,0,11,0,0,0,0,0,0,0});
 gap_connect(12);gap_connect(14,BLE_GAP_ROLE_SLAVE,13);
 gap_disconnect(11);worker_once();
 check(arbiter.reservation().owner==RadioMode::matter_induction,"one disconnect released another peripheral");
 uint64_t admission=99;check(reserve_consortium(false,admission)==ESP_ERR_INVALID_STATE&&admission==0,"idle handoff ignored the physical peripheral");
 gap_disconnect(12);worker_once();
 check(arbiter.reservation().owner==RadioMode::operational,"last physical disconnect did not release radio");
 check(gap_roles.count(13)==1,"the existing central-role remote was disconnected");
 check(remote_request(20,nullptr),"central-role remote incorrectly blocks a fresh scan");
}
void ip_only_commissioning_does_not_hold_ble(){
 for(unsigned guard=0;guard<4;++guard){
  reset();idle_matter();window_open=guard==3;
  pase_session=guard==0;fail_safe=guard==1;fail_safe_busy=guard==2;
  disable_matter();worker_once();
  check(arbiter.reservation().owner==RadioMode::operational,"IP-only Matter state retained BLE ownership");
  check(remote_request(20,nullptr),"IP-only Matter state rejected a remote scan");
 }
 for(unsigned guard=0;guard<3;++guard){
  reset();idle_matter();window_open=false;
  pase_session=guard==0;fail_safe=guard==1;fail_safe_busy=guard==2;
  uint64_t admitted=0;
  check(reserve_consortium(false,admitted)==ESP_OK&&admitted,"IP-only Matter state rejected consortium setup");
 }
}
void committed_disable_protects_opening_and_later_retry(){
 {esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);check(chip::DeviceLayer::ConnectivityMgr().SetBLEAdvertisingEnabled(true)==CHIP_NO_ERROR,"could not reserve initial Matter advertisement");}
 check(desired_advertising&&!advertising,"fixture did not retain the initial scheduled-advertisement gap");
 clock_ms=900001;worker_once();
 check(arbiter.reservation().owner==RadioMode::matter_induction,"physical idle released pending initial advertising");
 // A rejected disable does not certify that the pending SDK advertisement is gone.
 advertising_error=7;
 {esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);check(chip::DeviceLayer::ConnectivityMgr().SetBLEAdvertisingEnabled(false)==7,"failed SDK disable changed return");}
 worker_once();check(arbiter.reservation().owner==RadioMode::matter_induction&&desired_advertising,"failed disable released pending advertising");
 advertising_error=0;disable_matter();worker_once();
 check(arbiter.reservation().owner==RadioMode::operational,"committed physical-idle disable did not release");
 pase_session=fail_safe=true;
 {esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);check(chip::DeviceLayer::ConnectivityMgr().SetBLEAdvertisingEnabled(true)==CHIP_NO_ERROR,"later SDK retry did not reacquire BLE");}
 check(desired_advertising&&matter_disabled_generation==0&&arbiter.reservation().owner==RadioMode::matter_induction,"SDK retry retained old disable authority");
 worker_once();check(arbiter.reservation().owner==RadioMode::matter_induction,"old disable released pending retried advertisement");
 advertising=true;gap_connect(25);disable_matter();worker_once();
 check(arbiter.reservation().owner==RadioMode::matter_induction,"SDK retry physical link was preempted");
 gap_disconnect(25);worker_once();check(arbiter.reservation().owner==RadioMode::operational,"retried advertisement cleanup did not release");
}
void disable_intent_fences_the_original_generation(){
 idle_matter();window_open=false;disable_matter();const auto previous=arbiter.reservation();
 replace_owner();release_idle_matter(previous.generation);
 check(arbiter.reservation().owner==RadioMode::remote_pairing,"old disable released replacement remote owner");
 disable_matter();check(arbiter.reservation().owner==RadioMode::remote_pairing,"SDK disable for another owner changed radio lease");
 const auto remote=arbiter.reservation();arbiter.release(remote.generation,now());arbiter.cleanup_complete(arbiter.reservation().generation,now());
 check(arbiter.request(RadioMode::matter_induction,now(),300000,false)==RadioResult::granted,"could not install a new Matter reservation");
 const auto fresh=arbiter.reservation();release_idle_matter(previous.generation);worker_once();
 check(arbiter.reservation().generation==fresh.generation&&arbiter.reservation().owner==RadioMode::matter_induction,"old disable affected new opening generation");
}
void physical_cleanup_during_transition_is_required(){
 idle_matter();window_open=advertising=false;disable_matter();
 check(arbiter.release(arbiter.reservation().generation,now())==RadioResult::cancelling,"could not begin handoff");
 gap_connect(31);worker_once();
 check(arbiter.reservation().owner==RadioMode::transition,"a late peripheral connection escaped handoff cleanup");
 gap_disconnect(31);worker_once();
 check(arbiter.reservation().owner==RadioMode::operational,"physical cleanup was not consumed after the late disconnect");
}
void invalid_physical_role_fails_closed(){
 idle_matter();window_open=advertising=false;disable_matter();
 gap_connect(51,2);worker_once();
 check(arbiter.reservation().owner==RadioMode::matter_induction,"invalid physical role silently released Matter");
 controller_reset(1);worker_once();
 check(arbiter.reservation().owner==RadioMode::matter_induction,"failed controller reset cleared uncertain physical evidence");
 controller_reset();worker_once();
 check(arbiter.reservation().owner==RadioMode::operational,"successful controller reset did not clear physical uncertainty");
}
void physical_tracking_uncertainty_fails_closed(){
 idle_matter();window_open=advertising=false;disable_matter();
 for(uint16_t handle=1;handle<=4;++handle)gap_connect(handle);
 for(uint16_t handle=1;handle<=4;++handle)gap_disconnect(handle);
 worker_once();
 check(arbiter.reservation().owner==RadioMode::matter_induction,"overflowed physical evidence silently released radio");
}
void raw_hci_connection_forms_and_status(){
 for(const uint8_t subevent:{BLE_HCI_LE_SUBEV_CONN_COMPLETE,BLE_HCI_LE_SUBEV_ENH_CONN_COMPLETE,BLE_HCI_LE_SUBEV_ENH_CONN_COMPLETE_V2}){
  reset();gap_connect(0,BLE_GAP_ROLE_MASTER,0,subevent);check(!peripheral_active(),"central-role remote was tracked as setup");
  gap_connect(0,BLE_GAP_ROLE_SLAVE,13,subevent);check(!peripheral_active(),"failed connection created a physical handle");
  gap_connect(0,BLE_GAP_ROLE_SLAVE,0,subevent);gap_connect(0,BLE_GAP_ROLE_SLAVE,0,subevent);
  gap_connect(BLE_HCI_LE_CONN_HANDLE_MAX,BLE_GAP_ROLE_SLAVE,0,subevent);
  check(peripheral_active()&&!peripheral_tracking_failed,"valid connection form/zero handle/duplicate was rejected");
  gap_disconnect(0,1);check(peripheral_active(),"failed disconnection removed a live handle");
  gap_disconnect(0);check(peripheral_active(),"one physical disconnect released another peripheral");
  gap_disconnect(BLE_HCI_LE_CONN_HANDLE_MAX);check(!peripheral_active(),"last physical disconnect did not clear its handle");
 }
}
void raw_hci_minimum_lengths_are_bounded(){
 for(const uint8_t subevent:{BLE_HCI_LE_SUBEV_CONN_COMPLETE,BLE_HCI_LE_SUBEV_ENH_CONN_COMPLETE,BLE_HCI_LE_SUBEV_ENH_CONN_COMPLETE_V2}){
  const unsigned full=subevent==BLE_HCI_LE_SUBEV_CONN_COMPLETE?19:subevent==BLE_HCI_LE_SUBEV_ENH_CONN_COMPLETE?31:34;
  for(unsigned length=1;length<full;++length){
   reset();std::vector<uint8_t> payload(length);payload[0]=subevent;
   hci_event(BLE_HCI_EVCODE_LE_META,payload);
   check(peripheral_tracking_failed,"truncated connection event certified physical idle");
  }
  reset();hci_event(BLE_HCI_EVCODE_LE_META,{subevent,13});
  check(!peripheral_active(),"short failed-connect event was treated as a live link");
 }
 for(unsigned length=0;length<4;++length){
  reset();hci_event(BLE_HCI_EVCODE_DISCONN_CMP,std::vector<uint8_t>(length));
  check(peripheral_tracking_failed,"truncated disconnection certified physical idle");
 }
 reset();hci_event(BLE_HCI_EVCODE_LE_META,{});hci_event(BLE_HCI_EVCODE_LE_META,{0xff});
 for(unsigned length=0;length<4;++length)hci_event(BLE_HCI_EVCODE_COMMAND_COMPLETE,std::vector<uint8_t>(length));
 check(!peripheral_active(),"unrelated/short command or unknown LE events changed physical tracking");
 gap_connect(0x0f00);check(peripheral_tracking_failed,"invalid physical handle was silently accepted");
}
void raw_hci_precedes_host_delivery_and_stop_ack(){
 idle_matter();window_open=false;advert_stop_at=100;disable_matter();
 on_dispatch=[](uint8_t* packet,void*){
  if(packet[0]==BLE_HCI_EVCODE_LE_META){
   check(peripheral_active(),"physical connection was invisible until host delivery");
   // The controller connection exists before its application callback and
   // remote-info exchanges have begun.
   check(!desired_advertising,"fixture did not commit BLE disable before its stop ACK");
  }
 };
 gap_connect(41);
 on_dispatch=[](uint8_t*,void*){advertising=false;worker_once();};
 hci_event(BLE_HCI_EVCODE_COMMAND_COMPLETE,{1,0x0a,0x20,0});
 check(arbiter.reservation().owner==RadioMode::matter_induction,"stop ACK released a pre-ACK physical connection before GAP connect delivery");
 on_dispatch={};gap_disconnect(41);worker_once();
 check(arbiter.reservation().owner==RadioMode::operational,"raw physical disconnection did not permit completed Matter cleanup");
}
void raw_hci_observation_never_waits_for_radio_mutex(){
 std::array<uint8_t,21> packet{};packet[0]=BLE_HCI_EVCODE_LE_META;packet[1]=19;packet[2]=1;packet[4]=8;packet[6]=1;
 {Lock lock;matter_hci_event(packet.data());check(peripheral_active(),"raw callback could not record a link while application owned radio mutex");}
}
void raw_controller_reset_is_authoritative(){
 idle_matter();window_open=advertising=false;disable_matter();gap_connect(1);gap_connect(2);
 hci_event(BLE_HCI_EVCODE_COMMAND_COMPLETE,{1,0x0a,0x20,0});worker_once();
 check(arbiter.reservation().owner==RadioMode::matter_induction,"ordinary successful command cleared live links");
 controller_reset(1);worker_once();check(peripheral_active(),"failed controller reset cleared live links");
 controller_reset();worker_once();
 check(!peripheral_active()&&arbiter.reservation().owner==RadioMode::operational,"successful controller reset stranded physical handles");
}
int main(){struct Test{const char* name;void(*run)();};unsigned failed=0;
 for(auto test:{Test{"idle Matter hands off to candidate and inviter",idle_handoff},Test{"physical Matter BLE denies takeover",active_denied},Test{"other busy owner unchanged",other_busy},Test{"stale disable generation fenced",stale_disable},Test{"stale cleanup generation fenced",stale_cleanup},Test{"shared five-second cleanup bound",bounded_timeout},Test{"late confirmed cleanup releases radio and fences expired setup",late_cleanup},Test{"late cleanup keeps timeout result",late_cleanup_result},Test{"worker waits for actual late advertiser cleanup",late_advertising_cleanup},Test{"ordinary admission preserved",ordinary},Test{"worker progress during committed disable preserves handoff",worker_cleanup_can_finish_while_handoff_disables},Test{"BLE disable before physical disconnect releases completed setup",disabled_before_disconnect},Test{"physical disconnect before BLE disable releases",disconnect_before_disable},Test{"multiple physical peripherals ignore unrelated events and central links",physical_links_outlive_unrelated_events},Test{"IP-only PASE/fail-safe/window state does not own BLE",ip_only_commissioning_does_not_hold_ble},Test{"committed disable fences initial advertising and SDK retry",committed_disable_protects_opening_and_later_retry},Test{"committed disable generation is fenced",disable_intent_fences_the_original_generation},Test{"late peripheral delays transition cleanup",physical_cleanup_during_transition_is_required},Test{"invalid role fails closed until successful controller reset",invalid_physical_role_fails_closed},Test{"physical tracking uncertainty fails closed",physical_tracking_uncertainty_fails_closed},Test{"raw legacy/enhanced connection forms and statuses",raw_hci_connection_forms_and_status},Test{"raw HCI minimum lengths and malformed metadata",raw_hci_minimum_lengths_are_bounded},Test{"raw connection precedes SDK delivery and advertising-stop ACK",raw_hci_precedes_host_delivery_and_stop_ack},Test{"raw observation is nonblocking under application radio mutex",raw_hci_observation_never_waits_for_radio_mutex},Test{"successful controller reset is authoritative physical cleanup",raw_controller_reset_is_authoritative}}){reset();try{test.run();std::cout<<"PASS "<<test.name<<'\n';}catch(const std::exception& error){++failed;std::cerr<<"FAIL "<<test.name<<": "<<error.what()<<'\n';}}
 return failed?1:0;
}
'''


def main():
    source = (ROOT / 'components/radio_control/radio_control.cpp').read_text()
    actual = function(source, 'class PeripheralLock') + ';\n'
    actual += ''.join(function(source, declaration) for declaration in (
        'bool peripheral_active()',
        'void peripheral_uncertain()',
        'void matter_hci_event(const uint8_t* buffer)',
        'void release_idle_matter(uint64_t generation)',
        'bool remote_request(uint32_t seconds,void*)',
        'esp_err_t finish_cancellation(uint64_t generation)',
        'void worker(void*)',
        'void matter_advertising_disabled()', 'bool matter_will_advertise()',
        'esp_err_t reserve_consortium(bool inviter,uint64_t& generation)',
        'void release(uint64_t generation)'))
    wrapper = function(source, 'extern \"C\" int __wrap_ble_hs_hci_rx_evt(uint8_t* event,void* context)')
    advertising_wrapper = function(source, 'extern \"C\" CHIP_ERROR __wrap__ZN4chip11DeviceLayer8Internal14BLEManagerImpl22_SetAdvertisingEnabledEb(void* self,bool enabled)')
    with tempfile.TemporaryDirectory(prefix='amity-radio-matter-handoff-') as temporary:
        cpp = Path(temporary) / 'actual.cpp'
        binary = Path(temporary) / 'actual'
        cpp.write_text(PREFIX + actual +
            'namespace amity::radio {using ::matter_hci_event;using ::matter_will_advertise;using ::matter_advertising_disabled;}\n' + wrapper + advertising_wrapper + SUITE)
        subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + [
            '-std=c++17', '-Wall', '-Wextra', '-Werror', '-g', '-fsanitize=address,undefined',
            '-I' + str(ROOT / 'components/consortium/include'),
            str(ROOT / 'components/consortium/radio.cpp'), str(cpp), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
