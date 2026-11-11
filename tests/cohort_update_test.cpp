#include "group_firmware_update.hpp"
#include "amity/consortium/update.hpp"
#include "config_store.hpp"
#include "system_services.hpp"
#include "esp_app_desc.h"
#include <openssl/sha.h>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sys/wait.h>
#include <unistd.h>
namespace c=amity::consortium;namespace m=amity::system;namespace u=system_services::updates;
template<class T>T identity(unsigned n){char text[33];std::snprintf(text,sizeof(text),"%032x",n);return *T::parse(text);}
const auto local=identity<m::OctopusId>(1),one=identity<m::OctopusId>(2),two=identity<m::OctopusId>(3);
uint64_t clock_ms=100;
int64_t esp_timer_get_time(){return int64_t(clock_ms*1000);}
struct Node {
    m::BootId boot;std::string elf=std::string(64,'e'),version="old",phase="idle",bytes;
    std::optional<u::ReleaseManifest> received,staged;m::MessageId accepted_begin;
    uint64_t reboot_at=0;bool healthy=true,selected=false,maintenance=false;
};
struct Event {m::OctopusId owner;std::string action;uint64_t time;};
std::map<m::OctopusId,Node> nodes;
std::vector<c::RuntimePeerStatus> peers;std::vector<Event> events;c::RuntimeUpdateHooks transport;c::RuntimeStatus runtime_role;
std::vector<std::pair<m::OctopusId,bool>> maintenance_fences;
bool interrupt_next_chunk=false,missing_running_release=false,release_allows_sync=false;
m::OctopusId destination=local;u::ReleaseManifest uploaded_release,running_release;
std::string manifest,staged_bytes(4096,'s'),running_bytes(4096,'r');std::set<m::OctopusId> reject_stage;
unsigned disk_writes=0,staged_reads=0,running_reads=0,chunk_admissions=0,chunk_results=0,stage_attempts=0,full_chunk_attempts=0,full_stage_attempts=0;
bool corrupt_next_chunk=false,drop_next_chunk=false;
c::Json parse(std::string_view body){c::Json value;c::JsonError error;assert(c::parse_json(body,value,error));return value;}
std::string field(const c::Json& value,const char* key){const auto* v=value.get(key);return v&&v->type==c::Json::Type::string?v->string:"";}
std::string sha(std::string_view data){unsigned char bytes[32];SHA256(reinterpret_cast<const unsigned char*>(data.data()),data.size(),bytes);char text[65]{};for(unsigned n=0;n<32;++n)std::snprintf(text+2*n,3,"%02x",bytes[n]);return text;}
void event(std::string action){events.push_back({destination,std::move(action),clock_ms});}
void esp_fill_random(void* data,size_t size){static uint64_t n=1;std::memset(data,0,size);auto* bytes=static_cast<unsigned char*>(data);for(size_t i=0;i<size&&i<8;++i)bytes[size-1-i]=static_cast<unsigned char>(n>>(8*i));++n;}
const esp_app_desc_t* esp_app_get_description(){static esp_app_desc_t app{};std::snprintf(app.version,sizeof(app.version),"%s",nodes[destination].version.c_str());for(size_t n=0;n<32;++n)app.app_elf_sha256[n]=uint8_t(std::stoul(nodes[destination].elf.substr(2*n,2),nullptr,16));return &app;}
namespace config_store {
esp_err_t read(const char*,std::string&){assert(false);return ESP_FAIL;}
esp_err_t write(const char*,const std::string&){++disk_writes;assert(false);return ESP_FAIL;}
std::string sha256_hex(std::string_view data){return sha(data);}
}
namespace system_services {
const char* boot_id(){static std::string text;text=nodes[destination].boot.str();return text.c_str();}
bool firmware_ready(){return nodes[destination].healthy;}
}
namespace system_services::updates {
std::string status_json(){const auto& n=nodes[destination];return "{\"phase\":\""+n.phase+"\",\"received_bytes\":\""+std::to_string(n.bytes.size())+"\"}";}
esp_err_t partition_release(bool running,ReleaseManifest& release,std::string& signature,std::string& error){
    signature.clear();if(running){assert(destination==local);if(missing_running_release){error="Running firmware release could not be verified";return ESP_FAIL;}release=running_release;return ESP_OK;}
    if(!nodes[destination].staged){error="No durable staged image";return ESP_ERR_INVALID_STATE;}release=*nodes[destination].staged;return ESP_OK;
}
esp_err_t read_running_image(uint64_t offset,size_t count,std::string& bytes,std::string&){assert(destination==local&&offset+count<=running_bytes.size());++running_reads;bytes=running_bytes.substr(offset,count);return ESP_OK;}
esp_err_t read_staged_image(uint64_t offset,size_t count,std::string& bytes,std::string&){assert(destination==local&&nodes[local].staged&&offset+count<=staged_bytes.size());++staged_reads;bytes=staged_bytes.substr(offset,count);return ESP_OK;}
esp_err_t peer_begin(std::string_view body,std::string_view,std::string& error){
    event("begin");ReleaseManifest release;if(!parse_release_manifest(body,release,error))return ESP_ERR_INVALID_ARG;
    auto& n=nodes[destination];n.received=release;n.bytes.clear();n.phase="installing";n.maintenance=true;return ESP_OK;
}
esp_err_t peer_chunk(std::string_view release,uint64_t offset,std::string_view bytes,std::string&){
    auto& n=nodes[destination];if(!n.received||n.received->release_id!=release||offset!=n.bytes.size())return ESP_ERR_INVALID_ARG;
    event("chunk");n.bytes.append(bytes);if(corrupt_next_chunk){corrupt_next_chunk=false;n.bytes.back()^=1;}return ESP_OK;
}
esp_err_t peer_stage(std::string_view release,std::string&){
    auto& n=nodes[destination];if(reject_stage.count(destination)||!n.received||n.received->release_id!=release||n.bytes.size()!=n.received->image_bytes||sha(n.bytes)!=n.received->image_sha256)return ESP_ERR_INVALID_STATE;
    event("stage");n.staged=n.received;n.received.reset();n.phase="staged_image";n.maintenance=false;return ESP_OK;
}
esp_err_t peer_install(std::string_view release,std::string&,uint32_t delay){
    auto& n=nodes[destination];if(!n.staged||n.staged->release_id!=release||delay>15000)return ESP_ERR_INVALID_STATE;
    event("activate");n.selected=true;n.reboot_at=clock_ms+delay;n.phase="boot_checking";return ESP_OK;
}
void peer_abort(const std::string&){event("abort");auto& n=nodes[destination];n.received.reset();n.maintenance=false;if(n.staged)n.phase="staged_image";else {n.phase="aborted";n.bytes.clear();}}
}
namespace amity::consortium {
RuntimeStatus runtime_status(){return runtime_role;}
std::vector<RuntimePeerStatus> runtime_peers(){return peers;}
void runtime_update_hooks(RuntimeUpdateHooks hooks){transport=std::move(hooks);}
esp_err_t runtime_send_update(m::OctopusId to,Kind kind,std::string body,uint32_t){
    Json value;UpdateChunk chunk;
    if(kind==Kind::update_chunk){assert(decode_update_chunk(body,chunk)&&body.size()==24+chunk.bytes.size());}
    else {value=parse(body);assert(valid_update(kind,value));}
    if(kind==Kind::update_result){if(field(value,"stage")=="progress")++chunk_results;transport.result(destination,value);return ESP_OK;} // Wire admission is independent of accepting a late completion.
    assert(to!=local); // The primary stages one browser upload locally.
    const auto linked=std::find_if(peers.begin(),peers.end(),[&](const auto& peer){return peer.id==to;});
    if(linked==peers.end()||(!linked->ready&&!linked->management_ready))return ESP_ERR_INVALID_STATE;
    if(kind==Kind::update_chunk){
        if(full_chunk_attempts){--full_chunk_attempts;return ESP_ERR_NO_MEM;}
        ++chunk_admissions;
        if(drop_next_chunk){drop_next_chunk=false;return ESP_OK;}
    }
    if(kind==Kind::update_install&&field(value,"phase")=="stage"){
        ++stage_attempts;if(full_stage_attempts){--full_stage_attempts;return ESP_ERR_NO_MEM;}
    }
    assert(kind==Kind::update_begin||kind==Kind::update_chunk||kind==Kind::update_install||kind==Kind::update_status||kind==Kind::update_abort);
    const auto old=destination;destination=to;
    Envelope envelope;envelope.kind=kind;envelope.payload=value;envelope.origin_octopus_id=local;envelope.destination_octopus_id=to;
    envelope.origin_boot_id=nodes[local].boot;envelope.destination_boot_id=nodes[to].boot;envelope.config_revision=7;envelope.coordinator_epoch=1;
    envelope.consortium_id=identity<m::ConsortiumId>(100);envelope.ttl_ms=5000;
    if(kind==Kind::update_chunk){
        const auto frame=update_chunk_envelope(envelope,body);
        assert(frame.size()==128+chunk.bytes.size()&&parse_envelope(frame,envelope)==Error::none);
        assert(envelope.update_chunk&&envelope.update_chunk->bytes==chunk.bytes);
    }
    const bool interrupted=kind==Kind::update_chunk&&interrupt_next_chunk;
    if(interrupted)interrupt_next_chunk=false;
    const auto handled=transport.handle(local,envelope);
    if(handled==Error::none&&kind==Kind::update_begin)nodes[to].accepted_begin=*m::MessageId::parse(field(value,"request_id"));
    if(interrupted){transport.disconnected(local,nodes[to].accepted_begin);for(auto& peer:peers)if(peer.id==to)peer.ready=peer.management_ready=false;}
    destination=old;return ESP_OK;
}
esp_err_t runtime_stage_update(m::OctopusId to,std::string body,uint32_t ttl){
    assert(field(parse(body),"phase")=="stage");
    return runtime_send_update(to,Kind::update_install,std::move(body),ttl);
}
}
c::Json cohort(){return parse(u::cohort_status_json());}
std::string phase(){return field(cohort(),"phase");}
c::RuntimePeerStatus& add_peer(m::OctopusId owner,bool online=true,bool management=false){
    auto& n=nodes[owner];n.boot=identity<m::BootId>(owner==one?11:12);
    c::RuntimePeerStatus peer;peer.id=owner;peer.boot_id=n.boot;peer.enrolled=true;peer.ready=online&&!management;peer.management_ready=online&&management;peer.negotiated_capabilities=c::known_capabilities;peer.firmware_identity=n.elf;peers.push_back(peer);return peers.back();
}
void setup(){
    // Exercise every byte value, including NUL and JSON delimiters, on the wire.
    for(size_t i=0;i<staged_bytes.size();++i){staged_bytes[i]=char(i);running_bytes[i]=char(255-i);}
    std::string error;assert(u::parse_release_manifest(manifest,uploaded_release,error));
    const auto replace=[](std::string& text,const std::string& from,const std::string& to){const auto at=text.find(from);assert(at!=std::string::npos);text.replace(at,from.size(),to);};
    replace(uploaded_release.canonical,uploaded_release.image_sha256,sha(staged_bytes));uploaded_release.image_sha256=sha(staged_bytes);
    running_release=uploaded_release;
    running_release.release_id="running-release";running_release.image_elf_sha256=std::string(64,'d');
    running_release.canonical=uploaded_release.canonical;auto at=running_release.canonical.find(uploaded_release.release_id);assert(at!=std::string::npos);running_release.canonical.replace(at,uploaded_release.release_id.size(),running_release.release_id);
    at=running_release.canonical.find(uploaded_release.image_elf_sha256);assert(at!=std::string::npos);running_release.canonical.replace(at,64,running_release.image_elf_sha256);
    replace(running_release.canonical,running_release.image_sha256,sha(running_bytes));running_release.image_sha256=sha(running_bytes);
    nodes[local].boot=identity<m::BootId>(10);nodes[local].elf=running_release.image_elf_sha256;
    runtime_role.initialized=runtime_role.coordinator=runtime_role.authority_ready=true;
    u::start_group_updates(local,{[](m::OctopusId owner,bool active){maintenance_fences.emplace_back(owner,active);
        if(release_allows_sync&&owner==one&&!active){assert(field(parse(u::cohort_status_json()),"phase")=="activating");peers.front().ready=true;release_allows_sync=false;}
    },{},{}});assert(phase()=="idle");
}
void upload_once(){
    std::string error;assert(u::begin_system_upload(uploaded_release,error)==ESP_OK&&phase()=="uploading");
    // HTTP upload tests cover writes and validation; model its completed source partition.
    nodes[local].staged=uploaded_release;nodes[local].bytes=staged_bytes;nodes[local].phase="staged_image";event("stage");
    u::system_upload_progress(staged_bytes.size());assert(u::finish_system_upload(error)==ESP_OK&&phase()=="distributing");
}
void poll(){u::poll_group_update();clock_ms+=1000;}
template<class Predicate>void until(Predicate condition,unsigned limit=400){for(unsigned n=0;n<limit&&!condition();++n)poll();if(!condition())std::cerr<<u::cohort_status_json()<<"\n";assert(condition());}
size_t find(m::OctopusId owner,std::string_view action){for(size_t i=0;i<events.size();++i)if(events[i].owner==owner&&events[i].action==action)return i;assert(false);return 0;}
void distribution(){
    setup();add_peer(one);add_peer(two);upload_once();until([]{return nodes[local].selected;});
    assert(nodes[one].bytes==staged_bytes&&nodes[two].bytes==staged_bytes&&staged_reads==4&&running_reads==0);
    assert(find(one,"stage")<find(two,"begin")&&find(two,"stage")<find(one,"activate"));
    assert(find(two,"stage")<find(two,"activate")&&find(two,"activate")<find(local,"activate"));
    assert(nodes[one].reboot_at==nodes[two].reboot_at&&nodes[two].reboot_at==nodes[local].reboot_at);
    assert(phase()=="activating");const auto count=events.size();for(unsigned n=0;n<10;++n)poll();assert(events.size()==count&&disk_writes==0);
    std::cout<<"PASS one primary upload, ordered peer staging before any activation, common delayed restart and no persisted desired release\n";
}
void skipped_offline(){
    setup();add_peer(one,false);add_peer(two);upload_once();const auto started=clock_ms;until([]{return nodes[local].selected;});
    const auto targets=cohort().get("targets")->array;assert(field(targets[1],"phase")=="skipped");
    assert(find(two,"begin")<find(two,"stage")&&find(two,"stage")<find(two,"activate")&&find(two,"activate")<find(local,"activate"));
    assert(events[find(two,"begin")].time-started>=120000&&!nodes[one].selected&&nodes[two].selected);
    assert(nodes[two].bytes==staged_bytes&&clock_ms-started<150000&&disk_writes==0);
    std::cout<<"PASS unavailable peer times out at120seconds; later peers stage and activate with primary\n";
}
void catchup(){
    setup();add_peer(one,true,true);peers.front().negotiated_capabilities=c::peer_firmware_update;
    assert(!peers.front().ready&&peers.front().management_ready);poll();assert(phase()=="distributing"&&cohort().get("automatic")->boolean);
    until([]{return nodes[one].selected;});assert(nodes[one].bytes==running_bytes&&running_reads==2&&staged_reads==0&&!nodes[local].selected);
    assert(find(one,"stage")<find(one,"activate"));
    nodes[one].elf=running_release.image_elf_sha256;nodes[one].boot=identity<m::BootId>(21);peers.front().firmware_identity=nodes[one].elf;peers.front().boot_id=nodes[one].boot;peers.front().ready=true;peers.front().management_ready=false;
    until([]{return phase()=="complete";});const auto count=events.size();for(unsigned n=0;n<4;++n)poll();assert(events.size()==count&&disk_writes==0);
    std::cout<<"PASS newly added/returning update-only peer receives primary running image; matching reconnect completes without another update\n";
}
void matching_hello_releases_sync_fence(){
    alarm(5); // A maintenance callback under the cohort lock would deadlock.
    setup();add_peer(one,true,true);until([]{return nodes[one].selected;});
    assert(maintenance_fences.back()==std::make_pair(one,true));
    nodes[one].elf=running_release.image_elf_sha256;nodes[one].boot=identity<m::BootId>(21);
    peers.front().firmware_identity=nodes[one].elf;peers.front().boot_id=nodes[one].boot;
    peers.front().ready=peers.front().management_ready=false;release_allows_sync=true;
    const auto arrived=clock_ms;poll();
    assert(!release_allows_sync&&peers.front().ready&&phase()=="activating"&&maintenance_fences.back()==std::make_pair(one,false));
    poll();assert(phase()=="complete"&&clock_ms-arrived==2000&&!nodes[local].selected);
    alarm(0);std::cout<<"PASS matching authenticated hello releases sender maintenance before readiness; synchronization then completes catch-up promptly without120s failure\n";
}
void same_image(){
    setup();add_peer(one);peers.front().firmware_identity=nodes[local].elf;
    for(unsigned n=0;n<4;++n)poll();
    assert(phase()=="idle"&&events.empty()&&staged_reads==0&&running_reads==0&&disk_writes==0);
    std::cout<<"PASS matching firmware does no catch-up work\n";
}
void receiver_staging(){
    setup();add_peer(one,true,true);upload_once();until([]{return nodes[one].staged.has_value();});
    const auto accepted=nodes[one].accepted_begin;assert(accepted);destination=one;
    transport.disconnected(local,identity<m::MessageId>(999));assert(nodes[one].staged&&!nodes[one].selected);
    transport.disconnected(local,accepted);assert(nodes[one].staged&&!nodes[one].selected&&nodes[one].phase=="staged_image");
    destination=local;until([]{return nodes[local].selected;});assert(nodes[one].selected);
    std::cout<<"PASS finalized peer image survives authenticated connection cleanup and activates on a fresh stream\n";
}
void retry_transient(){
    setup();add_peer(one,true,true);reject_stage.insert(one);
    const auto begins=[] {size_t count=0;for(const auto& item:events)if(item.owner==one&&item.action=="begin")++count;return count;};
    until([&]{return begins()>=4;},1000);assert(!nodes[one].selected&&!nodes[local].selected);
    reject_stage.clear();until([]{return nodes[one].selected;});assert(nodes[one].bytes==running_bytes&&disk_writes==0);
    std::cout<<"PASS transient transfer failures remain retryable after three attempts without rebooting primary\n";
}
void authority_denials(){
    setup();add_peer(one,true,true);const auto allowed=runtime_role;
    for(unsigned failure=0;failure<5;++failure){runtime_role=allowed;nodes[local].healthy=true;
        if(failure==0)runtime_role.initialized=false;
        if(failure==1)runtime_role.coordinator=false;
        if(failure==2)runtime_role.authority_ready=false;
        if(failure==3)runtime_role.member=true;
        if(failure==4)nodes[local].healthy=false;
        std::string error;assert(u::begin_system_upload(uploaded_release,error)==ESP_ERR_INVALID_STATE);poll();assert(phase()=="idle"&&events.empty());
    }
    assert(disk_writes==0);std::cout<<"PASS current healthy main authority is required for user upload and automatic catch-up\n";
}
void rollback_budget(){
    setup();add_peer(one,true,true);
    const auto activations=[] {size_t count=0;for(const auto& item:events)if(item.owner==one&&item.action=="activate")++count;return count;};
    for(unsigned attempt=1;attempt<=3;++attempt){
        nodes[one].selected=false;until([]{return nodes[one].selected;});assert(activations()==attempt);
        // A real new boot came back with the same failed image instead of the
        // selected primary image. Its changed BootId cannot reset this budget.
        nodes[one].boot=identity<m::BootId>(20+attempt);peers.front().boot_id=nodes[one].boot;
        until([]{return phase()=="failed";});
        const auto failed=cohort().get("targets")->array.front();assert(field(failed,"phase")=="failed"&&!field(failed,"error").empty());
    }
    for(unsigned n=0;n<200;++n)poll();
    assert(activations()==3);
    nodes[one].boot=identity<m::BootId>(99);peers.front().boot_id=nodes[one].boot;
    for(unsigned n=0;n<100;++n)poll();
    assert(activations()==3&&phase()=="failed");
    // A different starting image receives a fresh budget.
    nodes[one].selected=false;nodes[one].elf=std::string(64,'f');peers.front().firmware_identity=nodes[one].elf;
    until([]{return nodes[one].selected;});assert(activations()==4&&!nodes[local].selected&&disk_writes==0);
    std::cout<<"PASS three actual rollback boots stop repeated activation for the same image pair; reboot alone cannot reset the budget, changed image can\n";
}
void upload_failure_reporting(){
    setup();add_peer(one,true,true);std::string error;
    assert(u::begin_system_upload(uploaded_release,error)==ESP_OK);
    assert(field(cohort(),"image_elf_sha256")==uploaded_release.image_elf_sha256&&field(cohort(),"image_bytes")==std::to_string(uploaded_release.image_bytes));
    u::fail_system_upload("Firmware upload was interrupted");
    const auto failed=*cohort().get("last_failed_upload");
    assert(field(failed,"phase")=="failed"&&field(failed,"image_elf_sha256")==uploaded_release.image_elf_sha256);
    assert(field(failed.get("targets")->array.front(),"phase")=="failed");
    poll();assert(phase()=="distributing"&&cohort().get("automatic")->boolean);
    assert(field(*cohort().get("last_failed_upload"),"job_id")==field(failed,"job_id"));
    until([]{return nodes[one].selected;});peers.front().firmware_identity=running_release.image_elf_sha256;peers.front().ready=true;peers.front().management_ready=false;
    until([]{return phase()=="complete";});
    assert(u::begin_system_upload(uploaded_release,error)==ESP_OK&&cohort().get("last_failed_upload")->type==c::Json::Type::null_value);
    assert(disk_writes==0);
    std::cout<<"PASS incoming image identity is available before staging; failed upload survives automatic catch-up reporting and clears only on a new user upload\n";
}
void source_failure_reporting(){
    setup();add_peer(one,true,true);missing_running_release=true;poll();
    const auto report=cohort();assert(field(report,"phase")=="failed"&&report.get("automatic")->boolean);
    assert(field(report,"image_elf_sha256")==nodes[local].elf);
    const auto target=report.get("targets")->array.front();assert(field(target,"octopus_id")==one.str()&&field(target,"phase")=="failed"&&!field(target,"error").empty());
    for(unsigned n=0;n<5;++n)poll();
    assert(events.empty()&&disk_writes==0);
    std::cout<<"PASS unavailable primary source reports the exact automatic target as failed without pretending to transfer\n";
}
void multiple_failure_reporting(){
    setup();add_peer(one,true,true);add_peer(two,true,true);missing_running_release=true;
    poll();poll();const auto report=cohort();
    assert(field(report.get("targets")->array.front(),"octopus_id")==two.str());
    const auto failures=report.get("automatic_failures")->array;assert(failures.size()==2);
    assert(field(failures[0],"octopus_id")==one.str()&&field(failures[1],"octopus_id")==two.str());
    peers.erase(peers.begin());assert(cohort().get("automatic_failures")->array.size()==1);
    peers.front().firmware_identity=nodes[local].elf;assert(cohort().get("automatic_failures")->array.empty());
    peers.front().firmware_identity=nodes[two].elf;nodes[local].elf=std::string(64,'c');
    assert(cohort().get("automatic_failures")->array.empty()&&events.empty()&&disk_writes==0);
    std::cout<<"PASS terminal failures remain visible for multiple members and are filtered after removal, matching image, or primary image change\n";
}
void logical_restart_during_chunk(){
    setup();auto& peer=add_peer(one);nodes[one].elf=nodes[local].elf;peer.firmware_identity=nodes[one].elf;
    const auto original_boot=nodes[one].boot;upload_once();until([]{return nodes[one].received.has_value();});
    assert(nodes[one].maintenance&&maintenance_fences.back()==std::make_pair(one,true));
    interrupt_next_chunk=true;const auto began=std::chrono::steady_clock::now();poll();
    assert(std::chrono::steady_clock::now()-began<std::chrono::seconds(3));
    const auto target=cohort().get("targets")->array[1];assert(field(target,"phase")=="queued"&&field(target,"offset")=="0");
    assert(maintenance_fences.back()==std::make_pair(one,false)&&!nodes[one].maintenance&&!nodes[one].received&&nodes[one].bytes.empty());
    assert(nodes[one].boot==original_boot);peers.front().ready=true;
    until([]{return nodes[one].staged.has_value();});
    const auto last_member_fence=std::find_if(maintenance_fences.rbegin(),maintenance_fences.rend(),[](const auto& fence){return fence.first==one;});
    assert(last_member_fence!=maintenance_fences.rend()&&!last_member_fence->second);
    until([]{return nodes[local].selected;});assert(nodes[one].selected&&nodes[one].bytes==staged_bytes&&nodes[one].boot==original_boot);
    size_t begins=0;for(const auto& item:events)if(item.owner==one&&item.action=="begin")++begins;assert(begins==2);
    std::cout<<"PASS streamed connection interruption releases maintenance and later completes over recovered TLS\n";
}
void streaming_without_chunk_results(){
    alarm(5);setup();add_peer(one,true,true);until([]{return nodes[one].staged.has_value();});
    assert(chunk_admissions==2&&chunk_results==0&&stage_attempts==1&&nodes[one].bytes==running_bytes&&!nodes[one].selected);
    alarm(0);std::cout<<"PASS ordered chunks stream without progress replies; one final stage confirms the complete image\n";
}
void binary_chunks_bind_to_the_accepted_begin(){
    setup();add_peer(one);upload_once();until([]{return nodes[one].received.has_value();});
    const auto accepted=nodes[one].accepted_begin;assert(accepted);destination=one;
    c::Envelope envelope;envelope.kind=c::Kind::update_chunk;
    envelope.update_chunk=c::UpdateChunk{identity<m::MessageId>(999),0,staged_bytes.substr(0,2048)};
    assert(transport.handle(local,envelope)==c::Error::transaction_conflict&&nodes[one].bytes.empty()&&nodes[one].received);
    envelope.update_chunk->begin_id=accepted;
    assert(transport.handle(two,envelope)==c::Error::transaction_conflict&&nodes[one].bytes.empty()&&nodes[one].received);
    transport.disconnected(local,identity<m::MessageId>(999));assert(nodes[one].received&&nodes[one].maintenance);
    assert(transport.handle(local,envelope)==c::Error::none&&nodes[one].bytes==staged_bytes.substr(0,2048));
    assert(transport.handle(local,envelope)==c::Error::transaction_conflict&&!nodes[one].received&&!nodes[one].maintenance&&nodes[one].bytes.empty());
    destination=local;assert(!nodes[one].staged&&!nodes[one].selected&&!nodes[local].selected);
    std::cout<<"PASS binary chunks require the accepted begin and owner; stale cleanup cannot abort it; repeated offsets abort only the incomplete upload\n";
}
void fifo_backpressure_is_local(){
    alarm(5);setup();add_peer(one,true,true);until([]{return nodes[one].received.has_value();});
    full_chunk_attempts=1;u::poll_group_update();
    auto target=cohort().get("targets")->array.front();assert(field(target,"offset")=="0"&&field(target,"error").empty()&&chunk_admissions==0);
    // Capacity alone changed: another turn at the same clock must make progress.
    u::poll_group_update();target=cohort().get("targets")->array.front();
    assert(field(target,"offset")=="4096"&&chunk_admissions==2);
    full_stage_attempts=2;u::poll_group_update();
    assert(nodes[one].staged&&stage_attempts==3&&chunk_results==0);
    alarm(0);std::cout<<"PASS full existing FIFO preserves offset without retry backoff; final stage waits only for local capacity\n";
}
void rejected_stream_can_later_complete(bool missing){
    setup();add_peer(one,true,true);const auto original_boot=nodes[one].boot;
    if(missing)drop_next_chunk=true;else corrupt_next_chunk=true;
    until([]{return phase()=="failed";});
    const auto failed=cohort().get("targets")->array.front();
    assert(field(failed,"phase")=="failed"&&!field(failed,"error").empty()&&!nodes[one].staged&&!nodes[one].selected&&!nodes[local].selected&&stage_attempts==1);
    until([]{return nodes[one].selected;});
    assert(nodes[one].boot==original_boot&&nodes[one].bytes==running_bytes&&chunk_results==0);
    size_t begins=0;for(const auto& item:events)if(item.owner==one&&item.action=="begin")++begins;
    assert(begins==2&&stage_attempts==2);
    std::cout<<"PASS "<<(missing?"missing/expired chunk":"complete-image SHA mismatch")<<" fails final validation without activation; a later transfer succeeds on the healthy connection\n";
}
void missing_stream(){rejected_stream_can_later_complete(true);}void bad_stream_hash(){rejected_stream_can_later_complete(false);}
void run(void(*scenario)()){const auto child=fork();assert(child>=0);if(child==0){scenario();std::cout.flush();_exit(0);}int status=0;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);}
void offline(){skipped_offline();}
void rejected(){
    setup();add_peer(one);add_peer(two);
    nodes[one].elf=nodes[two].elf=nodes[local].elf;for(auto& peer:peers)peer.firmware_identity=nodes[local].elf;
    upload_once();until([]{return nodes[one].staged.has_value();});
    assert(!nodes[one].selected&&!nodes[local].selected);corrupt_next_chunk=true;
    until([]{return phase()=="failed";});
    const auto report=cohort();const auto targets=report.get("targets")->array;
    assert(field(targets[1],"phase")=="staged"&&field(targets[2],"phase")=="failed"&&!field(targets[2],"error").empty());
    assert(!nodes[local].selected&&!nodes[one].selected&&!nodes[two].selected&&!nodes[two].staged);
    assert(field(*report.get("last_failed_upload"),"phase")=="failed");
    for(const auto& item:events)assert(item.action!="activate");
    assert(!nodes[one].maintenance&&!nodes[two].maintenance);
    const auto count=events.size();for(unsigned turn=0;turn<8;++turn)poll();assert(phase()=="failed"&&events.size()==count);
    std::cout<<"PASS browser cohort with one staged member and a second full-image SHA rejection fails before any activation\n";
}
int main(int argc,char** argv){assert(argc==2);std::ifstream file(argv[1]);manifest.assign(std::istreambuf_iterator<char>(file),{});assert(!manifest.empty());run(distribution);run(offline);run(catchup);run(matching_hello_releases_sync_fence);run(same_image);run(receiver_staging);run(rejected);run(retry_transient);run(authority_denials);run(rollback_budget);run(logical_restart_during_chunk);run(streaming_without_chunk_results);run(binary_chunks_bind_to_the_accepted_begin);run(fifo_backpressure_is_local);run(missing_stream);run(bad_stream_hash);run(upload_failure_reporting);run(source_failure_reporting);run(multiple_failure_reporting);}
