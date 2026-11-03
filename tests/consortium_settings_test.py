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
struct Active{Session session;model::OctopusId peer;std::shared_ptr<FakeTls> tls=std::make_shared<FakeTls>();std::mutex mutex;std::deque<Queued> queue;bool cancelled=false,retiring=false,ready=true,management_ready=false,restart_requested=false,execution_fenced=false;RuntimePeerStatus status;};
std::mutex lock;Group group;Identity identity;RuntimeStatus status;model::BootId boot=ident<model::BootId>(6);uint64_t configuration=1;std::vector<std::shared_ptr<Active>> links;
unsigned saves=0,restarts=0;uint64_t revision=2;bool send_success=true,applied_sent=false,http_sent=false;unsigned effect_fault=0;
std::shared_ptr<Active> current_link;std::vector<std::string> sent;
bool current_peer_credential(const Active& active,const Group& current){for(const auto& peer:current.peers)if(peer.id==active.peer&&peer.id==active.tls->authenticated&&peer.active)return true;return false;}
bool send(Active&,Kind kind,std::string_view payload,uint32_t){assert(kind==Kind::settings_result);sent.emplace_back(payload);Json parsed;JsonError detail;assert(parse_json(payload,parsed,detail)&&valid_settings_payload(kind,parsed));if(send_success&&parsed.get("stage")->string=="applied")applied_sent=true;return send_success;}
}
'''
HELPERS=r'''
namespace amity::consortium {
std::string snapshot(model::BootId target={}){if(!target)target=boot;return "{\"boot_id\":\""+target.str()+"\",\"settings_revision\":\""+std::to_string(revision)+"\",\"adapters\":[{\"tentacle_id\":3,\"name\":\"HDMI adapter 4\",\"configured\":true,\"default_front_physical_address\":\"A.0.0.0\",\"observed_front_physical_address\":null,\"effective_front_physical_address\":\"1.0.0.0\"}]}";}
Json parsed(std::string_view text){Json out;JsonError detail;assert(parse_json(text,out,detail));return out;}
Json core(model::BootId target={},uint64_t expected=2){if(!target)target=boot;return parsed("{\"expected_boot_id\":\""+target.str()+"\",\"expected_settings_revision\":\""+std::to_string(expected)+"\",\"changes\":[{\"tentacle_id\":3,\"default_front_physical_address\":\"A.0.0.0\"}]}");}
Error apply(model::MessageId id,const Json& body,uint64_t deadline,std::string& response){
 DeviceSettingsWrite write;Error error=parse_device_settings_write(body,write);
 if(effect_fault==1)clock_ms=deadline;
 if(effect_fault==2&&current_link)current_link->cancelled=true;
 if(effect_fault==3)++configuration;
 if(error==Error::none&&!runtime_device_settings_authorized(id))error=now_ms()>=deadline?Error::expired:Error::not_ready;
 if(error==Error::none&&write.expected_boot!=boot)error=Error::stale_boot;
 if(error==Error::none&&write.expected_revision!=revision)error=Error::transaction_conflict;
 if(error==Error::none){++saves;++revision;}
 response="{\"request_id\":\""+id.str()+"\",\"stage\":\""+(error==Error::none?"applied":"failed")+"\",\"error\":\""+(error==Error::none?"":settings_error(error))+"\",\"snapshot\":"+(error==Error::none?snapshot():"null")+"}";return error;
}
void reset(bool main){
 clock_ms=100;revision=2;configuration=1;saves=restarts=0;send_success=true;applied_sent=http_sent=false;effect_fault=0;sent.clear();links.clear();current_link.reset();group={};identity={};status={};settings_caches.clear();settings_operations.clear();settings_received.clear();settings_authorization={};identity.id=ident<model::OctopusId>(4);group.role=main?Group::coordinator:Group::member;group.authority=main?identity.id:ident<model::OctopusId>(3);group.consortium=ident<model::ConsortiumId>(2);group.epoch=1;group.revision=1;group.peers.push_back({ident<model::OctopusId>(3),"leaf","pin",true});status.authority_ready=main;status.coordinator=main;status.member=!main;
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
int main(int argc,char** argv){if(argc==2&&std::string(argv[1])=="--vectors"){using namespace amity::consortium;std::string line;while(std::getline(std::cin,line)){const auto at=line.find('\t');Json value;JsonError detail;bool valid=false;if(at!=std::string::npos&&parse_json(line.substr(at+1),value,detail)){const auto name=line.substr(0,at);if(name=="DeviceSettingsSnapshot")valid=valid_settings_snapshot(value);else if(name=="DeviceSettingsWrite"){DeviceSettingsWrite write;valid=parse_device_settings_write(value,write)==Error::none;}else if(name=="SettingsRead")valid=valid_settings_payload(Kind::settings_read,value);else if(name=="SettingsWrite")valid=valid_settings_payload(Kind::settings_write,value);else if(name=="SettingsResult")valid=valid_settings_payload(Kind::settings_result,value);}std::cout<<(valid?'1':'0')<<'\n';}return 0;}amity::consortium::codec_tests();amity::consortium::receiver_tests();amity::consortium::logical_session_authorization_tests();amity::consortium::local_tests();amity::consortium::sender_tests();}
'''
def schema_vectors(binary):
 try:
  import jsonschema
  from referencing import Registry,Resource
 except ImportError:
  print('Optional JSON Schema vector comparison unavailable in this Python; actual codec/dispatch tests passed.')
  return
 protocol=json.loads((ROOT/'components/consortium/protocol-v1.schema.json').read_text())
 resources=[protocol]+[json.loads(path.read_text()) for path in (ROOT/'amity-octopus-consortium/schemas').glob('*.json')]
 registry=Registry().with_resources((value['$id'],Resource.from_contents(value)) for value in resources)
 jsonschema.Draft202012Validator.check_schema(protocol)
 snapshot={'boot_id':'1'*32,'settings_revision':'2','adapters':[{'tentacle_id':3,'name':'HDMI adapter 4','configured':True,'default_front_physical_address':'A.0.0.0','observed_front_physical_address':None,'effective_front_physical_address':'1.0.0.0'}]}
 core={'expected_boot_id':'1'*32,'expected_settings_revision':'2','changes':[{'tentacle_id':3,'default_front_physical_address':'A.0.0.0'}]}
 vectors=[('DeviceSettingsSnapshot',snapshot),('DeviceSettingsWrite',core),('SettingsRead',{'request_id':'2'*32}),('SettingsWrite',{'request_id':'2'*32,'settings':core})]
 for pa in ['', '0.0.0.0','A.0.0.0','a.b.0.0','1.2.3.4','F.F.F.E','F.F.F.F','f.f.f.f','1.0.1.0','0.1.0.0','1.2.0.3','x.0.0.0','1.0.0','1.0.0.0.0']:
  value=copy.deepcopy(core);value['changes'][0]['default_front_physical_address']=pa;vectors.append(('DeviceSettingsWrite',value))
  value=copy.deepcopy(snapshot);value['adapters'][0]['default_front_physical_address']=pa;vectors.append(('DeviceSettingsSnapshot',value))
 for field,value in [('expected_boot_id','0'*32),('expected_settings_revision','0'),('expected_settings_revision','01'),('expected_settings_revision',2),('expected_settings_revision','18446744073709551616'),('confirm_restart',True),('extra',True)]:
  candidate=copy.deepcopy(core);candidate[field]=value;vectors.append(('DeviceSettingsWrite',candidate))
 for field,value in [('tentacle_id',4),('tentacle_id',-1),('tentacle_id','3'),('extra',True)]:
  candidate=copy.deepcopy(core);candidate['changes'][0][field]=value;vectors.append(('DeviceSettingsWrite',candidate))
 candidate=copy.deepcopy(snapshot);candidate['adapters'][0]['tentacle_id']=4;vectors.append(('DeviceSettingsSnapshot',candidate))
 for count in [0,2,5]:
  candidate=copy.deepcopy(core);candidate['changes']=candidate['changes']*count;vectors.append(('DeviceSettingsWrite',candidate))
 for configured in [False,True,'true',0,None]:
  candidate=copy.deepcopy(snapshot);candidate['adapters'][0]['configured']=configured;vectors.append(('DeviceSettingsSnapshot',candidate))
 for operation,stage,error,payload,reboot in itertools.product(['read','write'],['admitted','applied','failed'],['','INVALID_MESSAGE','STALE_BOOT','CONFLICT','NOT_READY','EXPIRED','STORAGE_FAILURE','QUEUE_FULL'],[None,snapshot,{}],[False,True]):
  vectors.append(('SettingsResult',{'request_id':'2'*32,'operation':operation,'stage':stage,'error':error,'snapshot':payload,'rebooting':reboot}))
 completed=subprocess.run([str(binary),'--vectors'],input=''.join(name+'\t'+json.dumps(value,separators=(',',':'))+'\n' for name,value in vectors),capture_output=True,text=True,check=True)
 actual=[value=='1' for value in completed.stdout.splitlines()];assert len(actual)==len(vectors)
 validators={name:jsonschema.Draft202012Validator({'$ref':protocol['$id']+'#/$defs/'+name},registry=registry) for name,_ in vectors}
 expected=[validators[name].is_valid(value) for name,value in vectors]
 for index,(left,right) in enumerate(zip(actual,expected)):
  assert left==right,(index,vectors[index],left,right)
 report={'vectors':len(vectors),'positive':sum(expected),'negative':len(expected)-sum(expected),'discrepancies':0,'actual_cpp_codec':True}
 print('PASS actual settings codec/JSON Schema:',report)

def main():
 source=(ROOT/'components/consortium/runtime.cpp').read_text();private=(ROOT/'components/consortium/settings_runtime.inc').read_text()
 public=''.join(function(source,name) for name in ['void runtime_settings_hooks(','bool runtime_device_settings_authorized(','std::string runtime_device_settings(','esp_err_t runtime_write_device_settings(','std::string runtime_device_settings_operation('])
 with tempfile.TemporaryDirectory(prefix='amity-settings-') as directory:
  directory=Path(directory);(directory/'esp_err.h').write_text('#pragma once\nusing esp_err_t=int;constexpr int ESP_OK=0,ESP_ERR_INVALID_ARG=1,ESP_ERR_INVALID_STATE=2,ESP_ERR_NO_MEM=3,ESP_ERR_TIMEOUT=4,ESP_FAIL=5;\n');cpp=directory/'test.cpp';binary=directory/'test';cpp.write_text(PREFIX+'namespace amity::consortium {\n'+private+public+'\n}\n'+HELPERS)
  sources=[ROOT/'components/consortium'/name for name in ['json.cpp','wire.cpp','session.cpp','owned_intent.cpp','settings.cpp','update.cpp','transfer.cpp']]+[ROOT/'components/amity_system/model.cpp']
  subprocess.run(shlex.split(os.environ.get('CXX','c++'))+['-std=c++17','-Wall','-Wextra','-Werror','-g','-fsanitize=address,undefined','-I'+str(directory)]+['-I'+str(ROOT/path) for path in ['components/consortium','components/consortium/include','components/amity_system/include']]+[str(cpp)]+list(map(str,sources))+['-o',str(binary)],check=True)
  subprocess.run([str(binary)],check=True)
  schema_vectors(binary)
if __name__=='__main__':main()
