#!/usr/bin/env python3
"""Exercise the actual operational IPC pump and incremental receive callsite."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
from consortium_gatt_callback_test import function
from consortium_live_configuration_test import PREFIX, SUPPORT
ROOT=Path(__file__).resolve().parents[1]
EXTRA=r'''
namespace amity::consortium {
unsigned select_calls=0,recoveries=0;bool scripted_tls_pending=false;uint64_t recovered_at=0;
std::vector<std::string> recovery_reasons;
std::function<void(Active&,bool)> during_recovery;
bool restart_synchronization_scope(Active&,const SynchronizationRound&,LinkTrace&);
bool recover_operational_frame(Active& active,bool server,LinkTrace& trace){
 ++recoveries;recovered_at=clock_ms;recovery_reasons.emplace_back(trace.reason);assert(active.tls->usable());
 const auto* tls=active.tls.get();if(during_recovery)during_recovery(active,server);
 SynchronizationRound round{random_id<model::MessageId>(),active.session.scope().config_revision};
 if(!restart_synchronization_scope(active,round,trace))return false;
 assert(active.session.synchronized(round.revision,true,true)==Error::none&&active.tls.get()==tls);
 active.ready=active.status.ready=true;active.execution_fenced=false;active.restart_requested=false;scripted_tls_pending=true;return true;
}
void reset_worker(){select_calls=recoveries=0;scripted_tls_pending=false;recovered_at=0;recovery_reasons.clear();during_recovery={};}
int mbedtls_ssl_check_pending(const int*){return scripted_tls_pending?1:0;}
}
int scripted_select(int,fd_set*,fd_set*,fd_set*,timeval*){
 using namespace amity::consortium;clock_ms+=20;return ++select_calls==1?1:0;
}
#define select scripted_select
'''
SUITE=r'''
namespace amity::consortium {
model::OwnedIntent action(const Active& a){model::OwnedIntent i;i.kind=model::OwnedIntentKind::activity;i.target={{a.session.scope().consortium_id,a.peer,0,model::Side::back},"Player"};i.generation=1;i.config_revision=7;i.on=true;return i;}
void pending_receive_keeps_pumping(){
 auto a=start(true);a->tls->socket=3;status.authority_ready=true;reset_worker();on_send={};unsigned polls=0;uint64_t admitted=0,budget=0;
 auto admit=[&]{auto i=std::make_shared<const model::OwnedIntent>(action(*a));admitted=clock_ms;assert(queue_control(a->peer,Kind::scoped_intent,{},1000,a->session.scope().link_id,7,i,admitted)==ESP_OK);};
 a->tls->poller=[&](std::string& text){++polls;if(polls==1)admit();if(clock_ms<1300)return FakeTls::ReceiveResult::pending;text=device_status;return FakeTls::ReceiveResult::frame;};
 // Only the old operational whole-frame read invokes this blocking seam.
 a->tls->incoming.push_back(device_status);a->tls->before_receive=[&]{admit();clock_ms+=1180;};
 assert(operational_receive(*a,budget));
 assert(a->tls->sent.size()==1&&a->tls->sent[0].kind==Kind::scoped_intent&&clock_ms-admitted>=1180&&a->queue.empty());
 assert(!a->cancelled&&diagnostic_logs.empty()&&budget==0&&select_calls>1);
}
void partial_plaintext_deadline_recovers_on_idle_socket(){
 auto a=start(true);a->tls->socket=3;reset_worker();uint64_t budget=0;FrameReader reader;unsigned polls=0;uint64_t began=0;const auto* tls=a->tls.get();const auto old=a->session.scope();
 a->tls->poller=[&](std::string& text){
  if(recoveries){text=device_status;return FakeTls::ReceiveResult::frame;}
  ++polls;if(polls==1){began=clock_ms;uint8_t header[]={0,0,0,8};size_t used=0;assert(reader.feed(header,4,used,clock_ms)==FrameReader::Result::incomplete&&used==4);}
  return reader.check_deadline(clock_ms)==FrameReader::Result::timed_out?FakeTls::ReceiveResult::expired:FakeTls::ReceiveResult::pending;
 };
 during_recovery=[&](auto& current,bool server){assert(server&&current.tls.get()==tls&&current.session.scope()==old);};
 assert(operational_receive(*a,budget));
 assert(recovered_at-began==FrameReader::progress_timeout_ms&&polls>1&&recoveries==1&&budget==0&&recovery_reasons==std::vector<std::string>{"frame_receive_expired"});
 assert(diagnostic_logs.size()==1&&diagnostic_logs.front().find("Session restart requested reason=frame_receive_expired")!=std::string::npos);
 assert(a->tls.get()==tls&&a->tls->usable()&&a->session.scope().link_id!=old.link_id&&restart_fences.size()==1&&shutdowns==0);
}
void partial_encrypted_record_keeps_original_outer_bound(){
 auto a=start(true);a->tls->socket=3;reset_worker();uint64_t budget=0,began=0;unsigned polls=0;const auto* tls=a->tls.get();const auto old=a->session.scope();
 std::weak_ptr<const model::OwnedIntent> dropped;
 a->tls->poller=[&](std::string& text){if(recoveries){text=device_status;return FakeTls::ReceiveResult::frame;}if(++polls==1)began=clock_ms;return FakeTls::ReceiveResult::pending;};
 a->tls->before_expire=[&]{auto work=std::make_shared<const model::OwnedIntent>(action(*a));dropped=work;a->queue.push_back({Kind::scoped_intent,{},1000,clock_ms,7,work});};
 during_recovery=[&](auto& current,bool server){assert(server&&current.tls.get()==tls&&current.session.scope()==old&&current.queue.size()==1&&!dropped.expired());};
 assert(operational_receive(*a,budget));
 assert(recovered_at-began==5000&&polls>1&&a->tls->expirations==1&&recoveries==1&&budget==0);
 assert(a->tls.get()==tls&&a->queue.empty()&&a->tls->sent.empty()&&dropped.expired()&&shutdowns==0);
 assert(diagnostic_logs.size()==1&&diagnostic_logs.front().find("Session restart requested reason=frame_receive_expired")!=std::string::npos);
}
void bulk_prefix_allows_retransmission_and_keeps_fixed_outer_bound(){
 for(bool expire:{false,true}){
  auto a=start(true);a->tls->socket=3;reset_worker();uint64_t budget=0,began=0;unsigned polls=0;
  a->tls->poller=[&](std::string& text){
   if(recoveries){a->tls->bulk_receiving=false;text=device_status;return FakeTls::ReceiveResult::frame;}
   if(++polls==1){began=clock_ms;a->tls->bulk_receiving=true;}
   if(expire||clock_ms-began<7000)return FakeTls::ReceiveResult::pending;
   a->tls->bulk_receiving=false;text=device_status;return FakeTls::ReceiveResult::frame;
  };
  assert(operational_receive(*a,budget)&&budget==0&&polls>1);
  if(expire)assert(recoveries==1&&recovered_at-began==60000&&a->tls->expirations==1);
  else assert(recoveries==0&&clock_ms-began==7000&&a->tls->expirations==0);
  assert(a->tls->usable()&&shutdowns==0);
 }
}
void bulk_class_never_authorizes_controls(){
 auto a=start(true);
 for(auto kind:{Kind::scoped_intent,Kind::scoped_result,Kind::device_status,Kind::settings_read,Kind::synchronization,Kind::membership_event}){
  a->tls->last_bulk=false;assert(classify_received(*a,kind));a->tls->last_bulk=true;assert(!classify_received(*a,kind));
 }
 for(auto kind:{Kind::update_begin,Kind::update_chunk,Kind::update_install,Kind::update_status,Kind::update_abort,Kind::update_result}){
  a->tls->last_bulk=true;assert(classify_received(*a,kind));a->tls->last_bulk=false;assert(!classify_received(*a,kind));
 }
}
void application_failures_restart_scope_without_replaying_work(){
 for(bool asynchronous:{false,true}){
  auto a=start(true);a->tls->socket=3;reset_worker();uint64_t budget=0;const auto* tls=a->tls.get();const auto original=a->session.scope();
  auto expired=std::make_shared<const model::OwnedIntent>(action(*a)),following=std::make_shared<const model::OwnedIntent>(action(*a));
  std::weak_ptr<const model::OwnedIntent> ended=expired,discarded=following;
  a->queue.push_back({Kind::scoped_intent,{},1000,100,7,expired});a->queue.push_back({Kind::scoped_intent,{},1000,1200,7,following});
  expired.reset();following.reset();clock_ms=1200;
  if(asynchronous){std::lock_guard<std::mutex> global(lock);std::lock_guard<std::mutex> queue(a->mutex);request_session_restart(*a);}
  a->tls->poller=[&](std::string& text){assert(recoveries==1);text=device_status;return FakeTls::ReceiveResult::frame;};
  during_recovery=[&](auto& current,bool server){assert(server&&current.tls.get()==tls&&current.session.scope()==original&&ended.expired());
   assert(current.queue.size()==(asynchronous?0u:1u)&&discarded.expired()==asynchronous);
  };
  assert(operational_receive(*a,budget)&&recoveries==1&&budget==0);
  assert(recovery_reasons==std::vector<std::string>{asynchronous?"application_restart_requested":"causal_expired"});
  assert(a->tls.get()==tls&&a->tls->usable()&&!a->cancelled&&!a->restart_requested&&!a->execution_fenced&&shutdowns==0);
  assert(a->session.scope().link_id!=original.link_id&&a->queue.empty()&&a->tls->sent.empty()&&discarded.expired()&&restart_fences.size()==1);
 }
}
void invalid_local_ipc_restarts_scope_without_sending_or_replaying(){
 auto a=start(true);a->tls->socket=3;reset_worker();uint64_t budget=0;const auto original=a->session.scope();const auto* tls=a->tls.get();
 auto malformed=action(*a);malformed.target.port.octopus_id={};
 auto work=std::make_shared<const model::OwnedIntent>(malformed);std::weak_ptr<const model::OwnedIntent> abandoned=work;
 a->queue.push_back({Kind::scoped_intent,{},1000,clock_ms,7,work});work.reset();
 a->tls->poller=[&](std::string& text){assert(recoveries==1);text=device_status;return FakeTls::ReceiveResult::frame;};
 during_recovery=[&](auto& current,bool server){assert(server&&current.tls.get()==tls&&current.session.scope()==original&&current.queue.size()==1&&!abandoned.expired());};
 assert(operational_receive(*a,budget)&&recoveries==1&&recovery_reasons==std::vector<std::string>{"queued_invalid"});
 assert(a->tls.get()==tls&&a->tls->usable()&&!a->cancelled&&shutdowns==0&&a->session.scope().link_id!=original.link_id);
 assert(a->queue.empty()&&a->tls->sent.empty()&&abandoned.expired()&&restart_fences.size()==1);
}
void fatal_receive_ends_worker_without_recovery(){
 auto a=start(true);a->tls->socket=3;reset_worker();uint64_t budget=0;
 a->tls->poller=[&](std::string&){a->tls->healthy=false;return FakeTls::ReceiveResult::failed;};
 assert(!operational_receive(*a,budget)&&recoveries==0&&!a->tls->usable()&&restart_fences.empty());
 assert(diagnostic_logs.size()==1&&diagnostic_logs[0].find("reason=frame_receive ")!=std::string::npos);
}
void retiring_does_not_pump_ordinary_work_during_partial_receive(){
 auto a=start(true);a->tls->socket=3;status.authority_ready=true;reset_worker();uint64_t budget=0;unsigned polls=0;
 a->tls->poller=[&](std::string& text){if(++polls==1){auto i=std::make_shared<const model::OwnedIntent>(action(*a));assert(queue_control(a->peer,Kind::scoped_intent,{},1000,a->session.scope().link_id,7,i,clock_ms)==ESP_OK);a->retiring=true;}if(clock_ms<300)return FakeTls::ReceiveResult::pending;text=device_status;return FakeTls::ReceiveResult::frame;};
 assert(operational_receive(*a,budget)&&a->tls->sent.empty()&&a->queue.size()==1&&budget==0);
}
std::string update_base(){return "\"request_id\":\"11111111111111111111111111111111\",\"release_id\":\"release-one\",\"image_sha256\":\""+std::string(64,'a')+"\"";}
std::string image_chunk(unsigned offset){return "{"+update_base()+",\"offset\":\""+std::to_string(offset)+"\",\"data_hex\":\""+std::string(4096,'1')+"\",\"chunk_sha256\":\""+std::string(64,'b')+"\"}";}
std::string stage_image(){return "{"+update_base()+",\"phase\":\"stage\"}";}
std::string activate_image(){return "{"+update_base()+",\"phase\":\"activate\",\"reboot_delay_ms\":\"0\"}";}
void aged_firmware_survives_control_pruning_and_stays_before_validation(){
 auto a=start(true);a->tls->socket=3;reset_worker();uint64_t budget=0;const auto* tls=a->tls.get();const auto scope=a->session.scope();
 a->queue.push_back({Kind::update_chunk,image_chunk(0),5000,100,7,{}});
 a->queue.push_back({Kind::device_status,"{\"hostname\":\"expired\"}",1000,100,7,{}});
 a->queue.push_back({Kind::update_chunk,image_chunk(2048),5000,100,7,{}});
 a->queue.push_back({Kind::update_install,activate_image(),5000,100,7,{}});
 a->queue.push_back({Kind::update_install,stage_image(),5000,100,7,{}});
 clock_ms=12100;
 assert(queue_control(a->peer,Kind::device_status,"{\"hostname\":\"fresh\"}",1000,{})==ESP_OK);
 assert(a->queue.size()==4&&a->queue[0].kind==Kind::update_chunk&&a->queue[1].kind==Kind::update_chunk&&a->queue[2].payload==stage_image());
 unsigned chunks=0;bool staged=false;
 on_send=[&](Active&,const Envelope& e){
  if(e.kind==Kind::update_chunk){uint64_t offset=0;assert(!staged&&decimal(e.payload.get("offset"),offset)&&offset==chunks*2048&&e.ttl_ms==5000);++chunks;}
  if(e.kind==Kind::update_install){assert(e.payload.get("phase")->string=="stage"&&chunks==2&&e.ttl_ms==5000);staged=true;}
 };
 a->tls->poller=[&](std::string& text){if(!a->queue.empty())return FakeTls::ReceiveResult::pending;text=device_status;return FakeTls::ReceiveResult::frame;};
 assert(operational_receive(*a,budget)&&staged&&chunks==2&&a->tls->sent.size()==4&&a->queue.empty());
 assert(a->tls.get()==tls&&a->session.scope()==scope&&a->tls->usable()&&recoveries==0&&shutdowns==0&&diagnostic_logs.empty());
}
void worker_still_discards_expired_controls_activation_and_malformed_stage(){
 auto a=start(true);a->tls->socket=3;reset_worker();uint64_t budget=0;
 a->queue.push_back({Kind::device_status,"{\"hostname\":\"expired\"}",1000,100,7,{}});
 a->queue.push_back({Kind::update_install,activate_image(),5000,100,7,{}});
 a->queue.push_back({Kind::update_install,"{\"phase\":\"stage\"}",5000,100,7,{}});
 // The exemption must not bypass the existing configuration fence either.
 a->queue.push_back({Kind::update_chunk,image_chunk(0),5000,100,6,{}});
 clock_ms=12100;
 a->queue.push_back({Kind::device_status,"{\"hostname\":\"fresh\"}",1000,clock_ms,7,{}});
 a->tls->poller=[&](std::string& text){if(!a->queue.empty())return FakeTls::ReceiveResult::pending;text=device_status;return FakeTls::ReceiveResult::frame;};
 assert(operational_receive(*a,budget)&&a->queue.empty()&&a->tls->sent.size()==1);
 assert(a->tls->sent.front().kind==Kind::device_status&&a->tls->sent.front().payload.get("hostname")->string=="fresh");
 assert(recoveries==0&&shutdowns==0&&diagnostic_logs.empty());
}
void aged_firmware_keeps_existing_fifo_capacity(){
 auto a=start(true);reset_worker();
 for(unsigned n=0;n<16;++n)a->queue.push_back({Kind::update_chunk,image_chunk(n*2048),5000,100,7,{}});
 clock_ms=12100;
 assert(queue_control(a->peer,Kind::device_status,"{\"hostname\":\"fresh\"}",1000,{})==ESP_ERR_NO_MEM);
 assert(a->queue.size()==16&&recoveries==0&&shutdowns==0);
}
}
int main(){using namespace amity::consortium;pending_receive_keeps_pumping();partial_plaintext_deadline_recovers_on_idle_socket();partial_encrypted_record_keeps_original_outer_bound();bulk_prefix_allows_retransmission_and_keeps_fixed_outer_bound();bulk_class_never_authorizes_controls();application_failures_restart_scope_without_replaying_work();invalid_local_ipc_restarts_scope_without_sending_or_replaying();fatal_receive_ends_worker_without_recovery();retiring_does_not_pump_ordinary_work_during_partial_receive();aged_firmware_survives_control_pruning_and_stays_before_validation();worker_still_discards_expired_controls_activation_and_malformed_stage();aged_firmware_keeps_existing_fifo_capacity();std::cout<<"PASS actual operational worker: original plaintext2s/encrypted5s and input deadlines; bulk retransmission7s/fixed60s outer bound; class validation rejects bulk controls; sameTLS recovery without replay, responsive IPC and retirement fences; aged firmware bytes/stage retain bounded FIFO order and valid wireTTL without renewing controls/activation\n";}
'''
def main():
    source=(ROOT/'components/consortium/runtime.cpp').read_text()
    queue_start=source.index('        Queued queued;std::string payload;')
    receive_start=source.index('        fd_set reads;',queue_start)
    receive_end=source.index('Envelope envelope;',receive_start)
    recovery_start=source.index('      if(active->cancelled||active->retiring||!application_failure_recoverable(')
    recovery_end=source.index('\n    }',recovery_start)
    restart_start=source.index('        if(active->restart_requested){',source.index('void active_loop_work('))
    restart_end=source.index('\n',restart_start)
    worker='\nbool operational_receive(Active& storage,uint64_t& receive_until){uint64_t receive_started=0;auto* active=&storage;const bool server=true;LinkTrace trace;trace.phase="operational";while(!active->cancelled){while(!active->cancelled){'+source[restart_start:restart_end]+'const auto now=now_ms();'+source[queue_start:receive_start]+source[receive_start:receive_end]+'return true;}'+source[recovery_start:recovery_end]+'}trace_link_closed(storage,trace,now_ms());return false;}\n'
    class_start=source.index('        if(active->tls->received_bulk()!=is_update_kind(envelope.kind))')
    class_end=source.index('\n',class_start)
    worker+='\nbool classify_received(Active& storage,Kind kind){auto* active=&storage;Envelope envelope;envelope.kind=kind;LinkTrace trace;while(true){'+source[class_start:class_end]+'return true;}return false;}\n'
    helpers=source[source.index('struct LinkTraceSnapshot {'):source.index('std::optional<RuntimeSnapshotRetry> snapshot_retry(')]
    actual=function(source,'void request_session_restart(')+helpers+''.join(function(source,n) for n in ('Envelope outgoing(','static bool queued_firmware_stream(','static bool queued_expired(','static bool pure_key_release(', 'static bool queued_cleanup(','static bool queued_causal('))
    actual+=source[source.index('std::optional<RuntimeSnapshotRetry> snapshot_retry('):source.index('enum class QueuedState')]
    actual+=function(source,'static esp_err_t queue_control(')
    actual+=function(source,'enum class QueuedState').rstrip()+';\n'+function(source,'QueuedState prepare_queued(')
    actual+=function(source,'bool fence_synchronization_scope(')+function(source,'bool restart_synchronization_scope(')
    actual+=''.join(function(source,n) for n in ('bool synchronization_current(','bool synchronization_recoverable(','bool application_failure_recoverable('))
    trace=function(source,'struct LinkTrace {').rstrip()+';\n'
    prefix=PREFIX.replace('struct Active{',trace+'using TlsLink=FakeTls;\nstruct Active{').replace('int socket=-1;model::OctopusId authenticated;','enum class ReceiveResult{pending,frame,expired,failed};\n unsigned expirations=0;std::function<void()> before_expire;ReceiveResult expire_receive(){++expirations;if(before_expire)before_expire();return ReceiveResult::expired;}\n int ssl=0;std::function<ReceiveResult(std::string&)> poller;ReceiveResult poll_receive(std::string& out){return poller(out);}\n int socket=-1;model::OctopusId authenticated;')
    with tempfile.TemporaryDirectory(prefix='amity-worker-poll-') as directory:
        temp=Path(directory);(temp/'esp_err.h').write_text('#pragma once\nusing esp_err_t=int;constexpr int ESP_OK=0,ESP_ERR_INVALID_ARG=1,ESP_ERR_INVALID_STATE=2,ESP_ERR_NO_MEM=3,ESP_ERR_TIMEOUT=4,ESP_FAIL=5,ESP_ERR_NOT_FOUND=6,ESP_ERR_NOT_SUPPORTED=7;\n')
        cpp,binary=temp/'test.cpp',temp/'test';cpp.write_text(prefix+EXTRA+'namespace amity::consortium {\n'+actual+worker+'\n}\n'+SUPPORT+SUITE)
        sources=[ROOT/'components/consortium'/n for n in ('json.cpp','wire.cpp','owned_intent.cpp','session.cpp','settings.cpp','update.cpp','transfer.cpp')]+[ROOT/'components/amity_system/model.cpp']
        subprocess.run(shlex.split(os.environ.get('CXX','c++'))+['-std=c++17','-Wall','-Wextra','-Werror','-pthread','-g','-fsanitize=address,undefined','-I'+str(temp)]+['-I'+str(ROOT/n) for n in ('components/consortium','components/consortium/include','components/amity_system/include')]+list(map(str,[cpp]+sources))+['-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)
if __name__=='__main__':main()
