// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#include "group_firmware_update.hpp"
#include "config_store.hpp"
#include "system_configuration.hpp"
#include "system_services.hpp"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include <algorithm>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <map>
namespace system_services::updates {
namespace {
namespace c=amity::consortium;namespace m=amity::system;
constexpr uint64_t peer_timeout_ms=120000,retry_ms=60000,activation_window_ms=10000;
struct Target {
    m::OctopusId owner;
    uint64_t offset=0,last_progress=0,next_attempt=0;
    std::string phase="queued",error;
};
struct Cohort {
    m::MessageId id;
    ReleaseManifest release;
    std::string signature,error,phase="idle";
    bool automatic=false;
    size_t current=0;
    uint64_t activate_at=0;
    std::vector<Target> targets;
};
std::mutex gate,worker_gate,ack_gate;
std::condition_variable changed;
Cohort cohort;CohortHooks hooks;m::OctopusId local;
std::string last_failed_upload="null";
struct Pending {m::OctopusId owner;m::MessageId request;std::string release_id,image_hash;std::optional<c::Json> result;};
std::optional<Pending> pending;
struct Retry {m::BootId boot;std::string image,source;unsigned attempts=0;uint64_t after=0;};
std::map<m::OctopusId,Retry> retries;
std::mutex incoming_gate;
m::OctopusId incoming_owner;
m::MessageId incoming_begin;
std::string incoming_release,incoming_hash;
uint64_t now(){return uint64_t(esp_timer_get_time()/1000);}
std::string quote(std::string_view value){std::string out="\"";for(unsigned char b:value){if(b=='"'||b=='\\'){out+='\\';out+=char(b);}else if(b>=32&&b<127)out+=char(b);else out+='?';}return out+'"';}
std::string string(const c::Json& value,const char* field){const auto* p=value.get(field);return p&&p->type==c::Json::Type::string?p->string:"";}
uint64_t decimal(const c::Json& value,const char* field){uint64_t n=0;c::decimal(value.get(field),n);return n;}
m::MessageId fresh(){return m::MessageId::generate([](uint8_t* bytes,size_t count){esp_fill_random(bytes,count);return true;}).value_or(m::MessageId{});}
std::string hex(std::string_view bytes){const char alphabet[]="0123456789abcdef";std::string out(2*bytes.size(),'0');for(size_t n=0;n<bytes.size();++n){const auto b=uint8_t(bytes[n]);out[2*n]=alphabet[b>>4];out[2*n+1]=alphabet[b&15];}return out;}
bool unhex(std::string_view bytes,std::string& out){if(bytes.size()%2||bytes.size()>4096)return false;out.clear();for(size_t n=0;n<bytes.size();n+=2){const auto digit=[](char b){return b>='0'&&b<='9'?b-'0':b>='a'&&b<='f'?b-'a'+10:-1;};const int a=digit(bytes[n]),b=digit(bytes[n+1]);if(a<0||b<0)return false;out+=char(a*16+b);}return true;}
std::string base(m::MessageId id,const ReleaseManifest& release){return "\"request_id\":"+quote(id.str())+",\"release_id\":"+quote(release.release_id)+",\"image_sha256\":"+quote(release.image_sha256);}
bool busy(){return cohort.phase=="uploading"||cohort.phase=="distributing"||cohort.phase=="activating";}
std::string report_job(const Cohort& value);
void fail_job(const std::string& error){
    std::vector<m::OctopusId> owners;
    {std::lock_guard<std::mutex> lock(gate);cohort.phase="failed";cohort.error=error;
        for(auto& target:cohort.targets){
            if(target.owner!=local)owners.push_back(target.owner);
            if(target.phase=="uploading"||target.phase=="activating"||
               (target.owner==local&&cohort.current>=cohort.targets.size())){target.phase="failed";target.error=error;}
        }
        if(!cohort.automatic)last_failed_upload=report_job(cohort);
    }
    if(hooks.owner_maintenance)for(auto owner:owners)hooks.owner_maintenance(owner,false);
}
bool authority(){const auto state=c::runtime_status();return state.initialized&&state.coordinator&&state.authority_ready&&!state.member&&firmware_ready();}
std::optional<c::RuntimePeerStatus> peer(m::OctopusId owner){for(const auto& p:c::runtime_peers())if(p.id==owner)return p;return {};}
bool available(const c::RuntimePeerStatus& p){return p.enrolled&&!p.provisional&&(p.ready||p.management_ready)&&(p.negotiated_capabilities&c::peer_firmware_update);}
std::string report_job(const Cohort& value){
    std::string out="{\"job_id\":"+quote(value.id?value.id.str():"")+",\"phase\":"+quote(value.phase)+",\"automatic\":"+(value.automatic?"true":"false")+",\"error\":"+quote(value.error)+",\"firmware_version\":"+quote(value.release.firmware_version)+",\"image_elf_sha256\":"+quote(value.release.image_elf_sha256)+",\"image_bytes\":\""+std::to_string(value.release.image_bytes)+"\",\"targets\":[";
    for(const auto& target:value.targets){if(out.back()!='[')out+=',';out+="{\"octopus_id\":"+quote(target.owner.str())+",\"offset\":\""+std::to_string(target.offset)+"\",\"phase\":"+quote(target.phase)+",\"error\":"+quote(target.error)+"}";}
    return out+"]}";
}
bool exchange(m::OctopusId owner,c::Kind kind,std::string payload,m::MessageId request,const ReleaseManifest& release,c::Json& response,uint32_t timeout=65000){
    {std::lock_guard<std::mutex> lock(ack_gate);pending=Pending{owner,request,release.release_id,release.image_sha256,{}};}
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(timeout);
    bool admitted=false;
    while(std::chrono::steady_clock::now()<deadline){
        const auto sent=c::runtime_send_update(owner,kind,payload,5000);
        if(sent==ESP_OK){admitted=true;break;}
        if(sent!=ESP_ERR_NO_MEM)break;
        const auto current=peer(owner);if(!current||!available(*current))break;
        // This waits only for local FIFO capacity. No application message has
        // been admitted yet, so retrying cannot duplicate a stream frame.
        std::unique_lock<std::mutex> lock(ack_gate);
        changed.wait_until(lock,std::min(deadline,std::chrono::steady_clock::now()+std::chrono::milliseconds(20)));
    }
    std::unique_lock<std::mutex> lock(ack_gate);
    if(!admitted){pending.reset();return false;}
    bool received=false;
    while(std::chrono::steady_clock::now()<deadline){
        const auto tick=std::min(deadline,std::chrono::steady_clock::now()+std::chrono::milliseconds(250));
        if(changed.wait_until(lock,tick,[]{return pending&&pending->result.has_value();})){received=true;break;}
        // A logical connection restart aborts the old upload. Stop waiting so
        // its maintenance fence cannot prevent that same TLS from recovering.
        lock.unlock();const auto current=peer(owner);const bool usable=current&&available(*current);lock.lock();
        if(!usable)break;
    }
    if(received)response=std::move(*pending->result);
    pending.reset();return received;
}
c::Error result(m::OctopusId owner,const c::Json& value){
    std::lock_guard<std::mutex> lock(ack_gate);m::MessageId id;
    if(!pending||pending->owner!=owner||!c::id(value.get("request_id"),id)||pending->request!=id||string(value,"release_id")!=pending->release_id||string(value,"image_sha256")!=pending->image_hash)return c::Error::transaction_conflict;
    pending->result=value;changed.notify_all();return c::Error::none;
}
std::string receiver_result(const c::Envelope& envelope,const char* stage,const char* error){
    c::Json report;c::JsonError detail;c::parse_json(status_json(),report,detail);
    ReleaseManifest release;release.release_id=string(envelope.payload,"release_id");release.image_sha256=string(envelope.payload,"image_sha256");
    return "{"+base(*m::MessageId::parse(string(envelope.payload,"request_id")),release)+
        ",\"offset\":"+quote(string(report,"received_bytes").empty()?"0":string(report,"received_bytes"))+",\"stage\":"+quote(stage)+",\"error\":"+quote(error)+"}";
}
c::Error receive(m::OctopusId owner,const c::Envelope& envelope){
    std::string error;esp_err_t applied=ESP_ERR_INVALID_ARG;const char* stage="rejected";const char* code="INVALID_MESSAGE";
    const auto& request=envelope.payload;const auto release_id=string(request,"release_id"),image_hash=string(request,"image_sha256");
    bool matching=false;{std::lock_guard<std::mutex> lock(incoming_gate);matching=incoming_begin&&incoming_owner==owner&&incoming_release==release_id&&incoming_hash==image_hash;}
    // Durable staged images may be activated on a fresh authenticated stream.
    if(envelope.kind==c::Kind::update_install&&string(request,"phase")=="activate"&&!matching){ReleaseManifest staged;std::string signature;matching=partition_release(false,staged,signature,error)==ESP_OK&&staged.release_id==release_id&&staged.image_sha256==image_hash;}
    if(envelope.kind!=c::Kind::update_begin&&envelope.kind!=c::Kind::update_status&&!matching){c::runtime_send_update(owner,c::Kind::update_result,receiver_result(envelope,"rejected","CONFLICT"),5000);return c::Error::transaction_conflict;}
    if(envelope.kind==c::Kind::update_begin){ReleaseManifest manifest;
        if(parse_release_manifest(string(request,"canonical_manifest"),manifest,error)&&manifest.release_id==release_id&&manifest.image_sha256==image_hash){
            {
                std::lock_guard<std::mutex> lock(incoming_gate);
                if(incoming_begin&&incoming_owner==owner&&incoming_begin!=*m::MessageId::parse(string(request,"request_id"))){
                    peer_abort("A new firmware transfer replaced the incomplete upload");incoming_begin={};
                }
            }
            applied=peer_begin(manifest.canonical,string(request,"signature_hex"),error);code=applied==ESP_ERR_INVALID_ARG?"INVALID_MANIFEST":"NOT_READY";
            if(applied==ESP_OK){stage="ready";std::lock_guard<std::mutex> lock(incoming_gate);incoming_owner=owner;incoming_begin=*m::MessageId::parse(string(request,"request_id"));incoming_release=release_id;incoming_hash=image_hash;}
        }
    }else if(envelope.kind==c::Kind::update_chunk){std::string bytes;if(unhex(string(request,"data_hex"),bytes)&&config_store::sha256_hex(bytes)==string(request,"chunk_sha256")){applied=peer_chunk(release_id,decimal(request,"offset"),bytes,error);code=applied==ESP_ERR_INVALID_ARG?"OFFSET_MISMATCH":"STORAGE_FAILURE";if(applied==ESP_OK)stage="progress";}}
    else if(envelope.kind==c::Kind::update_install){
        const bool staging=string(request,"phase")=="stage";
        applied=staging?peer_stage(release_id,error):peer_install(release_id,error,uint32_t(decimal(request,"reboot_delay_ms")));
        if(staging&&applied!=ESP_OK)peer_abort("Firmware staging failed; current firmware retained");
        code="HASH_MISMATCH";if(applied==ESP_OK)stage=staging?"staged":"installing";
    }else if(envelope.kind==c::Kind::update_abort){peer_abort("Main Amity cancelled the update");applied=ESP_OK;stage="aborted";}
    else if(envelope.kind==c::Kind::update_status){applied=ESP_OK;stage="status";}
    else return c::Error::unsupported_kind;
    // Ordered stream chunks need no successful application acknowledgement.
    // The final stage response confirms the complete image and its validation.
    if(envelope.kind==c::Kind::update_chunk&&applied==ESP_OK)return c::Error::none;
    if(c::runtime_send_update(owner,c::Kind::update_result,receiver_result(envelope,stage,applied==ESP_OK?"":code),5000)!=ESP_OK)return c::Error::queue_full;
    return applied==ESP_OK?c::Error::none:applied==ESP_ERR_INVALID_ARG?c::Error::invalid_message:applied==ESP_ERR_INVALID_STATE?c::Error::not_ready:c::Error::storage_failure;
}
void disconnected(m::OctopusId owner,m::MessageId begin){std::lock_guard<std::mutex> lock(incoming_gate);if(incoming_owner==owner&&incoming_begin==begin){incoming_begin={};peer_abort("The update connection closed; current firmware retained");}}
void retry_later(const Target& target,const ReleaseManifest& source,bool failed_boot=false){
    const auto current=peer(target.owner);if(!current)return;
    std::lock_guard<std::mutex> lock(gate);
    auto& retry=retries[target.owner];
    if(retry.image!=current->firmware_identity||retry.source!=source.image_elf_sha256)retry={current->boot_id,current->firmware_identity,source.image_elf_sha256,0,0};
    if(failed_boot&&current->boot_id&&current->boot_id!=retry.boot)++retry.attempts;
    retry.after=now()+retry_ms;
}
void start_catchup(){
    if(!authority())return;
    const auto* app=esp_app_get_description();const auto own=hex(std::string_view(reinterpret_cast<const char*>(app->app_elf_sha256),32));
    for(const auto& current:c::runtime_peers()){
        if(!available(current)||current.firmware_identity.empty()||current.firmware_identity==own)continue;
        {
            std::lock_guard<std::mutex> lock(gate);if(busy())return;
            auto& retry=retries[current.id];
            if(retry.image!=current.firmware_identity||retry.source!=own)retry={current.boot_id,current.firmware_identity,own,0,0};
            if(retry.attempts>=3||now()<retry.after)continue;
            retry.boot=current.boot_id;
        }
        ReleaseManifest release;std::string signature,error;
        if(partition_release(true,release,signature,error)!=ESP_OK){
            Cohort failed;failed.id=fresh();failed.automatic=true;failed.phase="failed";failed.error=error;
            failed.release.image_elf_sha256=own;failed.release.firmware_version=app->version;
            failed.targets.push_back({current.id,0,now(),0,"failed",error});
            std::lock_guard<std::mutex> lock(gate);retries[current.id].attempts=3;if(!busy())cohort=std::move(failed);return;
        }
        Cohort next;next.id=fresh();next.release=std::move(release);next.signature=std::move(signature);next.automatic=true;next.phase="distributing";next.targets.push_back({current.id,0,now(),0,"queued",""});
        std::lock_guard<std::mutex> lock(gate);if(!busy())cohort=std::move(next);return;
    }
}
void distribute(){
    Cohort snapshot;{std::lock_guard<std::mutex> lock(gate);snapshot=cohort;}
    if(snapshot.current>=snapshot.targets.size()){
        std::lock_guard<std::mutex> lock(gate);
        if(snapshot.automatic&&snapshot.targets.front().phase=="skipped"){cohort.phase="failed";cohort.error=snapshot.targets.front().error;return;}
        cohort.phase="activating";cohort.current=0;cohort.activate_at=now()+activation_window_ms;return;
    }
    auto target=snapshot.targets[snapshot.current];
    if(target.owner==local||target.phase=="staged"||target.phase=="skipped"){
        std::lock_guard<std::mutex> lock(gate);++cohort.current;return;
    }
    if(now()<target.next_attempt)return;
    if(!target.last_progress)target.last_progress=now();
    const auto current=peer(target.owner);
    std::string error;
    bool success=false,backpressured=false,stream_failed=false,staging=false,rejected_image=false;
    c::Json response;const auto id=fresh();
    if(!current||!available(*current))error="Waiting for this Amity to connect.";
    else if(target.phase=="queued"){
        if(hooks.owner_maintenance)hooks.owner_maintenance(target.owner,true);
        success=exchange(target.owner,c::Kind::update_begin,"{"+base(id,snapshot.release)+",\"canonical_manifest\":"+quote(snapshot.release.canonical)+",\"signature_hex\":"+quote(snapshot.signature)+"}",id,snapshot.release,response)&&string(response,"stage")=="ready";
        if(success){target.phase="uploading";target.offset=0;}else {error="This Amity has not accepted the firmware.";if(hooks.owner_maintenance)hooks.owner_maintenance(target.owner,false);}
    }else if(target.offset<snapshot.release.image_bytes){
        // Feed the existing bounded link FIFO, never another transfer queue.
        // Offset means locally admitted bytes; only staging confirms receipt.
        for(unsigned turn=0;turn<16&&target.offset<snapshot.release.image_bytes;++turn){
            std::string bytes;const auto count=size_t(std::min<uint64_t>(2048,snapshot.release.image_bytes-target.offset));
            const auto read=snapshot.automatic?read_running_image(target.offset,count,bytes,error):read_staged_image(target.offset,count,bytes,error);
            if(read!=ESP_OK){const auto abort=fresh();c::Json ignored;exchange(target.owner,c::Kind::update_abort,"{"+base(abort,snapshot.release)+",\"reason\":\"failed\"}",abort,snapshot.release,ignored,1000);fail_job(error);return;}
            const auto chunk=fresh();
            const auto sent=c::runtime_send_update(target.owner,c::Kind::update_chunk,"{"+base(chunk,snapshot.release)+",\"offset\":\""+std::to_string(target.offset)+"\",\"data_hex\":"+quote(hex(bytes))+",\"chunk_sha256\":"+quote(config_store::sha256_hex(bytes))+"}",5000);
            if(sent==ESP_ERR_NO_MEM){backpressured=true;break;}
            if(sent!=ESP_OK){stream_failed=true;error="Waiting for the firmware connection.";break;}
            target.offset+=bytes.size();success=true;
        }
    }else{
        staging=true;
        success=exchange(target.owner,c::Kind::update_install,"{"+base(id,snapshot.release)+",\"phase\":\"stage\"}",id,snapshot.release,response)&&string(response,"stage")=="staged"&&decimal(response,"offset")==snapshot.release.image_bytes;
        rejected_image=string(response,"stage")=="rejected";
        if(success){target.phase="staged";if(hooks.owner_maintenance)hooks.owner_maintenance(target.owner,false);}else error="This Amity has not confirmed firmware validation.";
    }
    if(success&&!stream_failed){if(target.offset>snapshot.targets[snapshot.current].offset)target.last_progress=now();target.error.clear();target.next_attempt=0;}
    else{
        target.error=backpressured?"":std::move(error);target.next_attempt=backpressured?0:now()+1000;
        if(rejected_image){
            retry_later(target,snapshot.release);
            fail_job("Firmware image validation failed. All devices retained their current firmware.");
            return;
        }
        // If a peer restarted or rejected its transfer, begin again rather than
        // repeatedly submitting an obsolete offset to a fresh receiver.
        const auto after=peer(target.owner);
        if(target.phase!="skipped"&&(!after||!available(*after)||stream_failed||staging)){
            if(hooks.owner_maintenance)hooks.owner_maintenance(target.owner,false);
            target.phase="queued";target.offset=0;
        }
        if(target.phase!="skipped"&&now()-target.last_progress>=peer_timeout_ms){
            if(hooks.owner_maintenance)hooks.owner_maintenance(target.owner,false);
            target.phase="skipped";target.error="Firmware update timed out. It will be retried when this Amity reconnects.";retry_later(target,snapshot.release);
            if(current&&available(*current)){const auto abort=fresh();c::Json ignored;exchange(target.owner,c::Kind::update_abort,"{"+base(abort,snapshot.release)+",\"reason\":\"timeout\"}",abort,snapshot.release,ignored,1000);}
        }
    }
    std::lock_guard<std::mutex> lock(gate);cohort.targets[snapshot.current]=std::move(target);
    if(cohort.targets[snapshot.current].phase=="staged"||cohort.targets[snapshot.current].phase=="skipped")++cohort.current;
}
void activate(){
    Cohort snapshot;{std::lock_guard<std::mutex> lock(gate);snapshot=cohort;}
    if(snapshot.current<snapshot.targets.size()){
        auto target=snapshot.targets[snapshot.current];
        if(target.owner!=local&&target.phase=="staged"){
            if(hooks.owner_maintenance)hooks.owner_maintenance(target.owner,true);
            const auto delay=snapshot.activate_at>now()?snapshot.activate_at-now():uint64_t(1000);
            const auto id=fresh();
            // Validation finished at the staging barrier. Admit every restart
            // instruction before selecting our own image, without serializing
            // reboot timing behind application replies. Fresh hellos verify
            // the running image after activation.
            const bool sent=c::runtime_send_update(target.owner,c::Kind::update_install,"{"+base(id,snapshot.release)+",\"phase\":\"activate\",\"reboot_delay_ms\":\""+std::to_string(delay)+"\"}",5000)==ESP_OK;
            target.phase=sent?"activating":"skipped";
            if(!sent)target.error="Restart could not be sent. The firmware will be checked on reconnection.";
        }
        std::lock_guard<std::mutex> lock(gate);cohort.targets[snapshot.current]=std::move(target);++cohort.current;return;
    }
    if(snapshot.automatic){
        const auto current=peer(snapshot.targets.front().owner);
        if(current&&current->firmware_identity==snapshot.release.image_elf_sha256){
            // Matching authenticated firmware may synchronize normally. Do not
            // keep its owner fenced while waiting for that synchronization.
            if(hooks.owner_maintenance)hooks.owner_maintenance(current->id,false);
            if(current->ready){std::lock_guard<std::mutex> lock(gate);cohort.targets.front().phase="complete";cohort.phase="complete";retries.erase(current->id);return;}
        }
        if(now()<snapshot.activate_at+peer_timeout_ms)return;
        retry_later(snapshot.targets.front(),snapshot.release,true);
        fail_job("The Amity has not returned with the expected firmware.");return;
    }
    std::string error;
    const auto delay=uint32_t(snapshot.activate_at>now()?snapshot.activate_at-now():1000);
    if(peer_install(snapshot.release.release_id,error,delay)!=ESP_OK){fail_job(error);return;}
    // Once boot selection succeeds, remain in activation until reboot. The
    // current running image must never start a catch-up during this window.
    std::lock_guard<std::mutex> lock(gate);for(auto& target:cohort.targets)if(target.owner==local)target.phase="activating";cohort.current=SIZE_MAX;
}
}
void start_group_updates(m::OctopusId owner,CohortHooks callbacks){std::lock_guard<std::mutex> lock(gate);local=owner;hooks=std::move(callbacks);c::runtime_update_hooks({receive,result,disconnected});}
esp_err_t begin_system_upload(const ReleaseManifest& release,std::string& error){
    std::lock_guard<std::mutex> lock(gate);
    if(!authority()){error="The main Amity must be ready before updating firmware.";return ESP_ERR_INVALID_STATE;}
    if(busy()){error="A firmware update is already in progress.";return ESP_ERR_INVALID_STATE;}
    Cohort next;next.id=fresh();next.release=release;next.phase="uploading";next.targets.push_back({local,0,now(),0,"uploading",""});cohort=std::move(next);last_failed_upload="null";return ESP_OK;
}
void system_upload_progress(uint64_t bytes){std::lock_guard<std::mutex> lock(gate);if(cohort.phase=="uploading")cohort.targets.front().offset=bytes;}
esp_err_t finish_system_upload(std::string& error){
    ReleaseManifest release;std::string signature;if(partition_release(false,release,signature,error)!=ESP_OK)return ESP_FAIL;
    std::lock_guard<std::mutex> lock(gate);
    if(cohort.phase!="uploading"){error="The firmware upload is no longer active.";return ESP_ERR_INVALID_STATE;}
    cohort.release=std::move(release);cohort.signature=std::move(signature);cohort.phase="distributing";cohort.targets.front().phase="staged";cohort.targets.front().offset=cohort.release.image_bytes;
    for(const auto& p:c::runtime_peers())if(p.enrolled&&!p.provisional)cohort.targets.push_back({p.id,0,0,0,"queued",""});
    return ESP_OK;
}
void fail_system_upload(const std::string& error){fail_job(error);}
std::string cohort_status_json(){
    // Capture runtime state before the report lock; retry bookkeeping shares
    // this lock with the worker, without holding it across flash or transport.
    const auto current=c::runtime_peers();const auto* app=esp_app_get_description();
    const auto own=hex(std::string_view(reinterpret_cast<const char*>(app->app_elf_sha256),32));
    std::lock_guard<std::mutex> lock(gate);auto out=report_job(cohort);out.pop_back();
    out+=",\"last_failed_upload\":"+last_failed_upload+",\"automatic_failures\":[";
    bool comma=false;
    for(const auto& p:current){
        const auto failed=retries.find(p.id);
        if(!p.enrolled||p.provisional||failed==retries.end()||failed->second.attempts<3||
           failed->second.source!=own||failed->second.image!=p.firmware_identity||p.firmware_identity==own)continue;
        if(comma)out+=',';
        comma=true;
        out+="{\"octopus_id\":"+quote(p.id.str())+",\"phase\":\"failed\",\"image_elf_sha256\":"+quote(own)+",\"firmware_identity\":"+quote(p.firmware_identity)+"}";
    }
    return out+"]}";
}
void poll_group_update(){
    std::unique_lock<std::mutex> worker(worker_gate,std::try_to_lock);if(!worker.owns_lock())return;
    std::string phase;size_t current;{std::lock_guard<std::mutex> lock(gate);phase=cohort.phase;current=cohort.current;}
    if(phase=="uploading"||(phase=="activating"&&current==SIZE_MAX))return;
    if(phase=="distributing")distribute();else if(phase=="activating")activate();else start_catchup();
}
} // namespace system_services::updates
