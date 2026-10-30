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
unsigned select_calls=0;bool scripted_tls_pending=false;
int mbedtls_ssl_check_pending(const int*){return scripted_tls_pending?1:0;}
}
int scripted_select(int,fd_set*,fd_set*,fd_set*,timeval*){
 using namespace amity::consortium;clock_ms+=20;return ++select_calls==1?1:0;
}
#define select scripted_select
'''
SUITE=r'''
namespace amity::consortium {
model::OwnedIntent action(const Active& a){model::OwnedIntent i;i.kind=model::OwnedIntentKind::power;i.target={{a.session.scope().consortium_id,a.peer,0,model::Side::back},"Player"};i.generation=i.sequence=1;i.config_revision=7;i.on=true;return i;}
void pending_receive_keeps_pumping(){
 auto a=start(true);a->tls->socket=3;status.authority_ready=true;select_calls=0;scripted_tls_pending=false;on_send={};unsigned polls=0;uint64_t admitted=0,budget=0;
 auto admit=[&]{auto i=std::make_shared<const model::OwnedIntent>(action(*a));admitted=clock_ms;assert(queue_control(a->peer,Kind::scoped_intent,{},1000,a->session.scope().link_id,7,i,admitted)==ESP_OK);};
 a->tls->poller=[&](std::string& text){++polls;if(polls==1)admit();if(clock_ms<1300)return FakeTls::ReceiveResult::pending;text=heartbeat;return FakeTls::ReceiveResult::frame;};
 // Only the old operational whole-frame read invokes this blocking seam.
 a->tls->incoming.push_back(heartbeat);a->tls->before_receive=[&]{admit();clock_ms+=1180;};
 assert(operational_receive(*a,budget));
 assert(a->tls->sent.size()==1&&a->tls->sent[0].kind==Kind::scoped_intent&&clock_ms-admitted>=1180&&a->queue.empty());
 assert(!a->cancelled&&diagnostic_logs.empty()&&budget==0&&select_calls>1);
}
void partial_plaintext_deadline_runs_on_idle_socket(){
 auto a=start(true);a->tls->socket=3;select_calls=0;scripted_tls_pending=false;uint64_t budget=0;FrameReader reader;unsigned polls=0;uint64_t began=0;
 a->tls->poller=[&](std::string&){++polls;if(polls==1){began=clock_ms;uint8_t header[]={0,0,0,8};size_t used=0;assert(reader.feed(header,4,used,clock_ms)==FrameReader::Result::incomplete&&used==4);}return reader.check_deadline(clock_ms)==FrameReader::Result::timed_out?FakeTls::ReceiveResult::failed:FakeTls::ReceiveResult::pending;};
 assert(!operational_receive(*a,budget));assert(clock_ms-began==FrameReader::progress_timeout_ms&&polls>1&&diagnostic_logs.size()==1&&diagnostic_logs[0].find("reason=frame_receive ")!=std::string::npos);
}
void partial_encrypted_record_keeps_original_outer_bound(){
 auto a=start(true);a->tls->socket=3;select_calls=0;scripted_tls_pending=false;uint64_t budget=0,began=0;unsigned polls=0;
 a->tls->poller=[&](std::string&){if(++polls==1)began=clock_ms;return FakeTls::ReceiveResult::pending;};
 assert(!operational_receive(*a,budget));assert(clock_ms-began==5000&&polls>1&&diagnostic_logs.size()==1&&diagnostic_logs[0].find("reason=frame_receive_timeout")!=std::string::npos);
}
void retiring_does_not_pump_ordinary_work_during_partial_receive(){
 auto a=start(true);a->tls->socket=3;status.authority_ready=true;select_calls=0;scripted_tls_pending=false;uint64_t budget=0;unsigned polls=0;
 a->tls->poller=[&](std::string& text){if(++polls==1){auto i=std::make_shared<const model::OwnedIntent>(action(*a));assert(queue_control(a->peer,Kind::scoped_intent,{},1000,a->session.scope().link_id,7,i,clock_ms)==ESP_OK);a->retiring=true;}if(clock_ms<300)return FakeTls::ReceiveResult::pending;text=heartbeat;return FakeTls::ReceiveResult::frame;};
 assert(operational_receive(*a,budget)&&a->tls->sent.empty()&&a->queue.size()==1&&budget==0);
}
}
int main(){using namespace amity::consortium;pending_receive_keeps_pumping();partial_plaintext_deadline_runs_on_idle_socket();partial_encrypted_record_keeps_original_outer_bound();retiring_does_not_pump_ordinary_work_during_partial_receive();std::cout<<"PASS actual operational worker: pending RX keeps original IPC pump responsive; idle partial plaintext2s/encrypted5s bounds; retirement emits no queued positive work\n";}
'''
def main():
    source=(ROOT/'components/consortium/runtime.cpp').read_text()
    queue_start=source.index('        Queued queued;std::string payload;')
    receive_start=source.index('        fd_set reads;',queue_start)
    receive_end=source.index('Envelope envelope;',receive_start)
    worker='\nbool operational_receive(Active& storage,uint64_t& receive_until){auto* active=&storage;LinkTrace trace;trace.phase="operational";static_cast<void>(receive_until);while(true){const auto now=now_ms();'+source[queue_start:receive_start]+source[receive_start:receive_end]+'return true;}trace_link_closed(storage,trace,now_ms());return false;}\n'
    helpers=source[source.index('struct LinkTraceSnapshot {'):source.index('std::optional<RuntimeSnapshotRetry> snapshot_retry(')]
    actual=helpers+''.join(function(source,n) for n in ('Envelope outgoing(','static bool queued_cleanup(','static bool queued_causal('))
    actual+=source[source.index('std::optional<RuntimeSnapshotRetry> snapshot_retry('):source.index('enum class QueuedState')]
    actual+=function(source,'static esp_err_t queue_control(')
    actual+=source[source.index('enum class QueuedState'):source.index('void active_loop_work(')]
    trace=function(source,'struct LinkTrace {').rstrip()+';\n'
    prefix=PREFIX.replace('struct Active{',trace+'using TlsLink=FakeTls;\nstruct Active{').replace('int socket=-1;model::OctopusId authenticated;','enum class ReceiveResult{pending,frame,failed};\n int ssl=0;std::function<ReceiveResult(std::string&)> poller;ReceiveResult poll_receive(std::string& out){return poller(out);}\n int socket=-1;model::OctopusId authenticated;')
    with tempfile.TemporaryDirectory(prefix='amity-worker-poll-') as directory:
        temp=Path(directory);(temp/'esp_err.h').write_text('#pragma once\nusing esp_err_t=int;constexpr int ESP_OK=0,ESP_ERR_INVALID_ARG=1,ESP_ERR_INVALID_STATE=2,ESP_ERR_NO_MEM=3,ESP_ERR_TIMEOUT=4,ESP_FAIL=5,ESP_ERR_NOT_FOUND=6,ESP_ERR_NOT_SUPPORTED=7;\n')
        cpp,binary=temp/'test.cpp',temp/'test';cpp.write_text(prefix+EXTRA+'namespace amity::consortium {\n'+actual+worker+'\n}\n'+SUPPORT+SUITE)
        sources=[ROOT/'components/consortium'/n for n in ('json.cpp','wire.cpp','owned_intent.cpp','session.cpp','settings.cpp','update.cpp','transfer.cpp')]+[ROOT/'components/amity_system/model.cpp']
        subprocess.run(shlex.split(os.environ.get('CXX','c++'))+['-std=c++17','-Wall','-Wextra','-Werror','-pthread','-g','-fsanitize=address,undefined','-I'+str(temp)]+['-I'+str(ROOT/n) for n in ('components/consortium','components/consortium/include','components/amity_system/include')]+list(map(str,[cpp]+sources))+['-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)
if __name__=='__main__':main()
