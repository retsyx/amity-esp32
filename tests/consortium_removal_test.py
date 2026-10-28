#!/usr/bin/env python3
"""Run actual removal admission, dispatch, one-shot permit and result cache with real Session/codecs."""
from pathlib import Path
import os,shlex,subprocess,tempfile,json,copy,itertools
from consortium_gatt_callback_test import function
ROOT=Path(__file__).resolve().parents[1]
PREFIX=r'''
#include "amity/consortium/runtime.hpp"
#include "controller_policy.hpp"
#include "enrollment_abort.hpp"
#include <algorithm>
#include <cassert>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <sys/socket.h>
namespace amity::consortium {
using namespace platform;
uint64_t clock_ms=100;uint64_t now_ms(){return clock_ms;}
template<class T>T ident(unsigned n){char value[33];snprintf(value,sizeof(value),"%032x",n);return *T::parse(value);}
struct FakeTls{bool server=false;int socket=-1;model::OctopusId authenticated;std::vector<Peer> allowed;std::vector<std::string> frames;size_t next=0;bool receive(std::string& out,uint32_t timeout){assert(timeout<=2000);if(next==frames.size()){clock_ms+=timeout;return false;}out=frames[next++];return true;}};
struct Active{Session session;model::OctopusId peer;std::shared_ptr<FakeTls> tls=std::make_shared<FakeTls>();std::mutex mutex;std::deque<int> queue;bool cancelled=false,retiring=false,ready=true,management_ready=false;RuntimePeerStatus status;std::optional<EnrollmentAbort> enrollment_abort;model::MessageId removal_request;uint64_t removal_until=0;std::string retirement_payload;};
std::mutex lock;Group group;Identity identity;RuntimeStatus status;RuntimeLifecycleHooks lifecycle;model::BootId boot;uint64_t configuration=1;std::vector<std::shared_ptr<Active>> links;
unsigned group_saves=0,quiesces=0,markers=0,restarts=0,revocations=0;bool disk_ok=true,send_ok=true;unsigned failure=0;std::vector<std::string> events,sent;
void gatt_client_disconnect(){}
esp_err_t save_group(const Group&){++group_saves;return disk_ok?ESP_OK:ESP_FAIL;}
bool current_peer_credential(const Active& active,const Group& value){for(const auto& peer:value.peers)if(peer.id==active.peer&&peer.id==active.tls->authenticated)for(const auto& allowed:active.tls->allowed)if(peer.id==allowed.id&&peer.der==allowed.der&&peer.pin==allowed.pin)return true;return false;}
// The cryptographic primitive is injected, not the production scope/digest
// construction. Production SHA-256 has separate known-vector tests.
std::string sha256(std::string_view text){uint64_t h=1469598103934665603ull;for(unsigned char c:text){h^=c;h*=1099511628211ull;}char out[65];snprintf(out,sizeof(out),"%016llx%016llx%016llx%016llx",(unsigned long long)h,(unsigned long long)h,(unsigned long long)h,(unsigned long long)h);return out;}
std::string settings_quote(std::string_view text){return "\""+std::string(text)+"\"";}
Json parsed(std::string_view text){Json value;JsonError detail;assert(parse_json(text,value,detail));return value;}
bool send(Active&,Kind kind,std::string_view payload,uint32_t timeout){assert(timeout<=2000);assert(kind==Kind::membership_event||kind==Kind::membership_result);sent.emplace_back(payload);events.push_back(kind==Kind::membership_result?"response_attempt":"request_attempt");return send_ok;}
}
'''
SUITE=r'''
namespace amity::consortium {
void reset(bool main){
 clock_ms=100;configuration=1;group_saves=quiesces=markers=restarts=revocations=0;disk_ok=send_ok=true;failure=0;events.clear();sent.clear();links.clear();removal_operations.clear();removal_authorization={};identity={};group={};status={};boot=ident<model::BootId>(6);identity.id=ident<model::OctopusId>(4);group.role=main?Group::coordinator:Group::member;group.consortium=ident<model::ConsortiumId>(2);group.authority=main?identity.id:ident<model::OctopusId>(3);group.epoch=1;group.revision=1;status.authority_ready=main;status.coordinator=main;status.member=!main;group.peers.push_back({ident<model::OctopusId>(3),"leaf","pin",true});
 lifecycle.retire=[]{++quiesces;events.push_back("quiescent");if(failure==1)return ESP_FAIL;if(failure==2)clock_ms+=2000;if(failure==3)links.front()->cancelled=true;if(failure==4)++configuration;return ESP_OK;};
 lifecycle.revoked=[](model::OctopusId){++revocations;};
 lifecycle.factory_reset=[](model::OctopusId issuer){assert(quiesces&&runtime_removed_device_reset_authorized(issuer));events.push_back("marker");if(failure==5)return ESP_FAIL;++markers;return ESP_OK;};
 lifecycle.after_factory_reset=[]{assert(markers&&events.back()=="response_attempt");++restarts;events.push_back("restart");};
}
std::shared_ptr<Active> connection(bool receiver,bool management=false,uint32_t caps=known_capabilities){
 auto active=std::make_shared<Active>();active->peer=ident<model::OctopusId>(3);active->tls->server=!receiver;active->tls->authenticated=active->peer;active->tls->allowed=group.peers;
 LinkScope sc{ident<model::LinkId>(5),group.consortium,identity.id,active->peer,group.authority,boot,ident<model::BootId>(7),1,1};
 assert(active->session.authenticate(sc,sc.peer_id,receiver?PeerRole::coordinator:PeerRole::actuator,true)==Error::none);HelloFrame hello;hello.consortium_id=sc.consortium_id;hello.octopus_id=sc.peer_id;hello.boot_id=sc.peer_boot_id;hello.coordinator_epoch=1;hello.config_revision=1;hello.required_capabilities=caps;
 assert(active->session.hello(hello,known_capabilities)==Error::none&&active->session.synchronized(1,true,true)==Error::none);if(management)active->session.restrict_management();
 active->ready=!management;active->management_ready=management;active->status.id=active->peer;active->status.boot_id=sc.peer_boot_id;active->status.config_revision=1;active->status.negotiated_capabilities=caps;active->status.ready=!management;active->status.management_ready=management;links.push_back(active);return active;
}
Envelope message(const std::shared_ptr<Active>& active,unsigned id=20){const auto& sc=active->session.scope();Envelope e;e.consortium_id=sc.consortium_id;e.origin_octopus_id=sc.peer_id;e.destination_octopus_id=sc.local_id;e.origin_boot_id=sc.peer_boot_id;e.destination_boot_id=sc.local_boot_id;e.coordinator_epoch=1;e.config_revision=1;e.message_id=ident<model::MessageId>(id);e.kind=Kind::membership_event;e.ttl_ms=2000;auto mutation=ident<model::MutationId>(id);const auto digest=retirement_hash(sc,mutation,2);e.payload=parsed("{\"mutation_id\":\""+mutation.str()+"\",\"proposal_digest\":\""+digest+"\",\"phase\":\"factory_reset\",\"membership_revision\":\"2\",\"expected_boot_id\":\""+boot.str()+"\"}");return e;}
bool dispatch(const std::shared_ptr<Active>& active,const Envelope& e){const auto received=clock_ms;return active->session.admit(e,received,received)==Error::none&&retire_member(*active,e,received);}
void receiver_tests(){
 for(bool management:{false,true}){reset(false);auto active=connection(true,management);assert(dispatch(active,message(active))&&group.role==Group::retired&&markers==1&&restarts==1);assert(events==std::vector<std::string>({"quiescent","marker","response_attempt","restart"}));assert(parsed(sent.back()).get("result")->string=="reset_scheduled");assert(!runtime_removed_device_reset_authorized(active->peer));assert(!dispatch(active,message(active,21))&&markers==1);}
 for(unsigned fault=0;fault<14;++fault){reset(false);auto active=connection(true);auto e=message(active);switch(fault){case 0:e.origin_boot_id=ident<model::BootId>(99);break;case 1:e.destination_boot_id=ident<model::BootId>(99);break;case 2:e.origin_octopus_id=ident<model::OctopusId>(99);break;case 3:e.destination_octopus_id=ident<model::OctopusId>(99);break;case 4:e.consortium_id=ident<model::ConsortiumId>(99);break;case 5:++e.config_revision;break;case 6:++e.coordinator_epoch;break;case 7:e.payload.get("expected_boot_id")->string=ident<model::BootId>(99).str();break;case 8:e.payload.get("membership_revision")->string="1";break;case 9:e.payload.get("proposal_digest")->string=std::string(64,'a');break;case 10:e.payload.get("phase")->string="retire";break;case 11:active->tls->authenticated=ident<model::OctopusId>(99);break;case 12:e.ttl_ms=2001;break;case 13:configuration=2;break;}assert(!dispatch(active,e)&&!markers&&!restarts);}
 for(unsigned fault=1;fault<=5;++fault){reset(false);auto active=connection(true);failure=fault;assert(!dispatch(active,message(active))&&!markers&&!restarts&&group.role==Group::retired);}
 reset(false);auto active=connection(true);disk_ok=false;assert(!dispatch(active,message(active))&&group.role==Group::member&&!quiesces&&!markers);
 reset(false);active=connection(true);send_ok=false;assert(!dispatch(active,message(active))&&markers==1&&restarts==1); // LostACK still finishes explicitly authorized durable reset.
 reset(true);active=connection(false);assert(!dispatch(active,message(active))&&!group_saves&&!markers); // Main can never be reset by a member.
 reset(false);active=connection(true,false,known_capabilities&~factory_reset_removal);assert(!dispatch(active,message(active))&&!markers);
 reset(false);active=connection(true);auto a=active->session.scope(),b=a;std::swap(b.local_id,b.peer_id);std::swap(b.local_boot_id,b.peer_boot_id);assert(retirement_hash(a,ident<model::MutationId>(9),2)==retirement_hash(b,ident<model::MutationId>(9),2));b.peer_boot_id=ident<model::BootId>(99);assert(retirement_hash(a,ident<model::MutationId>(9),2)!=retirement_hash(b,ident<model::MutationId>(9),2));
 std::cout<<"PASS actual target removal: exact scope/boots/revision/digest/capability, main denial, ready+management, quiescence before durable marker, originaldeadline/revocation, ACK-loss restart, no replay\n";
}
void source_tests(){
 const auto request=ident<model::MessageId>(30),other=ident<model::MessageId>(31);const auto peer=ident<model::OctopusId>(3);
 for(bool management:{false,true}){reset(true);auto active=connection(false,management);assert(runtime_remove_octopus(peer,request,1,active->status.boot_id)==ESP_OK&&group.peers.empty()&&revocations==1&&active->retiring);assert(parsed(runtime_removal_operation(peer,request)).get("stage")->string=="admitted");assert(runtime_remove_octopus(peer,request,1,active->status.boot_id)==ESP_OK&&group_saves==1);assert(runtime_remove_octopus(identity.id,request,1,{})==ESP_ERR_INVALID_ARG);const auto original=active->retirement_payload;assert(parsed(original).get("expected_boot_id")->string==active->status.boot_id.str());clock_ms+=2000;assert(!finish_retirement(*active)&&sent.empty());assert(parsed(runtime_removal_operation(peer,request)).get("reset")->string=="unconfirmed");}
 reset(true);assert(runtime_remove_octopus(peer,request,1,{})==ESP_OK&&group.peers.empty());assert(parsed(runtime_removal_operation(peer,request)).get("reset")->string=="offline");assert(links.empty()&&sent.empty());
 reset(true);auto active=connection(false,false,known_capabilities&~factory_reset_removal);assert(runtime_remove_octopus(peer,request,1,{})==ESP_ERR_NOT_SUPPORTED&&group.peers.size()==1&&!group_saves);
 reset(true);active=connection(false);assert(runtime_remove_octopus(peer,request,2,{})==ESP_ERR_INVALID_STATE&&runtime_remove_octopus(peer,request,1,ident<model::BootId>(99))==ESP_ERR_INVALID_STATE&&!group_saves);
 reset(true);active=connection(false);disk_ok=false;assert(runtime_remove_octopus(peer,request,1,{})==ESP_FAIL&&group.peers.size()==1&&!active->retiring);
 reset(true);active=connection(false);active->cancelled=true;assert(runtime_remove_octopus(peer,request,1,{})==ESP_OK&&!active->retiring&&parsed(runtime_removal_operation(peer,request)).get("reset")->string=="offline");
 reset(false);assert(runtime_remove_octopus(peer,request,1,{})==ESP_ERR_INVALID_STATE&&!group_saves);
 reset(true);assert(runtime_remove_octopus(identity.id,request,1,{})==ESP_ERR_INVALID_STATE&&!group_saves);
 reset(true);group.join_id=ident<model::JoinId>(9);group.joining_id=peer;group.join_digest=std::string(64,'a');active=connection(false);assert(runtime_remove_octopus(peer,request,1,{})==ESP_OK&&active->enrollment_abort&&!active->retiring&&parsed(runtime_removal_operation(peer,request)).get("reset")->string=="unconfirmed");
 reset(true);for(unsigned n=0;n<24;++n){group.peers={{peer,"leaf","pin",true}};assert(runtime_remove_octopus(peer,ident<model::MessageId>(100+n),group.revision,{})==ESP_OK);assert(removal_operations.size()<=8);}assert(parsed(runtime_removal_operation(peer,other)).get("reset")->string=="unconfirmed");
 std::cout<<"PASS actual main removal: atomic revision+reviewedBoot CAS, durable localdrop, unsupportedpeer preflight, offline noqueue, duplicate noop, candidate abort distinct, bounded8terminal/evictedunknown\n";
}
void ack_tests(){
 reset(true);auto active=connection(false);const auto request=ident<model::MessageId>(80);assert(runtime_remove_octopus(active->peer,request,1,{})==ESP_OK);auto response=parsed(active->retirement_payload);response.object.emplace_back("result",parsed("\"reset_scheduled\""));const auto sc=active->session.scope();Envelope e;e.kind=Kind::membership_result;e.consortium_id=sc.consortium_id;e.origin_octopus_id=sc.peer_id;e.destination_octopus_id=sc.local_id;e.origin_boot_id=sc.peer_boot_id;e.destination_boot_id=sc.local_boot_id;e.coordinator_epoch=1;e.config_revision=1;e.message_id=ident<model::MessageId>(81);e.ttl_ms=2000;std::string payload=active->retirement_payload;payload.pop_back();payload+=",\"result\":\"reset_scheduled\"}";
 active->tls->frames.push_back(envelope_json(e,payload));assert(finish_retirement(*active));assert(parsed(runtime_removal_operation(active->peer,request)).get("reset")->string=="scheduled");assert(!markers&&!restarts);
 std::cout<<"PASS exact authenticated durable-marker ACK updates only its removal operation; admission never claims completed reset\n";
}
}
int main(int argc,char** argv){if(argc==2&&std::string(argv[1])=="--vectors"){using namespace amity::consortium;std::string line;while(std::getline(std::cin,line)){Envelope value;std::cout<<(parse_envelope(line,value)==Error::none?'1':'0')<<'\n';}return 0;}amity::consortium::receiver_tests();amity::consortium::source_tests();amity::consortium::ack_tests();}
'''
def schema_vectors(binary):
 try:
  import jsonschema
  from referencing import Registry,Resource
 except ImportError:
  print('Optional JSON Schema comparison unavailable; actual Session/runtime tests passed.')
  return
 production=json.loads((ROOT/'components/consortium/protocol-v1.schema.json').read_text())
 design=json.loads((ROOT/'amity-octopus-consortium/schemas/wire-envelope.schema.json').read_text())
 resources=[production]+[json.loads(path.read_text()) for path in (ROOT/'amity-octopus-consortium/schemas').glob('*.json')]
 registry=Registry().with_resources((value['$id'],Resource.from_contents(value)) for value in resources)
 jsonschema.Draft202012Validator.check_schema(production)
 payload={'mutation_id':'9'*32,'proposal_digest':'a'*64,'phase':'factory_reset','membership_revision':'2','expected_boot_id':'6'*32}
 base={'protocol_version':1,'consortium_id':'1'*32,'message_id':'2'*32,'origin_octopus_id':'3'*32,'origin_boot_id':'4'*32,'destination_octopus_id':'5'*32,'destination_boot_id':'6'*32,'coordinator_epoch':'1','config_revision':'1','kind':'membership_event','ttl_ms':2000,'payload':payload}
 vectors=[]
 for kind,result,ttl in itertools.product(['membership_event','membership_result'],[None,'reset_scheduled','rejected','unknown','retired',''],[0,1,2000,2001,10000]):
  value=copy.deepcopy(base);value.update(kind=kind,ttl_ms=ttl)
  if result is not None:value['payload']['result']=result
  vectors.append(value)
 for field in payload:
  for changed in [None,'',True,0,'0'*32,'01','18446744073709551616']:
   value=copy.deepcopy(base);value['payload'][field]=changed;vectors.append(value)
  value=copy.deepcopy(base);del value['payload'][field];vectors.append(value)
 for field,changed in [('phase','retire'),('phase','handover_commit'),('extra',True),('membership_revision','0'),('membership_revision','18446744073709551615')]:
  value=copy.deepcopy(base);value['payload'][field]=changed;vectors.append(value)
 schemas={kind:jsonschema.Draft202012Validator({'allOf':[{'$ref':design['$id']},{'properties':{'payload':{'$ref':production['$id']+'#/$defs/'+name}}}]},registry=registry) for kind,name in [('membership_event','MembershipEvent'),('membership_result','MembershipResult')]}
 expected=[schemas[value['kind']].is_valid(value) for value in vectors]
 output=subprocess.run([str(binary),'--vectors'],input=''.join(json.dumps(value,separators=(',',':'))+'\n' for value in vectors),capture_output=True,text=True,check=True)
 actual=[line=='1' for line in output.stdout.splitlines()];assert len(actual)==len(vectors)
 for n,(left,right) in enumerate(zip(actual,expected)):assert left==right,(n,left,right,vectors[n])
 report={'vectors':len(vectors),'positive':sum(actual),'negative':len(actual)-sum(actual),'discrepancies':0,'actual_cpp_envelope_parser':True}
 (ROOT/'test-results/removal-reset-schema-vectors.json').write_text(json.dumps(report,indent=2)+'\n')
 print('PASS actual removal codec/closed schemas:',report)

def main():
 source=(ROOT/'components/consortium/runtime.cpp').read_text();private=(ROOT/'components/consortium/removal_runtime.inc').read_text()
 actual=function((ROOT/'components/consortium/tls_link.cpp').read_text(),'std::string envelope_json(')+''.join(function(source,name) for name in ['bool lifecycle_receive(','std::string retirement_hash(','bool finish_retirement(','bool retire_member(','esp_err_t runtime_remove_octopus(','std::string runtime_removal_operation(','bool runtime_removed_device_reset_authorized('])
 with tempfile.TemporaryDirectory(prefix='amity-removal-') as temp:
  temp=Path(temp);(temp/'esp_err.h').write_text('#pragma once\nusing esp_err_t=int;constexpr int ESP_OK=0,ESP_ERR_INVALID_ARG=1,ESP_ERR_INVALID_STATE=2,ESP_ERR_NO_MEM=3,ESP_ERR_TIMEOUT=4,ESP_FAIL=5,ESP_ERR_NOT_FOUND=6,ESP_ERR_NOT_SUPPORTED=7;\n')
  cpp=temp/'test.cpp';binary=temp/'test';cpp.write_text(PREFIX+'namespace amity::consortium {\n'+private+actual+'\n}\n'+SUITE)
  sources=[ROOT/'components/consortium'/name for name in ['json.cpp','wire.cpp','session.cpp','owned_intent.cpp','settings.cpp','update.cpp','transfer.cpp']]+[ROOT/'components/amity_system/model.cpp']
  subprocess.run(shlex.split(os.environ.get('CXX','c++'))+['-std=c++17','-Wall','-Wextra','-Werror','-g','-fsanitize=address,undefined','-I'+str(temp)]+['-I'+str(ROOT/path) for path in ['components/consortium','components/consortium/include','components/amity_system/include']]+list(map(str,[cpp]+sources))+['-o',str(binary)],check=True)
  subprocess.run([str(binary)],check=True)
  schema_vectors(binary)
if __name__=='__main__':main()
