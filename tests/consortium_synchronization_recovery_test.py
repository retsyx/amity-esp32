#!/usr/bin/env python3
"""Exercise actual synchronization recovery on an existing authenticated stream."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

from consortium_gatt_callback_test import function
from consortium_live_configuration_test import PREFIX, SUPPORT
from setup_timing_support import timing_support

ROOT = Path(__file__).resolve().parents[1]

EXTRA = r'''
#include <chrono>
#include <sys/wait.h>
#include <unistd.h>
namespace amity::consortium {
bool credentials_current=true;
bool current_peer_credential(const Active& active,const Group&){return credentials_current&&active.tls->authenticated==active.peer;}
unsigned remembered=0,hostname_failures=0;
Error remember_hostname(model::OctopusId,const std::string&,std::unique_lock<std::mutex>&){return ++remembered<=hostname_failures?Error::storage_failure:Error::none;}
unsigned pdMS_TO_TICKS(unsigned ms){return ms;}
void vTaskDelay(unsigned ms){clock_ms+=ms;assert(clock_ms<1000000);}
}
'''

SUITE = r'''
namespace amity::consortium {
std::shared_ptr<Active> initial(bool server){
 auto active=start(server);credentials_current=true;remembered=hostname_failures=0;const auto scope=active->session.scope();
 HelloFrame hello;hello.consortium_id=scope.consortium_id;hello.octopus_id=scope.peer_id;hello.boot_id=scope.peer_boot_id;
 hello.config_revision=scope.config_revision;
 assert(active->session.authenticate(scope,scope.peer_id,server?PeerRole::actuator:PeerRole::coordinator,true)==Error::none);
 assert(active->session.hello(hello)==Error::none);
 active->ready=active->status.ready=false;active->context_ready=false;
 controls.synchronize=[](auto,uint64_t){return Error::none;};
 return active;
}
Envelope sync_frame(const LinkScope& scope,model::MessageId round,std::string_view phase,uint64_t target,uint64_t envelope_revision=0){
 return inbound(scope,envelope_revision?envelope_revision:scope.config_revision,Kind::synchronization,synchronization_json(round,phase,target));
}
void same_connection(const Active& active,const LinkScope& original,const FakeTls* tls,uint64_t revision){
 auto expected=original;expected.config_revision=revision;if(active.synchronization.id)expected.link_id=*model::LinkId::parse(active.synchronization.id.str());
 assert(active.tls.get()==tls&&active.tls->usable()&&active.session.scope()==expected&&shutdowns==0&&!active.cancelled);
}
// Plays the member for the main: hostname, then the barrier reply.
struct ServerScript {
 LinkScope original;unsigned rounds=0;std::vector<model::MessageId> ids;
 bool stale_only_second=false,reject_first=false;
 explicit ServerScript(LinkScope scope):original(scope){}
 void operator()(Active& active,const Envelope& envelope){
  if(envelope.kind==Kind::device_status){feed(active,inbound(original,envelope.config_revision,Kind::device_status,"{\"hostname\":\"amity-member\"}"));return;}
  assert(envelope.kind==Kind::synchronization);
  model::MessageId round;std::string phase;uint64_t target;
  assert(parse_synchronization(envelope.payload,round,phase,target)==Error::none&&phase=="begin");
  assert(std::find(ids.begin(),ids.end(),round)==ids.end());ids.push_back(round);++rounds;
  assert(!active.ready&&!active.status.ready);
  if(rounds>1)feed(active,sync_frame(original,ids.front(),"ack",target)); // An old round's reply cannot complete a new barrier.
  if(stale_only_second&&rounds==2)return;
  // A peer may already have advanced after an earlier incomplete exchange.
  // Only the typed round barrier may cross that revision boundary.
  feed(active,sync_frame(original,round,reject_first&&rounds==1?"reject":"ack",target,target));
 }
};
void coordinator_recovers_rejection_and_ignores_old_round(){
 auto active=initial(true);const auto original=active->session.scope();const auto* tls=active->tls.get();
 auto seen=inbound(original,7,Kind::device_status,"{\"hostname\":\"amity-member\"}");
 assert(active->session.admit(seen,clock_ms,clock_ms)==Error::none);
 controls.synchronize=[](auto,uint64_t){assert(false&&"The main binds no configuration");return Error::none;};
 ServerScript script{original};script.reject_first=true;script.stale_only_second=true;
 on_send=[&](auto& a,const auto& e){script(a,e);};
 assert(synchronize_connection(*active,true)&&active->ready&&active->status.ready&&script.rounds==3);
 same_connection(*active,original,tls,7);
 assert(!diagnostic_logs.empty()&&clock_ms>=12100); // Rejection, failed barrier and the one-second recovery delays.
}
// Plays the main for a member: barrier, then hostname.
struct MemberScript {
 LinkScope original;uint64_t target=7;unsigned rounds=1,acks=0;bool interrupt_hostname_first=false;
 model::MessageId round=ident<model::MessageId>(200);
 explicit MemberScript(LinkScope scope):original(scope){}
 void begin(Active& active){feed(active,sync_frame(original,round,"begin",target));}
 void next(Active& active){round=random_id<model::MessageId>();++rounds;begin(active);}
 void operator()(Active& active,const Envelope& envelope){
  if(envelope.kind==Kind::device_status){if(interrupt_hostname_first&&rounds==1){next(active);return;}feed(active,inbound(original,envelope.config_revision,Kind::device_status,"{\"hostname\":\"amity-main\"}"));return;}
  assert(envelope.kind==Kind::synchronization);
  model::MessageId id;std::string phase;uint64_t revision;
  assert(parse_synchronization(envelope.payload,id,phase,revision)==Error::none&&id==round&&revision==target);
  if(phase=="reject"){next(active);return;}
  assert(phase=="ack");++acks;assert(!active.ready&&!active.status.ready);
 }
};
void member_binds_before_acknowledging(){
 auto active=initial(false);const auto original=active->session.scope();const auto* tls=active->tls.get();
 MemberScript script{original};script.begin(*active);unsigned applies=0;
 controls.synchronize=[&](auto owner,uint64_t revision){assert(owner==original.peer_id&&revision==7);++applies;return applies==1?Error::not_ready:Error::none;};
 on_send=[&](auto& a,const auto& e){script(a,e);};
 // The failed bind is answered with a rejection, never an acknowledgement.
 assert(synchronize_connection(*active,false)&&script.rounds==2&&script.acks==1&&applies==2);
 same_connection(*active,original,tls,7);assert(active->ready&&!active->pending_synchronization);
}
void new_begin_unwinds_pending_round_without_reauthenticating(){
 auto active=initial(false);const auto original=active->session.scope();const auto* tls=active->tls.get();
 MemberScript script{original};script.interrupt_hostname_first=true;script.begin(*active);unsigned applies=0;
 controls.synchronize=[&](auto,uint64_t){++applies;return Error::none;};on_send=[&](auto& a,const auto& e){script(a,e);};
 assert(synchronize_connection(*active,false)&&script.rounds==2&&script.acks==2&&applies==2&&!active->pending_synchronization);
 same_connection(*active,original,tls,7);
 for(const auto& e:active->tls->sent)if(e.kind==Kind::synchronization){model::MessageId round;std::string phase;uint64_t revision;assert(parse_synchronization(e.payload,round,phase,revision)==Error::none&&phase=="ack");}
}
void hostname_persistence_failure_retains_same_stream(){
 for(bool server:{false,true}){
  auto active=initial(server);const auto original=active->session.scope();const auto* tls=active->tls.get();hostname_failures=1;
  ServerScript main{original};MemberScript member{original};
  if(server)on_send=[&](auto& a,const auto& e){main(a,e);};else{member.begin(*active);on_send=[&](auto& a,const auto& e){member(a,e);};}
  assert(synchronize_connection(*active,server)&&remembered==2&&(server?main.rounds==2:member.rounds==2));
  same_connection(*active,original,tls,7);assert(active->status.hostname==(server?"amity-member":"amity-main"));
 }
}
void recovery_revision_preserves_identity_and_fences_ordinary_revision(){
 for(bool server:{false,true}){
  auto active=start(server);credentials_current=true;const auto original=active->session.scope();const auto* tls=active->tls.get();configuration=8;
  controls.synchronize=[](auto,uint64_t revision){assert(revision==8);return Error::none;};
  ServerScript main{original};MemberScript member{original};member.target=8;
  if(server)on_send=[&](auto& a,const auto& e){main(a,e);};
  else{member.begin(*active);on_send=[&](auto& a,const auto& e){member(a,e);};}
  assert(synchronize_connection(*active,server,true,8));same_connection(*active,original,tls,8);
  assert(server?main.rounds==1:member.rounds==1&&member.acks==1);
  auto old=inbound(original,7,Kind::device_status,device_status);assert(active->session.admit(old,clock_ms,clock_ms)==Error::stale_config);
  auto future=inbound(original,9,Kind::device_status,device_status);assert(active->session.admit(future,clock_ms,clock_ms)==Error::stale_config);
 }
}
void broken_transport_stops_recovery(){
 auto active=initial(true);const auto* tls=active->tls.get();unsigned begins=0;
 on_send=[&](Active&,const Envelope& envelope){assert(envelope.kind==Kind::synchronization);++begins;};
 active->tls->before_receive=[&]{active->tls->healthy=false;};
 assert(!synchronize_connection(*active,true)&&begins==1&&!active->ready&&!active->status.ready&&active->tls.get()==tls&&shutdowns==0);
 assert(!active->tls->usable());
 auto revoked=initial(true);credentials_current=false;assert(!synchronize_connection(*revoked,true)&&revoked->tls->sent.empty());
}
void interleaved_observations_complete_recovery(){
 {
  auto active=start(true);credentials_current=true;const auto original=active->session.scope();const auto* tls=active->tls.get();
  controls.control=[](const auto&,const auto&,auto){assert(false&&"No remote CEC completion exists");return Error::none;};
  SynchronizationRound round{ident<model::MessageId>(600),7};
  feed(*active,inbound(original,7,Kind::scoped_result,observation(original)));feed(*active,sync_frame(original,round.id,"ack",7));
  LinkTrace trace;assert(synchronization_barrier(*active,true,round,trace));
  same_connection(*active,original,tls,7);assert(restart_fences.empty());
 }
 {
  auto active=start(true);credentials_current=true;const auto original=active->session.scope();const auto* tls=active->tls.get();bool injected=false;
  ServerScript script{original};on_send=[&](Active& a,const Envelope& envelope){
   if(!injected&&envelope.kind==Kind::synchronization){
    injected=true;assert(active->execution_fenced);feed(a,inbound(original,7,Kind::scoped_result,observation(original)));
   }
   script(a,envelope);
  };
  LinkTrace trace;const bool ready=synchronize_connection(*active,true,true,0,&trace);
  same_connection(*active,original,tls,7);assert(injected&&script.rounds==1&&restart_fences.size()==1);
  assert(ready&&active->ready&&!active->execution_fenced&&active->session.scope().link_id!=original.link_id);
 }
}
void logical_restart_clears_old_ipc_and_update_without_replaying_work(){
 auto active=start(true);credentials_current=true;const auto original=active->session.scope();const auto* tls=active->tls.get();
 auto old_scope_frame=inbound(original,7,Kind::device_status,device_status);assert(active->session.admit(old_scope_frame,clock_ms,clock_ms)==Error::none);
 model::OwnedIntent work;work.kind=model::OwnedIntentKind::activity;work.target={{original.consortium_id,original.peer_id,0,model::Side::back},"Player"};work.config_revision=7;work.generation=1;work.on=true;
 const auto admitted=std::make_shared<const model::OwnedIntent>(work);
 active->queue.push_back({Kind::scoped_intent,{},1000,clock_ms,7,admitted});
 const auto update_id=ident<model::MessageId>(500);active->update_begin_id=update_id;
 active->ready=active->status.ready=false;std::vector<std::string> cache={"TV","AVR","Player"};
 unsigned fences=0,disconnects=0,aborted_updates=0;bool fence_ready=false;
 controls.disconnected=[&](auto,auto,auto){++disconnects;cache.clear();return true;};
 controls.restart_session=[&](auto owner,auto boot,auto link){
  assert(owner==original.peer_id&&boot==original.peer_boot_id&&link==active->session.scope().link_id&&active->tls.get()==tls&&cache.size()==3);
  ++fences;return fence_ready;
 };
 updates.disconnected=[&](auto owner,auto request){++aborted_updates;assert(owner==original.peer_id&&request==update_id&&active->session.scope()==original&&active->queue.size()==1&&!active->update_begin_id);
  assert(lock.try_lock());lock.unlock();assert(active->mutex.try_lock());active->mutex.unlock();};
 SynchronizationRound round{ident<model::MessageId>(501),7};LinkTrace trace;
 assert(!restart_synchronization_scope(*active,round,trace)&&std::string(trace.reason)=="synchronization_fence");
 assert(active->session.scope()==original&&active->queue.size()==1&&active->queue.front().owned==admitted&&!active->update_begin_id&&aborted_updates==1&&cache.size()==3);
 fence_ready=true;assert(restart_synchronization_scope(*active,round,trace));
 same_connection(*active,original,tls,7);
 assert(active->queue.empty()&&!active->update_begin_id&&fences==2&&aborted_updates==1&&disconnects==0&&cache.size()==3&&active->tls->sent.empty());
 assert(!active->context_ready&&!active->context_requested&&active->refreshing);
 // A fresh upload can start after this logical scope was already fenced. A
 // repeated fence must still abort it exactly once before taking its shortcut.
 const auto resumed_update=ident<model::MessageId>(504);active->update_begin_id=resumed_update;
 updates.disconnected=[&](auto owner,auto request){++aborted_updates;assert(owner==original.peer_id&&request==resumed_update&&!active->update_begin_id);
  assert(lock.try_lock());lock.unlock();assert(active->mutex.try_lock());active->mutex.unlock();};
 active->cleaned_link=active->session.scope().link_id;
 assert(fence_synchronization_scope(*active,trace)&&aborted_updates==2&&!active->update_begin_id);
 assert(fence_synchronization_scope(*active,trace)&&aborted_updates==2);
 assert(active->session.synchronized(7,true,true)==Error::none);
 assert(active->queue.empty()&&active->tls->sent.empty()); // Fresh authentication never resurrects admitted work from the ended link.
 round.id=ident<model::MessageId>(503);assert(restart_synchronization_scope(*active,round,trace));
 assert(aborted_updates==2&&disconnects==0&&cache.size()==3&&active->queue.empty());
}
void late_old_link_callbacks_cannot_cancel_fenced_tls(){
 auto active=start(true);credentials_current=true;const auto original=active->session.scope();const auto* tls=active->tls.get();unsigned fences=0,barrier_callbacks=0;
 controls.restart_session=[&](auto owner,auto boot,auto old_link){
  ++fences;assert(owner==original.peer_id&&boot==original.peer_boot_id&&old_link==original.link_id&&active->execution_fenced&&!active->ready);
  runtime_fence_owned_link(owner,old_link);assert(!active->cancelled&&shutdowns==0);return true;
 };
 ServerScript script{original};on_send=[&](Active& a,const Envelope& envelope){
  if(envelope.kind==Kind::synchronization){++barrier_callbacks;assert(a.execution_fenced&&a.session.scope()==original);runtime_fence_owned_link(a.peer,original.link_id);assert(!a.cancelled&&shutdowns==0);}
  script(a,envelope);
 };
 assert(synchronize_connection(*active,true)&&fences==1&&barrier_callbacks==1&&!active->execution_fenced);
 same_connection(*active,original,tls,7);assert(active->session.scope().link_id!=original.link_id);
 runtime_fence_owned_link(active->peer,original.link_id);assert(!active->cancelled&&shutdowns==0&&active->tls.get()==tls);
}
void expired_operational_frame_uses_existing_connection(){
 for(bool server:{false,true}){
  auto active=start(server);credentials_current=true;const auto original=active->session.scope();const auto* tls=active->tls.get();
  controls.synchronize=[](auto,uint64_t){return Error::none;};
  ServerScript main{original};MemberScript member{original};active->synchronization={member.round,7};unsigned rejects=0;
  on_send=[&](Active& a,const Envelope& envelope){
   if(envelope.kind==Kind::synchronization&&envelope.payload.get("phase")->string=="reject"){++rejects;assert(!server&&envelope.payload.get("round_id")->string==member.round.str());}
   if(server)main(a,envelope);else member(a,envelope);
  };
  LinkTrace trace;trace.fail("frame_receive_expired");assert(recover_operational_frame(*active,server,trace));
  same_connection(*active,original,tls,7);assert(active->ready&&!active->execution_fenced&&active->session.scope().link_id!=original.link_id&&restart_fences.size()==1);
  assert(rejects==(server?0u:1u));
 }
 for(bool revoked:{false,true}){
  auto active=start(true);credentials_current=!revoked;active->tls->healthy=revoked;LinkTrace trace;trace.fail("frame_receive_expired");
  assert(!recover_operational_frame(*active,true,trace)&&active->tls->sent.empty()&&restart_fences.empty());
 }
 credentials_current=true;
}
void application_restart_waits_for_exact_cleanup_before_fresh_scope(){
 auto active=start(true);credentials_current=true;const auto original=active->session.scope();const auto* tls=active->tls.get();
 auto old_scope_frame=inbound(original,7,Kind::device_status,device_status);assert(active->session.admit(old_scope_frame,clock_ms,clock_ms)==Error::none);
 model::OwnedIntent work;work.kind=model::OwnedIntentKind::activity;work.target={{original.consortium_id,original.peer_id,0,model::Side::back},"Player"};work.config_revision=7;work.generation=1;work.on=true;
 auto pending=std::make_shared<const model::OwnedIntent>(work);std::weak_ptr<const model::OwnedIntent> abandoned=pending;
 active->queue.push_back({Kind::scoped_intent,{},1000,clock_ms,7,pending});pending.reset();
 unsigned fences=0,disconnects=0;std::vector<std::string> observations={"TV","AVR","Player"};
 controls.disconnected=[&](auto,auto,auto){++disconnects;observations.clear();return true;};
 controls.restart_session=[&](auto owner,auto boot,auto link){
  ++fences;assert(owner==original.peer_id&&boot==original.peer_boot_id&&link==original.link_id);
  assert(active->execution_fenced&&!active->ready&&active->session.scope()==original&&active->tls.get()==tls&&active->tls->sent.empty());
  assert(active->queue.size()==1&&!abandoned.expired()&&observations.size()==3&&shutdowns==0);
  return fences>=3;
 };
 ServerScript script{original};on_send=[&](auto& a,const auto& envelope){assert(fences==3&&a.queue.empty()&&abandoned.expired());script(a,envelope);};
 LinkTrace trace;trace.fail("control_dispatch",Error::not_ready,LinkTrace::Recovery::recoverable);assert(application_failure_recoverable(*active,trace));
 assert(recover_operational_frame(*active,true,trace)&&fences==3&&script.rounds==1&&clock_ms>=2100);
 same_connection(*active,original,tls,7);assert(active->ready&&!active->execution_fenced&&active->queue.empty()&&abandoned.expired()&&disconnects==0&&observations.size()==3);
 assert(active->session.scope().link_id!=original.link_id);
 assert(std::none_of(active->tls->sent.begin(),active->tls->sent.end(),[](const auto& e){return e.kind==Kind::scoped_intent;}));
}
void logical_recovery_keeps_security_and_transport_fences(){
 using Recovery=LinkTrace::Recovery;
 auto active=start(true);credentials_current=true;LinkTrace trace;
 trace.fail("queued_send",Error::none,Recovery::recoverable);assert(application_failure_recoverable(*active,trace));
 trace=LinkTrace{};trace.fail("control_dispatch",Error::not_ready,Recovery::recoverable);assert(application_failure_recoverable(*active,trace));
 for(auto error:{Error::invalid_message,Error::stale_boot,Error::identity_conflict,Error::unauthorized_role,Error::wrong_consortium}){
  trace=LinkTrace{};trace.fail("control_dispatch",error,Recovery::recoverable);assert(!application_failure_recoverable(*active,trace));
 }
 // An unclassified failure recovers only when a restart was already requested;
 // a terminal one never does. The reason text plays no part.
 trace=LinkTrace{};trace.fail("queued_send");assert(!application_failure_recoverable(*active,trace));
 active->restart_requested=true;assert(application_failure_recoverable(*active,trace));
 trace=LinkTrace{};trace.fail("queued_send",Error::none,Recovery::terminal);assert(!application_failure_recoverable(*active,trace));
 trace=LinkTrace{};trace.fail("cleanup_queue_full",Error::none,Recovery::recoverable);active->tls->healthy=false;assert(!application_failure_recoverable(*active,trace));
 active->tls->healthy=true;credentials_current=false;assert(!application_failure_recoverable(*active,trace));credentials_current=true;
 active->retiring=true;assert(!application_failure_recoverable(*active,trace));active->retiring=false;
 group.role=Group::prepared;assert(!application_failure_recoverable(*active,trace));
}
struct PairedResult {bool ready=false,scope_intact=false,context_withheld=false;unsigned applies=0,rounds=0;model::LinkId link;};
PairedResult paired_endpoint(bool server,int fd){
 auto active=initial(server);active->tls->socket=fd;const auto original=active->session.scope();const auto* tls=active->tls.get();
 auto seen=inbound(original,7,Kind::device_status,"{\"hostname\":\"amity-peer\"}");assert(active->session.admit(seen,clock_ms,clock_ms)==Error::none);
 const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(8);
 auto read_exact=[&](char* data,size_t length){
  size_t offset=0;
  while(offset<length){
   if(std::chrono::steady_clock::now()>=deadline)return false;
   fd_set readers;FD_ZERO(&readers);FD_SET(fd,&readers);timeval wait{0,100000};
   const auto available=::select(fd+1,&readers,nullptr,nullptr,&wait);if(available<0)return false;if(!available)continue;
   const auto count=::read(fd,data+offset,length-offset);if(count<=0)return false;offset+=static_cast<size_t>(count);
  }
  return true;
 };
 active->tls->before_receive=[&]{
  if(!active->tls->incoming.empty())return;
  uint8_t header[4];if(!read_exact(reinterpret_cast<char*>(header),sizeof(header))){active->tls->healthy=false;return;}
  const uint32_t length=(uint32_t(header[0])<<24)|(uint32_t(header[1])<<16)|(uint32_t(header[2])<<8)|header[3];
  assert(length>0&&length<=max_frame_bytes);std::string body(length,'\0');
  if(!read_exact(body.data(),body.size())){active->tls->healthy=false;return;}
  active->tls->incoming.push_back(std::move(body));
 };
 unsigned applies=0,rounds=0;
 controls.synchronize=[&](auto peer,uint64_t revision){
  assert(!server&&peer==original.peer_id&&revision==7);
  ++applies;return applies==1?Error::not_ready:Error::none; // The real member fails its first bind.
 };
 on_send=[&](Active& current,const Envelope& envelope){
  assert(&current==active.get()&&current.tls.get()==tls&&current.tls->socket==fd&&!current.ready&&!current.status.ready);
  if(envelope.kind==Kind::synchronization){model::MessageId round;std::string phase;uint64_t revision;assert(parse_synchronization(envelope.payload,round,phase,revision)==Error::none);if(phase==(server?"begin":"ack"))++rounds;}
  else assert(!current.context_ready);
  const auto bytes=frame(packet(envelope));size_t offset=0;
  while(offset<bytes.size()){
   const auto count=::write(fd,bytes.data()+offset,bytes.size()-offset);
   if(count<=0){current.tls->healthy=false;return;}offset+=static_cast<size_t>(count);
  }
 };
 const bool ready=synchronize_connection(*active,server);
 PairedResult result;result.ready=ready&&active->ready&&active->status.ready&&active->session.state()==ConnectionState::ready;
 auto expected=original;expected.link_id=*model::LinkId::parse(active->synchronization.id.str());result.link=expected.link_id;
 result.scope_intact=active->session.scope()==expected&&expected.link_id!=original.link_id&&active->tls.get()==tls&&active->tls->socket==fd&&active->tls->usable()&&shutdowns==0;
 result.context_withheld=!active->context_ready;
 result.applies=applies;result.rounds=rounds;
 return result;
}
void paired_endpoints_recover_crossed_application_failures(){
 int sockets[2],reports[2];assert(::socketpair(AF_UNIX,SOCK_STREAM,0,sockets)==0&&::pipe(reports)==0);
 const auto child=::fork();assert(child>=0);
 if(child==0){
  ::close(sockets[0]);::close(reports[0]);const auto result=paired_endpoint(false,sockets[1]);
  assert(::write(reports[1],&result,sizeof(result))==sizeof(result));::close(sockets[1]);::close(reports[1]);::_exit(0);
 }
 ::close(sockets[1]);::close(reports[1]);const auto main=paired_endpoint(true,sockets[0]);::close(sockets[0]);
 PairedResult member;const auto read=::read(reports[0],&member,sizeof(member));::close(reports[0]);int status=0;assert(::waitpid(child,&status,0)==child);
 assert(read==sizeof(member)&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
 assert(main.link&&main.link==member.link);
 for(const auto& endpoint:{main,member})assert(endpoint.ready&&endpoint.scope_intact&&endpoint.context_withheld);
 assert(main.applies==0&&main.rounds>=2&&member.applies>=2&&member.rounds>=1);
}
}
int main(){using namespace amity::consortium;
 coordinator_recovers_rejection_and_ignores_old_round();
 member_binds_before_acknowledging();new_begin_unwinds_pending_round_without_reauthenticating();
 recovery_revision_preserves_identity_and_fences_ordinary_revision();broken_transport_stops_recovery();
 interleaved_observations_complete_recovery();logical_restart_clears_old_ipc_and_update_without_replaying_work();
 hostname_persistence_failure_retains_same_stream();late_old_link_callbacks_cannot_cancel_fenced_tls();expired_operational_frame_uses_existing_connection();
 application_restart_waits_for_exact_cleanup_before_fresh_scope();logical_recovery_keeps_security_and_transport_fences();
 paired_endpoints_recover_crossed_application_failures();
 std::cout<<"PASS actual synchronization recovery: scripted and paired fork/socket endpoints recover bind failure/rejection/timeout/new-begin on same TLS/boots with fresh logical links and fresh context withheld; members bind before acknowledging; old-round ACK cannot satisfy new barrier; revised barrier preserves ordinary exact-revision fences; fatal transport/revoked authority stop recovery\n";
}
'''


def main():
    source = (ROOT / 'components/consortium/runtime.cpp').read_text()
    trace = function(source, 'struct LinkTrace {').rstrip() + ';\n'
    round_type = function(source, 'struct SynchronizationRound {').rstrip() + ';\n'
    prefix = PREFIX.replace('struct Active{', trace + 'struct Active{')
    prefix = prefix.replace('struct SynchronizationRound {model::MessageId id;uint64_t revision=0;};', round_type)
    prefix = prefix.replace('++receives;if(before_receive)before_receive();',
                            'if(++receives>=200){for(const auto& log:diagnostic_logs)std::cerr<<log<<"\\n";assert(false);}if(before_receive)before_receive();if(!healthy)return false;')
    prefix = prefix.replace('bool current_peer_credential(const Active&,const Group&){return true;}\n', '')
    prefix = prefix.replace('Error remember_hostname(model::OctopusId,const std::string&,std::unique_lock<std::mutex>&){return Error::none;}\n', '')
    prefix = prefix.replace('unsigned pdMS_TO_TICKS(unsigned ms){return ms/10;}\nvoid vTaskDelay(unsigned ticks){clock_ms+=ticks*10;}\n', '')
    actual = ''.join(function(source, name) for name in (
        'Envelope outgoing(', 'bool bind_configuration(', 'bool apply_configuration_advance(', 'bool advance_configuration(', 'bool finish_synchronization(', 'bool exchange_hostname(Active& active,const std::string& expected,LinkTrace* diagnostic=nullptr) {',
        'bool synchronization_current(', 'bool synchronization_recoverable(',
        'bool synchronization_barrier(', 'bool fence_synchronization_scope(', 'bool restart_synchronization_scope(', 'bool synchronize_connection(', 'bool recover_operational_frame(', 'bool application_failure_recoverable(', 'void runtime_fence_owned_link('))
    actual = function(source, 'static bool pure_key_release(') + function(source, 'void request_session_restart(') + source[source.index('struct LinkTraceSnapshot {'):source.index('std::optional<uint8_t> dropped_report(')] + actual
    with tempfile.TemporaryDirectory(prefix='amity-synchronization-recovery-') as directory:
        temp = Path(directory)
        (temp / 'esp_err.h').write_text('#pragma once\nusing esp_err_t=int;constexpr int ESP_OK=0,ESP_ERR_INVALID_ARG=1,ESP_ERR_INVALID_STATE=2,ESP_ERR_NO_MEM=3,ESP_ERR_TIMEOUT=4,ESP_FAIL=5,ESP_ERR_NOT_FOUND=6,ESP_ERR_NOT_SUPPORTED=7;\n')
        cpp, binary = temp / 'test.cpp', temp / 'test'
        cpp.write_text(timing_support() + prefix + EXTRA + 'namespace amity::consortium {\n' + actual + '\n}\n' + SUPPORT + SUITE)
        sources = [ROOT / 'components/consortium' / name for name in
                   ('json.cpp', 'wire.cpp', 'owned_intent.cpp', 'session.cpp', 'settings.cpp', 'update.cpp')]
        sources.append(ROOT / 'components/amity_system/model.cpp')
        subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) +
                       ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-pthread', '-g', '-fsanitize=address,undefined',
                        '-I' + str(temp), '-I' + str(ROOT / 'components/consortium'),
                        '-I' + str(ROOT / 'components/consortium/include'), '-I' + str(ROOT / 'components/amity_system/include'),
                        '-I' + str(ROOT / 'components/network_diagnostics/include')] +
                       list(map(str, [cpp] + sources)) + ['-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
