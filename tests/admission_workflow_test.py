#!/usr/bin/env python3
"""Run actual discovery/admission runtime against deterministic task/radio seams."""
import argparse
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
from consortium_gatt_callback_test import function
from setup_timing_support import timing_support

ROOT = Path(__file__).resolve().parents[1]
PREFIX = r'''
#include "amity/consortium/runtime.hpp"
#include "amity/consortium/bootstrap.hpp"
#include "amity/consortium/credential_types.hpp"
#include "amity/consortium/enrollment.hpp"
#include "amity/consortium/settings.hpp"
#include "hostname_registry.hpp"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <iostream>
#include <memory>
#include <mutex>
namespace amity::consortium {
namespace platform { enum class BootstrapFailure : uint8_t { none=0,crypto_self_test_failed=1 }; }
using namespace amity::consortium::platform;
constexpr int pdPASS=1,MALLOC_CAP_SPIRAM=1,MALLOC_CAP_8BIT=2;
#define pdMS_TO_TICKS(x) (x)
std::mutex lock;
Group group;Identity identity;RuntimeStatus status;RuntimeRadioHooks radio;RuntimeNetworkHooks network;HostnameRegistry hostnames;
struct InviteRequest {model::MessageId scan_id;};
struct Admission {RuntimeAdmissionStatus view;uint64_t generation=0,deadline=0;std::string selected_token,hostname;uint32_t pin=0;bool submitted=false;} admission;
std::atomic_bool invitation_running{false},invitation_cancelled{false};
struct Active {model::OctopusId peer;bool cancelled=false,ready=false;std::mutex mutex;struct{bool ready=false;std::string hostname;}status;};
std::vector<std::shared_ptr<Active>> links;
struct SettingsLink {std::shared_ptr<Active> active;RuntimePeerStatus status;};
SettingsLink settings_link(model::OctopusId owner){for(const auto& active:links)if(active->peer==owner&&!active->cancelled&&active->ready){RuntimePeerStatus status;status.hostname=active->status.hostname;return {active,status};}return {};}
struct SettingsOperation {model::OctopusId owner;std::string core,stage="admitted";uint64_t expires=0;};
std::vector<SettingsOperation> settings_operations;
void settings_expire(uint64_t now){for(auto& operation:settings_operations)if(operation.stage=="admitted"&&now>=operation.expires)operation.stage="unknown";}

enum class AdmissionMode {button,setup_code};
AdmissionMode mode=AdmissionMode::button;
AdmissionMode configured_admission_mode(){return mode;}
bool crypto_ok=true,candidate_crypto_failed=false;
unsigned self_tests=0,connections=0,handshakes=0;
std::function<void()> during_selftest;
bool admission_crypto_self_test(AdmissionMode chosen){assert(chosen==mode);++self_tests;if(during_selftest)during_selftest();return crypto_ok;}
bool parse_admission_setup_code(std::string_view text,uint32_t& pin){pin=text=="valid-matter-code"?20202021:0;return pin!=0;}
uint64_t clock_ms=100;
uint64_t now_ms(){return clock_ms;}
std::function<void()> during_wait;
void vTaskDelay(unsigned ms){clock_ms+=ms;if(during_wait)during_wait();}
bool spawn_ok=true,discover_ok=true,exchange_ok=true,networked=false,bad_identity=false,bad_certificate=false;
std::string fail_phase;
unsigned discoveries=0,reservations=0,releases=0,disconnects=0,wifi_exports=0,encrypted_exchanges=0,saves=0,certificates=0;
unsigned candidate_snapshots=0;bool early_candidate_published=false;
void* task_argument=nullptr;
void invite_worker(void*){}
int xTaskCreateWithCaps(void(*)(void*),const char*,unsigned,void* arg,unsigned,void*,unsigned){if(!spawn_ok)return 0;task_argument=arg;return pdPASS;}
void gatt_client_disconnect(){++disconnects;}
void defer_radio_release(uint64_t generation){assert(generation==77);++releases;}
int gatt_client_discover(uint64_t generation,std::vector<RuntimeAdmissionCandidate>& out){assert(generation==77);++discoveries;if(discover_ok&&discoveries>1){out={{"one","Amity-123456"}};clock_ms+=100;}else clock_ms+=20000;return ESP_OK;}
int gatt_client_candidates(uint64_t generation,std::vector<RuntimeAdmissionCandidate>& out){
 assert(generation==77);++candidate_snapshots;
 if(candidate_snapshots==1)early_candidate_published=admission.view.phase=="found"&&admission.view.candidates.size()==1&&admission.view.candidates[0].token=="one";
 out={{"one","Amity-123456"},{"two","Amity-654321"}};return ESP_OK;
}
int gatt_client_connect_candidate(const std::string& token,uint64_t generation){assert(token=="two"&&generation==77&&self_tests==1&&crypto_ok);++connections;return ESP_OK;}
std::string hex(std::string_view value){std::string out;for(uint8_t c:value){out+="0123456789abcdef"[c>>4];out+="0123456789abcdef"[c&15];}return out;}
bool unhex(std::string_view text,std::string& value,size_t){if(text==hex("candidate-leaf")){value="candidate-leaf";return true;}return false;}
std::string sha256(std::string_view){return std::string(64,'d');}
bool certificate(std::string_view leaf,model::OctopusId,std::string& pin){++certificates;pin=std::string(64,leaf=="controller-leaf"?'a':'c');return leaf=="controller-leaf"||(leaf=="candidate-leaf"&&!bad_certificate);}
template<class T>T ident(unsigned n){char text[33];snprintf(text,sizeof(text),"%032x",n);return *T::parse(text);}
template<class T>T random_id(){return ident<T>(90);}
std::string host_ip(){return "192.0.2.1";}
int save_group(const Group&){++saves;return ESP_OK;}
Error remember_hostname(model::OctopusId owner,const std::string& name){return hostnames.remember(owner,name,[](std::string_view){return true;});}
void mbedtls_platform_zeroize(void* data,size_t count){std::fill_n(static_cast<char*>(data),count,0);}
struct AdmissionClient {
    AdmissionClient(AdmissionMode chosen,uint32_t pin){assert(chosen==mode&&(mode==AdmissionMode::button?pin==0:pin==20202021));}
    bool handshake(const std::function<bool(std::string_view,std::string&)>& exchange){std::string response;return exchange("public-handshake",response);}
    std::string peer_id()const{return ident<model::OctopusId>(3).str();}
    bool encrypt(std::string_view value,std::string& out){out="encrypted:"+std::string(value);return true;}
    bool decrypt(std::string_view value,std::string& out){if(value.substr(0,10)!="encrypted:")return false;out=value.substr(10);return true;}
};
bool gatt_client_exchange(bool encrypted,std::string_view input,std::string& out,BootstrapFailure* failure=nullptr){
    if(failure)*failure=BootstrapFailure::none;
    if(!exchange_ok)return false;
    if(!encrypted){assert(input=="public-handshake");++handshakes;if(candidate_crypto_failed){if(failure)*failure=BootstrapFailure::crypto_self_test_failed;return false;}out="public-response";return true;}
    ++encrypted_exchanges;assert(input.substr(0,10)=="encrypted:");Json request;JsonError detail;assert(parse_json(input.substr(10),request,detail));
    const auto phase=request.get("phase")->string;
    if(phase==fail_phase)return false;
    if(phase=="identify")out="{\"phase\":\"identity\",\"bootstrap_profile\":2,\"octopus_id\":\""+ident<model::OctopusId>(bad_identity?4:3).str()+"\",\"leaf\":\""+hex("candidate-leaf")+"\",\"networked\":"+(networked?"true":"false")+"}";
    else if(phase=="prepare"){
        JoinProposal proposal;assert(decode_join_proposal(request.get("proposal")->string,proposal));
        assert(proposal.coordinator_id==identity.id&&proposal.coordinator_leaf==identity.der&&proposal.coordinator_pin==identity.spki);
        assert(proposal.candidate_id==ident<model::OctopusId>(3)&&proposal.candidate_leaf=="candidate-leaf"&&proposal.candidate_pin==std::string(64,'c'));
        out="{\"phase\":\"prepared\",\"join_id\":\""+group.join_id.str()+"\",\"digest\":\""+group.join_digest+"\",\"hostname\":\""+admission.hostname+"\"}";
    }
    else {assert(phase=="wifi");assert(request.get("ssid")->string==hex("private-network"));assert(request.get("password")->string==hex("private-password"));out="{\"phase\":\"networked\",\"ready\":true}";}
    out="encrypted:"+out;return true;
}
'''
SUITE = r'''
void reset(){
    if(task_argument){delete static_cast<InviteRequest*>(task_argument);task_argument=nullptr;}
    group={};group.role=Group::coordinator;group.consortium=ident<model::ConsortiumId>(1);group.authority=ident<model::OctopusId>(2);group.epoch=group.revision=1;
    identity={};identity.id=group.authority;identity.der="controller-leaf";identity.spki=std::string(64,'a');status={};status.initialized=status.credential_ready=status.authority_ready=true;
    admission={};invitation_running=invitation_cancelled=false;hostnames={};links.clear();settings_operations.clear();
    fail_phase.clear();clock_ms=100;during_wait={};mode=AdmissionMode::button;spawn_ok=discover_ok=exchange_ok=true;networked=bad_identity=bad_certificate=false;
    discoveries=reservations=releases=disconnects=wifi_exports=encrypted_exchanges=saves=certificates=0;candidate_snapshots=0;early_candidate_published=false;
    self_tests=connections=handshakes=0;crypto_ok=true;candidate_crypto_failed=false;during_selftest={};
    platform::recorded_setup_timings.clear();
    radio.reserve=[](bool inviter,uint64_t& generation){assert(inviter);++reservations;generation=77;return ESP_OK;};
    network.get_hostname=[]{return "main-amity";};
    network.export_credentials=[](std::string& ssid,std::string& password){++wifi_exports;ssid="private-network";password="private-password";return true;};
}
void run(){auto* arg=task_argument;task_argument=nullptr;invite_worker_work(arg);invitation_running=false;}
void select(){
    if(admission.view.phase!="found")return;
    assert(runtime_add_octopus(ident<model::MessageId>(99),"two","room",mode==AdmissionMode::setup_code?"valid-matter-code":"")==ESP_ERR_INVALID_STATE);
    assert(runtime_add_octopus(ident<model::MessageId>(1),"absent","room")==ESP_ERR_INVALID_ARG);
    assert(runtime_add_octopus(ident<model::MessageId>(1),"two","Invalid Name")==ESP_ERR_INVALID_ARG);
    assert(runtime_add_octopus(ident<model::MessageId>(1),"two","main-amity")==ESP_ERR_INVALID_ARG);
    if(mode==AdmissionMode::setup_code){assert(runtime_add_octopus(ident<model::MessageId>(1),"two","room","bad-code")==ESP_ERR_INVALID_ARG);}
    else assert(runtime_add_octopus(ident<model::MessageId>(1),"two","room","unexpected")==ESP_ERR_INVALID_ARG);
    assert(runtime_add_octopus(ident<model::MessageId>(1),"two","room",mode==AdmissionMode::setup_code?"valid-matter-code":"")==ESP_OK);
    assert(runtime_add_octopus(ident<model::MessageId>(1),"two","room")==ESP_ERR_INVALID_STATE || mode==AdmissionMode::setup_code);
}
void proposal_record_lifetime(){
    reset();auto local=identity;Peer peer{ident<model::OctopusId>(3),"candidate-leaf",std::string(64,'c'),false};
    const auto bytes=proposal_json(ident<model::JoinId>(90),group.consortium,local,peer,1,2,"192.0.2.1","room");
    assert(!bytes.empty()&&certificates==0); // No repeat signature or global cache.
    local.der="changed-local";local.spki=std::string(64,'b');peer.der="changed-peer";peer.pin=std::string(64,'d');
    JoinProposal decoded;assert(decode_join_proposal(bytes,decoded));
    assert(decoded.coordinator_leaf=="controller-leaf"&&decoded.coordinator_pin==std::string(64,'a')&&decoded.candidate_leaf=="candidate-leaf"&&decoded.candidate_pin==std::string(64,'c'));
    peer.der="candidate-leaf";peer.pin.clear();
    assert(proposal_json(ident<model::JoinId>(90),group.consortium,identity,peer,1,2,"192.0.2.1","room").empty());
    peer.pin=identity.spki;assert(proposal_json(ident<model::JoinId>(90),group.consortium,identity,peer,1,2,"192.0.2.1","room").empty());
}
void roster_capacity(){
    reset();
    for(unsigned n=0;n<2;++n)group.peers.push_back({ident<model::OctopusId>(10+n),"existing-leaf",std::string(64,'e'+n),true});
    assert(runtime_scan_admission(ident<model::MessageId>(1))==ESP_OK);
    during_wait=select;run();
    assert(group.peers.size()==3&&saves==1&&wifi_exports==1&&admission.view.phase=="finishing");
    group.join_id={};group.joining_id={};group.join_digest.clear();group.join_hostname.clear();group.join_revision=0;
    assert(runtime_scan_admission(ident<model::MessageId>(2))==ESP_ERR_INVALID_STATE&&!invitation_running);
    assert(reservations==1&&saves==1&&wifi_exports==1); // A fourth Add never starts radio or transfers credentials.

    reset();assert(runtime_scan_admission(ident<model::MessageId>(1))==ESP_OK);
    during_wait=[](){if(admission.view.phase=="found"){
        group.peers.resize(3);
        assert(runtime_add_octopus(ident<model::MessageId>(1),"two","room")==ESP_ERR_INVALID_STATE);
        assert(runtime_cancel_admission(ident<model::MessageId>(1))==ESP_OK);
    }};
    run();assert(!saves&&!wifi_exports&&!connections&&admission.view.phase=="cancelled");

    reset();assert(runtime_scan_admission(ident<model::MessageId>(1))==ESP_OK);
    during_wait=select;during_selftest=[](){group.peers.resize(3);};run();
    assert(!saves&&!wifi_exports&&group.peers.size()==3&&admission.view.phase=="failed");
    std::cout<<"PASS three secondary Amities admitted; fourth rejected before scan, submission and prepared membership storage\n";
}
void rename_name_reservation(){
    reset();invitation_running=true;admission.view.scan_id=ident<model::MessageId>(1);admission.view.phase="found";admission.deadline=1000;admission.view.candidates={{"two","Amity-654321"}};
    const auto owner=ident<model::OctopusId>(3);group.peers.push_back({owner,"leaf","pin",true});assert(remember_hostname(owner,"old-room")==Error::none);
    auto active=std::make_shared<Active>();active->peer=owner;active->ready=active->status.ready=true;active->status.hostname="live-room";links.push_back(active);
    assert(runtime_add_octopus(ident<model::MessageId>(1),"two","live-room")==ESP_ERR_INVALID_ARG); // Live authenticated name wins if its metadata storage failed.
    DeviceSettingsWrite rename;rename.expected_boot=ident<model::BootId>(7);rename.expected_hostname_revision=2;rename.hostname="room";
    settings_operations.push_back({owner,device_settings_write_json(rename),"admitted",500});
    assert(runtime_add_octopus(ident<model::MessageId>(1),"two","room")==ESP_ERR_INVALID_ARG&&admission.view.phase=="found");
    assert(!connections&&!saves&&!wifi_exports&&!disconnects); // A rejected conflicting Add has no networking effect.
    clock_ms=500;assert(runtime_add_octopus(ident<model::MessageId>(1),"two","room")==ESP_OK&&admission.submitted);
    assert(settings_operations.front().stage=="unknown");
    std::cout<<"PASS Add preserves hostname uniqueness against current live names and admitted renames before any network or enrollment work\n";
}
int run_tests(){
    rename_name_reservation();
    proposal_record_lifetime();
    roster_capacity();
    for(auto selected_mode:{AdmissionMode::button,AdmissionMode::setup_code}){
        reset();mode=selected_mode;assert(runtime_scan_admission(ident<model::MessageId>(1))==ESP_OK);
        assert(runtime_admission_status().phase=="scanning"&&self_tests==0);assert(runtime_admission_status().requires_setup_code==(mode==AdmissionMode::setup_code));
        assert(runtime_scan_admission(ident<model::MessageId>(2))==ESP_ERR_INVALID_STATE);
        during_wait=select;run();assert(discoveries==2&&reservations==1&&releases==1&&wifi_exports==1&&encrypted_exchanges==3&&saves==1);
        assert(self_tests==1&&connections==1&&handshakes==1);
        assert(early_candidate_published&&candidate_snapshots>=2&&clock_ms<21000); // Later candidate remained selectable without another full scan.
        assert(certificates==1); // Verify the untrusted candidate once, reuse exact validated records.
        for(const char* stage:{"worker_selected","selected_total","admission_selftest","gatt_connect","crypto_handshake","identify_rpc","peer_validation","proposal_store","prepare_rpc","prepare_ack","wifi_rpc","finishing"}){
            const auto found=std::find_if(platform::recorded_setup_timings.begin(),platform::recorded_setup_timings.end(),[&](const auto& row){return std::string(row.area)=="invite"&&std::string(row.entry.stage)==stage;});
            assert(found!=platform::recorded_setup_timings.end()&&std::string(found->entry.result)=="ok"&&found->scan==ident<model::MessageId>(1)&&found->peer==ident<model::OctopusId>(3));
        }
        assert(admission.view.phase=="finishing"&&admission.pin==0&&group.joining_id==ident<model::OctopusId>(3)&&hostnames.lookup(group.joining_id)=="room");
        assert(runtime_cancel_admission(ident<model::MessageId>(1))==ESP_ERR_INVALID_STATE);
        auto link=std::make_shared<Active>();links={link};admission.view.error="The result was not confirmed yet.";
        for(unsigned failure=0;failure<5;++failure){
            link->peer=group.joining_id;link->ready=link->status.ready=true;link->cancelled=false;link->status.hostname="room";
            switch(failure){case 0:link->peer=ident<model::OctopusId>(99);break;case 1:link->ready=false;break;case 2:link->status.ready=false;break;case 3:link->cancelled=true;break;case 4:link->status.hostname="wrong";break;}
            const auto pending=runtime_admission_status();assert(pending.phase=="finishing"&&!pending.error.empty()&&admission.view.phase=="finishing");
        }
        link->status.hostname="room";assert(runtime_admission_status().phase=="complete"&&admission.view.phase=="complete"&&admission.view.error.empty());
        const auto completed=runtime_admission_status();
        link->cancelled=true;link->ready=link->status.ready=false;links.clear();group.peers.clear();group.join_id={};group.joining_id={};group.join_digest.clear();group.join_hostname.clear();group.join_revision=0;
        const auto removed=runtime_admission_status();assert(removed.phase=="complete"&&removed.error.empty()&&removed.candidate_id==completed.candidate_id&&removed.scan_id==completed.scan_id);
        assert(runtime_scan_admission(ident<model::MessageId>(2))==ESP_OK);
        const auto next=runtime_admission_status();assert(next.phase=="scanning"&&next.scan_id==ident<model::MessageId>(2)&&!next.candidate_id&&next.error.empty());
        std::cout<<"PASS actual "<<(mode==AdmissionMode::button?"normal":"code-verified")<<" discovery-before-button, exact selection/name, encrypted setup, membership and readiness\n";
    }
    reset();assert(runtime_scan_admission(ident<model::MessageId>(1))==ESP_OK);assert(runtime_cancel_admission(ident<model::MessageId>(2))==ESP_ERR_INVALID_STATE);assert(!invitation_cancelled);
    assert(runtime_cancel_admission(ident<model::MessageId>(1))==ESP_OK);run();assert(reservations==0&&saves==0&&wifi_exports==0&&self_tests==0&&admission.view.phase=="cancelled");
    reset();assert(runtime_scan_admission(ident<model::MessageId>(1))==ESP_OK);during_wait=[](){if(admission.view.phase=="found")assert(runtime_cancel_admission(ident<model::MessageId>(1))==ESP_OK);};run();assert(releases==1&&saves==0&&encrypted_exchanges==0&&self_tests==0);
    reset();discover_ok=false;assert(runtime_scan_admission(ident<model::MessageId>(1))==ESP_OK);run();assert(admission.view.phase=="failed"&&releases==1&&saves==0&&wifi_exports==0&&discoveries<=9);
    reset();bad_identity=true;assert(runtime_scan_admission(ident<model::MessageId>(1))==ESP_OK);during_wait=select;run();assert(admission.view.phase=="failed"&&saves==0&&wifi_exports==0);
    reset();bad_certificate=true;assert(runtime_scan_admission(ident<model::MessageId>(1))==ESP_OK);during_wait=select;run();assert(certificates==1&&admission.view.phase=="failed"&&saves==0&&wifi_exports==0);
    reset();networked=true;assert(runtime_scan_admission(ident<model::MessageId>(1))==ESP_OK);during_wait=select;run();assert(admission.view.phase=="finishing"&&wifi_exports==0&&encrypted_exchanges==2);
    for(const std::string phase:{"prepare","wifi"}){
        reset();fail_phase=phase;assert(runtime_scan_admission(ident<model::MessageId>(1))==ESP_OK);during_wait=select;run();
        assert(saves==1&&group.join_id&&admission.view.phase=="finishing"&&!admission.view.error.empty());
        auto link=std::make_shared<Active>();link->peer=group.joining_id;link->ready=link->status.ready=true;link->status.hostname="room";links={link};
        assert(runtime_admission_status().phase=="complete"&&runtime_admission_status().error.empty());
        links.clear();assert(runtime_admission_status().phase=="complete"&&admission.view.error.empty());
    }
    reset();spawn_ok=false;assert(runtime_scan_admission(ident<model::MessageId>(1))==ESP_ERR_NO_MEM);assert(!invitation_running&&admission.view.phase=="failed");
    reset();group.role=Group::member;assert(runtime_scan_admission(ident<model::MessageId>(1))==ESP_ERR_INVALID_STATE&&!invitation_running);
    for(const auto selected_mode:{AdmissionMode::button,AdmissionMode::setup_code}){
        reset();mode=selected_mode;crypto_ok=false;assert(runtime_scan_admission(ident<model::MessageId>(1))==ESP_OK&&self_tests==0);during_wait=select;run();
        assert(self_tests==1&&!connections&&!handshakes&&!saves&&!wifi_exports&&!encrypted_exchanges&&releases==1&&!admission.pin);
        assert(admission.view.phase=="failed"&&admission.view.error=="This Amity failed its setup security check. Restart it and try again.");
        reset();mode=selected_mode;candidate_crypto_failed=true;assert(runtime_scan_admission(ident<model::MessageId>(1))==ESP_OK);during_wait=select;run();
        assert(self_tests==1&&connections==1&&handshakes==1&&!certificates&&!saves&&!wifi_exports&&!encrypted_exchanges&&releases==1&&!admission.pin);
        assert(admission.view.phase=="failed"&&admission.view.error=="The new Amity reported that its setup security check failed. Restart it and try again.");
    }
    for(bool cancel:{false,true})for(bool passes:{false,true}){
        reset();crypto_ok=passes;assert(runtime_scan_admission(ident<model::MessageId>(1))==ESP_OK);during_wait=select;
        during_selftest=[&]{if(cancel)invitation_cancelled=true;else clock_ms=admission.deadline;};run();
        assert(self_tests==1&&!connections&&!handshakes&&!saves&&!wifi_exports&&!encrypted_exchanges&&releases==1);
        assert(admission.view.phase==(cancel?"cancelled":"failed"));
        assert(admission.view.error==(cancel?"Adding this Amity was cancelled.":"No Amity was added before setup expired. Open Add Amity and try again."));
    }
    for(unsigned unavailable=0;unavailable<2;++unavailable){reset();if(unavailable)status.credential_ready=false;else status.initialized=false;assert(runtime_scan_admission(ident<model::MessageId>(1))==ESP_ERR_INVALID_STATE&&!invitation_running&&!self_tests);}
    std::cout<<"PASS self-test only on selected Add in both profiles; primary and candidate security failures reach Add status before credentials/membership, with cancellation/expiry cleanup\n";
    std::cout<<"PASS actual stale IDs/tokens, invalid inputs, pre-scan/found cancellation, timeout, identity mismatch, configured network preservation and task/role rejection; exact readiness latches transient completion across removal until a new scan\n";
    return 0;
}
}
int main(){return amity::consortium::run_tests();}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT / 'components/consortium/runtime.cpp')
    source = parser.parse_args().source.read_text()
    names = ['std::string proposal_json(', 'void admission_failed(', 'void invite_worker_work(', 'RuntimeAdmissionStatus runtime_admission_status(',
             'esp_err_t runtime_scan_admission(', 'esp_err_t runtime_cancel_admission(', 'esp_err_t runtime_add_octopus(']
    actual = function((ROOT / 'components/consortium/settings_runtime.inc').read_text(), 'bool settings_hostname_pending(') + ''.join(function(source, name) for name in names)
    if 'const Identity& coordinator,const Peer& candidate' not in source:
        actual += 'std::string proposal_json(model::JoinId join,model::ConsortiumId consortium,const Identity& local,const Peer& peer,uint64_t epoch,uint64_t revision,const std::string& host,const std::string& name){return proposal_json(join,consortium,local.id,peer.id,epoch,revision,local.der,peer.der,host,name);}\n'
    with tempfile.TemporaryDirectory(prefix='amity-admission-workflow-') as directory:
        cpp = Path(directory) / 'test.cpp'
        binary = Path(directory) / 'test'
        cpp.write_text(timing_support() + PREFIX + actual + SUITE)
        include = ['tests/reset_fakes', 'components/consortium', 'components/consortium/include',
                   'components/amity_system/include', 'components/device_settings/include']
        sources = sorted((ROOT / 'components/consortium').glob('*.cpp')) + [ROOT / 'components/amity_system/model.cpp']
        subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) +
                       ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-g', '-fsanitize=address,undefined'] +
                       ['-I' + str(ROOT / path) for path in include] +
                       [str(cpp)] + [str(path) for path in sources] + ['-o', str(binary)], check=True)
        return subprocess.run([str(binary)], check=False).returncode


if __name__ == '__main__':
    raise SystemExit(main())
