// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#include "group_firmware_update.hpp"
#include "config_store.hpp"
#include "system_configuration.hpp"
#include "system_services.hpp"
#include "esp_random.h"
#include "esp_app_desc.h"
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <set>
namespace system_services::updates {
namespace {
namespace c=amity::consortium;namespace m=amity::system;
struct Target {m::OctopusId owner;m::BootId previous_boot;uint64_t offset=0;std::string phase="queued";uint64_t last_offset=0;std::string last_hash;};
struct Cohort {m::MessageId id;ReleaseManifest release;std::string manifest,signature,error,phase="idle";uint64_t config_revision=0;size_t current=0;std::vector<Target> targets;};
std::mutex gate,ack_gate,exchange_gate;
std::condition_variable changed;
Cohort cohort;CohortHooks hooks;m::OctopusId local;
struct Pending {m::OctopusId owner;m::MessageId request;std::string release_id,image_hash;std::optional<c::Json> result;};
std::optional<Pending> pending;
std::mutex incoming_gate;m::OctopusId incoming_owner;m::MessageId incoming_begin;std::string incoming_release,incoming_hash;
std::string quote(std::string_view value){std::string out="\"";for(unsigned char b:value){if(b=='"'||b=='\\'){out+='\\';out+=char(b);}else if(b>=32&&b<127)out+=char(b);else out+='?';}return out+'"';}
std::string string(const c::Json& value,const char* field){const auto* p=value.get(field);return p&&p->type==c::Json::Type::string?p->string:"";}
uint64_t decimal(const c::Json& value,const char* field){uint64_t n=0;c::decimal(value.get(field),n);return n;}
m::MessageId fresh(){return m::MessageId::generate([](uint8_t* bytes,size_t count){esp_fill_random(bytes,count);return true;}).value_or(m::MessageId{});}
std::string hex(std::string_view bytes){const char alphabet[]="0123456789abcdef";std::string out(2*bytes.size(),'0');for(size_t n=0;n<bytes.size();++n){const auto b=uint8_t(bytes[n]);out[2*n]=alphabet[b>>4];out[2*n+1]=alphabet[b&15];}return out;}
bool unhex(std::string_view bytes,std::string& out){if(bytes.size()%2||bytes.size()>4096)return false;out.clear();for(size_t n=0;n<bytes.size();n+=2){const auto digit=[](char b){return b>='0'&&b<='9'?b-'0':b>='a'&&b<='f'?b-'a'+10:-1;};int a=digit(bytes[n]),b=digit(bytes[n+1]);if(a<0||b<0)return false;out+=char(a*16+b);}return true;}
std::string base(m::MessageId id,const ReleaseManifest& release){return "\"request_id\":"+quote(id.str())+",\"release_id\":"+quote(release.release_id)+",\"image_sha256\":"+quote(release.image_sha256);}
std::string report(){
    std::string out="{\"version\":1,\"job_id\":"+quote(cohort.id?cohort.id.str():"")+",\"phase\":"+quote(cohort.phase)+",\"error\":"+quote(cohort.error)+",\"manifest\":"+quote(cohort.manifest)+",\"signature_hex\":"+quote(cohort.signature)+",\"config_revision\":\""+std::to_string(cohort.config_revision)+"\",\"current_index\":\""+std::to_string(cohort.current)+"\",\"image_bytes\":\""+std::to_string(cohort.release.image_bytes)+"\",\"release_id\":"+quote(cohort.release.release_id)+",\"image_sha256\":"+quote(cohort.release.image_sha256)+",\"targets\":[";
    for(size_t n=0;n<cohort.targets.size();++n){const auto& target=cohort.targets[n];if(n)out+=',';out+="{\"octopus_id\":"+quote(target.owner.str())+",\"previous_boot_id\":"+quote(target.previous_boot?target.previous_boot.str():"")+",\"offset\":\""+std::to_string(target.offset)+"\",\"phase\":"+quote(target.phase)+",\"last_offset\":\""+std::to_string(target.last_offset)+"\",\"last_hash\":"+quote(target.last_hash)+"}";}
    return out+"]}";
}
esp_err_t persist(){const auto bytes=report();return config_store::write("ota_cohort","AMITYCOHORT1\n"+config_store::sha256_hex(bytes)+"\n"+bytes);}
std::optional<c::RuntimePeerStatus> peer(m::OctopusId owner){for(const auto& p:c::runtime_peers())if(p.id==owner)return p;return {};}
bool exchange(m::OctopusId owner,c::Kind kind,std::string payload,m::MessageId request,c::Json& response,uint32_t timeout=7000,const ReleaseManifest* expected=nullptr){
    std::lock_guard<std::mutex> one(exchange_gate);
    {std::lock_guard<std::mutex> lock(ack_gate);const auto& release=expected?*expected:cohort.release;pending=Pending{owner,request,release.release_id,release.image_sha256,{}};}
    if(c::runtime_send_update(owner,kind,std::move(payload),5000)!=ESP_OK){std::lock_guard<std::mutex> lock(ack_gate);pending.reset();return false;}
    std::unique_lock<std::mutex> lock(ack_gate);const bool received=changed.wait_for(lock,std::chrono::milliseconds(timeout),[]{return pending&&pending->result.has_value();});
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
    uint64_t revision=hooks.applied_config_revision?hooks.applied_config_revision():0;
    std::string out="{\"request_id\":"+quote(string(envelope.payload,"request_id"))+",\"release_id\":"+quote(string(envelope.payload,"release_id"))+",\"image_sha256\":"+quote(string(envelope.payload,"image_sha256"))+",\"offset\":"+quote(string(report,"received_bytes").empty()?"0":string(report,"received_bytes"))+",\"stage\":"+quote(stage)+",\"error\":"+quote(error)+",\"boot_id\":"+quote(boot_id())+",\"image_elf_sha256\":"+quote(string(report,"running_elf_sha256"))+",\"version\":"+quote(esp_app_get_description()->version)+",\"local_healthy\":"+(firmware_ready()?"true":"false")+",\"config_revision\":\""+std::to_string(revision)+"\"";
    if(hooks.applied_policy_hash){const auto policy=hooks.applied_policy_hash();if(policy.size()==64)out+=",\"policy_hash\":"+quote(policy);}
    out+=",\"update_phase\":"+quote(string(report,"phase"));
    return out+"}";
}
c::Error receive(m::OctopusId owner,const c::Envelope& envelope){
    std::string error;esp_err_t applied=ESP_ERR_INVALID_ARG;const char* stage="rejected";const char* code="INVALID_MESSAGE";
    const auto& request=envelope.payload;const auto release_id=string(request,"release_id"),image_hash=string(request,"image_sha256");
    bool matching=false;{std::lock_guard<std::mutex> lock(incoming_gate);matching=incoming_begin&&incoming_owner==owner&&incoming_release==release_id&&incoming_hash==image_hash;}
    if(envelope.kind!=c::Kind::update_begin&&envelope.kind!=c::Kind::update_status&&!matching){
        // An exact already-aborted durable job can acknowledge cleanup after a
        // reconnect. It cannot cancel a newer image or another active upload.
        c::Json saved;c::JsonError detail;const bool inactive=!installation_active()&&c::parse_json(status_json(),saved,detail)&&string(saved,"release_id")==release_id&&string(saved,"expected_image_sha256")==image_hash;
        if(envelope.kind!=c::Kind::update_abort||!inactive){c::runtime_send_update(owner,c::Kind::update_result,receiver_result(envelope,"rejected","CONFLICT"),5000);return c::Error::transaction_conflict;}
    }
    if(envelope.kind==c::Kind::update_begin){ReleaseManifest manifest;
        if(parse_release_manifest(string(request,"canonical_manifest"),manifest,error)&&manifest.release_id==release_id&&manifest.image_sha256==image_hash){
            const auto* validation=request.get("validate_only");const bool validate_only=validation&&validation->type==c::Json::Type::boolean&&validation->boolean;
            applied=validate_only?stage_release(manifest.canonical,string(request,"signature_hex"),error):peer_begin(manifest.canonical,string(request,"signature_hex"),error);
            code=applied==ESP_ERR_INVALID_ARG?"INVALID_MANIFEST":"NOT_READY";
            if(applied==ESP_OK){stage="ready";if(!validate_only){std::lock_guard<std::mutex> lock(incoming_gate);incoming_owner=owner;incoming_begin=*m::MessageId::parse(string(request,"request_id"));incoming_release=release_id;incoming_hash=image_hash;}}
        }
    }else if(envelope.kind==c::Kind::update_chunk){std::string bytes;if(unhex(string(request,"data_hex"),bytes)&&config_store::sha256_hex(bytes)==string(request,"chunk_sha256")){applied=peer_chunk(release_id,decimal(request,"offset"),bytes,error);code=applied==ESP_ERR_INVALID_ARG?"OFFSET_MISMATCH":"STORAGE_FAILURE";if(applied==ESP_OK)stage="progress";}}
    else if(envelope.kind==c::Kind::update_install){applied=peer_install(release_id,error);code="HASH_MISMATCH";if(applied==ESP_OK)stage="installing";}
    else if(envelope.kind==c::Kind::update_abort){peer_abort("Coordinator cancelled the peer update");applied=ESP_OK;stage="aborted";}
    else if(envelope.kind==c::Kind::update_status){applied=ESP_OK;stage="status";}
    else return c::Error::unsupported_kind;
    if(c::runtime_send_update(owner,c::Kind::update_result,receiver_result(envelope,stage,applied==ESP_OK?"":code),5000)!=ESP_OK)return c::Error::queue_full;
    return applied==ESP_OK?c::Error::none:applied==ESP_ERR_INVALID_ARG?c::Error::invalid_message:applied==ESP_ERR_INVALID_STATE?c::Error::not_ready:c::Error::storage_failure;
}
void disconnected(m::OctopusId owner,m::MessageId begin){std::lock_guard<std::mutex> lock(incoming_gate);if(incoming_owner==owner&&incoming_begin==begin){incoming_begin={};peer_abort("The authenticated update connection closed; current firmware retained");}}
void recover(){
    std::string saved;if(config_store::read("ota_cohort",saved)!=ESP_OK)return;
    if(saved.size()<78||saved.substr(0,13)!="AMITYCOHORT1\n"||saved[77]!='\n'||config_store::sha256_hex(std::string_view(saved).substr(78))!=saved.substr(13,64)){cohort.phase="recovery_required";cohort.error="Update job integrity failed; no installation was resumed";return;}
    saved.erase(0,78);c::Json value;c::JsonError detail;Cohort recovered;
    const auto invalid=[&]{cohort.phase="recovery_required";cohort.error="Update job is invalid; no installation or image transfer was resumed";};
    uint64_t version=0,index=0,bytes=0;
    const auto hex_value=[](std::string_view value,size_t length){return value.size()==length&&std::all_of(value.begin(),value.end(),[](char b){return (b>='0'&&b<='9')||(b>='a'&&b<='f');});};
    const auto phase_allowed=[](const std::string& phase,std::initializer_list<const char*> allowed){return std::any_of(allowed.begin(),allowed.end(),[&](const char* name){return phase==name;});};
    if(!c::parse_json(saved,value,detail,16384)||!c::closed(value,{"version","job_id","phase","error","manifest","signature_hex","config_revision","current_index","image_bytes","release_id","image_sha256","targets"})||
       !c::json_uint(value.get("version"),version,1,1)||!c::id(value.get("job_id"),recovered.id)||!parse_release_manifest(string(value,"manifest"),recovered.release,recovered.error)||
       !c::decimal(value.get("config_revision"),recovered.config_revision)||!recovered.config_revision||!c::decimal(value.get("current_index"),index)||!c::decimal(value.get("image_bytes"),bytes)||bytes!=recovered.release.image_bytes||
       string(value,"release_id")!=recovered.release.release_id||string(value,"image_sha256")!=recovered.release.image_sha256||!c::json_string(value.get("error"),recovered.error,0,512)) {invalid();return;}
    recovered.manifest=string(value,"manifest");recovered.phase=string(value,"phase");
    if(!c::json_string(value.get("signature_hex"),recovered.signature,0,128)){invalid();return;}
    if((!recovered.signature.empty()&&!hex_value(recovered.signature,128))||!phase_allowed(recovered.phase,{"staged","starting","receiving","boot_checking","next_target","complete","aborted","failed","interrupted","rolled_back"})){invalid();return;}
    const auto* targets=value.get("targets");if(!targets||targets->type!=c::Json::Type::array||targets->array.empty()||targets->array.size()>3||index>targets->array.size()){invalid();return;}
    recovered.current=static_cast<size_t>(index);std::set<m::OctopusId> seen;
    for(const auto& item:targets->array){
        Target target;const auto previous=m::BootId::parse(string(item,"previous_boot_id"));
        if(!c::closed(item,{"octopus_id","previous_boot_id","offset","phase","last_offset","last_hash"})||!c::id(item.get("octopus_id"),target.owner)||!seen.insert(target.owner).second||!previous||
           !c::decimal(item.get("offset"),target.offset)||!c::decimal(item.get("last_offset"),target.last_offset)){invalid();return;}
        target.previous_boot=*previous;target.phase=string(item,"phase");
        if(!c::json_string(item.get("last_hash"),target.last_hash,0,64)){invalid();return;}
        if(target.offset>recovered.release.image_bytes||target.last_offset>target.offset||(target.offset%2048&&target.offset!=recovered.release.image_bytes)||
           (!target.offset&&(!target.last_hash.empty()||target.last_offset))||(target.offset&&(!hex_value(target.last_hash,64)||target.last_offset>=target.offset||target.offset-target.last_offset>2048||target.last_offset%2048))||
           !phase_allowed(target.phase,{"queued","maintenance","receiving","boot_checking","healthy","aborted","failed","rolled_back"})){invalid();return;}
        const auto position=recovered.targets.size();
        if((target.phase=="healthy"&&target.offset!=recovered.release.image_bytes)||(target.phase=="queued"&&target.offset)||(position<recovered.current&&target.phase!="healthy")||(position>recovered.current&&target.phase!="queued")||(target.owner==local&&position+1!=targets->array.size())){invalid();return;}
        recovered.targets.push_back(std::move(target));
    }
    if((recovered.phase=="complete")!=(recovered.current==recovered.targets.size())||
       (recovered.phase=="boot_checking"&&(recovered.targets[recovered.current].phase!="boot_checking"||recovered.targets[recovered.current].offset!=recovered.release.image_bytes))||
       ((recovered.phase=="staged"||recovered.phase=="next_target")&&recovered.targets[recovered.current].phase!="queued")){invalid();return;}
    cohort=std::move(recovered);
    if(cohort.phase=="receiving"||cohort.phase=="starting"){cohort.phase="interrupted";cohort.error="Reselect the same release to restart the interrupted target; completed targets are retained";}
    if(cohort.current<cohort.targets.size()&&cohort.targets[cohort.current].phase!="queued"&&hooks.owner_maintenance)hooks.owner_maintenance(cohort.targets[cohort.current].owner,true);
}
bool healthy(const c::Json& value,const Target& target,const c::RuntimePeerStatus& current){
    m::BootId boot;const auto* health=value.get("local_healthy");return c::id(value.get("boot_id"),boot)&&boot==current.boot_id&&boot!=target.previous_boot&&current.firmware_identity==cohort.release.image_elf_sha256&&string(value,"image_elf_sha256")==cohort.release.image_elf_sha256&&string(value,"version")==cohort.release.firmware_version&&health&&health->type==c::Json::Type::boolean&&health->boolean&&decimal(value,"config_revision")==cohort.config_revision;
}
bool sender_authority(std::string& error,bool revision=true){const auto status=c::runtime_status();if(!status.initialized||!status.coordinator||!status.authority_ready||status.member||status.phase=="retired"){error="Main Amity authority is required for this update job";return false;}if(!hooks.applied_config_revision||!hooks.applied_config_revision()||(revision&&hooks.applied_config_revision()!=cohort.config_revision)){error="Configuration changed; reconcile this update job before continuing";return false;}return true;}
void poll(){
    if(cohort.phase!="boot_checking"||cohort.current>=cohort.targets.size())return;
    auto& target=cohort.targets[cohort.current];bool complete=false,rolled_back=false,failed=false;
    if(target.owner==local){c::Json status;c::JsonError error;if(c::parse_json(status_json(),status,error)){const auto current=m::BootId::parse(boot_id());complete=current&&*current!=target.previous_boot&&firmware_ready()&&string(status,"running_elf_sha256")==cohort.release.image_elf_sha256&&string(status,"running_version")==cohort.release.firmware_version&&hooks.applied_config_revision&&hooks.applied_config_revision()==cohort.config_revision;rolled_back=current&&*current!=target.previous_boot&&firmware_ready()&&string(status,"running_elf_sha256")!=cohort.release.image_elf_sha256;failed=string(status,"phase")=="failed"||string(status,"phase")=="interrupted";}}
    else {auto current=peer(target.owner);if(!current||!current->ready)return;c::Json response;const auto id=fresh();if(exchange(target.owner,c::Kind::update_status,"{"+base(id,cohort.release)+"}",id,response,1000)){const auto after=peer(target.owner);complete=after&&after->ready&&healthy(response,target,*after);rolled_back=after&&string(response,"boot_id")==after->boot_id.str()&&after->boot_id!=target.previous_boot&&string(response,"image_elf_sha256")!=cohort.release.image_elf_sha256;failed=string(response,"update_phase")=="failed"||string(response,"update_phase")=="interrupted"||string(response,"update_phase")=="aborted";}}
    if(rolled_back||failed){cohort.phase=rolled_back?"rolled_back":"failed";target.phase=cohort.phase;cohort.error=rolled_back?"Target booted a different image; its previous image was retained":"Target reports an interrupted or rejected installation";persist();return;}
    if(!complete)return;
    target.phase="healthy";if(hooks.owner_maintenance)hooks.owner_maintenance(target.owner,false);++cohort.current;cohort.phase=cohort.current==cohort.targets.size()?"complete":"next_target";cohort.error.clear();persist();
}
}
void start_group_updates(m::OctopusId owner,CohortHooks callbacks){std::lock_guard<std::mutex> lock(gate);local=owner;hooks=std::move(callbacks);recover();c::runtime_update_hooks({receive,result,disconnected});}
esp_err_t stage_cohort(std::string_view manifest,std::string_view proof,std::vector<m::OctopusId> targets,std::string& error){
    ReleaseManifest release;if(!parse_release_manifest(manifest,release,error)||targets.empty()||targets.size()>3)return ESP_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lock(gate);
    if(!sender_authority(error,false))return ESP_ERR_INVALID_STATE;
    if(cohort.phase=="receiving"||cohort.phase=="starting"||cohort.phase=="boot_checking"){error="Another cohort update is in progress";return ESP_ERR_INVALID_STATE;}
    std::set<m::OctopusId> seen;for(auto target:targets){if(!target||!seen.insert(target).second)return ESP_ERR_INVALID_ARG;if(target!=local){const auto status=peer(target);if(!status||(!status->ready&&!status->management_ready)||!(status->negotiated_capabilities&c::peer_firmware_update)){error="Every selected executor must support managed updates and be synchronized before an update starts";return ESP_ERR_INVALID_STATE;}}}
    const bool incomplete=cohort.phase=="failed"||cohort.phase=="interrupted"||cohort.phase=="rolled_back";
    const bool resume=incomplete||(cohort.phase=="aborted"&&cohort.manifest==manifest&&cohort.targets.size()==seen.size()&&std::all_of(cohort.targets.begin(),cohort.targets.end(),[&](const auto& target){return seen.count(target.owner)!=0;}));
    if(resume&&cohort.current>=cohort.targets.size()){error="The previous job has no incomplete target";return ESP_ERR_INVALID_STATE;}
    if(resume){
        std::set<m::OctopusId> previous;for(const auto& target:cohort.targets)previous.insert(target.owner);
        if(cohort.manifest!=manifest||previous!=seen){error="Cancel the previous incomplete update before choosing another release or cohort";return ESP_ERR_INVALID_STATE;}
        if(cohort.current<cohort.targets.size()){
            const auto& target=cohort.targets[cohort.current];bool confirmed=target.owner==local;
            if(confirmed)peer_abort("Restarting this explicitly selected image transfer");
            else {const auto id=fresh();c::Json response;confirmed=exchange(target.owner,c::Kind::update_abort,"{"+base(id,cohort.release)+",\"reason\":\"cancelled\"}",id,response)&&string(response,"stage")=="aborted";}
            if(!confirmed){error="Target could not confirm cleanup of the previous image transfer";return ESP_ERR_TIMEOUT;}
        }
    }
    if(stage_release(manifest,proof,error)!=ESP_OK)return ESP_ERR_INVALID_ARG;
    // Every target independently checks its compiled verifier/profile/storage
    // compatibility before image bytes are sent. Current executors validate
    // without maintenance; an older management-only peer uses its original
    // begin/abort protocol while execution remains quarantined by the runtime.
    for(bool legacy_phase:{false,true})for(auto target:targets)if(target!=local){
        const auto status=peer(target);const bool legacy=status&&status->management_ready&&!(status->negotiated_capabilities&c::qualified_lookup);
        if(legacy!=legacy_phase)continue;
        const auto id=fresh();c::Json response;
        const auto request="{"+base(id,release)+",\"canonical_manifest\":"+quote(manifest)+",\"signature_hex\":"+quote(proof)+(legacy?"}":",\"validate_only\":true}");
        const bool valid=exchange(target,c::Kind::update_begin,request,id,response,7000,&release)&&string(response,"stage")=="ready"&&string(response,"error").empty();
        if(legacy){
            const auto abort=fresh();c::Json cleanup;
            if(!exchange(target,c::Kind::update_abort,"{"+base(abort,release)+",\"reason\":\"cancelled\"}",abort,cleanup,7000,&release)||string(cleanup,"stage")!="aborted"){
                error="An older executor could not confirm cleanup of its compatibility check; no image was sent and execution remains quarantined";return ESP_ERR_INVALID_STATE;
            }
        }
        if(!valid){error="A selected executor rejected or did not acknowledge independent release compatibility; no new image transfer was started";return ESP_ERR_INVALID_STATE;}
    }
    if(resume){
        auto& target=cohort.targets[cohort.current];target.offset=0;target.last_hash.clear();target.last_offset=0;target.phase="queued";target.previous_boot=target.owner==local?*m::BootId::parse(boot_id()):peer(target.owner)->boot_id;
        cohort.phase="staged";cohort.error.clear();cohort.config_revision=hooks.applied_config_revision?hooks.applied_config_revision():0;return persist();
    }
    Cohort next;next.id=fresh();next.release=std::move(release);next.manifest=manifest;next.signature=proof;next.phase="staged";next.config_revision=hooks.applied_config_revision?hooks.applied_config_revision():0;
    // Keep the integration gateway/coordinator last. Target ordering otherwise
    // preserves the administrator's selected dependency order.
    for(auto target:targets)if(target!=local)next.targets.push_back({target,peer(target)->boot_id,0,"queued",0,{}});
    if(seen.count(local))next.targets.push_back({local,*m::BootId::parse(boot_id()),0,"queued",0,{}});
    cohort=std::move(next);return persist();
}
esp_err_t cohort_chunk(m::MessageId job,m::OctopusId owner,uint64_t offset,std::string_view bytes,std::string& error){
    std::lock_guard<std::mutex> lock(gate);if(!sender_authority(error))return ESP_ERR_INVALID_STATE;if(cohort.id!=job||cohort.current>=cohort.targets.size()||cohort.targets[cohort.current].owner!=owner||(cohort.phase!="staged"&&cohort.phase!="next_target"&&cohort.phase!="receiving")){error="No matching selected target is ready for image transfer";return ESP_ERR_INVALID_STATE;}
    auto& target=cohort.targets[cohort.current];
    if(!bytes.empty()&&bytes.size()<=2048&&!target.last_hash.empty()&&offset==target.last_offset&&offset+bytes.size()==target.offset&&config_store::sha256_hex(bytes)==target.last_hash)return ESP_OK;
    if(offset!=target.offset||bytes.empty()||bytes.size()>2048||offset+bytes.size()>cohort.release.image_bytes){error="Browser offset differs from the target checkpoint";return ESP_ERR_INVALID_ARG;}
    c::Json response;
    if(cohort.phase!="receiving"){
        cohort.phase="starting";target.phase="maintenance";if(persist()!=ESP_OK)return ESP_FAIL;if(hooks.owner_maintenance)hooks.owner_maintenance(target.owner,true);
        bool accepted=false;if(target.owner==local)accepted=peer_begin(cohort.manifest,cohort.signature,error)==ESP_OK;
        else {const auto id=fresh();accepted=exchange(target.owner,c::Kind::update_begin,"{"+base(id,cohort.release)+",\"canonical_manifest\":"+quote(cohort.manifest)+",\"signature_hex\":"+quote(cohort.signature)+"}",id,response)&&string(response,"stage")=="ready"&&decimal(response,"offset")==0;}
        if(!accepted){cohort.phase="failed";cohort.error=error.empty()?"Target did not acknowledge release validation and maintenance":error;persist();error=cohort.error;return ESP_FAIL;}
        cohort.phase="receiving";target.phase="receiving";
    }
    bool accepted=false;if(target.owner==local)accepted=peer_chunk(cohort.release.release_id,offset,bytes,error)==ESP_OK;
    else {const auto id=fresh();accepted=exchange(target.owner,c::Kind::update_chunk,"{"+base(id,cohort.release)+",\"offset\":\""+std::to_string(offset)+"\",\"data_hex\":"+quote(hex(bytes))+",\"chunk_sha256\":"+quote(config_store::sha256_hex(bytes))+"}",id,response)&&string(response,"stage")=="progress"&&decimal(response,"offset")==offset+bytes.size();}
    if(!accepted){cohort.phase="failed";cohort.error=error.empty()?"Target did not acknowledge this exact image checkpoint":error;persist();error=cohort.error;return ESP_FAIL;}
    target.last_offset=target.offset;target.last_hash=config_store::sha256_hex(bytes);target.offset+=bytes.size();if(persist()!=ESP_OK){cohort.phase="failed";error="Coordinator image checkpoint could not be stored";return ESP_FAIL;}return ESP_OK;
}
esp_err_t cohort_install(m::MessageId job,m::OctopusId owner,std::string& error){
    std::lock_guard<std::mutex> lock(gate);if(!sender_authority(error))return ESP_ERR_INVALID_STATE;if(cohort.id!=job||cohort.phase!="receiving"||cohort.current>=cohort.targets.size()||cohort.targets[cohort.current].owner!=owner||cohort.targets[cohort.current].offset!=cohort.release.image_bytes){error="The exact current target has not received the whole image";return ESP_ERR_INVALID_STATE;}
    auto& target=cohort.targets[cohort.current];cohort.phase="boot_checking";target.phase="boot_checking";if(persist()!=ESP_OK)return ESP_FAIL;
    bool accepted=false;if(target.owner==local)accepted=peer_install(cohort.release.release_id,error)==ESP_OK;
    else {const auto id=fresh();c::Json response;accepted=exchange(target.owner,c::Kind::update_install,"{"+base(id,cohort.release)+"}",id,response)&&string(response,"stage")=="installing";}
    if(!accepted){cohort.error="Install acknowledgment was lost or rejected; check the target's new boot and image before retrying";persist();error=cohort.error;return ESP_ERR_TIMEOUT;}return ESP_OK;
}
esp_err_t cancel_cohort(m::MessageId job,std::string& error){
    std::lock_guard<std::mutex> lock(gate);if(cohort.id!=job){error="Update job changed";return ESP_ERR_INVALID_STATE;}if(cohort.current>=cohort.targets.size())return ESP_OK;auto& target=cohort.targets[cohort.current];
    if(cohort.phase=="boot_checking"){error="Image selection may already have occurred; wait for its boot report";return ESP_ERR_INVALID_STATE;}
    bool confirmed=target.phase=="queued"||target.owner==local;if(target.owner==local)peer_abort("Administrator cancelled the update");
    else if(!confirmed){const auto id=fresh();c::Json response;confirmed=exchange(target.owner,c::Kind::update_abort,"{"+base(id,cohort.release)+",\"reason\":\"cancelled\"}",id,response)&&string(response,"stage")=="aborted";}
    if(!confirmed){error="Target is unavailable; its maintenance fence remains until a verified reconciliation";return ESP_ERR_TIMEOUT;}
    if(hooks.owner_maintenance)hooks.owner_maintenance(target.owner,false);
    target.phase="aborted";cohort.phase="aborted";cohort.error.clear();return persist();
}
std::string cohort_status_json(){std::lock_guard<std::mutex> lock(gate);poll();return report();}
void poll_group_update(){std::lock_guard<std::mutex> lock(gate);poll();}
} // namespace system_services::updates
