#!/usr/bin/env python3
"""Execute typed settings codec, actual runtime dispatch/ledger and HTTP boundaries."""
from pathlib import Path
import os,shlex,subprocess,tempfile,json,itertools,copy
from consortium_gatt_callback_test import function
ROOT=Path(__file__).resolve().parents[1]
PREFIX=r'''
#include "amity/consortium/runtime.hpp"
#include "amity/consortium/settings.hpp"
#include "controller_policy.hpp"
#include "hostname_registry.hpp"
#include <cassert>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
namespace amity::consortium {
using namespace platform;
uint64_t clock_ms=100;uint64_t now_ms(){return clock_ms;}
template<class T>T ident(unsigned n){char v[33];snprintf(v,sizeof(v),"%032x",n);return *T::parse(v);}
template<class T>T random_id(){static unsigned next=1000;return ident<T>(next++);}
struct Queued{Kind kind;std::string payload;uint32_t ttl;uint64_t created;uint64_t revision=0;std::shared_ptr<const model::OwnedIntent> owned;};
struct FakeTls{model::OctopusId authenticated;};
struct Active{Session session;model::OctopusId peer;std::shared_ptr<FakeTls> tls=std::make_shared<FakeTls>();std::mutex mutex;std::deque<Queued> queue;bool cancelled=false,retiring=false,ready=true,management_ready=false,restart_requested=false,execution_fenced=false;RuntimePeerStatus status;std::string advertised_hostname;};
std::mutex lock;Group group;Identity identity;RuntimeStatus status;HostnameRegistry hostnames;RuntimeNetworkHooks network;struct{bool submitted=false;std::string hostname;RuntimeAdmissionStatus view;}admission;std::string local_hostname="main-name";bool registry_failure=false;model::BootId boot=ident<model::BootId>(6);uint64_t configuration=1;std::vector<std::shared_ptr<Active>> links;
unsigned saves=0,restarts=0;uint64_t revision=2,hostname_revision=10;bool send_success=true,applied_sent=false,http_sent=false;unsigned effect_fault=0;
std::shared_ptr<Active> current_link;std::vector<std::string> sent;
bool current_peer_credential(const Active& active,const Group& current){for(const auto& peer:current.peers)if(peer.id==active.peer&&peer.id==active.tls->authenticated&&peer.active)return true;return false;}
Error remember_hostname(model::OctopusId owner,const std::string& name){return hostnames.remember(owner,name,[](std::string_view){return !registry_failure;});}
bool send(Active&,Kind kind,std::string_view payload,uint32_t){sent.emplace_back(payload);Json parsed;JsonError detail;assert(parse_json(payload,parsed,detail)&&valid_envelope_payload(kind,parsed));if(send_success&&kind==Kind::settings_result&&parsed.get("stage")->string=="applied")applied_sent=true;return send_success;}
}
'''
HELPERS=r'''
namespace amity::consortium {
std::string snapshot(model::BootId target={}){if(!target)target=boot;return "{\"boot_id\":\""+target.str()+"\",\"settings_revision\":\""+std::to_string(revision)+"\",\"hostname_revision\":\""+std::to_string(hostname_revision)+"\",\"hostname\":\""+local_hostname+"\",\"adapters\":[{\"tentacle_id\":3,\"name\":\"HDMI adapter 4\",\"configured\":true,\"default_front_physical_address\":\"A.0.0.0\",\"observed_front_physical_address\":null,\"effective_front_physical_address\":\"1.0.0.0\"}]}";}
Json parsed(std::string_view text){Json out;JsonError detail;assert(parse_json(text,out,detail));return out;}
Json core(model::BootId target={},uint64_t expected=2){if(!target)target=boot;return parsed("{\"expected_boot_id\":\""+target.str()+"\",\"expected_settings_revision\":\""+std::to_string(expected)+"\",\"changes\":[{\"tentacle_id\":3,\"default_front_physical_address\":\"A.0.0.0\"}]}");}
Json rename_core(std::string_view name="Living-Room",model::BootId target={},uint64_t expected=10){if(!target)target=boot;return parsed("{\"expected_boot_id\":\""+target.str()+"\",\"expected_hostname_revision\":\""+std::to_string(expected)+"\",\"hostname\":\""+std::string(name)+"\",\"changes\":[]}");}
Error apply(model::MessageId id,const Json& body,uint64_t deadline,std::string& response){
 DeviceSettingsWrite write;Error error=parse_device_settings_write(body,write);
 if(effect_fault==1)clock_ms=deadline;
 if(effect_fault==2&&current_link)current_link->cancelled=true;
 if(effect_fault==3)++configuration;
 if(error==Error::none&&!runtime_device_settings_authorized(id))error=now_ms()>=deadline?Error::expired:Error::not_ready;
 if(error==Error::none&&write.expected_boot!=boot)error=Error::stale_boot;
 if(error==Error::none&&(write.hostname?write.expected_hostname_revision!=hostname_revision:write.expected_revision!=revision))error=Error::transaction_conflict;
 if(error==Error::none){++saves;if(write.hostname){local_hostname=*write.hostname;++hostname_revision;}else ++revision;}
 response="{\"request_id\":\""+id.str()+"\",\"stage\":\""+(error==Error::none?"applied":"failed")+"\",\"error\":\""+(error==Error::none?"":settings_error(error))+"\",\"snapshot\":"+(error==Error::none?snapshot():"null")+"}";return error;
}
void reset(bool main){
 clock_ms=100;revision=2;hostname_revision=10;local_hostname="main-name";registry_failure=false;hostnames={};network={};admission={};network.get_hostname=[]{return local_hostname;};configuration=1;saves=restarts=0;send_success=true;applied_sent=http_sent=false;effect_fault=0;sent.clear();links.clear();current_link.reset();group={};identity={};status={};settings_caches.clear();settings_operations.clear();settings_received.clear();settings_authorization={};identity.id=ident<model::OctopusId>(4);group.role=main?Group::coordinator:Group::member;group.authority=main?identity.id:ident<model::OctopusId>(3);group.consortium=ident<model::ConsortiumId>(2);group.epoch=1;group.revision=1;group.peers.push_back({ident<model::OctopusId>(3),"leaf","pin",true});status.authority_ready=main;status.coordinator=main;status.member=!main;
 settings_hooks={[]{return snapshot();},apply};
}
std::shared_ptr<Active> connection(bool receiver){
 auto active=std::make_shared<Active>();active->peer=ident<model::OctopusId>(3);active->tls->authenticated=active->peer;
 LinkScope sc{ident<model::LinkId>(5),group.consortium,identity.id,active->peer,group.authority,boot,ident<model::BootId>(7),1,1};
 assert(active->session.authenticate(sc,sc.peer_id,receiver?PeerRole::coordinator:PeerRole::actuator,true)==Error::none);
 HelloFrame hello;hello.consortium_id=sc.consortium_id;hello.octopus_id=sc.peer_id;hello.boot_id=sc.peer_boot_id;hello.coordinator_epoch=1;hello.config_revision=1;hello.required_capabilities=known_capabilities;
 assert(active->session.hello(hello,known_capabilities)==Error::none&&active->session.synchronized(1,true,true)==Error::none);
 active->status.id=active->peer;active->status.boot_id=sc.peer_boot_id;active->status.config_revision=1;active->status.negotiated_capabilities=known_capabilities;active->status.ready=true;
 links.push_back(active);current_link=active;return active;
}
Envelope message(const std::shared_ptr<Active>& active,Kind kind,const Json& payload){auto sc=active->session.scope();Envelope e;e.consortium_id=sc.consortium_id;e.origin_octopus_id=sc.peer_id;e.destination_octopus_id=sc.local_id;e.origin_boot_id=sc.peer_boot_id;e.destination_boot_id=sc.local_boot_id;e.coordinator_epoch=1;e.config_revision=1;e.kind=kind;e.ttl_ms=1000;e.payload=payload;return e;}
Envelope write_message(const std::shared_ptr<Active>& active,unsigned request=20,const Json* body=nullptr){auto content=body?*body:core();return message(active,Kind::settings_write,parsed("{\"request_id\":\""+ident<model::MessageId>(request).str()+"\",\"settings\":"+settings_json(content)+"}"));}
void codec_tests(){
 reset(true);auto snap=parsed(snapshot());assert(valid_settings_snapshot(snap));snap.get("adapters")->array[0].get("configured")->boolean=false;assert(valid_settings_snapshot(snap));
 for(const auto* valid:{"","0.0.0.0","A.0.0.0","a.1.0.0","1.2.3.4"}){auto value=core();value.get("changes")->array[0].get("default_front_physical_address")->string=valid;DeviceSettingsWrite write;assert(parse_device_settings_write(value,write)==Error::none);}
 for(const auto* invalid:{"F.F.F.F","1.0.1.0","1.2.0.3","x.0.0.0","1.0.0","1.0.0.0.0"}){auto value=core();value.get("changes")->array[0].get("default_front_physical_address")->string=invalid;DeviceSettingsWrite write;assert(parse_device_settings_write(value,write)!=Error::none);}
 auto duplicate=core();duplicate.get("changes")->array.push_back(duplicate.get("changes")->array[0]);DeviceSettingsWrite decoded;assert(parse_device_settings_write(duplicate,decoded)!=Error::none);
 auto unsupported=core();unsupported.get("changes")->array[0].get("tentacle_id")->integer=4;assert(parse_device_settings_write(unsupported,decoded)!=Error::none);
 snap=parsed(snapshot());snap.get("adapters")->array[0].get("tentacle_id")->integer=4;assert(!valid_settings_snapshot(snap));
 std::cout<<"PASS closed settings codec: fourth stable slot, configuredslot validation, blank/root/case PAcompat, invalidtopology/duplicate slots rejected\n";
}
void hostname_tests(){
 reset(true);DeviceSettingsWrite decoded;
 auto rename=rename_core();
 assert(parse_device_settings_write(rename,decoded)==Error::none&&decoded.hostname&&*decoded.hostname=="living-room"&&decoded.changes.empty());
 assert(parsed(device_settings_write_json(decoded)).get("hostname")->string=="living-room");
 for(const auto* invalid:{"","-room","room-","room.local","room name","a_b","abcdefghijklmnopqrstuvwxyz1234567"}){auto bad=rename;bad.get("hostname")->string=invalid;assert(parse_device_settings_write(bad,decoded)==Error::invalid_message);}
 auto empty=core();empty.get("changes")->array.clear();assert(parse_device_settings_write(empty,decoded)==Error::invalid_message);
 auto bad_snapshot=parsed(snapshot());bad_snapshot.get("hostname")->string="Mixed-Case";assert(!valid_settings_snapshot(bad_snapshot));
 auto mixed=rename;mixed.get("changes")->array=core().get("changes")->array;assert(parse_device_settings_write(mixed,decoded)==Error::invalid_message);
 auto wrong_revision=rename;wrong_revision.object.emplace_back("expected_settings_revision",parsed("\"2\""));assert(parse_device_settings_write(wrong_revision,decoded)==Error::invalid_message);
 auto cross_revision=core();cross_revision.object.emplace_back("expected_hostname_revision",parsed("\"10\""));assert(parse_device_settings_write(cross_revision,decoded)==Error::invalid_message);

 reset(false);auto active=connection(true);const auto original=active->session.scope();const auto tls=active->tls;
 assert(settings_dispatch(active,write_message(active,40,&rename),clock_ms)&&saves==1&&local_hostname=="living-room"&&revision==2&&hostname_revision==11);
 assert(active->session.scope()==original&&active->tls==tls&&!active->cancelled&&!restarts);
 assert(parsed(sent.back()).get("snapshot")->get("hostname")->string=="living-room");
 // A hostname save admitted from an older adapter snapshot stays valid,
 // and changing the hostname does not stale a prepared adapter update.
 reset(false);active=connection(true);const auto adapter_update=core({},3);
 assert(settings_dispatch(active,write_message(active,41),clock_ms)&&revision==3&&hostname_revision==10);
 assert(settings_dispatch(active,write_message(active,42,&rename),clock_ms)&&revision==3&&hostname_revision==11);
 assert(settings_dispatch(active,write_message(active,43,&adapter_update),clock_ms)&&revision==4&&hostname_revision==11&&saves==3);
 std::cout<<"PASS hostname codec/receiver: normalized rename-only write, closed current snapshot, durable same-boot change on the same authenticated TLS stream\n";
}
void live_hostname_tests(){
 reset(true);auto active=connection(false);const auto original=active->session.scope();const auto tls=active->tls;
 active->advertised_hostname=local_hostname;assert(refresh_local_hostname(*active)&&sent.empty());
 local_hostname="renamed-main";assert(refresh_local_hostname(*active)&&sent.size()==1&&parsed(sent.back()).get("hostname")->string==local_hostname);
 assert(refresh_local_hostname(*active)&&sent.size()==1);
 auto update=message(active,Kind::device_status,parsed("{\"hostname\":\"renamed-member\"}"));
 assert(receive_live_hostname(*active,update,clock_ms)&&active->status.hostname=="renamed-member"&&hostnames.lookup(active->peer)=="renamed-member");
 registry_failure=true;update.payload.get("hostname")->string="live-member";
 assert(receive_live_hostname(*active,update,clock_ms)&&active->status.hostname=="live-member"&&hostnames.lookup(active->peer)=="renamed-member"&&!status.last_error.empty());
 update.destination_boot_id=ident<model::BootId>(99);assert(!receive_live_hostname(*active,update,clock_ms));
 assert(active->session.scope()==original&&active->tls==tls&&active->ready&&!active->cancelled&&!restarts);
 std::cout<<"PASS live hostname metadata: one update per changed name, same TLS/session/boot, authenticated admission, metadata storage failure keeps the healthy connection\n";
}
void receiver_tests(){
 reset(false);auto active=connection(true);auto request=write_message(active);assert(settings_dispatch(active,request,clock_ms)&&saves==1&&restarts==0);assert(parsed(sent.back()).get("stage")->string=="applied");
 assert(settings_dispatch(active,request,clock_ms)&&saves==1&&restarts==0);
 const auto original_scope=active->session.scope();const auto original_tls=active->tls;
 const auto next=core({},3);assert(settings_dispatch(active,write_message(active,21,&next),clock_ms)&&saves==2&&revision==4);
 assert(active->session.scope()==original_scope&&active->tls==original_tls&&active->ready&&!active->cancelled&&!restarts);
 for(unsigned failure=0;failure<7;++failure){reset(false);active=connection(true);request=write_message(active);if(failure==0)request.destination_boot_id=ident<model::BootId>(99);if(failure==1)request.origin_octopus_id=ident<model::OctopusId>(99);if(failure==2)request.config_revision=2;if(failure>=3&&failure<=5)effect_fault=failure-2;if(failure==6){active->session.disconnect();}assert(!settings_dispatch(active,request,clock_ms)||parsed(sent.back()).get("stage")->string=="failed");assert(!saves&&!restarts);}
 reset(false);active=connection(true);send_success=false;request=write_message(active);assert(!settings_dispatch(active,request,clock_ms)&&saves==1&&restarts==0);send_success=true;assert(settings_dispatch(active,request,clock_ms)&&saves==1&&restarts==0); // Lost results do not resave or restart the live owner.
 reset(true);active=connection(false);request=write_message(active);assert(!settings_dispatch(active,request,clock_ms)&&!saves&&!restarts);
 std::cout<<"PASS actual target dispatch: originalmain scope/deadline/config/revocation guards, no memberforwarding, exact duplicate outcome, same-stream consecutive saves and lost-result safety\n";
}
void logical_session_authorization_tests(){
 reset(false);auto active=connection(true);const auto original=active->session.scope();const auto tls=active->tls;
 const auto fresh_link=ident<model::LinkId>(91);
 bool checked=false;
 settings_hooks.apply=[&](model::MessageId id,const Json&,uint64_t deadline,std::string& response){
  assert(runtime_device_settings_authorized(id)&&deadline>now_ms());
  active->execution_fenced=true;assert(!runtime_device_settings_authorized(id));active->execution_fenced=false;
  active->restart_requested=true;assert(!runtime_device_settings_authorized(id));active->restart_requested=false;
  assert(runtime_device_settings_authorized(id));
  {std::lock_guard<std::mutex> guard(lock);active->ready=false;assert(active->session.begin_synchronization(original.config_revision,fresh_link)==Error::none);}
  assert(!runtime_device_settings_authorized(id));
  {std::lock_guard<std::mutex> guard(lock);
   assert(active->session.synchronized(original.config_revision,true,true)==Error::none);
   active->ready=true;
  }
  // Ready and fresh authority on the same TLS object must not revive the
  // settings write admitted to the ended logical execution session.
  assert(!runtime_device_settings_authorized(id));
  auto expected=original;expected.link_id=fresh_link;assert(active->session.scope()==expected&&active->tls==tls);
  checked=true;response="{\"request_id\":\""+id.str()+"\",\"stage\":\"failed\",\"error\":\"NOT_READY\",\"snapshot\":null}";return Error::not_ready;
 };
 auto request=write_message(active);assert(settings_dispatch(active,request,clock_ms)&&checked&&!saves&&!restarts);
 assert(parsed(sent.back()).get("stage")->string=="failed");
 settings_hooks.apply=apply;request=write_message(active,21);
 assert(settings_dispatch(active,request,clock_ms)&&saves==1&&restarts==0);
 assert(parsed(sent.back()).get("stage")->string=="applied"&&active->tls==tls&&active->session.scope().link_id==fresh_link);
 std::cout<<"PASS actual settings authorization: ended logical-session permit stays invalid after same-TLS/boots/config synchronization; fresh-session write commits normally\n";
}
void local_tests(){
 reset(true);const auto id=ident<model::MessageId>(30);const auto original_boot=boot;
 assert(runtime_write_device_settings(identity.id,id,core(),clock_ms+5000)==ESP_OK&&saves==1&&!restarts);
 auto result=parsed(runtime_device_settings_operation(identity.id,id));assert(result.get("stage")->string=="applied"&&!result.get("rebooting")->boolean&&boot==original_boot);
 assert(runtime_write_device_settings(identity.id,id,core(),clock_ms+5000)==ESP_OK&&saves==1&&!restarts);
 assert(runtime_write_device_settings(identity.id,ident<model::MessageId>(31),core({},3),clock_ms+5000)==ESP_OK&&saves==2&&revision==4&&boot==original_boot);
 // Every terminal outcome has the same bounded lifetime. Eviction never
 // authorizes a stale revision to repeat an already committed write.
 for(unsigned n=0;n<12;++n)assert(runtime_write_device_settings(identity.id,ident<model::MessageId>(100+n),core({},1),clock_ms+5000)==ESP_OK);
 assert(settings_operations.size()<=8&&settings_received.size()<=8);
 assert(parsed(runtime_device_settings_operation(identity.id,id)).get("stage")->string=="unknown");
 assert(runtime_write_device_settings(identity.id,id,core(),clock_ms+5000)==ESP_OK&&saves==2&&!restarts);
 assert(parsed(runtime_device_settings_operation(identity.id,id)).get("stage")->string=="failed");
 reset(false);assert(runtime_write_device_settings(identity.id,id,core(),clock_ms+5000)==ESP_ERR_INVALID_STATE&&!saves);
 std::cout<<"PASS actual local HTTP boundary: original main only, live durable outcome, same-boot consecutive saves, pure polling, bounded eight terminal outcomes and stale-eviction fences\n";
}
void rename_sender_tests(){
 reset(true);auto active=connection(false);const auto original=active->session.scope();const auto tls=active->tls;
 assert(remember_hostname(active->peer,"member-old")==Error::none);
 auto rename=rename_core("member-new",active->status.boot_id);
 const auto request=ident<model::MessageId>(90);
 admission.submitted=true;admission.view.phase="connecting";admission.hostname="member-new";
 assert(runtime_write_device_settings(active->peer,request,rename,clock_ms+5000)==ESP_ERR_INVALID_ARG);
 admission={};
 assert(runtime_write_device_settings(active->peer,request,rename,clock_ms+5000)==ESP_OK);
 assert(settings_hostname_pending({},"member-new")&&!settings_hostname_pending(active->peer,"member-new"));
 auto concurrent=rename_core("member-new");
 assert(runtime_write_device_settings(identity.id,ident<model::MessageId>(91),concurrent,clock_ms+5000)==ESP_ERR_INVALID_ARG);
 auto renamed=parsed(snapshot(active->status.boot_id));renamed.get("hostname")->string="member-new";
 auto result=message(active,Kind::settings_result,parsed(settings_result(request,"write","applied","",settings_json(renamed))));
 assert(settings_dispatch(active,result,clock_ms)&&hostnames.lookup(active->peer)=="member-new"&&active->status.hostname=="member-new");
 assert(parsed(runtime_device_settings_operation(active->peer,request)).get("stage")->string=="applied");
 assert(runtime_write_device_settings(identity.id,ident<model::MessageId>(92),concurrent,clock_ms+5000)==ESP_ERR_INVALID_ARG);
 rename.get("hostname")->string=local_hostname;assert(runtime_write_device_settings(active->peer,ident<model::MessageId>(93),rename,clock_ms+5000)==ESP_ERR_INVALID_ARG);
 assert(active->session.scope()==original&&active->tls==tls&&active->ready&&!active->cancelled&&!restarts);
 std::cout<<"PASS rename sender: pending duplicate-name reservation, main/member name collision rejection and immediate authenticated member metadata on the existing connection\n";
}
void sender_tests(){
 reset(true);auto active=connection(false);auto wrapper=parsed(runtime_device_settings(active->peer));assert(wrapper.get("status")->string=="reading"&&active->queue.size()==1);runtime_device_settings(active->peer);assert(active->queue.size()==1);
 active->execution_fenced=true;assert(!settings_link(active->peer).active);active->execution_fenced=false;
 active->restart_requested=true;assert(!settings_link(active->peer).active);active->restart_requested=false;
 assert(settings_link(active->peer).active==active&&active->queue.size()==1);
 const auto read=parsed(active->queue.front().payload);const auto id=*model::MessageId::parse(read.get("request_id")->string);auto reply=message(active,Kind::settings_result,parsed(settings_result(id,"read","applied","",snapshot(active->status.boot_id))));assert(settings_dispatch(active,reply,clock_ms));assert(parsed(runtime_device_settings(active->peer)).get("status")->string=="ready");
 const auto command=ident<model::MessageId>(80);assert(runtime_write_device_settings(active->peer,command,core(active->status.boot_id),clock_ms+500)==ESP_OK);assert(active->queue.back().ttl<=500);assert(parsed(runtime_device_settings_operation(active->peer,command)).get("stage")->string=="admitted");
 active->cancelled=true;assert(parsed(runtime_device_settings(active->peer)).get("status")->string=="offline");assert(parsed(runtime_device_settings_operation(active->peer,command)).get("stage")->string=="unknown");const auto queued=active->queue.size();assert(runtime_write_device_settings(active->peer,command,core(active->status.boot_id),clock_ms+5000)==ESP_OK&&active->queue.size()==queued&&!restarts);
 active->cancelled=false;active->status.negotiated_capabilities&=~per_adapter_settings;assert(parsed(runtime_device_settings(active->peer)).get("status")->string=="unsupported");
 std::cout<<"PASS actual main client: coalescedreadonlyGET, authenticatedfreshcache, offline/unsupporteduneditable, preservedoriginalTTL, disconnectunknown and noautomaticresend\n";
}
}
int main(int argc,char** argv){if(argc==2&&std::string(argv[1])=="--vectors"){using namespace amity::consortium;std::string line;while(std::getline(std::cin,line)){const auto at=line.find('\t');Json value;JsonError detail;bool valid=false;if(at!=std::string::npos&&parse_json(line.substr(at+1),value,detail)){const auto name=line.substr(0,at);if(name=="DeviceSettingsSnapshot")valid=valid_settings_snapshot(value);else if(name=="DeviceSettingsWrite"){DeviceSettingsWrite write;valid=parse_device_settings_write(value,write)==Error::none;}else if(name=="SettingsRead")valid=valid_settings_payload(Kind::settings_read,value);else if(name=="SettingsWrite")valid=valid_settings_payload(Kind::settings_write,value);else if(name=="SettingsResult")valid=valid_settings_payload(Kind::settings_result,value);}std::cout<<(valid?'1':'0')<<'\n';}return 0;}amity::consortium::codec_tests();amity::consortium::hostname_tests();amity::consortium::live_hostname_tests();amity::consortium::rename_sender_tests();amity::consortium::receiver_tests();amity::consortium::logical_session_authorization_tests();amity::consortium::local_tests();amity::consortium::sender_tests();}
'''
def parser_vectors(binary):
 # These are protocol boundary expectations, independent of a second schema.
 # Serialize at insertion so later fixture mutations cannot change an earlier
 # case, and reject conflicting expectations for the same input.
 vectors = {}
 def add(name, value, expected):
  key = (name, json.dumps(value, separators=(',', ':'), sort_keys=True))
  if key in vectors:
   assert vectors[key] == expected, ('conflicting expectations', key)
  vectors[key] = expected

 snapshot={'boot_id':'1'*32,'settings_revision':'2','hostname_revision':'10','hostname':'amity','adapters':[{'tentacle_id':3,'name':'HDMI adapter 4','configured':True,'default_front_physical_address':'A.0.0.0','observed_front_physical_address':None,'effective_front_physical_address':'1.0.0.0'}]}
 core={'expected_boot_id':'1'*32,'expected_settings_revision':'2','changes':[{'tentacle_id':3,'default_front_physical_address':'A.0.0.0'}]}
 add('DeviceSettingsSnapshot', snapshot, True)
 add('DeviceSettingsWrite', core, True)
 add('SettingsRead', {'request_id':'2'*32}, True)
 add('SettingsWrite', {'request_id':'2'*32,'settings':core}, True)
 # Writes normalize ASCII case; observed names are already canonical. A rename
 # has its own revision and cannot carry adapter edits or their revision.
 names=[('amity',True,True),('Living-Room',True,False),('a',True,True),('a'*32,True,True),
        ('',False,False),('a'*33,False,False),('-room',False,False),('room-',False,False),
        ('room.local',False,False),('room name',False,False),('a_b',False,False),
        ('room\n',False,False),('room\r',False,False),(None,False,False),(42,False,False)]
 for name, writable, observable in names:
  candidate=copy.deepcopy(core);candidate['hostname']=name;add('DeviceSettingsWrite',candidate,False)
  candidate['changes']=[];add('DeviceSettingsWrite',candidate,False)
  del candidate['expected_settings_revision'];candidate['expected_hostname_revision']='10';add('DeviceSettingsWrite',candidate,writable)
  candidate=copy.deepcopy(snapshot);candidate['hostname']=name;add('DeviceSettingsSnapshot',candidate,observable)
 for field in ['hostname','hostname_revision']:
  candidate=copy.deepcopy(snapshot);del candidate[field];add('DeviceSettingsSnapshot',candidate,False)
 rename={'expected_boot_id':'1'*32,'expected_hostname_revision':'10','hostname':'amity','changes':[]}
 for field,value in [('expected_hostname_revision','0'),('expected_hostname_revision','01'),('expected_hostname_revision',10),('expected_hostname_revision','18446744073709551616'),('expected_settings_revision','2'),('changes',core['changes'])]:
  candidate=copy.deepcopy(rename);candidate[field]=value;add('DeviceSettingsWrite',candidate,False)
 for field in ['expected_hostname_revision','hostname']:
  candidate=copy.deepcopy(rename);del candidate[field];add('DeviceSettingsWrite',candidate,False)

 addresses=[('',True,False),('0.0.0.0',True,True),('A.0.0.0',True,True),('a.b.0.0',True,True),
            ('1.2.3.4',True,True),('F.F.F.E',True,True),('F.F.F.F',False,False),
            ('f.f.f.f',False,False),('1.0.1.0',False,False),('0.1.0.0',False,False),
            ('1.2.0.3',False,False),('x.0.0.0',False,False),('1.0.0',False,False),('1.0.0.0.0',False,False)]
 for address, writable, observable in addresses:
  candidate=copy.deepcopy(core);candidate['changes'][0]['default_front_physical_address']=address;add('DeviceSettingsWrite',candidate,writable)
  candidate=copy.deepcopy(snapshot);candidate['adapters'][0]['default_front_physical_address']=address;add('DeviceSettingsSnapshot',candidate,observable)
 for field,value in [('expected_boot_id','0'*32),('expected_settings_revision','0'),('expected_settings_revision','01'),('expected_settings_revision',2),('expected_settings_revision','18446744073709551616'),('confirm_restart',True),('extra',True)]:
  candidate=copy.deepcopy(core);candidate[field]=value;add('DeviceSettingsWrite',candidate,False)
 for field,value in [('tentacle_id',4),('tentacle_id',-1),('tentacle_id','3'),('extra',True)]:
  candidate=copy.deepcopy(core);candidate['changes'][0][field]=value;add('DeviceSettingsWrite',candidate,False)
 candidate=copy.deepcopy(snapshot);candidate['adapters'][0]['tentacle_id']=4;add('DeviceSettingsSnapshot',candidate,False)
 for count in [0,2,5]:
  candidate=copy.deepcopy(core);candidate['changes']=candidate['changes']*count;add('DeviceSettingsWrite',candidate,False)
 for configured, expected in [(False,True),(True,True),('true',False),(0,False),(None,False)]:
  candidate=copy.deepcopy(snapshot);candidate['adapters'][0]['configured']=configured;add('DeviceSettingsSnapshot',candidate,expected)

 # Complete results carry a snapshot; admission and failure carry none. Only
 # write operations have an admission result, and no settings result reboots.
 errors=['INVALID_MESSAGE','STALE_BOOT','CONFLICT','NOT_READY','EXPIRED','STORAGE_FAILURE','QUEUE_FULL']
 accepted_results={('read','applied','',1,False),('write','applied','',1,False),('write','admitted','',0,False)}
 accepted_results.update((operation,'failed',error,0,False) for operation in ['read','write'] for error in errors)
 payloads=[None,snapshot,{}]
 for operation,stage,error,payload_index,reboot in itertools.product(['read','write'],['admitted','applied','failed'],['']+errors,range(len(payloads)),[False,True]):
  key=(operation,stage,error,payload_index,reboot)
  add('SettingsResult',{'request_id':'2'*32,'operation':operation,'stage':stage,'error':error,'snapshot':payloads[payload_index],'rebooting':reboot},key in accepted_results)
 completed=subprocess.run([str(binary),'--vectors'],input=''.join(name+'\t'+value+'\n' for name,value in vectors),capture_output=True,text=True,check=True)
 actual=completed.stdout.splitlines()
 assert len(actual)==len(vectors) and set(actual)<={'0','1'}
 for (key,expected), observed in zip(vectors.items(),actual):
  assert (observed=='1')==expected,(key,observed,expected)
 positives=sum(vectors.values());negatives=len(vectors)-positives
 assert positives>0 and negatives>0
 print('PASS actual settings parser boundaries:',{'vectors':len(vectors),'accepted':positives,'rejected':negatives})

def main():
 source=(ROOT/'components/consortium/runtime.cpp').read_text();private=''.join(function(source,name) for name in ['void remember_live_hostname(','bool refresh_local_hostname(','bool receive_live_hostname('])+(ROOT/'components/consortium/settings_runtime.inc').read_text()
 public=''.join(function(source,name) for name in ['void runtime_settings_hooks(','bool runtime_device_settings_authorized(','std::string runtime_device_settings(','esp_err_t runtime_write_device_settings(','std::string runtime_device_settings_operation('])
 with tempfile.TemporaryDirectory(prefix='amity-settings-') as directory:
  directory=Path(directory);(directory/'esp_err.h').write_text('#pragma once\nusing esp_err_t=int;constexpr int ESP_OK=0,ESP_ERR_INVALID_ARG=1,ESP_ERR_INVALID_STATE=2,ESP_ERR_NO_MEM=3,ESP_ERR_TIMEOUT=4,ESP_FAIL=5;\n');cpp=directory/'test.cpp';binary=directory/'test';cpp.write_text(PREFIX+'namespace amity::consortium {\n'+private+public+'\n}\n'+HELPERS)
  sources=[ROOT/'components/consortium'/name for name in ['json.cpp','wire.cpp','session.cpp','owned_intent.cpp','settings.cpp','update.cpp','transfer.cpp']]+[ROOT/'components/amity_system/model.cpp']
  subprocess.run(shlex.split(os.environ.get('CXX','c++'))+['-std=c++17','-Wall','-Wextra','-Werror','-g','-fsanitize=address,undefined','-I'+str(directory)]+['-I'+str(ROOT/path) for path in ['components/consortium','components/consortium/include','components/amity_system/include']]+[str(cpp)]+list(map(str,sources))+['-o',str(binary)],check=True)
  subprocess.run([str(binary)],check=True)
  parser_vectors(binary)
if __name__=='__main__':main()
