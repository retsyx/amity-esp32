// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#ifdef ESP_PLATFORM
#include "platform.hpp"
#include "amity/consortium/bootstrap.hpp"
#include "protocomm.h"
#include "session.pb-c.h"
#include "protocomm_security2.h"
#include "platform/CHIPDeviceLayer.h"
#include "platform/internal/BLEManager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
extern "C" {
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "os/os_mbuf.h"
}
#include <atomic>
#include <cstring>
namespace amity::consortium::platform {
namespace {
const ble_uuid128_t service_uuid=BLE_UUID128_INIT(0x11,0xa3,0x0c,0x09,0x9d,0xcb,0xb5,0xa7,0x81,0x46,0x16,0x01,0xd0,0x7c,0x35,0xd6);
const ble_uuid128_t record_uuid=BLE_UUID128_INIT(0x12,0xa3,0x0c,0x09,0x9d,0xcb,0xb5,0xa7,0x81,0x46,0x16,0x01,0xd0,0x7c,0x35,0xd6);
struct Work {uint64_t generation=0,session=0;uint16_t connection=0,total=0,offset=0,length=0;uint32_t record=0;uint8_t endpoint=0,bytes[512]{};bool close=false;};
QueueHandle_t work_queue=nullptr;
std::mutex state_mutex;
uint64_t generation=0,deadline=0,server_session=0,pc_session=0;
uint16_t server_connection=BLE_HS_CONN_HANDLE_NONE,attribute_handle=0;
std::string srv_salt,srv_verifier,srv_response,advertised_name;
BootstrapHandler server_handler;
bool server_open=false,registered=false;
std::atomic_uint failed_proofs{0};
std::atomic_bool processing_record{false},pc_live{false};
std::atomic_uint outstanding_work{0};
bool post_work(const Work &work){++outstanding_work;if(work_queue&&xQueueSend(work_queue,&work,0)==pdTRUE)return true;--outstanding_work;return false;}
struct WorkFinished {~WorkFinished(){processing_record=false;--outstanding_work;}};
uint16_t cancelled_server=BLE_HS_CONN_HANDLE_NONE,cancelled_client=BLE_HS_CONN_HANDLE_NONE;
uint32_t response_record=0;uint16_t response_offset=0;
uint8_t response_endpoint=0,response_status=0;
protocomm_t *pc=nullptr;
std::string pc_salt,pc_verifier;
protocomm_security2_params_t pc_params{};
uint16_t pc_connection=BLE_HS_CONN_HANDLE_NONE;
BootstrapRecords records;
uint64_t pc_started=0,record_started=0;uint32_t pending_record=0;bool pc_authenticated=false;
esp_err_t endpoint(uint32_t,const uint8_t *in,ssize_t size,uint8_t **out,ssize_t *outsize,void*){
    if(size<=0||size>4080)return ESP_ERR_INVALID_SIZE;
    BootstrapHandler handler;{std::lock_guard<std::mutex> guard(state_mutex);if(!server_open||now_ms()>=deadline)return ESP_ERR_INVALID_STATE;handler=server_handler;}std::string response;auto result=handler?handler(std::string_view(reinterpret_cast<const char*>(in),size),response):ESP_ERR_INVALID_STATE;if(result!=ESP_OK||response.empty()||response.size()>4080)return result==ESP_OK?ESP_ERR_INVALID_SIZE:result;*out=static_cast<uint8_t*>(malloc(response.size()));if(!*out)return ESP_ERR_NO_MEM;memcpy(*out,response.data(),response.size());*outsize=response.size();return ESP_OK;
}
void close_pc(){if(pc){if(pc_connection!=BLE_HS_CONN_HANDLE_NONE)protocomm_close_session(pc,pc_connection);protocomm_delete(pc);pc=nullptr;}pc_live=false;pc_connection=BLE_HS_CONN_HANDLE_NONE;pc_session=0;pc_started=record_started=0;pending_record=0;pc_authenticated=false;records.disconnect();}
void worker(void*){Work work;while(true){if(xQueueReceive(work_queue,&work,pdMS_TO_TICKS(100))!=pdTRUE){
        if(pc&&((!pc_authenticated&&now_ms()-pc_started>=30000)||(record_started&&now_ms()-record_started>=30000))){
            const auto connection=pc_connection;close_pc();++failed_proofs;ble_gap_terminate(connection,BLE_ERR_REM_USER_CONN_TERM);
        }
        continue;
    }processing_record=true;WorkFinished finished;if(work.close){close_pc();continue;}std::string salt,verifier;{std::lock_guard<std::mutex> guard(state_mutex);if(!server_open||generation!=work.generation||server_session!=work.session||server_connection!=work.connection||now_ms()>=deadline||failed_proofs>=5)continue;salt=srv_salt;verifier=srv_verifier;}
        if(pc&&((!pc_authenticated&&now_ms()-pc_started>=30000)||(record_started&&now_ms()-record_started>=30000))){close_pc();++failed_proofs;ble_gap_terminate(work.connection,BLE_ERR_REM_USER_CONN_TERM);continue;}
        if(pc&&pc_session!=work.session)close_pc();
        if(!pc){pc_session=work.session;pc=protocomm_new();pc_live=pc!=nullptr;pc_salt=salt;pc_verifier=verifier;pc_params={pc_salt.data(),static_cast<uint16_t>(pc_salt.size()),pc_verifier.data(),static_cast<uint16_t>(pc_verifier.size())};if(!pc||protocomm_set_security(pc,"session",&protocomm_security2,&pc_params)!=ESP_OK||protocomm_add_endpoint(pc,"join",endpoint,nullptr)!=ESP_OK||protocomm_open_session(pc,work.connection)!=ESP_OK){close_pc();continue;}pc_connection=work.connection;pc_started=now_ms();records.connect(work.generation);}
        if(!record_started){pending_record=work.record;record_started=now_ms();}
        char rid[33];snprintf(rid,sizeof(rid),"%032x",static_cast<unsigned>(work.record));auto parsed=model::MessageId::parse(rid);if(!parsed)continue;auto result=records.fragment(work.generation,*parsed,work.total,work.offset,std::string_view(reinterpret_cast<char*>(work.bytes),work.length));if(result==BootstrapRecords::Result::incomplete)continue;
        std::string response;bool good=false;if(result==BootstrapRecords::Result::duplicate){response=std::string(records.cached_response());good=true;record_started=0;}
        else if(result==BootstrapRecords::Result::complete){processing_record=true;uint8_t *out=nullptr;ssize_t length=0;auto request=records.request();auto e=protocomm_req_handle(pc,work.endpoint?"join":"session",work.connection,reinterpret_cast<const uint8_t*>(request.data()),request.size(),&out,&length);good=e==ESP_OK&&out&&length>0&&length<=4096;if(good){response.assign(reinterpret_cast<char*>(out),length);good=records.finish(*parsed,response);record_started=0;
            if(!work.endpoint){SessionData *session=session_data__unpack(nullptr,response.size(),reinterpret_cast<const uint8_t*>(response.data()));if(session&&session->sec_ver==SEC_SCHEME_VERSION__SecScheme2&&session->sec2&&session->sec2->msg==SEC2_MSG_TYPE__S2Session_Response1&&session->sec2->sr1&&session->sec2->sr1->status==STATUS__Success)pc_authenticated=true;if(session)session_data__free_unpacked(session,nullptr);}
        }free(out);if(!good){++failed_proofs;close_pc();}}
        {std::lock_guard<std::mutex> guard(state_mutex);if(server_open&&generation==work.generation&&server_session==work.session&&server_connection==work.connection){response_record=work.record;response_endpoint=work.endpoint;response_status=good?1:2;srv_response=std::move(response);}}
        if(failed_proofs>=5){std::lock_guard<std::mutex> guard(state_mutex);const bool was_open=server_open;server_open=false;if(was_open)ble_gap_adv_stop();if(server_connection!=BLE_HS_CONN_HANDLE_NONE)ble_gap_terminate(server_connection,BLE_ERR_REM_USER_CONN_TERM);}
    }}
int access(uint16_t connection,uint16_t,ble_gatt_access_ctxt *context,void*){
    std::lock_guard<std::mutex> guard(state_mutex);if(!server_open||now_ms()>=deadline||failed_proofs>=5||server_connection!=connection)return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    if(context->op==BLE_GATT_ACCESS_OP_WRITE_CHR){const auto length=OS_MBUF_PKTLEN(context->om);if(length<7||length>BLE_ATT_ATTR_MAX_LEN)return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;uint8_t bytes[BLE_ATT_ATTR_MAX_LEN];if(os_mbuf_copydata(context->om,0,length,bytes))return BLE_ATT_ERR_UNLIKELY;
        if(bytes[0]==2&&length==7){const uint32_t rid=(uint32_t(bytes[1])<<24)|(uint32_t(bytes[2])<<16)|(uint32_t(bytes[3])<<8)|bytes[4];const uint16_t offset=(uint16_t(bytes[5])<<8)|bytes[6];if(rid==response_record&&offset<=srv_response.size())response_offset=offset;else response_offset=0;return 0;}
        if(length<=10||bytes[0]!=1||bytes[1]>1)return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        Work work;work.generation=generation;work.session=server_session;work.connection=connection;work.endpoint=bytes[1];work.record=(uint32_t(bytes[2])<<24)|(uint32_t(bytes[3])<<16)|(uint32_t(bytes[4])<<8)|bytes[5];work.total=(uint16_t(bytes[6])<<8)|bytes[7];work.offset=(uint16_t(bytes[8])<<8)|bytes[9];work.length=length-10;if(!work.record||!work.total||work.total>4096||work.offset>work.total||work.length>work.total-work.offset)return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;memcpy(work.bytes,bytes+10,work.length);if(uxQueueMessagesWaiting(work_queue)>=15||!post_work(work))return BLE_ATT_ERR_INSUFFICIENT_RES;return 0;}
    if(context->op==BLE_GATT_ACCESS_OP_READ_CHR){
        if(context->offset)return BLE_ATT_ERR_INVALID_OFFSET;
        uint8_t header[12]={1,response_status,uint8_t(response_record>>24),uint8_t(response_record>>16),uint8_t(response_record>>8),uint8_t(response_record),uint8_t(srv_response.size()>>8),uint8_t(srv_response.size()),response_endpoint,0,uint8_t(response_offset>>8),uint8_t(response_offset)};
        const size_t mtu=ble_att_mtu(connection),capacity=mtu>13?std::min<size_t>(500,mtu-13):1;
        const auto count=std::min(capacity,srv_response.size()-std::min<size_t>(response_offset,srv_response.size()));
        if(os_mbuf_append(context->om,header,sizeof(header)))return BLE_ATT_ERR_INSUFFICIENT_RES;
        return count&&os_mbuf_append(context->om,srv_response.data()+response_offset,count)?BLE_ATT_ERR_INSUFFICIENT_RES:0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}
int server_gap(ble_gap_event *,void *);
int advertise_remaining(){
    if(!server_open||now_ms()>=deadline||failed_proofs>=5)return BLE_HS_EAPP;
    ble_gap_adv_params params{};params.conn_mode=BLE_GAP_CONN_MODE_UND;params.disc_mode=BLE_GAP_DISC_MODE_GEN;
    return ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC,nullptr,static_cast<int32_t>(deadline-now_ms()),&params,server_gap,reinterpret_cast<void*>(static_cast<uintptr_t>(generation)));
}
int server_gap(ble_gap_event *event,void *token){
    const auto expected=reinterpret_cast<uintptr_t>(token);std::lock_guard<std::mutex> guard(state_mutex);if(expected!=generation)return 0;
    if(event->type==BLE_GAP_EVENT_CONNECT){
        if(!event->connect.status){if(!server_open||server_connection!=BLE_HS_CONN_HANDLE_NONE||server_session==UINT64_MAX)ble_gap_terminate(event->connect.conn_handle,BLE_ERR_REM_USER_CONN_TERM);else{++server_session;server_connection=event->connect.conn_handle;}}
        else advertise_remaining();
    }else if(event->type==BLE_GAP_EVENT_DISCONNECT&&event->disconnect.conn.conn_handle==server_connection){
        server_connection=BLE_HS_CONN_HANDLE_NONE;Work work;work.close=true;post_work(work);srv_response.clear();response_record=response_status=0;
        advertise_remaining();
    }
    return 0;
}
SemaphoreHandle_t client_done=nullptr;
std::mutex client_mutex;
uint64_t client_generation=0;
uint16_t client_connection=BLE_HS_CONN_HANDLE_NONE,client_service_start=0,client_service_end=0,client_attribute=0;
ble_addr_t candidate_address{};
model::OctopusId expected_candidate;
bool found_candidate=false,client_workflow=false,client_discovery_waiting=false;
int client_error=0;
std::string client_response;
uint32_t client_record=0;
void complete(int error){client_error=error;xSemaphoreGive(client_done);}
bool current(void *token){return reinterpret_cast<uintptr_t>(token)==client_generation;}
int client_gap(ble_gap_event *event,void *token){std::lock_guard<std::mutex> guard(client_mutex);if(!current(token))return 0;
    if(event->type==BLE_GAP_EVENT_DISC&&client_discovery_waiting){ble_hs_adv_fields fields{};if(ble_hs_adv_parse_fields(&fields,event->disc.data,event->disc.length_data)==0&&fields.name&&fields.name_len==8){const std::string name(reinterpret_cast<const char*>(fields.name),fields.name_len);if(name=="A-"+expected_candidate.str().substr(26)){
        candidate_address=event->disc.addr;found_candidate=true;client_discovery_waiting=false;
        // Pinned NimBLE cancellation disables scanning and resets its master
        // state without emitting DISC_COMPLETE. Wake this generation's scan
        // waiter explicitly, once cancellation has actually succeeded. A late
        // natural completion must not wake the following connection wait.
        complete(ble_gap_disc_cancel());
    }}}
    else if(event->type==BLE_GAP_EVENT_DISC_COMPLETE&&client_discovery_waiting){client_discovery_waiting=false;complete(found_candidate?0:BLE_HS_ENOENT);}
    else if(event->type==BLE_GAP_EVENT_CONNECT){if(!event->connect.status)client_connection=event->connect.conn_handle;complete(event->connect.status);}
    else if(event->type==BLE_GAP_EVENT_DISCONNECT){if(client_connection==event->disconnect.conn.conn_handle){client_connection=BLE_HS_CONN_HANDLE_NONE;complete(BLE_HS_ENOTCONN);}}
    return 0;
}
int on_service(uint16_t,const ble_gatt_error *error,const ble_gatt_svc *service,void *token){std::lock_guard<std::mutex> guard(client_mutex);if(!current(token))return BLE_HS_EDONE;if(!error->status&&service){client_service_start=service->start_handle;client_service_end=service->end_handle;}else complete(error->status==BLE_HS_EDONE?0:error->status);return 0;}
int on_characteristic(uint16_t,const ble_gatt_error *error,const ble_gatt_chr *characteristic,void *token){std::lock_guard<std::mutex> guard(client_mutex);if(!current(token))return BLE_HS_EDONE;if(!error->status&&characteristic)client_attribute=characteristic->val_handle;else complete(error->status==BLE_HS_EDONE?0:error->status);return 0;}
int on_write(uint16_t,const ble_gatt_error *error,ble_gatt_attr *,void *token){std::lock_guard<std::mutex> guard(client_mutex);if(current(token))complete(error->status);return 0;}
int on_read_fragment(uint16_t,const ble_gatt_error *error,ble_gatt_attr *attribute,void *token){
    std::lock_guard<std::mutex> guard(client_mutex);if(!current(token))return BLE_HS_EDONE;
    if(error->status||!attribute){complete(error->status?error->status:BLE_HS_EINVAL);return 0;}
    const auto count=OS_MBUF_PKTLEN(attribute->om);if(count>512){complete(BLE_HS_EMSGSIZE);return BLE_HS_EDONE;}
    client_response.resize(count);if(os_mbuf_copydata(attribute->om,0,count,client_response.data()))complete(BLE_HS_EINVAL);else complete(0);return 0;
}
bool wait(uint32_t ms){return xSemaphoreTake(client_done,pdMS_TO_TICKS(ms))==pdTRUE&&client_error==0;}
void clear_done(){while(xSemaphoreTake(client_done,0)==pdTRUE){}client_error=0;}
}
esp_err_t gatt_register(){if(registered)return ESP_ERR_INVALID_STATE;work_queue=xQueueCreate(16,sizeof(Work));client_done=xSemaphoreCreateBinary();if(!work_queue||!client_done)return ESP_ERR_NO_MEM;
    static ble_gatt_chr_def chars[2]{};chars[0].uuid=&record_uuid.u;chars[0].access_cb=access;chars[0].flags=BLE_GATT_CHR_F_READ|BLE_GATT_CHR_F_WRITE;chars[0].val_handle=&attribute_handle;
    static std::vector<ble_gatt_svc_def> services;ble_gatt_svc_def service{};service.type=BLE_GATT_SVC_TYPE_PRIMARY;service.uuid=&service_uuid.u;service.characteristics=chars;services.push_back(service);
    if(chip::DeviceLayer::Internal::BLEMgrImpl().ConfigureExtraServices(services,false)!=CHIP_NO_ERROR)return ESP_ERR_INVALID_STATE;
    if(xTaskCreateWithCaps(worker,"group_pake",16384,nullptr,2,nullptr,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)!=pdPASS)return ESP_ERR_NO_MEM;
    registered=true;return ESP_OK;
}
esp_err_t gatt_server_open(uint64_t gen,std::string salt,std::string verifier,BootstrapHandler handler){std::lock_guard<std::mutex> guard(state_mutex);if(!registered||server_open||!gen||salt.size()!=32||verifier.size()!=384)return ESP_ERR_INVALID_STATE;generation=gen;deadline=now_ms()+180000;srv_salt=std::move(salt);srv_verifier=std::move(verifier);server_handler=std::move(handler);server_connection=BLE_HS_CONN_HANDLE_NONE;failed_proofs=0;response_status=response_record=0;srv_response.clear();
    // Runtime identity is public; derive the advertisement suffix from its
    // already configured own certificate SAN through the callback-free ID API.
    std::string origin;auto e=read_record("amity_origin","committed",origin,128);if(e!=ESP_OK||origin.size()!=origin_record_prefix.size()+32||std::string_view(origin).substr(0,origin_record_prefix.size())!=origin_record_prefix)return ESP_ERR_INVALID_STATE;advertised_name="A-"+origin.substr(origin.size()-6);
    ble_hs_adv_fields fields{};fields.flags=BLE_HS_ADV_F_DISC_GEN|BLE_HS_ADV_F_BREDR_UNSUP;fields.uuids128=const_cast<ble_uuid128_t*>(&service_uuid);fields.num_uuids128=1;fields.uuids128_is_complete=1;fields.name=reinterpret_cast<const uint8_t*>(advertised_name.data());fields.name_len=advertised_name.size();fields.name_is_complete=1;
    int result=ble_gap_adv_set_fields(&fields);if(result)return ESP_FAIL;ble_gap_adv_params params{};params.conn_mode=BLE_GAP_CONN_MODE_UND;params.disc_mode=BLE_GAP_DISC_MODE_GEN;server_open=true;result=ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC,nullptr,180000,&params,server_gap,reinterpret_cast<void*>(static_cast<uintptr_t>(gen)));if(result){server_open=false;return ESP_FAIL;}return ESP_OK;
}
void gatt_server_close(){std::lock_guard<std::mutex> guard(state_mutex);const bool was_open=server_open;server_open=false;if(was_open)ble_gap_adv_stop();if(server_connection!=BLE_HS_CONN_HANDLE_NONE){cancelled_server=server_connection;ble_gap_terminate(server_connection,BLE_ERR_REM_USER_CONN_TERM);}server_connection=BLE_HS_CONN_HANDLE_NONE;if(was_open||pc_live){Work work;work.close=true;if(work_queue)post_work(work);}srv_response.clear();server_handler={};}
esp_err_t gatt_client_connect(model::OctopusId expected,uint64_t gen){if(!registered||!expected||!gen)return ESP_ERR_INVALID_STATE;{std::lock_guard<std::mutex> guard(client_mutex);client_generation=gen;client_workflow=true;expected_candidate=expected;client_connection=BLE_HS_CONN_HANDLE_NONE;client_service_start=client_service_end=client_attribute=0;found_candidate=false;client_discovery_waiting=true;client_record=0;clear_done();}
    void *token=reinterpret_cast<void*>(static_cast<uintptr_t>(gen));ble_gap_disc_params params{};params.passive=0;params.filter_duplicates=1;if(ble_gap_disc(BLE_OWN_ADDR_PUBLIC,20000,&params,client_gap,token)||!wait(21000))return ESP_FAIL;ble_gap_conn_params connect{};connect.scan_itvl=0x10;connect.scan_window=0x10;connect.itvl_min=24;connect.itvl_max=40;connect.supervision_timeout=400;clear_done();if(ble_gap_connect(BLE_OWN_ADDR_PUBLIC,&candidate_address,10000,&connect,client_gap,token)||!wait(11000))return ESP_FAIL;clear_done();if(ble_gattc_disc_svc_by_uuid(client_connection,&service_uuid.u,on_service,token)||!wait(5000)||!client_service_start)return ESP_FAIL;clear_done();if(ble_gattc_disc_chrs_by_uuid(client_connection,client_service_start,client_service_end,&record_uuid.u,on_characteristic,token)||!wait(5000)||!client_attribute)return ESP_FAIL;return ESP_OK;
}
bool gatt_client_exchange(bool encrypted,std::string_view request,std::string &response){if(request.empty()||request.size()>4096||client_connection==BLE_HS_CONN_HANDLE_NONE||client_record==UINT32_MAX)return false;const auto rid=++client_record;void *token=reinterpret_cast<void*>(static_cast<uintptr_t>(client_generation));const auto mtu=ble_att_mtu(client_connection);
    // The ATT value limit is 512 even when the negotiated MTU is 517 or 527.
    // Leave room for this protocol's ten-byte fragment header as well as the
    // write request's three-byte ATT header.
    const size_t fragment=mtu>13?std::min<size_t>(mtu-13,BLE_ATT_ATTR_MAX_LEN-10):7;
    for(size_t offset=0;offset<request.size();offset+=fragment){auto count=std::min(fragment,request.size()-offset);uint8_t bytes[BLE_ATT_ATTR_MAX_LEN]={1,uint8_t(encrypted),uint8_t(rid>>24),uint8_t(rid>>16),uint8_t(rid>>8),uint8_t(rid),uint8_t(request.size()>>8),uint8_t(request.size()),uint8_t(offset>>8),uint8_t(offset)};memcpy(bytes+10,request.data()+offset,count);clear_done();if(ble_gattc_write_flat(client_connection,client_attribute,bytes,count+10,on_write,token)||!wait(5000))return false;}
    const auto end=now_ms()+45000;size_t offset=0,total=0;response.clear();
    while(now_ms()<end){
        uint8_t cursor[7]={2,uint8_t(rid>>24),uint8_t(rid>>16),uint8_t(rid>>8),uint8_t(rid),uint8_t(offset>>8),uint8_t(offset)};
        clear_done();if(ble_gattc_write_flat(client_connection,client_attribute,cursor,sizeof(cursor),on_write,token)||!wait(5000))return false;
        clear_done();if(ble_gattc_read(client_connection,client_attribute,on_read_fragment,token)||!wait(5000))return false;
        std::string bytes;{std::lock_guard<std::mutex> guard(client_mutex);bytes=client_response;}
        if(bytes.size()>=12&&uint8_t(bytes[0])==1){
            const uint32_t returned=(uint32_t(uint8_t(bytes[2]))<<24)|(uint32_t(uint8_t(bytes[3]))<<16)|(uint32_t(uint8_t(bytes[4]))<<8)|uint8_t(bytes[5]);
            const size_t length=(uint16_t(uint8_t(bytes[6]))<<8)|uint8_t(bytes[7]);
            const size_t cursor=(uint16_t(uint8_t(bytes[10]))<<8)|uint8_t(bytes[11]);
            if(returned==rid&&uint8_t(bytes[8])==uint8_t(encrypted)){
                if(uint8_t(bytes[1])==2)return false;
                if(uint8_t(bytes[1])==1){if(length>4096||!length||cursor!=offset||(total&&total!=length)||bytes.size()-12>length-offset)return false;total=length;if(bytes.size()==12)return false;response.append(bytes.data()+12,bytes.size()-12);offset=response.size();if(offset==total)return true;continue;}
            }
        }
        vTaskDelay(pdMS_TO_TICKS(25));
    }
    return false;
}
void gatt_client_disconnect(){std::lock_guard<std::mutex> guard(client_mutex);if(!client_workflow)return;client_workflow=false;client_discovery_waiting=false;ble_gap_disc_cancel();ble_gap_conn_cancel();if(client_connection!=BLE_HS_CONN_HANDLE_NONE){cancelled_client=client_connection;ble_gap_terminate(client_connection,BLE_ERR_REM_USER_CONN_TERM);}client_connection=BLE_HS_CONN_HANDLE_NONE;++client_generation;}
bool gatt_candidate_transport_idle(){std::lock_guard<std::mutex> guard(state_mutex);return server_connection==BLE_HS_CONN_HANDLE_NONE&&!processing_record&&!outstanding_work;}
bool gatt_quiescent(){
    if(ble_gap_adv_active()||ble_gap_disc_active()||ble_gap_conn_active()||processing_record||outstanding_work||pc_live)return false;
    ble_gap_conn_desc descriptor{};
    {std::lock_guard<std::mutex> guard(state_mutex);if(server_open||server_connection!=BLE_HS_CONN_HANDLE_NONE)return false;if(cancelled_server!=BLE_HS_CONN_HANDLE_NONE&&ble_gap_conn_find(cancelled_server,&descriptor)==0)return false;cancelled_server=BLE_HS_CONN_HANDLE_NONE;}
    {std::lock_guard<std::mutex> guard(client_mutex);if(client_workflow||client_connection!=BLE_HS_CONN_HANDLE_NONE)return false;if(cancelled_client!=BLE_HS_CONN_HANDLE_NONE&&ble_gap_conn_find(cancelled_client,&descriptor)==0)return false;cancelled_client=BLE_HS_CONN_HANDLE_NONE;}
    return true;
}
} // namespace
#endif
