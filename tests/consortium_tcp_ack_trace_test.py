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
#include "network_diagnostics.hpp"
#include "amity/consortium/owned_intent.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
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

namespace amity::network_diagnostics {
Snapshot values;
unsigned reads=0;
Snapshot snapshot(){assert(!core_held&&!marked);++reads;return values;}
}

namespace amity::consortium {
uint64_t clock_ms=100;
uint64_t now_ms(){return clock_ms;}
struct Tls {int socket=1;};
struct Active {
 std::shared_ptr<Tls> tls=std::make_shared<Tls>();
 TcpAckTrace release_ack;model::OctopusId release_ack_owner;
 network_diagnostics::Snapshot release_network_before;
 uint64_t release_ack_generation=0;uint8_t release_ack_key=0,release_ack_edges=0;bool release_ack_down=false;
};
std::vector<std::string> logs,network_logs,path_logs,console_logs;
bool model_console_delay=false;
template<class... Args>void ESP_LOGI(const char*,const char* format,Args... args){
 assert(!core_held&&!marked);char text[512];const int size=snprintf(text,sizeof(text),format,args...);
 assert(size>0&&size<384);
 if(std::string(text).find("stage=network_sta ")!=std::string::npos){
  // Include the pinned SDK's maximum-width level/timestamp/tag/newline prefix.
  assert(size+28<384);network_logs.emplace_back(text);
 }else if(std::string(text).find("stage=network_path ")!=std::string::npos){
  assert(size+28<384);path_logs.emplace_back(text);
 }else if(std::string(text).find("stage=console_all ")!=std::string::npos){
  assert(size+28<384);console_logs.emplace_back(text);
 }else logs.emplace_back(text);
 if(model_console_delay){
  auto& network=network_diagnostics::values;
  ++network.console_calls;network.console_elapsed_ms+=200;++network.console_slow_calls;
  network.console_last_slow_began_ms=network.now_ms;network.console_last_slow_duration_ms=200;
 }
}
'''
SUITE = r'''
void reset(){
 assert(!core_held&&!marked);core_busy=false;takes=gives=marks=unmarks=accesses=0;
 for(auto& socket:sockets)socket.conn=nullptr;
 logs.clear();network_logs.clear();path_logs.clear();console_logs.clear();model_console_delay=false;clock_ms=100;network_diagnostics::values={};network_diagnostics::reads=0;
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
 auto intent=release();begin_release_ack(active,intent,clock_ms,network_diagnostics::snapshot());
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
 begin_release_ack(active,release(),100,network_diagnostics::snapshot());assert(active.release_ack.pending&&active.release_ack.pending_bytes==48);
 pcb.lastack=8;clock_ms=130;poll_release_ack(active);assert(active.release_ack.pending&&active.release_ack.pending_bytes==8);
 pcb.lastack=32;clock_ms=1650;poll_release_ack(active);
 assert(status("acked")&&active.release_ack.elapsed_ms==1550&&active.release_ack.poll_gap_ms==1520);
 // An already received ACK observed after a stalled pump is explicitly
 // bounded by its gap; it is not presented as precise network RTT.
 reset();pcb={500,500,3,0};conn.pcb.tcp=&pcb;sockets[1].conn=&conn;clock_ms=130;
 begin_release_ack(active,release(),100,network_diagnostics::snapshot());
 assert(status("acked")&&active.release_ack.elapsed_ms==30&&active.release_ack.poll_gap_ms==30);
}
void missing_busy_replaced_and_ended_observations(){
 reset();Active active;tcp_pcb pcb{1000,500,3,0};netconn conn;conn.pcb.tcp=&pcb;sockets[1].conn=&conn;
 core_busy=true;begin_release_ack(active,release(),100,network_diagnostics::snapshot());
 assert(status("unavailable")&&!active.release_ack.pending&&accesses==0);
 core_busy=false;logs.clear();begin_release_ack(active,release(),100,network_diagnostics::snapshot());core_busy=true;
 clock_ms=120;poll_release_ack(active);clock_ms=140;poll_release_ack(active);
 assert(active.release_ack.pending&&logs.empty()&&active.release_ack.busy_samples==2);
 core_busy=false;pcb.lastack=1000;clock_ms=160;poll_release_ack(active);
 assert(status("acked")&&active.release_ack.poll_gap_ms==60&&active.release_ack.busy_samples==2);
 logs.clear();pcb.lastack=500;auto down=release();down.edges[0].down=true;begin_release_ack(active,down,160,network_diagnostics::snapshot());clock_ms=180;
 auto next=release();next.edges[0]={4,true};next.edges[1]={4,false};next.edge_count=2;next.generation=8;pcb.snd_lbb=1200;
 begin_release_ack(active,next,180,network_diagnostics::snapshot());
 assert(logs.size()==1&&status("replaced")&&logs[0].find("key=66 generation=7 phase=down edge_count=1")!=std::string::npos);
 assert(active.release_ack.pending&&active.release_ack.target==1200&&active.release_ack_key==4&&active.release_ack_generation==8&&active.release_ack_edges==2);
 clock_ms=200;finish_release_ack(active);
 assert(logs.size()==2&&status("session_ended")&&!active.release_ack.pending&&logs.back().find("key=4 generation=8 phase=tap edge_count=2")!=std::string::npos);
 logs.clear();begin_release_ack(active,next,200,network_diagnostics::snapshot());conn.pcb.tcp=nullptr;clock_ms=220;poll_release_ack(active);
 assert(status("closed")&&!active.release_ack.pending); // Concurrent shutdown under core lock.
}
void observation_expiry_and_maximum_log_width(){
 reset();Active active;tcp_pcb pcb{1000,500,3,0};netconn conn;conn.pcb.tcp=&pcb;sockets[1].conn=&conn;
 begin_release_ack(active,release(),100,network_diagnostics::snapshot());clock_ms=10100;poll_release_ack(active);
 assert(status("timeout")&&!active.release_ack.pending&&pcb.snd_lbb==1000&&pcb.lastack==500&&pcb.nrtx==0);
 logs.clear();clock_ms=200;begin_release_ack(active,release(),200,network_diagnostics::snapshot());clock_ms=199;poll_release_ack(active);
 assert(status("clock_reversed")&&!active.release_ack.pending);
 active.release_ack_generation=UINT64_MAX;active.release_ack_key=UINT8_MAX;
 active.release_ack_edges=UINT8_MAX;active.release_ack_down=true;
 auto& trace=active.release_ack;trace.elapsed_ms=trace.poll_gap_ms=UINT64_MAX;
 trace.pending_bytes=trace.initial_rto_ms=trace.busy_samples=UINT32_MAX;trace.max_nrtx=UINT8_MAX;
 emit_release_ack(active,"session_ended");assert(logs.back().size()<384);
}
void every_user_key_edge_shape(){
 for(unsigned shape=0;shape<3;++shape){
  reset();Active active;tcp_pcb pcb{1000,500,3,0};netconn conn;conn.pcb.tcp=&pcb;sockets[1].conn=&conn;
  auto intent=release();intent.edges[0].down=shape!=1;
  if(shape==2){intent.edge_count=2;intent.edges[1]={intent.edges[0].key,false};}
  const auto edge_count=intent.edge_count;const auto down=intent.edges[0].down;
  begin_release_ack(active,intent,100,network_diagnostics::snapshot());
  assert(active.release_ack.pending&&active.release_ack_edges==edge_count&&active.release_ack_down==down&&logs.empty());
  pcb.lastack=1000;clock_ms=120;poll_release_ack(active);
  const std::string phase=shape==0?"down":shape==1?"up":"tap";
  assert(logs.size()==1&&status("acked"));
  assert(logs[0].find(shape==1?"Key release stage=ack":"Key transfer stage=ack")==0);
  assert(logs[0].find("phase="+phase+" edge_count="+std::to_string(edge_count))!=std::string::npos);
  assert(intent.edge_count==edge_count&&intent.edges[0].down==down&&intent.edges[0].key==66&&intent.generation==7);
  for(unsigned poll=0;poll<16;++poll){clock_ms+=20;poll_release_ack(active);}
  assert(logs.size()==1); // No periodic key log or extra observation.
 }
}
void station_window_deltas_progress_and_suppression(){
 reset();Active active;tcp_pcb pcb{1000,500,3,0};netconn conn;conn.pcb.tcp=&pcb;sockets[1].conn=&conn;
 auto& network=network_diagnostics::values;
 network.tx_done_registered=1;network.tx.no_mem=UINT32_MAX-1;network.tx.other=5;
 network.rx.no_mem=UINT32_MAX;network.rx.other=9;network.tx_done_ok=UINT32_MAX;network.tx_done_fail=4;
 begin_release_ack(active,release(),100,network_diagnostics::snapshot());assert(network_diagnostics::reads==1);
 // Snapshot deltas wrap just like the driver's unsigned counters; timestamp
 // ages remain valid when the station's millisecond clock wraps independently.
 network.now_ms=20;network.tx.attempts=1;network.tx.last_call_ms=UINT32_MAX-9;
 network.rx.attempts=1;network.rx.last_call_ms=10;network.tx_done_last_ms=UINT32_MAX-4;
 network.tx.no_mem=1;network.tx.other=7;network.rx.no_mem=1;network.rx.other=12;
 network.tx_done_ok=2;network.tx_done_fail=6;
 pcb.lastack=1000;clock_ms=400;poll_release_ack(active);
 assert(network_diagnostics::reads==2&&network_logs.size()==1);
 assert(path_logs.size()==2);
 const auto& line=network_logs[0];
 assert(line.find("stage=network_sta owner=11111111111111111111111111111111 key=66 generation=7 phase=up edge_count=1 status=acked")!=std::string::npos);
 assert(line.find("reg=1 regerr=0 tx_nm=3 tx_err=2 tx_done=3 tx_fail=2 rx_nm=2 rx_err=3 tx_age_ms=30 done_age_ms=25 rx_age_ms=10")!=std::string::npos);
 finish_release_ack(active);assert(network_diagnostics::reads==2&&network_logs.size()==1);
 // Ordinary fast success adds no station log or after-snapshot.
 reset();pcb={1000,500,3,0};conn.pcb.tcp=&pcb;sockets[1].conn=&conn;
 begin_release_ack(active,release(),100,network_diagnostics::snapshot());pcb.lastack=1000;clock_ms=120;poll_release_ack(active);
 assert(network_diagnostics::reads==1&&network_logs.empty()&&path_logs.empty());
 // A retransmission observed even before 250ms still merits one window.
 reset();pcb={1000,500,3,0};conn.pcb.tcp=&pcb;sockets[1].conn=&conn;
 begin_release_ack(active,release(),100,network_diagnostics::snapshot());pcb.lastack=1000;pcb.nrtx=1;clock_ms=120;poll_release_ack(active);
 assert(network_diagnostics::reads==2&&network_logs.size()==1);
 // Registration failure and no progress must stay visible, not report fresh
 // driver activity simply because its initial timestamp is zero.
 reset();core_busy=true;network_diagnostics::values.registration_error=-1;
 begin_release_ack(active,release(),100,network_diagnostics::snapshot());
 assert(network_logs.size()==1&&network_logs[0].find("status=unavailable reg=0 regerr=-1")!=std::string::npos);
 assert(network_logs[0].find("tx_age_ms=4294967295 done_age_ms=4294967295 rx_age_ms=4294967295")!=std::string::npos);
}
void station_window_replacement_and_maximum_width(){
 reset();Active active;tcp_pcb pcb{1000,500,3,0};netconn conn;conn.pcb.tcp=&pcb;sockets[1].conn=&conn;
 begin_release_ack(active,release(),100,network_diagnostics::snapshot());clock_ms=120;
 auto next=release();next.generation=8;next.edges[0].down=true;
 begin_release_ack(active,next,120,network_diagnostics::snapshot());
 assert(network_logs.empty()&&network_diagnostics::reads==2); // Fast replacement is not a failure.
 network_diagnostics::values.tx_done_fail=2;clock_ms=400;
 auto tap=release();tap.generation=9;tap.edge_count=2;tap.edges[0].down=true;tap.edges[1]={66,false};
 begin_release_ack(active,tap,400,network_diagnostics::snapshot());
 assert(network_logs.size()==1&&network_logs[0].find("generation=8 phase=down edge_count=1 status=replaced")!=std::string::npos);
 assert(network_logs[0].find("tx_fail=2")!=std::string::npos);
 network_diagnostics::values.tx_done_fail=3;clock_ms=420;finish_release_ack(active);
 assert(network_logs.size()==2&&network_logs[1].find("generation=9 phase=tap edge_count=2 status=session_ended")!=std::string::npos);
 assert(network_logs[1].find("tx_fail=1")!=std::string::npos); // New trace owns its own baseline.
 active.release_network_before={};auto& network=network_diagnostics::values;network={};
 network.now_ms=UINT32_MAX;network.tx.attempts=network.rx.attempts=UINT32_MAX;
 network.tx.no_mem=network.tx.other=network.rx.no_mem=network.rx.other=UINT32_MAX;
 network.tx.ok=network.rx.ok=network.tx.active=network.rx.active=UINT32_MAX;
 network.tx.last_duration_ms=network.rx.last_duration_ms=UINT32_MAX;
 network.tx.slow_calls=network.rx.slow_calls=network.tx.last_slow_duration_ms=network.rx.last_slow_duration_ms=UINT32_MAX;
 active.release_network_before.tx.active=active.release_network_before.rx.active=UINT32_MAX;
 network.tx_done_ok=network.tx_done_fail=UINT32_MAX;network.tx_done_registered=1;network.registration_error=INT32_MIN;
 network.console_calls=network.console_elapsed_ms=network.console_slow_calls=network.console_last_slow_duration_ms=UINT32_MAX;
 active.release_ack_owner=*model::OctopusId::parse("ffffffffffffffffffffffffffffffff");
 active.release_ack_generation=UINT64_MAX;active.release_ack_key=active.release_ack_edges=UINT8_MAX;active.release_ack_down=true;
 emit_release_ack(active,"clock_reversed");
 assert(network_logs.back().size()+28<384);
 assert(path_logs.size()>=2&&path_logs.back().size()+28<384&&path_logs[path_logs.size()-2].size()+28<384);
 assert(console_logs.back().size()+28<384);
}
void station_window_includes_send_and_failed_write(){
 reset();Active active;tcp_pcb pcb{1000,500,3,0};netconn conn;conn.pcb.tcp=&pcb;sockets[1].conn=&conn;
 auto& network=network_diagnostics::values;network.tx.no_mem=4;
 const auto before=network_diagnostics::snapshot();
 // An admission error can occur synchronously inside the existing TLS send,
 // before ACK observation begins. Its baseline must precede that send.
 network.tx.no_mem=5;clock_ms=150;
 begin_release_ack(active,release(),150,before);
 pcb.lastack=1000;clock_ms=450;poll_release_ack(active);
 assert(network_logs.size()==1&&network_logs[0].find("tx_nm=1")!=std::string::npos);
 // A failed new write gets a station window without starting/replacing an ACK
 // observation or mutating the previous in-flight key's metadata/baseline.
 reset();pcb={1000,500,3,0};conn.pcb.tcp=&pcb;sockets[1].conn=&conn;
 auto old=release();old.edges[0].down=true;
 begin_release_ack(active,old,100,network_diagnostics::snapshot());
 const auto failed_before=network_diagnostics::snapshot();network.tx.no_mem=2;
 auto failed=release();failed.generation=8;
 emit_key_network(failed.target.port.octopus_id,failed.edges[0].key,failed.generation,failed.edge_count,failed.edges[0].down,"write_failed",failed_before,network_diagnostics::snapshot());
 assert(network_logs.size()==1&&network_logs[0].find("generation=8 phase=up edge_count=1 status=write_failed")!=std::string::npos);
 assert(network_logs[0].find("tx_nm=2")!=std::string::npos);
 assert(active.release_ack.pending&&active.release_ack.target==1000&&active.release_ack_generation==7&&active.release_ack_down);
 assert(active.release_network_before.tx.no_mem==0&&pcb.snd_lbb==1000&&pcb.lastack==500);
}
void global_console_window_is_bounded_and_excludes_reporting(){
 reset();Active active;tcp_pcb pcb{1000,500,3,0};netconn conn;conn.pcb.tcp=&pcb;sockets[1].conn=&conn;
 begin_release_ack(active,release(),100,network_diagnostics::snapshot());
 // The original ACK and station reports can themselves block on console.
 // Both must be excluded by the one cached after-snapshot, without feedback.
 model_console_delay=true;pcb.lastack=1000;clock_ms=400;poll_release_ack(active);
 assert(network_diagnostics::reads==2&&logs.size()==1&&network_logs.size()==1&&console_logs.empty());
 assert(path_logs.size()==2&&network_diagnostics::values.console_elapsed_ms==800);
 // Console counters and last slow-call start ages use the same uint32 wrap
 // rules as network observations. The sum may overlap across callers.
 reset();pcb={1000,500,3,0};conn.pcb.tcp=&pcb;sockets[1].conn=&conn;
 auto& network=network_diagnostics::values;network.console_calls=UINT32_MAX-1;
 network.console_elapsed_ms=UINT32_MAX-49;network.console_slow_calls=UINT32_MAX;
 begin_release_ack(active,release(),100,network_diagnostics::snapshot());
 network.console_calls=1;network.console_elapsed_ms=50;network.console_slow_calls=0;
 network.console_last_slow_began_ms=UINT32_MAX-9;network.console_last_slow_duration_ms=150;network.now_ms=20;
 pcb.lastack=1000;clock_ms=400;poll_release_ack(active);
 assert(console_logs.size()==1);
 assert(console_logs[0].find("stage=console_all owner=11111111111111111111111111111111 key=66 generation=7 phase=up edge_count=1 status=acked")!=std::string::npos);
 assert(console_logs[0].find("calls=3 sum_ms=100 slow=1 last_began_age_ms=30 last_duration_ms=150")!=std::string::npos);
 finish_release_ack(active);assert(console_logs.size()==1&&network_diagnostics::reads==2);
 // Idle/insubstantial console windows remain quiet, while either inclusive
 // 100ms summed time or an independently observed slow call qualifies.
 for(unsigned scenario=0;scenario<4;++scenario){
  reset();pcb={1000,500,3,0};conn.pcb.tcp=&pcb;sockets[1].conn=&conn;
  begin_release_ack(active,release(),100,network_diagnostics::snapshot());
  network.console_calls=3;network.console_elapsed_ms=scenario==0?99:scenario==1?100:scenario==2?0:200;
  network.console_slow_calls=scenario==2?1:0;
  pcb.lastack=1000;clock_ms=scenario==3?120:400;poll_release_ack(active);
  assert(console_logs.size()==(scenario==1||scenario==2?1u:0u));
  if(scenario==1)assert(console_logs[0].find("last_began_age_ms=4294967295 last_duration_ms=0")!=std::string::npos);
  if(scenario==3)assert(network_diagnostics::reads==1); // No extra fast-ACK sampling/logging.
 }
}
void station_path_returns_active_calls_and_slow_spans(){
 reset();Active active;tcp_pcb pcb{1000,500,3,0};netconn conn;conn.pcb.tcp=&pcb;sockets[1].conn=&conn;
 auto& network=network_diagnostics::values;
 network.tx.attempts=UINT32_MAX-1;network.tx.ok=UINT32_MAX;network.tx.active=1;
 network.rx.attempts=UINT32_MAX;network.rx.ok=UINT32_MAX-1;network.rx.active=2;network.rx.slow_calls=UINT32_MAX;
 begin_release_ack(active,release(),100,network_diagnostics::snapshot());
 network.now_ms=20;
 network.tx.attempts=1;network.tx.ok=1;network.tx.active=0;network.tx.last_call_ms=UINT32_MAX-4;network.tx.last_ok_ms=UINT32_MAX-9;network.tx.last_duration_ms=7;
 network.rx.attempts=2;network.rx.ok=2;network.rx.active=3;network.rx.last_call_ms=10;network.rx.last_ok_ms=5;network.rx.last_duration_ms=200;
 network.rx.slow_calls=1;network.rx.last_slow_ms=UINT32_MAX-9;network.rx.last_slow_duration_ms=1020;
 pcb.lastack=1000;clock_ms=400;poll_release_ack(active);
 assert(network_diagnostics::reads==2&&path_logs.size()==2);
 assert(path_logs[0].find("path=tx calls=3 ok=2 active=1/0 in_age=25 ok_age=30 last_ms=7 slow=0 slow_age=4294967295 slow_ms=0")!=std::string::npos);
 assert(path_logs[1].find("path=rx calls=3 ok=4 active=2/3 in_age=10 ok_age=15 last_ms=200 slow=2 slow_age=30 slow_ms=1020")!=std::string::npos);
 // No success is a distinct observation even after a fresh receive entry.
 reset();active={};pcb={1000,500,3,0};conn.pcb.tcp=&pcb;sockets[1].conn=&conn;
 begin_release_ack(active,release(),100,network_diagnostics::snapshot());
 network.now_ms=400;network.rx.attempts=1;network.rx.active=1;network.rx.last_call_ms=350;
 pcb.lastack=1000;clock_ms=400;poll_release_ack(active);
 assert(path_logs[1].find("path=rx calls=1 ok=0 active=0/1 in_age=50 ok_age=4294967295 last_ms=0 slow=0 slow_age=4294967295 slow_ms=0")!=std::string::npos);
 finish_release_ack(active);assert(path_logs.size()==2&&network_diagnostics::reads==2);
}
}
int main(){using namespace amity::consortium;snapshots_are_bounded_and_read_only();delayed_ack_and_later_writes();wrapped_sequences_and_slow_sampler();missing_busy_replaced_and_ended_observations();observation_expiry_and_maximum_log_width();every_user_key_edge_shape();station_window_deltas_progress_and_suppression();station_window_replacement_and_maximum_width();station_window_includes_send_and_failed_write();global_console_window_is_bounded_and_excludes_reporting();station_path_returns_active_calls_and_slow_spans();std::cout<<"PASS passive TCP ACK diagnosis: unchanged ACK lifecycle, zero-wait core snapshots, station/path/console counter-time wrap and send/ACK/failure windows outside locks, active calls and retained slow spans, cached reports exclude their own console cost, suppression and bounded prefixed logs\n";}
'''


def main():
    runtime = (ROOT / 'components/consortium/runtime.cpp').read_text()
    actual = runtime[runtime.index('TcpAckSample sample_tcp_ack('):runtime.index('struct LinkTraceSnapshot {')]
    sampler = function(runtime, 'TcpAckSample sample_tcp_ack(')
    assert 'xSemaphoreTake(lock_tcpip_core,0)' in sampler
    assert 'ESP_LOG' not in sampler and 'for(' not in sampler and 'while(' not in sampler
    assert 'extern "C" {\n#include "lwip/priv/sockets_priv.h"' in runtime
    assert 'if(sent)begin_release_ack(*active,intent,completed,network_before);' in runtime
    assert runtime.index('const auto network_before=key?network_diagnostics::snapshot():network_diagnostics::Snapshot{};') < runtime.index('const bool sent=send(*active,queued.kind,payload,ttl);')
    assert '"write_failed",network_before,network_diagnostics::snapshot()' in runtime
    assert runtime.index('poll_release_ack(*active);', runtime.index('void active_loop_work(')) < runtime.index('if(active->restart_requested)', runtime.index('void active_loop_work('))
    with tempfile.TemporaryDirectory(prefix='amity-tcp-ack-') as directory:
        root = Path(directory);cpp = root / 'test.cpp';binary = root / 'test'
        cpp.write_text(PREFIX + actual + SUITE)
        subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + [
            '-std=c++17', '-Wall', '-Wextra', '-Werror', '-g', '-fsanitize=address,undefined',
            '-I' + str(ROOT / 'components/consortium'), '-I' + str(ROOT / 'components/consortium/include'),
            '-I' + str(ROOT / 'components/network_diagnostics/include'),
            '-I' + str(ROOT / 'components/amity_system/include'), str(cpp),
            str(ROOT / 'components/amity_system/model.cpp'), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
