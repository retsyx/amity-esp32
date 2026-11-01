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
Error remember_hostname(model::OctopusId,const std::string&){return ++remembered<=hostname_failures?Error::storage_failure:Error::none;}
unsigned pdMS_TO_TICKS(unsigned ms){return ms;}
void vTaskDelay(unsigned ms){clock_ms+=ms;assert(clock_ms<1000000);}
}
'''

SUITE = r'''
namespace amity::consortium {
std::shared_ptr<Active> initial(bool server){
 auto active=start(server);credentials_current=true;remembered=hostname_failures=0;const auto scope=active->session.scope();
 HelloFrame hello;hello.consortium_id=scope.consortium_id;hello.octopus_id=scope.peer_id;hello.boot_id=scope.peer_boot_id;
 hello.coordinator_epoch=scope.epoch;hello.config_revision=scope.config_revision;hello.required_capabilities=known_capabilities;
 assert(active->session.authenticate(scope,scope.peer_id,server?PeerRole::actuator:PeerRole::coordinator,true)==Error::none);
 assert(active->session.hello(hello,known_capabilities)==Error::none);
 active->ready=active->status.ready=false;active->context_ready=false;
 controls.snapshot=[](auto){return owner_state;};
 controls.synchronize=[](auto,const Json&){return Error::none;};
 return active;
}
Envelope sync_frame(const LinkScope& scope,model::MessageId round,std::string_view phase,uint64_t target,uint64_t envelope_revision=0){
 return inbound(scope,envelope_revision?envelope_revision:scope.config_revision,Kind::synchronization,synchronization_json(round,phase,target));
}
void same_connection(const Active& active,const LinkScope& original,const FakeTls* tls,uint64_t revision){
 auto expected=original;expected.config_revision=revision;if(active.synchronization.id)expected.link_id=*model::LinkId::parse(active.synchronization.id.str());
 assert(active.tls.get()==tls&&active.tls->usable()&&active.session.scope()==expected&&shutdowns==0&&!active.cancelled);
}
struct ServerScript {
 LinkScope original;unsigned rounds=0;std::vector<model::MessageId> ids;std::optional<SnapshotManifest> config;
 bool stale_only_second=false,reject_first_transfer=false;
 explicit ServerScript(LinkScope scope):original(scope){}
 void operator()(Active& active,const Envelope& envelope){
  assert(envelope.origin_boot_id==original.local_boot_id&&envelope.destination_boot_id==original.peer_boot_id);
  if(envelope.kind==Kind::device_status){feed(active,inbound(original,envelope.config_revision,Kind::device_status,"{\"hostname\":\"amity-member\"}"));return;}
  if(envelope.kind==Kind::synchronization){
   model::MessageId round;std::string phase;uint64_t target;
   assert(parse_synchronization(envelope.payload,round,phase,target)==Error::none&&phase=="begin");
   assert(std::find(ids.begin(),ids.end(),round)==ids.end());ids.push_back(round);++rounds;
   assert(!active.ready&&!active.status.ready);
   if(rounds>1)feed(active,sync_frame(original,ids.front(),"ack",target));
   if(stale_only_second&&rounds==2)return;
   // A peer may already have advanced after an earlier incomplete exchange.
   // Only the typed round barrier may cross that revision boundary.
   feed(active,sync_frame(original,round,"ack",target,target));return;
  }
  SnapshotManifest incoming;std::string phase;uint32_t next;
  if(envelope.kind==Kind::snapshot_request){
   assert(parse_snapshot_manifest(envelope.payload,incoming,phase,next)==Error::none);
   if(incoming.document_kind=="configuration"){
    assert(phase=="begin");config=incoming;
    if(reject_first_transfer&&rounds==1){feed(active,sync_frame(original,ids.back(),"reject",incoming.config_revision,incoming.config_revision));return;}
    feed(active,inbound(original,envelope.config_revision,Kind::snapshot_request,snapshot_manifest_json(incoming,"ack",0)));
   }
   return;
  }
  assert(envelope.kind==Kind::snapshot_chunk&&config);
  feed(active,inbound(original,envelope.config_revision,Kind::snapshot_request,snapshot_manifest_json(*config,"complete",1)));
  const auto state=manifest(original.peer_id,original.peer_boot_id,config->config_revision,"owner_state",owner_state);
  feed(active,inbound(original,config->config_revision,Kind::snapshot_request,snapshot_manifest_json(state,"begin",0)));
  feed(active,inbound(original,config->config_revision,Kind::snapshot_chunk,snapshot_chunk_json(state,0,owner_state)));
 }
};
void coordinator_recovers_capture_failure_and_ignores_old_round(){
 auto active=initial(true);const auto original=active->session.scope();const auto* tls=active->tls.get();
 auto seen=inbound(original,7,Kind::device_status,"{\"hostname\":\"amity-member\"}");
 assert(active->session.admit(seen,clock_ms,clock_ms)==Error::none);
 unsigned captures=0,applies=0;
 controls.configuration_snapshot=[&](auto owner,uint64_t revision){assert(owner==original.peer_id&&revision==7);++captures;return captures==1?std::string{}:controller_slice(original,revision);};
 controls.synchronize=[&](auto owner,const Json& document){assert(owner==original.peer_id&&document.string==owner_state);++applies;return Error::none;};
 ServerScript script{original};script.stale_only_second=true;
 on_send=[&](auto& a,const auto& e){script(a,e);};
 assert(synchronize_connection(*active,true)&&active->ready&&active->status.ready&&script.rounds==3&&captures==2&&applies==1);
 same_connection(*active,original,tls,7);
 assert(active->session.admit(seen,clock_ms,clock_ms)==Error::duplicate); // No reauthentication/replay reset.
 assert(!diagnostic_logs.empty()&&clock_ms>=12100); // Failed barrier and existing one-second recovery delays.
}
void coordinator_recovers_owner_apply_and_peer_rejection(){
 for(bool rejected:{false,true}){
  auto active=initial(true);const auto original=active->session.scope();const auto* tls=active->tls.get();
  unsigned applies=0;controls.configuration_snapshot=[&](auto,uint64_t revision){return controller_slice(original,revision);};
  controls.synchronize=[&](auto,const Json&){++applies;return !rejected&&applies==1?Error::not_ready:Error::none;};
  ServerScript script{original};script.reject_first_transfer=rejected;on_send=[&](auto& a,const auto& e){script(a,e);};
  assert(synchronize_connection(*active,true)&&script.rounds==2&&applies==(rejected?1u:2u));
  same_connection(*active,original,tls,7);assert(active->ready&&active->session.state()==ConnectionState::ready);
 }
}
struct MemberScript {
 LinkScope original;uint64_t target=7;unsigned rounds=1,acks=0,config_applies=0;bool interrupt_first=false,interrupt_hostname_first=false;
 model::MessageId round=ident<model::MessageId>(200);std::optional<SnapshotManifest> owner;
 explicit MemberScript(LinkScope scope):original(scope){}
 void begin(Active& active){feed(active,sync_frame(original,round,"begin",target));}
 void next(Active& active){round=random_id<model::MessageId>();++rounds;begin(active);}
 void configuration(Active& active){
  const auto data=controller_slice(original,target);const auto config=manifest(original.local_id,original.local_boot_id,target,"configuration",data);
  feed(active,inbound(original,target,Kind::snapshot_request,snapshot_manifest_json(config,"begin",0)));
  if(interrupt_first&&rounds==1)next(active); // New begin interrupts the old transfer before any chunk is applied.
  else feed(active,inbound(original,target,Kind::snapshot_chunk,snapshot_chunk_json(config,0,data)));
 }
 void operator()(Active& active,const Envelope& envelope){
  if(envelope.kind==Kind::device_status){if(interrupt_hostname_first&&rounds==1){next(active);return;}feed(active,inbound(original,envelope.config_revision,Kind::device_status,"{\"hostname\":\"amity-main\"}"));configuration(active);return;}
  if(envelope.kind==Kind::synchronization){
   model::MessageId id;std::string phase;uint64_t revision;
   assert(parse_synchronization(envelope.payload,id,phase,revision)==Error::none&&id==round&&revision==target);
   if(phase=="reject"){next(active);return;}
   assert(phase=="ack");++acks;assert(!active.ready&&!active.status.ready);
   return;
  }
  SnapshotManifest incoming;std::string phase;uint32_t next;
  if(envelope.kind==Kind::snapshot_request){
   assert(parse_snapshot_manifest(envelope.payload,incoming,phase,next)==Error::none);
   if(incoming.document_kind=="owner_state"){
    assert(phase=="begin");owner=incoming;
    feed(active,inbound(original,target,Kind::snapshot_request,snapshot_manifest_json(incoming,"ack",0)));
   }
   return;
  }
  assert(envelope.kind==Kind::snapshot_chunk&&owner);
  feed(active,inbound(original,target,Kind::snapshot_request,snapshot_manifest_json(*owner,"complete",1)));
 }
};
void member_recovers_configuration_apply_and_owner_capture(){
 for(bool capture_failed:{false,true}){
  auto active=initial(false);const auto original=active->session.scope();const auto* tls=active->tls.get();
  MemberScript script{original};script.begin(*active);unsigned applies=0,captures=0;
  controls.synchronize=[&](auto owner,const Json& document){assert(owner==original.peer_id&&document.string==controller_slice(original,7));++applies;return !capture_failed&&applies==1?Error::not_ready:Error::none;};
  controls.snapshot=[&](auto){++captures;return capture_failed&&captures==1?std::string{}:owner_state;};
  on_send=[&](auto& a,const auto& e){script(a,e);};
  assert(synchronize_connection(*active,false)&&script.rounds==2&&script.acks==2&&applies==2&&captures==(capture_failed?2u:1u));
  same_connection(*active,original,tls,7);assert(active->ready&&!active->pending_synchronization);
 }
}
void new_begin_unwinds_pending_transfer_without_reauthenticating(){
 for(bool hostname_phase:{false,true}){
  auto active=initial(false);const auto original=active->session.scope();const auto* tls=active->tls.get();
  MemberScript script{original};script.interrupt_first=!hostname_phase;script.interrupt_hostname_first=hostname_phase;script.begin(*active);unsigned applies=0;
  controls.synchronize=[&](auto,const Json&){++applies;return Error::none;};on_send=[&](auto& a,const auto& e){script(a,e);};
  assert(synchronize_connection(*active,false)&&script.rounds==2&&script.acks==2&&applies==1&&!active->pending_synchronization);
  same_connection(*active,original,tls,7);
  for(const auto& e:active->tls->sent)if(e.kind==Kind::synchronization){model::MessageId round;std::string phase;uint64_t revision;assert(parse_synchronization(e.payload,round,phase,revision)==Error::none&&phase=="ack");}
 }
}
void hostname_persistence_failure_retains_same_stream(){
 for(bool server:{false,true}){
  auto active=initial(server);const auto original=active->session.scope();const auto* tls=active->tls.get();hostname_failures=1;
  controls.configuration_snapshot=[&](auto,uint64_t revision){return controller_slice(original,revision);};
  ServerScript main{original};MemberScript member{original};
  if(server)on_send=[&](auto& a,const auto& e){main(a,e);};else{member.begin(*active);on_send=[&](auto& a,const auto& e){member(a,e);};}
  assert(synchronize_connection(*active,server)&&remembered==2&&(server?main.rounds==2:member.rounds==2));
  same_connection(*active,original,tls,7);assert(active->status.hostname==(server?"amity-member":"amity-main"));
 }
}
void recovery_revision_preserves_identity_and_fences_ordinary_revision(){
 for(bool server:{false,true}){
  auto active=start(server);credentials_current=true;const auto original=active->session.scope();const auto* tls=active->tls.get();configuration=8;
  controls.snapshot=[](auto){return owner_state;};controls.configuration_snapshot=[&](auto,uint64_t revision){assert(revision==8);return controller_slice(original,revision);};controls.synchronize=[](auto,const Json&){return Error::none;};
  ServerScript main{original};MemberScript member{original};member.target=8;
  if(server)on_send=[&](auto& a,const auto& e){main(a,e);};
  else{member.begin(*active);on_send=[&](auto& a,const auto& e){member(a,e);};}
  assert(synchronize_connection(*active,server,true,8));same_connection(*active,original,tls,8);
  assert(server?main.rounds==1:member.rounds==1&&member.acks==1);
  auto old=inbound(original,7,Kind::heartbeat,heartbeat);assert(active->session.admit(old,clock_ms,clock_ms)==Error::stale_config);
  auto future=inbound(original,9,Kind::heartbeat,heartbeat);assert(active->session.admit(future,clock_ms,clock_ms)==Error::stale_config);
 }
}
void broken_transport_and_wrong_boot_stop_recovery(){
 for(bool wrong_boot:{false,true}){
  auto active=initial(true);const auto original=active->session.scope();const auto* tls=active->tls.get();unsigned begins=0;
  on_send=[&](Active& a,const Envelope& envelope){
   assert(envelope.kind==Kind::synchronization);++begins;
   if(wrong_boot){model::MessageId round;std::string phase;uint64_t revision;assert(parse_synchronization(envelope.payload,round,phase,revision)==Error::none);auto ack=sync_frame(original,round,"ack",revision);ack.origin_boot_id=ident<model::BootId>(999);feed(a,ack);}
  };
  if(!wrong_boot)active->tls->before_receive=[&]{active->tls->healthy=false;};
  assert(!synchronize_connection(*active,true)&&begins==1&&!active->ready&&!active->status.ready&&active->tls.get()==tls&&shutdowns==0);
  if(wrong_boot)assert(active->tls->usable());else assert(!active->tls->usable());
 }
 auto revoked=initial(true);credentials_current=false;assert(!synchronize_connection(*revoked,true)&&revoked->tls->sent.empty());
}
void prebarrier_lookup_completion_and_fenced_result_discard(){
 // Before an execution fence, preserve a legitimate admitted old lookup once;
 // a missing or failed delivery hook cannot be treated as replaceable status.
 for(unsigned outcome=0;outcome<4;++outcome){
  auto active=start(true);credentials_current=true;const auto original=active->session.scope();const auto* tls=active->tls.get();unsigned delivered=0;
  if(outcome!=1)controls.control=[&](auto peer,const Envelope& envelope,uint64_t deadline,model::LinkId link){
   assert(peer==original.peer_id&&envelope.kind==Kind::scoped_result&&envelope.payload.get("input_result")&&envelope.config_revision==7&&envelope.origin_boot_id==original.peer_boot_id&&link==original.link_id&&deadline==0&&!active->execution_fenced);
   ++delivered;return outcome==2?Error::not_ready:Error::none;
  };
  SynchronizationRound round{ident<model::MessageId>(600),7};
  auto result=inbound(original,7,Kind::scoped_result,input_result);if(outcome==3)result.origin_boot_id=ident<model::BootId>(999);
  feed(*active,result);feed(*active,result);feed(*active,sync_frame(original,round.id,"ack",7));
  LinkTrace trace;const bool passed=synchronization_barrier(*active,true,round,trace);
  same_connection(*active,original,tls,7);assert(restart_fences.empty());
  if(outcome==0)assert(passed&&delivered==1);
  else{assert(!passed&&delivered==(outcome==2?1u:0u));
   if(outcome==1)assert(std::string(trace.reason)=="input_result_hook_missing");
   if(outcome==2)assert(std::string(trace.reason)=="input_result_delivery");
   if(outcome==3)assert(trace.error==Error::stale_boot);
  }
 }
 // A recovery wrapper ends that old execution scope before its barrier. Old
 // results then validate/discard both before ACK and after fresh-link rotation.
 for(bool barrier:{false,true})for(unsigned outcome=0;outcome<4;++outcome){
  auto active=start(true);credentials_current=true;const auto original=active->session.scope();const auto* tls=active->tls.get();unsigned delivered=0;bool injected=false;
  controls.snapshot=[](auto){return owner_state;};controls.synchronize=[](auto,const Json&){return Error::none;};
  controls.configuration_snapshot=[&](auto,uint64_t revision){return controller_slice(original,revision);};
  if(outcome!=1)controls.control=[&](auto,const auto&,auto,auto){++delivered;return outcome==2?Error::not_ready:Error::none;};
  ServerScript script{original};on_send=[&](Active& a,const Envelope& envelope){
   if(!injected&&((barrier&&envelope.kind==Kind::synchronization)||(!barrier&&envelope.kind==Kind::snapshot_request))){
    injected=true;assert(active->execution_fenced);auto result=inbound(original,7,Kind::scoped_result,input_result);if(outcome==3)result.origin_boot_id=ident<model::BootId>(999);
    feed(a,result);feed(a,result);
   }
   script(a,envelope);
  };
  LinkTrace trace;const bool ready=synchronize_connection(*active,true,true,0,&trace);
  same_connection(*active,original,tls,7);assert(injected&&script.rounds==1&&restart_fences.size()==1&&delivered==0);
  if(outcome!=3)assert(ready&&active->ready&&!active->execution_fenced&&active->session.scope().link_id!=original.link_id);
  else assert(!ready&&!active->ready&&!active->status.ready&&trace.error==Error::stale_boot);
 }
}
void logical_restart_clears_old_ipc_and_update_without_replaying_work(){
 auto active=start(true);credentials_current=true;const auto original=active->session.scope();const auto* tls=active->tls.get();
 auto remembered_frame=inbound(original,7,Kind::heartbeat,heartbeat);assert(active->session.admit(remembered_frame,clock_ms,clock_ms)==Error::none);
 model::OwnedIntent work;work.kind=model::OwnedIntentKind::power;work.target={{original.consortium_id,original.peer_id,0,model::Side::back},"Player"};work.config_revision=7;work.generation=work.sequence=1;work.on=true;
 const auto admitted=std::make_shared<const model::OwnedIntent>(work);
 active->queue.push_back({Kind::scoped_intent,{},1000,clock_ms,7,admitted});
 const auto old_lease=active->own_control;const auto update_id=ident<model::MessageId>(500);active->update_begin_id=update_id;
 active->ready=active->status.ready=false;std::vector<std::string> cache={"TV","AVR","Player"};
 unsigned fences=0,disconnects=0,aborted_updates=0;bool fence_ready=false;
 controls.disconnected=[&](auto,auto,auto){++disconnects;cache.clear();return true;};
 controls.restart_session=[&](auto owner,auto boot,auto link){
  assert(owner==original.peer_id&&boot==original.peer_boot_id&&link==active->session.scope().link_id&&active->tls.get()==tls&&cache.size()==3);
  ++fences;return fence_ready;
 };
 updates.disconnected=[&](auto owner,auto request){++aborted_updates;assert(owner==original.peer_id&&request==update_id&&active->session.scope()==original&&active->queue.size()==1);};
 SynchronizationRound round{ident<model::MessageId>(501),7};LinkTrace trace;
 assert(!restart_synchronization_scope(*active,round,trace)&&std::string(trace.reason)=="synchronization_fence");
 assert(active->session.scope()==original&&active->queue.size()==1&&active->queue.front().owned==admitted&&active->update_begin_id==update_id&&aborted_updates==0&&cache.size()==3);
 fence_ready=true;assert(restart_synchronization_scope(*active,round,trace));
 same_connection(*active,original,tls,7);
 assert(active->queue.empty()&&!active->update_begin_id&&fences==2&&aborted_updates==1&&disconnects==0&&cache.size()==3&&active->tls->sent.empty());
 assert(!active->context_ready&&!active->context_requested&&active->refreshing&&!active->own_control&&!active->own_realtime&&!active->status.control_lease&&!active->status.realtime_lease&&active->own_control_deadline==0&&active->own_realtime_deadline==0);
 assert(active->session.admit(remembered_frame,clock_ms,clock_ms)==Error::duplicate&&active->session.execution_leases().live(LeaseClass::control,clock_ms)==0);
 assert(active->session.synchronized(7,true,true)==Error::none);
 const auto fresh=ident<model::ExecutionLeaseId>(502);assert(active->session.execution_leases().issue(fresh,LeaseClass::control,5000,clock_ms)==Error::none);
 assert(!active->session.execution_leases().valid(old_lease,LeaseClass::control,active->session.scope(),clock_ms)
     &&!active->session.execution_leases().valid(fresh,LeaseClass::control,original,clock_ms)
     &&active->session.execution_leases().valid(fresh,LeaseClass::control,active->session.scope(),clock_ms));
 assert(active->queue.empty()&&active->tls->sent.empty()); // Fresh leases never resurrect admitted work from the ended link.
 round.id=ident<model::MessageId>(503);assert(restart_synchronization_scope(*active,round,trace));
 assert(aborted_updates==1&&disconnects==0&&cache.size()==3&&active->queue.empty());
}
void late_old_link_callbacks_cannot_cancel_fenced_tls(){
 auto active=start(true);credentials_current=true;const auto original=active->session.scope();const auto* tls=active->tls.get();unsigned fences=0,barrier_callbacks=0;
 controls.snapshot=[](auto){return owner_state;};controls.synchronize=[](auto,const Json&){return Error::none;};
 controls.configuration_snapshot=[&](auto,uint64_t revision){return controller_slice(original,revision);};
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
  controls.snapshot=[](auto){return owner_state;};controls.synchronize=[](auto,const Json&){return Error::none;};controls.configuration_snapshot=[&](auto,uint64_t revision){return controller_slice(original,revision);};
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
 const auto old_lease=active->own_control;
 auto remembered_frame=inbound(original,7,Kind::heartbeat,heartbeat);assert(active->session.admit(remembered_frame,clock_ms,clock_ms)==Error::none);
 model::OwnedIntent work;work.kind=model::OwnedIntentKind::power;work.target={{original.consortium_id,original.peer_id,0,model::Side::back},"Player"};work.config_revision=7;work.generation=work.sequence=1;work.on=true;
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
 controls.snapshot=[](auto){return owner_state;};controls.synchronize=[](auto,const Json&){return Error::none;};
 controls.configuration_snapshot=[&](auto,uint64_t revision){return controller_slice(original,revision);};
 ServerScript script{original};on_send=[&](auto& a,const auto& envelope){assert(fences==3&&a.queue.empty()&&abandoned.expired());script(a,envelope);};
 LinkTrace trace;trace.fail("control_dispatch",Error::not_ready);assert(application_failure_recoverable(*active,trace));
 assert(recover_operational_frame(*active,true,trace)&&fences==3&&script.rounds==1&&clock_ms>=2100);
 same_connection(*active,original,tls,7);assert(active->ready&&!active->execution_fenced&&active->queue.empty()&&abandoned.expired()&&disconnects==0&&observations.size()==3);
 assert(active->session.scope().link_id!=original.link_id&&active->session.admit(remembered_frame,clock_ms,clock_ms)==Error::duplicate);
 assert(!active->session.execution_leases().valid(old_lease,LeaseClass::control,active->session.scope(),clock_ms));
 assert(std::none_of(active->tls->sent.begin(),active->tls->sent.end(),[](const auto& e){return e.kind==Kind::scoped_intent;}));
}
void logical_recovery_keeps_security_and_transport_fences(){
 auto active=start(true);credentials_current=true;LinkTrace trace;
 for(const auto* reason:{"causal_expired","cleanup_queue_full","causal_result_fence","context_recapture_failed","queued_send"}){
  trace=LinkTrace{};trace.fail(reason);assert(application_failure_recoverable(*active,trace));
 }
 trace=LinkTrace{};trace.fail("control_dispatch",Error::not_ready);assert(application_failure_recoverable(*active,trace));
 for(auto error:{Error::invalid_message,Error::stale_boot,Error::identity_conflict,Error::unauthorized_role,Error::wrong_consortium}){
  trace=LinkTrace{};trace.fail("control_dispatch",error);assert(!application_failure_recoverable(*active,trace));
 }
 active->restart_requested=true;
 for(const auto* reason:{"control_admit","envelope_parse","membership_revoked","retirement","execution_lease_scope"}){
  trace=LinkTrace{};trace.fail(reason);assert(!application_failure_recoverable(*active,trace));
 }
 trace=LinkTrace{};trace.fail("causal_expired");active->tls->healthy=false;assert(!application_failure_recoverable(*active,trace));
 active->tls->healthy=true;credentials_current=false;assert(!application_failure_recoverable(*active,trace));credentials_current=true;
 active->retiring=true;assert(!application_failure_recoverable(*active,trace));active->retiring=false;
 active->lifecycle_only=true;assert(!application_failure_recoverable(*active,trace));active->lifecycle_only=false;
 group.role=Group::prepared;assert(!application_failure_recoverable(*active,trace));
}
struct PairedResult {bool ready=false,scope_intact=false,context_withheld=false,replay_intact=false;unsigned applies=0,rounds=0;model::LinkId link;};
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
 controls.configuration_snapshot=[&](auto,uint64_t revision){return controller_slice(original,revision);};
 controls.synchronize=[&](auto peer,const Json& document){
  assert(peer==original.peer_id&&document.string==(server?owner_state:controller_slice(original,7)));
  ++applies;return applies==1?Error::not_ready:Error::none; // Both real peers fail their first distinct application step.
 };
 on_send=[&](Active& current,const Envelope& envelope){
  assert(&current==active.get()&&current.tls.get()==tls&&current.tls->socket==fd&&!current.ready&&!current.status.ready);
  if(envelope.kind==Kind::synchronization){model::MessageId round;std::string phase;uint64_t revision;assert(parse_synchronization(envelope.payload,round,phase,revision)==Error::none);if(phase==(server?"begin":"ack"))++rounds;}
  else assert(!current.context_ready&&!current.own_control&&!current.own_realtime&&!current.status.control_lease&&!current.status.realtime_lease);
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
 result.context_withheld=!active->context_ready&&!active->own_control&&!active->own_realtime&&!active->status.control_lease&&!active->status.realtime_lease;
 result.replay_intact=active->session.admit(seen,clock_ms,clock_ms)==Error::duplicate;result.applies=applies;result.rounds=rounds;
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
 for(const auto& endpoint:{main,member})assert(endpoint.ready&&endpoint.scope_intact&&endpoint.context_withheld&&endpoint.replay_intact&&endpoint.applies>=2&&endpoint.rounds>=3);
}
}
int main(){using namespace amity::consortium;
 coordinator_recovers_capture_failure_and_ignores_old_round();coordinator_recovers_owner_apply_and_peer_rejection();
 member_recovers_configuration_apply_and_owner_capture();new_begin_unwinds_pending_transfer_without_reauthenticating();
 recovery_revision_preserves_identity_and_fences_ordinary_revision();broken_transport_and_wrong_boot_stop_recovery();
 prebarrier_lookup_completion_and_fenced_result_discard();logical_restart_clears_old_ipc_and_update_without_replaying_work();
 hostname_persistence_failure_retains_same_stream();late_old_link_callbacks_cannot_cancel_fenced_tls();expired_operational_frame_uses_existing_connection();
 application_restart_waits_for_exact_cleanup_before_fresh_scope();logical_recovery_keeps_security_and_transport_fences();
 paired_endpoints_recover_crossed_application_failures();
 std::cout<<"PASS actual synchronization recovery: scripted and paired fork/socket endpoints recover capture/apply/rejection/timeout/new-begin on same TLS/boots with fresh logical links, replay intact and fresh context/leases withheld; old-round ACK cannot satisfy new barrier; revised barrier preserves ordinary exact-revision fences; fatal transport/wrong boot/revoked authority stop recovery\n";
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
    prefix = prefix.replace('Error remember_hostname(model::OctopusId,const std::string&){return Error::none;}\n', '')
    prefix = prefix.replace('unsigned pdMS_TO_TICKS(unsigned ms){return ms/10;}\nvoid vTaskDelay(unsigned ticks){clock_ms+=ticks*10;}\n', '')
    actual = ''.join(function(source, name) for name in (
        'Envelope outgoing(', 'bool same_manifest(', 'bool deliver_synchronization_input(', 'bool synchronize(', 'bool exchange_hostname(Active& active,const std::string& expected,LinkTrace* diagnostic=nullptr) {',
        'bool synchronization_current(', 'bool synchronization_recoverable(',
        'bool synchronization_barrier(', 'bool fence_synchronization_scope(', 'bool restart_synchronization_scope(', 'bool synchronize_connection(', 'bool recover_operational_frame(', 'bool application_failure_recoverable(', 'void runtime_fence_owned_link('))
    actual = function(source, 'void request_session_restart(') + source[source.index('struct LinkTraceSnapshot {'):source.index('std::optional<RuntimeSnapshotRetry> snapshot_retry(')] + actual
    with tempfile.TemporaryDirectory(prefix='amity-synchronization-recovery-') as directory:
        temp = Path(directory)
        (temp / 'esp_err.h').write_text('#pragma once\nusing esp_err_t=int;constexpr int ESP_OK=0,ESP_ERR_INVALID_ARG=1,ESP_ERR_INVALID_STATE=2,ESP_ERR_NO_MEM=3,ESP_ERR_TIMEOUT=4,ESP_FAIL=5,ESP_ERR_NOT_FOUND=6,ESP_ERR_NOT_SUPPORTED=7;\n')
        cpp, binary = temp / 'test.cpp', temp / 'test'
        cpp.write_text(timing_support() + prefix + EXTRA + 'namespace amity::consortium {\n' + actual + '\n}\n' + SUPPORT + SUITE)
        sources = [ROOT / 'components/consortium' / name for name in
                   ('json.cpp', 'wire.cpp', 'owned_intent.cpp', 'session.cpp', 'settings.cpp', 'update.cpp', 'transfer.cpp')]
        sources.append(ROOT / 'components/amity_system/model.cpp')
        subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) +
                       ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-pthread', '-g', '-fsanitize=address,undefined',
                        '-I' + str(temp), '-I' + str(ROOT / 'components/consortium'),
                        '-I' + str(ROOT / 'components/consortium/include'), '-I' + str(ROOT / 'components/amity_system/include')] +
                       list(map(str, [cpp] + sources)) + ['-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
