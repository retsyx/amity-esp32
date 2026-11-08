#!/usr/bin/env python3
"""Run the real authenticated hello and firmware gate without application sync."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

from consortium_gatt_callback_test import function
from consortium_live_configuration_test import PREFIX
from setup_timing_support import timing_support

ROOT = Path(__file__).resolve().parents[1]

EXTRA = r'''
namespace amity::consortium {
model::BootId boot;
uint8_t hardware_tentacles=1,hardware_tentacles_mask=1;
unsigned enrollment_commits=0,synchronization_entries=0;
struct AppDescription {uint8_t app_elf_sha256[32];};
const AppDescription* esp_app_get_description(){static const auto app=[](){AppDescription out{};std::fill(std::begin(out.app_elf_sha256),std::end(out.app_elf_sha256),0xab);return out;}();return &app;}
std::string hex(std::string_view bytes){std::string out;for(unsigned char byte:bytes){out.push_back("0123456789abcdef"[byte>>4]);out.push_back("0123456789abcdef"[byte&15]);}return out;}
bool abort_peer_credential(const Active&,const Group&){return false;}
bool enrollment_abort_pending(const Active&){return false;}
bool finish_enrollment_abort(Active&){assert(false);return false;}
bool enroll_lan(Active&,bool){++enrollment_commits;return true;}
bool notify_link_closed(Active&){assert(false);return false;}
bool lifecycle_kind(Kind kind){return kind==Kind::membership_event||kind==Kind::membership_result;}
bool synchronization_current(const Active&){++synchronization_entries;return false;}
bool synchronization_recoverable(const Active&,const LinkTrace&){assert(false);return false;}
bool fence_synchronization_scope(Active&,LinkTrace&){assert(false);return false;}
bool synchronization_barrier(Active&,bool,SynchronizationRound&,LinkTrace&){assert(false);return false;}
bool restart_synchronization_scope(Active&,const SynchronizationRound&,LinkTrace&){assert(false);return false;}
bool exchange_hostname(Active&,const std::string&,LinkTrace*){assert(false);return false;}
bool synchronize(Active&,bool,bool,uint64_t,const Envelope*,LinkTrace*){assert(false);return false;}
}
'''

SUITE = r'''
namespace amity::consortium {
std::shared_ptr<Active> connection(bool server,bool matching,bool provisional,uint32_t capabilities=known_capabilities){
 auto active=std::make_shared<Active>();active->ready=active->status.ready=false;
 const auto main=ident<model::OctopusId>(2),member=ident<model::OctopusId>(3);
 group={};group.role=server?Group::coordinator:(provisional?Group::prepared:Group::member);
 group.consortium=ident<model::ConsortiumId>(1);group.authority=main;group.epoch=group.revision=1;
 identity.id=server?main:member;boot=ident<model::BootId>(server?4:5);
 active->tls->authenticated=server?member:main;
 HelloFrame peer;peer.consortium_id=group.consortium;peer.octopus_id=active->tls->authenticated;
 peer.boot_id=ident<model::BootId>(server?5:4);peer.coordinator_epoch=1;peer.config_revision=server?2:7;
 peer.config_hash=std::string(64,'a');peer.firmware_identity=matching?hex(std::string(32,char(0xab))):std::string(64,'c');
 peer.required_capabilities=capabilities;peer.optional_capabilities=0;peer.hardware_profile=true;
 peer.supported_tentacles=1;peer.configured_tentacles_mask=1;
 if(provisional){group.join_id=ident<model::JoinId>(8);group.join_digest=std::string(64,'d');group.joining_id=member;group.join_hostname="member";peer.pending_join_id=group.join_id;peer.join_digest=group.join_digest;}
 active->tls->incoming.push_back(hello_json(peer));
 if(!server)active->tls->incoming.push_back("{\"type\":\"link\",\"link_id\":\""+ident<model::LinkId>(9).str()+"\"}");
 links={active};configuration=7;enrollment_commits=synchronization_entries=0;shutdowns=0;
 return active;
}
Envelope inbound(const Active& active,Kind kind){
 const auto scope=active.session.scope();Envelope message;
 message.consortium_id=scope.consortium_id;message.origin_octopus_id=scope.peer_id;message.destination_octopus_id=scope.local_id;
 message.origin_boot_id=scope.peer_boot_id;message.destination_boot_id=scope.local_boot_id;
 message.coordinator_epoch=scope.epoch;message.config_revision=scope.config_revision;message.ttl_ms=1000;message.kind=kind;return message;
}
void mismatched(bool server,bool provisional,uint32_t capabilities){
 auto active=connection(server,false,provisional,capabilities);const auto* tls=active->tls.get();LinkTrace trace;
 assert(establish(*active,server,&trace));
 const auto scope=active->session.scope();const auto hello_frames=active->tls->raw_sent.size();
 assert(active->peer==scope.peer_id&&active->claimed_boot==scope.peer_boot_id&&!active->execution_supported);
 assert(enrollment_commits==unsigned(provisional));
 active->queue.push_back({Kind::settings_read,"{}",1000,clock_ms,scope.config_revision,{}});
 assert(synchronize_connection(*active,server,active->execution_supported,0,&trace));
 assert(active->management_ready&&active->status.management_ready&&!active->ready&&!active->status.ready);
 assert(!active->context_ready&&!active->context_requested&&active->queue.empty());
 assert(active->session.state()==ConnectionState::restricted&&active->session.scope()==scope);
 assert(active->tls.get()==tls&&active->tls->usable()&&!active->cancelled&&shutdowns==0);
 assert(active->tls->raw_sent.size()==hello_frames&&synchronization_entries==0);
 assert(!send(*active,Kind::device_status,"{}")&&!send(*active,Kind::settings_read,"{}")&&!send(*active,Kind::snapshot_request,"{}")&&!send(*active,Kind::synchronization,"{}"));
 assert(active->tls->raw_sent.size()==hello_frames);
 for(auto kind:{Kind::device_status,Kind::settings_read,Kind::settings_write,Kind::settings_result,Kind::snapshot_request,Kind::snapshot_chunk,Kind::synchronization,Kind::scoped_intent,Kind::scoped_result})assert(active->session.admit(inbound(*active,kind),100,100)!=Error::none);
 for(auto kind:{Kind::update_begin,Kind::update_chunk,Kind::update_install,Kind::update_abort,Kind::update_status,Kind::update_result}){
  auto message=inbound(*active,kind);const bool allowed=server?kind==Kind::update_result:kind!=Kind::update_result;
  assert(active->session.admit(message,100,100)==(allowed?Error::none:Error::unauthorized_role));
  if(allowed){message.origin_boot_id=ident<model::BootId>(99);assert(active->session.admit(message,100,100)==Error::stale_boot);}
 }
 assert(active->session.begin_synchronization(scope.config_revision)==Error::not_ready);
 // A later local configuration change cannot unlock or resynchronize this image.
 ++configuration;assert(synchronize_connection(*active,server,true,configuration,&trace));
 assert(active->session.scope()==scope&&active->management_ready&&!active->ready&&synchronization_entries==0);
 assert(send(*active,server?Kind::update_begin:Kind::update_result,"{}"));
 if(capabilities&factory_reset_removal){assert(active->session.admit(inbound(*active,server?Kind::membership_result:Kind::membership_event),100,100)==Error::none);}
}
}
int main(){using namespace amity::consortium;
 for(bool server:{false,true})for(bool provisional:{false,true}){
  mismatched(server,provisional,known_capabilities);
  mismatched(server,provisional,peer_firmware_update);
  auto active=connection(server,true,provisional);LinkTrace trace;assert(establish(*active,server,&trace));
  assert(active->execution_supported&&!active->management_ready&&active->session.state()==ConnectionState::synchronizing);
  assert(!synchronize_connection(*active,server,true,0,&trace)&&synchronization_entries==1); // Normal synchronization path remains selected.
 }
 std::cout<<"PASS actual firmware hello gate: every enrolled/new mismatched peer stays update-only on the same healthy TLS, before hostname/config/snapshot/settings/context; matching images synchronize normally; exact authority/boot fences and explicit removal retained\n";
}
'''


def main():
    source = (ROOT / 'components/consortium/runtime.cpp').read_text()
    prefix = PREFIX.replace('#include "controller_policy.hpp"', '#include "controller_policy.hpp"\n#include "enrollment_abort.hpp"')
    prefix = prefix.replace('struct FakeTls{', 'struct FakeTls{\n std::vector<std::string> raw_sent;std::vector<bool> raw_bulk;bool send(const std::string& value,bool bulk=false){raw_sent.push_back(value);raw_bulk.push_back(bulk);return true;}bool extended_profile()const{return true;}bool hardware_profile()const{return true;}')
    prefix = prefix.replace('model::OctopusId peer;', 'model::OctopusId peer;model::BootId claimed_boot;std::optional<EnrollmentAbort> enrollment_abort;')
    prefix = prefix.replace('struct Active{', function(source, 'struct LinkTrace {').rstrip() + ';\nstruct Active{')
    prefix = prefix.replace('bool send(Active&,Kind,std::string_view,uint32_t ttl=1000);', '')
    actual = function((ROOT / 'components/consortium/tls_link.cpp').read_text(), 'std::string envelope_json(')
    actual += ''.join(function(source, name) for name in ('Envelope outgoing(', 'bool send(Active &link,', 'bool fence_replaced_links(', 'bool establish(', 'bool synchronize_connection('))
    with tempfile.TemporaryDirectory(prefix='amity-firmware-identity-') as directory:
        temp = Path(directory)
        (temp / 'esp_err.h').write_text('#pragma once\nusing esp_err_t=int;constexpr int ESP_OK=0,ESP_ERR_INVALID_ARG=1,ESP_ERR_INVALID_STATE=2,ESP_ERR_NO_MEM=3,ESP_ERR_TIMEOUT=4,ESP_FAIL=5,ESP_ERR_NOT_FOUND=6,ESP_ERR_NOT_SUPPORTED=7;\n')
        cpp, binary = temp / 'test.cpp', temp / 'test'
        cpp.write_text(timing_support() + prefix + EXTRA + 'namespace amity::consortium {\n' + actual + '\n}\n' + SUITE)
        sources = [ROOT / 'components/consortium' / name for name in ('json.cpp', 'wire.cpp', 'owned_intent.cpp', 'session.cpp', 'settings.cpp', 'update.cpp', 'transfer.cpp')]
        sources.append(ROOT / 'components/amity_system/model.cpp')
        subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-pthread', '-g', '-fsanitize=address,undefined', '-I' + str(temp)] + ['-I' + str(ROOT / path) for path in ('components/consortium', 'components/consortium/include', 'components/amity_system/include','components/network_diagnostics/include')] + list(map(str, [cpp] + sources)) + ['-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
