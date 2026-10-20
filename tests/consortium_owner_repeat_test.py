#!/usr/bin/env python3
"""Execute actual link-loss fences, state-change codecs and release queue policy."""
from pathlib import Path
import os,shlex,subprocess,tempfile,json,copy,itertools
from consortium_gatt_callback_test import function
ROOT=Path(__file__).resolve().parents[1]
PREFIX=r'''
#include "amity/consortium/runtime.hpp"
#include "amity/consortium/update.hpp"
#include "controller_policy.hpp"
#include <algorithm>
#include <cassert>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <sys/socket.h>
namespace amity::consortium {
using namespace platform;
uint64_t clock_ms=100;uint64_t now_ms(){return clock_ms;}
template<class T>T ident(unsigned n){char value[33];snprintf(value,sizeof(value),"%032x",n);return *T::parse(value);}
struct Queued{Kind kind;std::string payload;uint32_t ttl;uint64_t created;};
struct Tls{int socket=-1;model::OctopusId authenticated;};
struct Active{Session session;std::shared_ptr<Tls> tls=std::make_shared<Tls>();model::OctopusId peer;std::mutex mutex,close_mutex;bool cancelled=false,retiring=false,ready=true,management_ready=false,closed_notified=false,credential=true;RuntimePeerStatus status;std::deque<Queued> queue;};
std::mutex lock;std::vector<std::shared_ptr<Active>> links;RuntimeControlHooks controls;RuntimeStatus status;Group group;
bool current_peer_credential(const Active& active,const Group&){return active.credential;}
esp_err_t runtime_send_update(model::OctopusId,Kind,std::string,uint32_t){return ESP_ERR_NOT_SUPPORTED;}
}
'''
SUITE=r'''
namespace amity::consortium {
std::shared_ptr<Active> connection(unsigned peer=3,unsigned link=7,unsigned peerboot=8){
 auto active=std::make_shared<Active>();active->peer=ident<model::OctopusId>(peer);active->tls->authenticated=active->peer;active->status.id=active->peer;active->status.boot_id=ident<model::BootId>(peerboot);
 const LinkScope scope{ident<model::LinkId>(link),ident<model::ConsortiumId>(1),ident<model::OctopusId>(2),active->peer,ident<model::OctopusId>(2),ident<model::BootId>(4),active->status.boot_id,1,1};
 assert(active->session.authenticate(scope,scope.peer_id,PeerRole::actuator,true)==Error::none);
 return active;
}
void reset(){links.clear();controls={};status={};status.authority_ready=true;group={};group.role=Group::coordinator;clock_ms=100;}
void closure_tests(){
 reset();auto old=connection(),current=connection(3,9);links={old,current};unsigned calls=0;model::LinkId notified;
 controls.disconnected=[&](auto peer,auto boot,auto link){++calls;assert(peer==old->peer&&boot==old->status.boot_id);notified=link;return true;};
 assert(fence_replaced_links(*current)&&old->cancelled&&!old->ready&&!current->cancelled&&calls==1&&notified==ident<model::LinkId>(7));
 assert(notify_link_closed(*old)&&calls==1);reap_closed_links();assert(links.size()==1&&links[0]==current);
 reset();old=connection();current=connection(3,9);links={old,current};calls=0;bool owner_ready=false;
 controls.disconnected=[&](auto,auto,auto){++calls;return owner_ready;};
 assert(!fence_replaced_links(*current)&&!old->closed_notified&&calls==1);reap_closed_links();assert(links.size()==2&&calls==2);owner_ready=true;assert(fence_replaced_links(*current)&&calls==3);reap_closed_links();assert(links.size()==1);
 // A replacement waits for the in-progress old owner fence, rather than seeing
 // a preemptively-set 'notified' flag and becoming ready before actual cleanup.
 reset();old=connection();current=connection(3,9);links={old,current};std::mutex gate;std::condition_variable cv;bool entered=false,finish=false;calls=0;
 controls.disconnected=[&](auto,auto,auto){std::unique_lock<std::mutex> guard(gate);++calls;entered=true;cv.notify_all();cv.wait(guard,[&]{return finish;});return true;};
 auto closing=std::async(std::launch::async,[&]{return notify_link_closed(*old);});{std::unique_lock<std::mutex> guard(gate);cv.wait(guard,[&]{return entered;});}
 auto replacement=std::async(std::launch::async,[&]{return fence_replaced_links(*current);});assert(replacement.wait_for(std::chrono::milliseconds(30))==std::future_status::timeout);
 {std::lock_guard<std::mutex> guard(gate);finish=true;}cv.notify_all();assert(closing.get()&&replacement.get()&&calls==1);
 std::cout<<"PASS actual link close/replacement: exact old boot/link, synchronous one-shot owner fence, failed-fence retry and no new-ready race\n";
}
void queue_tests(const std::string& root){
 reset();auto active=connection();links={active};for(unsigned n=0;n<15;++n)assert(runtime_send_control(active->peer,Kind::heartbeat,"{}",1000)==ESP_OK);
 assert(runtime_send_control(active->peer,Kind::heartbeat,"{}",1000)==ESP_ERR_NO_MEM);
 assert(runtime_send_cleanup(active->peer,"{}",active->session.scope().link_id,2000)==ESP_OK&&active->queue.size()==16&&active->queue.front().kind==Kind::hold_release);
 assert(runtime_send_cleanup(active->peer,"{}",active->session.scope().link_id,2000)==ESP_ERR_NO_MEM&&active->cancelled); // Hard link fence rather than losing key-up.
 reset();active=connection();links={active};assert(runtime_send_cleanup(active->peer,"{}",active->session.scope().link_id,2000)==ESP_OK);clock_ms+=2000;assert(expired_release(*active,active->queue.front()));assert(runtime_send_control(active->peer,Kind::heartbeat,"{}",1000)==ESP_ERR_INVALID_STATE&&active->cancelled);
 auto replacement=connection(3,9);links={replacement};assert(replacement->queue.empty());assert(runtime_send_control(active->peer,static_cast<Kind>(4),"{}",1000)==ESP_ERR_NOT_SUPPORTED);
 reset();active=connection();links={active};assert(runtime_send_control(active->peer,Kind::heartbeat,"{}",100)==ESP_OK);clock_ms+=100;assert(runtime_send_control(active->peer,Kind::heartbeat,"{}",100)==ESP_OK&&active->queue.size()==1&&active->queue.front().created==clock_ms);
 // Stable state-change priority preserves quick taps and A-up/B-down order.
 std::ifstream input(root+"/amity-octopus-consortium/examples/operation-press-key.json");const std::string text((std::istreambuf_iterator<char>(input)),{});Envelope example;assert(parse_envelope(text,example)==Error::none);Operation down;assert(parse_operation(example.payload,down)==Error::none);down.repeat=true;
 reset();active=connection();active->peer=down.target.port.octopus_id;links={active};assert(runtime_send_control(active->peer,Kind::heartbeat,"{}",1000)==ESP_OK);assert(runtime_send_operation(active->peer,down,active->session.scope().link_id,500)==ESP_OK);assert(runtime_send_cleanup(active->peer,"upA",active->session.scope().link_id,2000)==ESP_OK);
 assert(active->queue.size()==3&&active->queue[0].kind==Kind::operation&&active->queue[1].payload=="upA"&&active->queue[2].kind==Kind::heartbeat);
 auto second=down;second.command_id=ident<model::CommandId>(902);++second.command_sequence;second.hold_token=ident<model::HoldToken>(903);assert(runtime_send_operation(active->peer,second,active->session.scope().link_id,500)==ESP_OK);assert(runtime_send_cleanup(active->peer,"upB",active->session.scope().link_id,2000)==ESP_OK);
 assert(active->queue.size()==5&&active->queue[0].kind==Kind::operation&&active->queue[1].payload=="upA"&&active->queue[2].kind==Kind::operation&&active->queue[3].payload=="upB"&&active->queue[4].kind==Kind::heartbeat);
 assert(runtime_send_operation(active->peer,down,ident<model::LinkId>(999),500)==ESP_ERR_INVALID_STATE&&!active->cancelled);
 const auto previous_link=active->session.scope().link_id;auto new_link=connection(3,9);new_link->peer=active->peer;links={new_link};assert(runtime_send_operation(active->peer,down,previous_link,500)==ESP_ERR_INVALID_STATE&&!new_link->cancelled&&new_link->queue.empty());links={active};
 const auto queued=active->queue.size();assert(runtime_send_cleanup(active->peer,"oldLinkUp",ident<model::LinkId>(999),2000)==ESP_ERR_INVALID_STATE&&!active->cancelled&&active->queue.size()==queued);
 reset();active=connection();active->peer=down.target.port.octopus_id;links={active};assert(runtime_send_operation(active->peer,down,active->session.scope().link_id,500)==ESP_OK);auto in_flight=std::move(active->queue.front());active->queue.pop_front();assert(runtime_send_cleanup(active->peer,"upA",active->session.scope().link_id,2000)==ESP_OK&&in_flight.kind==Kind::operation&&active->queue.front().payload=="upA");
 std::cout<<"PASS actual cleanup transport: reserved priority, full/expired release forces exact-link loss, no replay/reconnect queue migration\n";
}
void receiver_cleanup_tests(){
 reset();auto active=connection();auto scope=active->session.scope();scope.coordinator_id=scope.peer_id;assert(active->session.authenticate(scope,scope.peer_id,PeerRole::coordinator,true)==Error::none);HelloFrame hello;hello.consortium_id=scope.consortium_id;hello.octopus_id=scope.peer_id;hello.boot_id=scope.peer_boot_id;hello.coordinator_epoch=1;hello.config_revision=1;hello.required_capabilities=known_capabilities;assert(active->session.hello(hello,known_capabilities)==Error::none&&active->session.synchronized(1,true,true)==Error::none);
 Envelope up;up.consortium_id=scope.consortium_id;up.origin_octopus_id=scope.peer_id;up.destination_octopus_id=scope.local_id;up.origin_boot_id=scope.peer_boot_id;up.destination_boot_id=scope.local_boot_id;up.coordinator_epoch=up.config_revision=1;up.kind=Kind::hold_release;up.ttl_ms=2000;
 unsigned next=900,calls=0;Error outcome=Error::none;controls.control=[&](auto,const auto&,auto,auto){++calls;return outcome;};
 for(const auto result:{Error::none,Error::duplicate,Error::stale_route,Error::stale_binding}){outcome=result;up.message_id=ident<model::MessageId>(next++);assert(dispatch_cleanup(*active,up)&&!active->cancelled);}
 const auto before=calls;assert(dispatch_cleanup(*active,up)&&calls==before); // Duplicate old up cannot cancel successor B.
 for(const auto result:{Error::queue_full,Error::lease_expired}){outcome=result;up.message_id=ident<model::MessageId>(next++);assert(!dispatch_cleanup(*active,up));}
 controls.control={};up.message_id=ident<model::MessageId>(next++);assert(dispatch_cleanup(*active,up));
 std::cout<<"PASS actual receive cleanup dispatch: stale/duplicate up harmless to newer hold; matched owner overload/timeout closes exact link\n";
}
void codec_tests(const std::string& root){
 std::ifstream file(root+"/amity-octopus-consortium/examples/operation-press-key.json");const std::string text((std::istreambuf_iterator<char>(file)),{});Envelope e;assert(parse_envelope(text,e)==Error::none);Operation op;assert(parse_operation(e.payload,op)==Error::none&&!op.repeat);e.payload.get("arguments")->get("repeat")->boolean=true;assert(parse_operation(e.payload,op)==Error::none&&op.repeat);
 reset();auto active=connection();active->peer=op.target.port.octopus_id;links={active};assert(runtime_send_operation(active->peer,op,active->session.scope().link_id,400)==ESP_OK&&active->queue.size()==1&&active->queue.front().ttl==400);assert(active->queue.front().payload.find("hold_lease_ms")==std::string::npos);Json payload;JsonError detail;assert(parse_json(active->queue.front().payload,payload,detail)&&parse_operation(payload,op)==Error::none&&op.repeat);
 Json old=payload;old.get("arguments")->object.emplace_back("hold_lease_ms",Json{});assert(parse_operation(old,op)==Error::invalid_message);
 auto scope=active->session.scope();scope.local_id=e.destination_octopus_id;scope.peer_id=e.origin_octopus_id;scope.coordinator_id=e.origin_octopus_id;scope.consortium_id=e.consortium_id;scope.local_boot_id=e.destination_boot_id;scope.peer_boot_id=e.origin_boot_id;scope.epoch=e.coordinator_epoch;scope.config_revision=e.config_revision;
 for(bool modern:{false,true}){Session session;assert(session.authenticate(scope,scope.peer_id,PeerRole::coordinator,true)==Error::none);HelloFrame hello;hello.consortium_id=scope.consortium_id;hello.octopus_id=scope.peer_id;hello.boot_id=scope.peer_boot_id;hello.coordinator_epoch=scope.epoch;hello.config_revision=scope.config_revision;hello.required_capabilities=modern?known_capabilities:known_capabilities&~owner_repeated_keys;
 assert(session.hello(hello,known_capabilities)==Error::none&&session.synchronized(scope.config_revision,true,true)==Error::none);assert(execution_profile_supported(hello,true,true,1,1)==modern);parse_operation(e.payload,op);session.execution_leases().issue(op.execution_lease_id,LeaseClass::realtime_input,500,100);
 assert(session.admit_operation(e,op,100,100,false)==(modern?Error::none:Error::unsupported_kind));if(modern)assert(session.admit_operation(e,op,100,600,false)!=Error::none);}
 std::cout<<"PASS actual state-change codec/capability gate: repeat flag, no per-key lease, retired renewal tag, old execution quarantined, original initial deadline\n";
}
}
int main(int argc,char** argv){assert(argc==2);if(std::string(argv[1])=="--vectors"){std::string line;while(std::getline(std::cin,line)){amity::consortium::Envelope value;std::cout<<(amity::consortium::parse_envelope(line,value)==amity::consortium::Error::none?'1':'0')<<'\n';}return 0;}amity::consortium::closure_tests();amity::consortium::queue_tests(argv[1]);amity::consortium::receiver_cleanup_tests();amity::consortium::codec_tests(argv[1]);}
'''
def schema_vectors(binary):
 try:
  import jsonschema
  from referencing import Registry,Resource
 except ImportError:
  print('Optional schema comparison unavailable; actual codecs/link/queue tests passed.')
  return
 production=json.loads((ROOT/'components/consortium/protocol-v1.schema.json').read_text())
 design=json.loads((ROOT/'amity-octopus-consortium/schemas/wire-envelope.schema.json').read_text())
 resources=[production]+[json.loads(path.read_text()) for path in (ROOT/'amity-octopus-consortium/schemas').glob('*.json')]
 registry=Registry().with_resources((value['$id'],Resource.from_contents(value)) for value in resources)
 jsonschema.Draft202012Validator.check_schema(production)
 base=json.loads((ROOT/'amity-octopus-consortium/examples/operation-press-key.json').read_text());vectors=[]
 for repeat,key in itertools.product([False,True],[0,0x41,0x42,0x43,0x65,0x66,0x76,255,-1,256,True,'65']):
  value=copy.deepcopy(base);value['payload']['arguments'].update(repeat=repeat,key_code=key);vectors.append(value)
 for field in ['key_code','hold_token','repeat']:
  for changed in [None,{},[],1,'',False]:
   value=copy.deepcopy(base);value['payload']['arguments'][field]=changed;vectors.append(value)
  value=copy.deepcopy(base);del value['payload']['arguments'][field];vectors.append(value)
 for legacy in [0,1,750,2000,2001]:
  value=copy.deepcopy(base);value['payload']['arguments']['hold_lease_ms']=legacy;vectors.append(value)
 for missing in ['command_sequence','command_id','plan_id','session_id','binding_id','execution_lease_id']:
  value=copy.deepcopy(base);del value['payload'][missing];vectors.append(value)
 for sequence in ['0','01',1,'18446744073709551616']:
  value=copy.deepcopy(base);value['payload']['command_sequence']=sequence;vectors.append(value)
 value=copy.deepcopy(base);value['kind']='hold_renew';vectors.append(value)
 validator=jsonschema.Draft202012Validator({'allOf':[{'$ref':design['$id']},{'properties':{'payload':{'$ref':production['$id']+'#/$defs/Operation'}}}]},registry=registry)
 expected=[validator.is_valid(value) for value in vectors]
 output=subprocess.run([str(binary),'--vectors'],input=''.join(json.dumps(value,separators=(',',':'))+'\n' for value in vectors),capture_output=True,text=True,check=True)
 actual=[line=='1' for line in output.stdout.splitlines()];assert len(actual)==len(vectors)
 for n,(left,right) in enumerate(zip(actual,expected)):assert left==right,(n,left,right,vectors[n])
 report={'vectors':len(vectors),'positive':sum(actual),'negative':len(actual)-sum(actual),'discrepancies':0,'actual_cpp_envelope_parser':True}
 (ROOT/'test-results/state-change-schema-vectors.json').write_text(json.dumps(report,indent=2)+'\n')
 print('PASS state-change codec/design/production schemas:',report)

def main():
 source=(ROOT/'components/consortium/runtime.cpp').read_text()
 actual=''.join(function(source,name) for name in ['bool notify_link_closed(','bool fence_replaced_links(','void reap_closed_links(','static bool queued_key_state(','static void enqueue_cleanup(','static esp_err_t queue_control(','esp_err_t runtime_send_control(','esp_err_t runtime_send_cleanup(','esp_err_t runtime_send_operation('])
 branch=next(line.strip() for line in source.splitlines() if line.strip().startswith('if(have&&queued.kind==Kind::hold_release'))
 cleanup_branch=next(line.strip() for line in source.splitlines() if line.strip().startswith('else if(envelope.kind!=Kind::heartbeat)'))
 dispatch='bool dispatch_cleanup(Active& storage,Envelope envelope){Active* active=&storage;const auto received=now_ms();while(true){if(false){}'+cleanup_branch.replace('continue;','return true;')+'return true;}return false;}\n'
 expire='bool expired_release(Active& storage,Queued queued){Active* active=&storage;(void)active;const bool have=true;const auto now=now_ms();while(true){'+branch+'return false;}return true;}\n'
 with tempfile.TemporaryDirectory(prefix='amity-owner-repeat-') as temp:
  temp=Path(temp);(temp/'esp_err.h').write_text('#pragma once\nusing esp_err_t=int;constexpr int ESP_OK=0,ESP_ERR_INVALID_ARG=1,ESP_ERR_INVALID_STATE=2,ESP_ERR_NO_MEM=3,ESP_ERR_TIMEOUT=4,ESP_FAIL=5,ESP_ERR_NOT_FOUND=6,ESP_ERR_NOT_SUPPORTED=7;\n')
  cpp=temp/'test.cpp';binary=temp/'test';cpp.write_text(PREFIX+'namespace amity::consortium {\n'+actual+expire+dispatch+'\n}\n'+SUITE)
  sources=[ROOT/'components/consortium'/name for name in ['json.cpp','wire.cpp','session.cpp','control.cpp','settings.cpp','update.cpp','relay.cpp','transfer.cpp']]+[ROOT/'components/amity_system/model.cpp']
  subprocess.run(shlex.split(os.environ.get('CXX','c++'))+['-std=c++17','-Wall','-Wextra','-Werror','-pthread','-g','-fsanitize=address,undefined','-I'+str(temp)]+['-I'+str(ROOT/path) for path in ['components/consortium','components/consortium/include','components/amity_system/include']]+list(map(str,[cpp]+sources))+['-o',str(binary)],check=True)
  subprocess.run([str(binary),str(ROOT)],check=True)
  schema_vectors(binary)
if __name__=='__main__':main()
