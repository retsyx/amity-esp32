#include "group_firmware_update.hpp"
#include "amity/consortium/update.hpp"
#include "config_store.hpp"
#include "system_services.hpp"
#include "esp_app_desc.h"
#include <openssl/sha.h>
#include <cassert>
#include <map>
#include <set>
#include <fstream>
#include <iostream>
#include <cstring>
#include <tuple>
namespace c=amity::consortium; namespace m=amity::system; namespace u=system_services::updates;
template<class T>T identity(unsigned n){char text[33];std::snprintf(text,sizeof(text),"%032x",n);return *T::parse(text);}
const auto local=identity<m::OctopusId>(1),one=identity<m::OctopusId>(2),two=identity<m::OctopusId>(3);
struct Node {m::BootId boot;std::string elf=std::string(64,'e'),version="old",phase="idle",release,hash,last_hash;uint64_t offset=0,last_offset=0,revision=7;bool healthy=true,selected=false,maintenance=false;};
std::map<m::OctopusId,Node> nodes;
std::map<std::string,std::string> disk;
std::vector<c::RuntimePeerStatus> peers;
c::RuntimeUpdateHooks transport;
std::vector<std::pair<m::OctopusId,bool>> fences;
std::vector<std::pair<m::OctopusId,c::Kind>> sent;
m::OctopusId destination=local;
std::optional<u::ReleaseManifest> staged;
bool drop_next=false,reject_install=false;
std::set<m::OctopusId> incompatible;
std::string manifest;
m::MessageId accepted_begin;
unsigned disk_writes=0,release_checks=0,begin_calls=0,chunk_calls=0,install_calls=0,abort_calls=0;
c::RuntimeStatus runtime_role;
c::Json parse(std::string_view body){c::Json value;c::JsonError error;assert(c::parse_json(body,value,error));return value;}
std::string field(const c::Json& j,const char* key){const auto* v=j.get(key);return v&&v->type==c::Json::Type::string?v->string:"";}
std::string sha(std::string_view data){unsigned char bytes[32];SHA256(reinterpret_cast<const unsigned char*>(data.data()),data.size(),bytes);char text[65]{};for(unsigned n=0;n<32;++n)std::snprintf(text+2*n,3,"%02x",bytes[n]);return text;}
void esp_fill_random(void* data,size_t size){static uint64_t n=1;std::memset(data,0,size);auto* bytes=static_cast<unsigned char*>(data);for(size_t i=0;i<size&&i<8;++i)bytes[size-1-i]=static_cast<unsigned char>(n>>(8*i));++n;}
const esp_app_desc_t* esp_app_get_description(){static esp_app_desc_t app{};std::snprintf(app.version,sizeof(app.version),"%s",nodes[destination].version.c_str());return &app;}
namespace config_store {
esp_err_t read(const char* key,std::string& value){auto found=disk.find(key);if(found==disk.end())return ESP_FAIL;value=found->second;return ESP_OK;}
esp_err_t write(const char* key,const std::string& value){++disk_writes;disk[key]=value;return ESP_OK;}
std::string sha256_hex(std::string_view data){return sha(data);}
}
namespace system_services {
const char* boot_id(){static std::string text;text=nodes[destination].boot.str();return text.c_str();}
bool firmware_ready(){return nodes[destination].healthy;}
}
namespace system_services::updates {
std::string status_json(){const auto& n=nodes[destination];return "{\"phase\":\""+n.phase+"\",\"received_bytes\":\""+std::to_string(n.offset)+"\",\"running_elf_sha256\":\""+n.elf+"\",\"running_version\":\""+n.version+"\",\"release_id\":\""+n.release+"\",\"expected_image_sha256\":\""+n.hash+"\"}";}
esp_err_t stage_release(std::string_view body,std::string_view,std::string& error){++release_checks;if(incompatible.count(destination)){error="Wrong hardware profile";return ESP_ERR_INVALID_ARG;}ReleaseManifest release;if(!parse_release_manifest(body,release,error))return ESP_ERR_INVALID_ARG;staged=release;return ESP_OK;}
esp_err_t peer_begin(std::string_view body,std::string_view,std::string& error){++begin_calls;ReleaseManifest r;if(!parse_release_manifest(body,r,error))return ESP_ERR_INVALID_ARG;auto& n=nodes[destination];n.release=r.release_id;n.hash=r.image_sha256;n.offset=0;n.phase="installing";n.maintenance=true;return ESP_OK;}
esp_err_t peer_chunk(std::string_view release,uint64_t offset,std::string_view data,std::string&){++chunk_calls;auto& n=nodes[destination];if(n.release!=release)return ESP_ERR_INVALID_ARG;if(n.offset&&offset==n.last_offset&&sha(data)==n.last_hash)return ESP_OK;if(offset!=n.offset)return ESP_ERR_INVALID_ARG;n.last_offset=offset;n.last_hash=sha(data);n.offset+=data.size();return ESP_OK;}
esp_err_t peer_install(std::string_view release,std::string&){++install_calls;auto& n=nodes[destination];if(reject_install||n.release!=release||!staged||n.offset!=staged->image_bytes){n.phase="failed";return ESP_FAIL;}n.selected=true;n.phase="boot_checking";return ESP_OK;}
void peer_abort(const std::string&){++abort_calls;auto& n=nodes[destination];if(n.phase=="installing"){n.phase="aborted";n.maintenance=false;n.offset=0;}}
bool installation_active(){return nodes[destination].phase=="installing"||nodes[destination].phase=="boot_checking";}
}
namespace amity::consortium {
RuntimeStatus runtime_status(){return runtime_role;}
std::vector<RuntimePeerStatus> runtime_peers(){return peers;}
void runtime_update_hooks(RuntimeUpdateHooks hooks){transport=std::move(hooks);}
esp_err_t runtime_send_update(m::OctopusId to,Kind kind,std::string body,uint32_t){
    auto value=parse(body);assert(valid_update(kind,value));
    if(kind==Kind::update_result){if(drop_next){drop_next=false;return ESP_OK;}return transport.result(destination,value)==Error::none?ESP_OK:ESP_FAIL;}
    for(const auto& peer:peers)if(peer.id==to&&peer.management_ready&&!(peer.negotiated_capabilities&qualified_lookup)&&kind==Kind::update_begin)assert(!value.get("validate_only"));
    sent.emplace_back(to,kind);const auto old=destination;destination=to;
    Envelope envelope;envelope.kind=kind;envelope.payload=value;envelope.origin_octopus_id=local;envelope.destination_octopus_id=to;envelope.origin_boot_id=nodes[local].boot;envelope.destination_boot_id=nodes[to].boot;envelope.config_revision=7;envelope.coordinator_epoch=1;
    const auto handled=transport.handle(local,envelope);
    if(handled==Error::none&&kind==Kind::update_begin&&(!value.get("validate_only")||!value.get("validate_only")->boolean))accepted_begin=*m::MessageId::parse(field(value,"request_id"));
    destination=old;return ESP_OK;
}
}
c::Json cohort(){return parse(u::cohort_status_json());}
m::MessageId job(){return *m::MessageId::parse(field(cohort(),"job_id"));}
m::OctopusId current_target(){auto s=cohort();uint64_t index=0;assert(c::decimal(s.get("current_index"),index));return *m::OctopusId::parse(field(s.get("targets")->array[index],"octopus_id"));}
void boot(m::OctopusId owner,unsigned id,const std::string& elf,const std::string& version,uint64_t revision=7,bool healthy=true){auto& n=nodes[owner];n.boot=identity<m::BootId>(id);n.elf=elf;n.version=version;n.revision=revision;n.healthy=healthy;n.phase="healthy";for(auto& p:peers)if(p.id==owner){p.boot_id=n.boot;p.firmware_identity=elf;p.ready=true;p.management_ready=false;p.negotiated_capabilities=c::known_capabilities;}}
int main(int argc,char** argv){
    assert(argc==2);std::ifstream file(argv[1]);manifest.assign(std::istreambuf_iterator<char>(file),{});
    nodes[local].boot=identity<m::BootId>(10);nodes[one].boot=identity<m::BootId>(11);nodes[two].boot=identity<m::BootId>(12);
    for(auto owner:{one,two}){c::RuntimePeerStatus p;p.id=owner;p.boot_id=nodes[owner].boot;p.ready=true;p.negotiated_capabilities=c::known_capabilities;peers.push_back(p);}
    peers[0].ready=false;peers[0].management_ready=true;peers[0].negotiated_capabilities=1023; // Older updater; no sequenced execution.
    auto start=[] {u::start_group_updates(local,{[](m::OctopusId owner,bool value){fences.emplace_back(owner,value);},[]{return nodes[destination].revision;},[]{return std::string(64,'f');}});};
    runtime_role.initialized=true;runtime_role.coordinator=true;runtime_role.authority_ready=true;runtime_role.phase="coordinator";runtime_role.epoch=1;
    const auto authorized_main=runtime_role;std::vector<c::RuntimeStatus> denied_roles;
    auto unclaimed=runtime_role;unclaimed.coordinator=unclaimed.authority_ready=false;unclaimed.phase="unclaimed";denied_roles.push_back(unclaimed);
    auto member=unclaimed;member.member=true;member.phase="member";denied_roles.push_back(member);
    auto unready=runtime_role;unready.authority_ready=false;denied_roles.push_back(unready);
    auto retired=runtime_role;retired.phase="retired";denied_roles.push_back(retired);
    auto uninitialized=runtime_role;uninitialized.initialized=false;denied_roles.push_back(uninitialized);
    auto contradictory=runtime_role;contradictory.member=true;denied_roles.push_back(contradictory);
    // Capture actual externally visible effects, not just return values. These
    // fixtures exercise otherwise admissible stage/chunk/install checkpoints.
    const auto effects=[] {return std::make_tuple(disk,sent.size(),fences.size(),disk_writes,release_checks,begin_calls,chunk_calls,install_calls,abort_calls);};
    start();std::string error;
    for(const auto& role:denied_roles){runtime_role=role;const auto before=effects();assert(u::stage_cohort(manifest,"",{local,one,two},error)==ESP_ERR_INVALID_STATE&&effects()==before);}
    runtime_role=authorized_main;
    incompatible.insert(two);
    assert(u::stage_cohort(manifest,"",{local,one,two},error)!=ESP_OK);
    assert(fences.empty()&&nodes[one].offset==0&&!nodes[one].maintenance&&!nodes[one].selected&&disk.count("ota_cohort")==0); // Later incompatible executor rejects before first-node effects.
    incompatible.clear();
    assert(u::stage_cohort(manifest,"",{local,one,two},error)==ESP_OK);
    assert(fences.empty()&&!nodes[one].maintenance&&!nodes[two].maintenance);
    const auto stage_messages=sent.size();
    auto state=cohort();assert(field(state,"phase")=="staged"&&field(state.get("targets")->array[0],"octopus_id")==one.str()&&field(state.get("targets")->array[2],"octopus_id")==local.str());
    const auto staged_job=job();const auto staged_target=current_target();const std::string positive_chunk(2048,'x');
    for(const auto& role:denied_roles){runtime_role=role;const auto before=effects();assert(u::cohort_chunk(staged_job,staged_target,0,positive_chunk,error)==ESP_ERR_INVALID_STATE&&effects()==before);}
    runtime_role=authorized_main;
    std::string bytes(2048,'x');assert(u::cohort_chunk(job(),current_target(),1,bytes,error)!=ESP_OK);assert(sent.size()==stage_messages);
    assert(u::cohort_chunk(job(),current_target(),0,bytes,error)==ESP_OK);assert(fences.back()==std::make_pair(one,true));
    // A repeated browser checkpoint is a no-op, including after a lost HTTP response.
    assert(u::cohort_chunk(job(),current_target(),0,bytes,error)==ESP_OK);
    assert(u::cohort_chunk(job(),current_target(),2048,bytes,error)==ESP_OK);
    const auto received_job=job();const auto received_target=current_target();
    for(const auto& role:denied_roles){runtime_role=role;const auto before=effects();assert(u::cohort_install(received_job,received_target,error)==ESP_ERR_INVALID_STATE&&effects()==before);}
    runtime_role=authorized_main;assert(u::cohort_install(job(),current_target(),error)==ESP_OK);
    state=cohort();assert(field(state,"phase")=="boot_checking"); // Same boot cannot complete.
    boot(one,21,std::string(64,'b'),"0.1.55",8);assert(field(cohort(),"phase")=="boot_checking"); // Exact applied config required.
    boot(one,21,std::string(64,'b'),"0.1.55",7,false);assert(field(cohort(),"phase")=="boot_checking");
    boot(one,21,std::string(64,'b'),"0.1.55");assert(field(cohort(),"phase")=="next_target");assert(fences.back()==std::make_pair(one,false));
    const auto initial_job=job();const auto before_stale=sent.size();
    assert(u::cohort_chunk(initial_job,one,0,bytes,error)==ESP_ERR_INVALID_STATE&&sent.size()==before_stale); // Delayed old-target HTTP cannot write next peer.
    assert(u::cohort_chunk(job(),current_target(),0,bytes,error)==ESP_OK);assert(fences.back()==std::make_pair(two,true));
    start();assert(field(cohort(),"phase")=="interrupted"); // Durable checkpoint never resumes bytes automatically.
    assert(field(cohort().get("targets")->array[0],"phase")=="healthy");
    assert(u::cancel_cohort(job(),error)==ESP_OK);assert(u::stage_cohort(manifest,"",{local,one,two},error)==ESP_OK);
    assert(field(cohort().get("targets")->array[0],"phase")=="healthy"); // Completed cohort members retained.
    assert(u::cohort_chunk(job(),current_target(),0,bytes,error)==ESP_OK);assert(u::cohort_chunk(job(),current_target(),2048,bytes,error)==ESP_OK);
    assert(u::cohort_install(job(),current_target(),error)==ESP_OK);boot(two,22,std::string(64,'d'),"old");assert(field(cohort(),"phase")=="rolled_back");
    assert(u::stage_cohort(manifest,"",{local,one,two},error)==ESP_OK);
    assert(field(cohort().get("targets")->array[0],"phase")=="healthy");
    assert(u::cohort_chunk(job(),current_target(),0,bytes,error)==ESP_OK&&u::cohort_chunk(job(),current_target(),2048,bytes,error)==ESP_OK);
    assert(u::cohort_install(job(),current_target(),error)==ESP_OK);boot(two,23,std::string(64,'b'),"0.1.55");assert(field(cohort(),"phase")=="next_target");
    assert(current_target()==local);assert(u::cohort_chunk(job(),local,0,bytes,error)==ESP_OK);assert(fences.back()==std::make_pair(local,true)&&nodes[local].maintenance);
    assert(u::cohort_chunk(job(),local,2048,bytes,error)==ESP_OK&&u::cohort_install(job(),local,error)==ESP_OK);
    assert(field(cohort(),"phase")=="boot_checking");boot(local,24,std::string(64,'b'),"0.1.55");assert(field(cohort(),"phase")=="complete");
    assert(fences.back()==std::make_pair(local,false));
    // Cleanup of the exact local unfinished transfer remains allowed after
    // authority is lost. It cannot select firmware or create a new transfer.
    assert(u::stage_cohort(manifest,"",{local},error)==ESP_OK&&u::cohort_chunk(job(),local,0,bytes,error)==ESP_OK);
    const auto cleanup_job=job();const auto before_cleanup_messages=sent.size();const auto before_cleanup_installs=install_calls,before_cleanup_begins=begin_calls,before_cleanup_aborts=abort_calls;
    runtime_role=unclaimed;assert(u::cancel_cohort(cleanup_job,error)==ESP_OK);assert(sent.size()==before_cleanup_messages&&install_calls==before_cleanup_installs&&begin_calls==before_cleanup_begins&&abort_calls==before_cleanup_aborts+1);assert(nodes[local].phase=="aborted"&&!nodes[local].maintenance&&fences.back()==std::make_pair(local,false));runtime_role=authorized_main;
    // Exact accepted begin ID scopes disconnect cleanup across reconnects.
    assert(u::stage_cohort(manifest,"",{one},error)==ESP_OK&&u::cohort_chunk(job(),one,0,bytes,error)==ESP_OK);
    const auto begin=accepted_begin;destination=one;
    transport.disconnected(local,identity<m::MessageId>(99999));assert(nodes[one].maintenance);
    transport.disconnected(local,begin);assert(!nodes[one].maintenance&&nodes[one].phase=="aborted");destination=local;
    assert(u::cancel_cohort(job(),error)==ESP_OK);
    const auto valid_record=disk.at("ota_cohort").substr(78);
    const auto replace_once=[](std::string input,const std::string& before,const std::string& after){const auto at=input.find(before);assert(at!=std::string::npos);input.replace(at,before.size(),after);return input;};
    std::vector<std::string> corrupt{
        replace_once(valid_record,"\"phase\":\"aborted\"","\"phase\":\"invented\""),
        replace_once(valid_record,"\"config_revision\":\"7\"","\"config_revision\":\"07\""),
        replace_once(valid_record,"\"signature_hex\":\"\"","\"signature_hex\":null"),
        replace_once(valid_record,"\"current_index\":\"0\"","\"current_index\":\"999\""),
        replace_once(valid_record,"\"previous_boot_id\":\""+nodes[one].boot.str()+"\"","\"previous_boot_id\":\"00000000000000000000000000000000\""),
        replace_once(valid_record,"\"offset\":\"2048\"","\"offset\":\"02048\"")
    };
    auto unknown=valid_record;unknown.insert(unknown.size()-1,",\"unexpected\":true");corrupt.push_back(unknown);
    for(const auto& malformed:corrupt){
        disk["ota_cohort"]="AMITYCOHORT1\n"+sha(malformed)+"\n"+malformed; // Valid checksum must not hide malformed persisted fields.
        const auto before_fences=fences.size(),before_messages=sent.size();
        const auto before_offset=nodes[one].offset;start();
        assert(field(cohort(),"phase")=="recovery_required"&&fences.size()==before_fences&&sent.size()==before_messages);
        assert(nodes[one].offset==before_offset&&!nodes[one].maintenance);
    }
    std::cout<<"PASS cohort original-main authority denials/no effects, exact local cleanup, ordering/checkpoints/restart, boot/hash/config health and maintenance boundaries\n";
}
