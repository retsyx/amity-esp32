#!/usr/bin/env python3
"""Exercise actual TCP snapshots and passive release ACK observation."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
from consortium_gatt_callback_test import function

ROOT = Path(__file__).resolve().parents[1]
PREFIX = r'''
#include "tcp_ack_trace.hpp"
#include "amity/consortium/owned_intent.hpp"
#include <cassert>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

// A bounded model of the pinned SDK's socket -> netconn -> TCP PCB fields.
// The real helper, not a copy, runs against these layouts and lock seams.
struct tcp_pcb {uint32_t snd_lbb=0,lastack=0;int16_t rto=3;uint8_t nrtx=0;};
struct netconn {unsigned type=0x10;union {tcp_pcb* tcp;}pcb;};
struct lwip_sock {netconn* conn=nullptr;};
static lwip_sock sockets[4];
static int lock_tcpip_core=123;
static bool core_busy=false,core_held=false,marked=false;
static unsigned takes=0,gives=0,marks=0,unmarks=0,accesses=0;
constexpr int pdTRUE=1,LWIP_CORE_LOCK_MARK_HOLDER=1,LWIP_CORE_LOCK_UNMARK_HOLDER=2;
constexpr uint32_t TCP_SLOW_INTERVAL=500;
#define NETCONN_TCP 0x10
#define NETCONNTYPE_GROUP(type) ((type)&0xf0)
int xSemaphoreTake(int mutex,unsigned wait){
 assert(mutex==lock_tcpip_core&&wait==0&&!core_held);++takes;
 if(core_busy)return 0;
 core_held=true;return pdTRUE;
}
bool sys_thread_tcpip(int action){
 assert(core_held);
 if(action==LWIP_CORE_LOCK_MARK_HOLDER){assert(!marked);marked=true;++marks;}
 else{assert(action==LWIP_CORE_LOCK_UNMARK_HOLDER&&marked);marked=false;++unmarks;}
 return true;
}
void xSemaphoreGive(int mutex){assert(mutex==lock_tcpip_core&&core_held&&!marked);core_held=false;++gives;}
#define UNLOCK_TCPIP_CORE() do{sys_thread_tcpip(LWIP_CORE_LOCK_UNMARK_HOLDER);xSemaphoreGive(lock_tcpip_core);}while(0)
lwip_sock* lwip_socket_dbg_get_socket(int fd){assert(core_held&&marked);++accesses;return fd>=0&&fd<4?&sockets[fd]:nullptr;}

namespace amity::consortium {
uint64_t clock_ms=100;
uint64_t now_ms(){return clock_ms;}
struct Tls {int socket=1;};
struct Active {
 std::shared_ptr<Tls> tls=std::make_shared<Tls>();
 TcpAckTrace release_ack;model::OctopusId release_ack_owner;
 uint64_t release_ack_generation=0;uint8_t release_ack_key=0;
};
std::vector<std::string> logs;
template<class... Args>void ESP_LOGI(const char*,const char* format,Args... args){
 assert(!core_held&&!marked);char text[512];const int size=snprintf(text,sizeof(text),format,args...);
 assert(size>0&&size<384);logs.emplace_back(text);
}
'''
SUITE = r'''
void reset(){
 assert(!core_held&&!marked);core_busy=false;takes=gives=marks=unmarks=accesses=0;
 for(auto& socket:sockets)socket.conn=nullptr;
 logs.clear();clock_ms=100;
}
bool status(const char* value){return !logs.empty()&&logs.back().find(std::string("status=")+value+" ")!=std::string::npos;}
model::OwnedIntent release(){
 model::OwnedIntent intent;intent.kind=model::OwnedIntentKind::key_edges;
 intent.target.port.octopus_id=*model::OctopusId::parse("11111111111111111111111111111111");
 intent.generation=7;intent.edge_count=1;intent.edges[0]={66,false};return intent;
}
void snapshots_are_bounded_and_read_only(){
 reset();tcp_pcb pcb{0x12345678,0x12345500,3,2};netconn conn;conn.pcb.tcp=&pcb;sockets[1].conn=&conn;
 const auto before=pcb;auto sample=sample_tcp_ack(1);
 assert(sample.state==TcpAckSampleState::ready&&sample.buffered==pcb.snd_lbb&&sample.acknowledged==pcb.lastack&&sample.rto_ms==1500&&sample.retransmissions==2);
 assert(pcb.snd_lbb==before.snd_lbb&&pcb.lastack==before.lastack&&pcb.rto==before.rto&&pcb.nrtx==before.nrtx);
 assert(takes==1&&accesses==1&&marks==1&&unmarks==1&&gives==1&&logs.empty());
 core_busy=true;const auto previous_accesses=accesses;sample=sample_tcp_ack(1);
 assert(sample.state==TcpAckSampleState::busy&&takes==2&&accesses==previous_accesses&&gives==1&&marks==1&&unmarks==1);
 core_busy=false;assert(sample_tcp_ack(-1).state==TcpAckSampleState::missing);
 assert(sample_tcp_ack(3).state==TcpAckSampleState::missing);
 conn.type=0x20;assert(sample_tcp_ack(1).state==TcpAckSampleState::missing);
 conn.type=0x18;assert(sample_tcp_ack(1).state==TcpAckSampleState::ready); // TCP/IPv6
 conn.pcb.tcp=nullptr;assert(sample_tcp_ack(1).state==TcpAckSampleState::missing);
 assert(!core_held&&!marked&&marks==unmarks&&marks==gives&&logs.empty());
}
void delayed_ack_and_later_writes(){
 reset();Active active;tcp_pcb pcb{1000,500,3,0};netconn conn;conn.pcb.tcp=&pcb;sockets[1].conn=&conn;
 auto intent=release();begin_release_ack(active,intent,clock_ms);
 assert(active.release_ack.pending&&active.release_ack.target==1000&&active.release_ack.pending_bytes==500&&logs.empty());
 // Another frame may enter this same TCP stream. The observation retains
 // the release's original byte boundary instead of following newer writes.
 pcb.snd_lbb=1500;clock_ms=120;poll_release_ack(active);assert(logs.empty()&&active.release_ack.target==1000);
 pcb.lastack=900;pcb.nrtx=1;pcb.rto=6;clock_ms=1520;poll_release_ack(active);
 assert(logs.empty()&&active.release_ack.pending_bytes==100&&active.release_ack.max_nrtx==1);
 pcb.lastack=1000;pcb.nrtx=0;clock_ms=1540;poll_release_ack(active);
 assert(!active.release_ack.pending&&logs.size()==1&&status("acked"));
 assert(logs[0].find("key=66 generation=7")!=std::string::npos&&logs[0].find("elapsed_ms=1440 pending_bytes=0 max_nrtx=1 initial_rto_ms=1500 poll_gap_ms=1400 busy=0")!=std::string::npos);
 const auto calls=takes;poll_release_ack(active);finish_release_ack(active);
 assert(logs.size()==1&&takes==calls); // Completed observations are inert.
}
void wrapped_sequences_and_slow_sampler(){
 reset();Active active;tcp_pcb pcb{16,UINT32_MAX-31,2,0};netconn conn;conn.pcb.tcp=&pcb;sockets[1].conn=&conn;
 begin_release_ack(active,release(),100);assert(active.release_ack.pending&&active.release_ack.pending_bytes==48);
 pcb.lastack=8;clock_ms=130;poll_release_ack(active);assert(active.release_ack.pending&&active.release_ack.pending_bytes==8);
 pcb.lastack=32;clock_ms=1650;poll_release_ack(active);
 assert(status("acked")&&active.release_ack.elapsed_ms==1550&&active.release_ack.poll_gap_ms==1520);
 // An already received ACK observed after a stalled pump is explicitly
 // bounded by its gap; it is not presented as precise network RTT.
 reset();pcb={500,500,3,0};conn.pcb.tcp=&pcb;sockets[1].conn=&conn;clock_ms=130;
 begin_release_ack(active,release(),100);
 assert(status("acked")&&active.release_ack.elapsed_ms==30&&active.release_ack.poll_gap_ms==30);
}
void missing_busy_replaced_and_ended_observations(){
 reset();Active active;tcp_pcb pcb{1000,500,3,0};netconn conn;conn.pcb.tcp=&pcb;sockets[1].conn=&conn;
 core_busy=true;begin_release_ack(active,release(),100);
 assert(status("unavailable")&&!active.release_ack.pending&&accesses==0);
 core_busy=false;logs.clear();begin_release_ack(active,release(),100);core_busy=true;
 clock_ms=120;poll_release_ack(active);clock_ms=140;poll_release_ack(active);
 assert(active.release_ack.pending&&logs.empty()&&active.release_ack.busy_samples==2);
 core_busy=false;pcb.lastack=1000;clock_ms=160;poll_release_ack(active);
 assert(status("acked")&&active.release_ack.poll_gap_ms==60&&active.release_ack.busy_samples==2);
 logs.clear();pcb.lastack=500;begin_release_ack(active,release(),160);clock_ms=180;
 auto next=release();next.edges[0].key=4;next.generation=8;pcb.snd_lbb=1200;
 begin_release_ack(active,next,180);
 assert(logs.size()==1&&status("replaced")&&logs[0].find("key=66 generation=7")!=std::string::npos);
 assert(active.release_ack.pending&&active.release_ack.target==1200&&active.release_ack_key==4&&active.release_ack_generation==8);
 clock_ms=200;finish_release_ack(active);
 assert(logs.size()==2&&status("session_ended")&&!active.release_ack.pending);
 logs.clear();begin_release_ack(active,next,200);conn.pcb.tcp=nullptr;clock_ms=220;poll_release_ack(active);
 assert(status("closed")&&!active.release_ack.pending); // Concurrent shutdown under core lock.
}
void observation_expiry_and_maximum_log_width(){
 reset();Active active;tcp_pcb pcb{1000,500,3,0};netconn conn;conn.pcb.tcp=&pcb;sockets[1].conn=&conn;
 begin_release_ack(active,release(),100);clock_ms=10100;poll_release_ack(active);
 assert(status("timeout")&&!active.release_ack.pending&&pcb.snd_lbb==1000&&pcb.lastack==500&&pcb.nrtx==0);
 logs.clear();clock_ms=200;begin_release_ack(active,release(),200);clock_ms=199;poll_release_ack(active);
 assert(status("clock_reversed")&&!active.release_ack.pending);
 active.release_ack_generation=UINT64_MAX;active.release_ack_key=UINT8_MAX;
 auto& trace=active.release_ack;trace.elapsed_ms=trace.poll_gap_ms=UINT64_MAX;
 trace.pending_bytes=trace.initial_rto_ms=trace.busy_samples=UINT32_MAX;trace.max_nrtx=UINT8_MAX;
 emit_release_ack(active,"session_ended");assert(logs.back().size()<384);
}
}
int main(){using namespace amity::consortium;snapshots_are_bounded_and_read_only();delayed_ack_and_later_writes();wrapped_sequences_and_slow_sampler();missing_busy_replaced_and_ended_observations();observation_expiry_and_maximum_log_width();std::cout<<"PASS passive TCP ACK diagnosis: zero-wait core lock, read-only snapshots, fixed release boundary/wrap, observed retransmit/gaps, busy/missing/replacement/session/10s diagnostic completion and bounded logs\n";}
'''


def main():
    runtime = (ROOT / 'components/consortium/runtime.cpp').read_text()
    actual = runtime[runtime.index('TcpAckSample sample_tcp_ack('):runtime.index('struct LinkTraceSnapshot {')]
    sampler = function(runtime, 'TcpAckSample sample_tcp_ack(')
    assert 'xSemaphoreTake(lock_tcpip_core,0)' in sampler
    assert 'ESP_LOG' not in sampler and 'for(' not in sampler and 'while(' not in sampler
    assert 'extern "C" {\n#include "lwip/priv/sockets_priv.h"' in runtime
    assert 'if(sent)begin_release_ack(*active,intent,completed);' in runtime
    assert runtime.index('poll_release_ack(*active);', runtime.index('void active_loop_work(')) < runtime.index('if(active->restart_requested)', runtime.index('void active_loop_work('))
    with tempfile.TemporaryDirectory(prefix='amity-tcp-ack-') as directory:
        root = Path(directory);cpp = root / 'test.cpp';binary = root / 'test'
        cpp.write_text(PREFIX + actual + SUITE)
        subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + [
            '-std=c++17', '-Wall', '-Wextra', '-Werror', '-g', '-fsanitize=address,undefined',
            '-I' + str(ROOT / 'components/consortium'), '-I' + str(ROOT / 'components/consortium/include'),
            '-I' + str(ROOT / 'components/amity_system/include'), str(cpp),
            str(ROOT / 'components/amity_system/model.cpp'), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
