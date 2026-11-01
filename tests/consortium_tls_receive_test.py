#!/usr/bin/env python3
"""Exercise actual incremental TLS receive with timed, nonblocking SDK seams."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

from consortium_gatt_callback_test import function
from setup_timing_support import timing_support

ROOT = Path(__file__).resolve().parents[1]
PREFIX = r'''
#include "amity/consortium/json.hpp"
#include "tls_receive_trace.hpp"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <vector>
constexpr int MSG_DONTWAIT=64;
int recv_flags=0,send_flags=0;
int recv(int,void*,size_t,int flags){recv_flags=flags;errno=EAGAIN;return -1;}
int send(int,const void*,size_t,int flags){send_flags=flags;errno=EAGAIN;return -1;}
namespace amity::consortium::platform {
constexpr int MBEDTLS_ERR_SSL_WANT_READ=-1,MBEDTLS_ERR_SSL_WANT_WRITE=-2,MBEDTLS_ERR_NET_RECV_FAILED=-3,MBEDTLS_ERR_NET_SEND_FAILED=-4;
constexpr int MBEDTLS_ERR_SSL_TIMEOUT=-5;
uint64_t clock_ms=0,read_elapsed=0;
unsigned clock_reads=0,jump_on_clock=0;uint64_t jump_to=0;
uint64_t now_ms(){if(++clock_reads==jump_on_clock)clock_ms=jump_to;return clock_ms;}
unsigned pdMS_TO_TICKS(unsigned ms){return ms/10;} // Real firmware tick is 100 Hz.
std::vector<unsigned> waits;
void vTaskDelay(unsigned ticks){waits.push_back(ticks);clock_ms+=std::max(1u,ticks)*10;}
struct Byte{uint64_t at;uint8_t value;};
std::vector<Byte> incoming;size_t cursor=0;unsigned reads=0;std::optional<int> read_error;
uint64_t writable_at=0;std::vector<uint8_t> written;const uint8_t* blocked_pointer=nullptr;size_t blocked_size=0;
size_t maximum_write=0,partial_write=3;unsigned write_calls=0,write_elapsed=0;
unsigned handshake_calls=0;int handshake_error=0;uint32_t verify_flags=0;
std::optional<int> write_error;unsigned error_after=0;bool want_read=false;
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void reset(){clock_ms=read_elapsed=jump_to=0;clock_reads=jump_on_clock=0;waits.clear();incoming.clear();cursor=0;reads=0;read_error.reset();recv_flags=send_flags=0;writable_at=0;written.clear();blocked_pointer=nullptr;blocked_size=0;maximum_write=0;partial_write=3;write_calls=write_elapsed=error_after=handshake_calls=0;handshake_error=0;verify_flags=0;write_error.reset();want_read=false;}
int mbedtls_ssl_handshake(int*){++handshake_calls;return handshake_error;}
uint32_t mbedtls_ssl_get_verify_result(int*){return verify_flags;}
void reject_handshake(int&,int,bool,int){}
void append(const std::string& body,uint64_t prefix_at=0,uint64_t body_at=0){
    const auto bytes=frame(body);for(size_t i=0;i<bytes.size();++i)incoming.push_back({i<4?prefix_at:body_at,bytes[i]});
}
int mbedtls_ssl_read(int*,uint8_t* bytes,size_t capacity){
    ++reads;clock_ms+=read_elapsed;if(read_error)return *read_error;
    size_t size=0;while(cursor<incoming.size()&&incoming[cursor].at<=clock_ms&&size<capacity)bytes[size++]=incoming[cursor++].value;
    return size?static_cast<int>(size):MBEDTLS_ERR_SSL_WANT_READ;
}
int mbedtls_ssl_write(int*,const uint8_t* bytes,size_t size){
    ++write_calls;maximum_write=std::max(maximum_write,size);clock_ms+=write_elapsed;
    if(blocked_pointer)check(blocked_pointer==bytes&&blocked_size==size,"WANT_WRITE changed its original buffer");
    if(clock_ms<writable_at){blocked_pointer=bytes;blocked_size=size;return want_read?MBEDTLS_ERR_SSL_WANT_READ:MBEDTLS_ERR_SSL_WANT_WRITE;}
    if(write_error&&write_calls>error_after)return *write_error;
    blocked_pointer=nullptr;const auto count=std::min(size,partial_write);written.insert(written.end(),bytes,bytes+count);return static_cast<int>(count);
}
struct TlsLink {
    int ssl=0,socket=3;bool server=true;
    model::OctopusId authenticated=*model::OctopusId::parse("00000000000000000000000000000001");
    bool configured=true,handshaken=true,transport_failed=false;
    uint64_t timing_verify_ms=0;uint32_t timing_verify_calls=0;
    bool handshake();
    bool usable()const{return configured&&!transport_failed&&!incoming_failed;}
    bool established()const{return handshaken&&usable();}
    enum class ReceiveResult{pending,frame,expired,failed};
    ReceiveResult poll_receive(std::string&);
    ReceiveResult expire_receive();
    bool receive(std::string&,uint32_t duration=5000);
    bool send(std::string_view);
    FrameReader incoming_reader;
    uint8_t incoming_prefix[4]{};
    size_t incoming_size=0,incoming_expected=4;
    bool incoming_failed=false,incoming_expired=false;
    uint64_t incoming_last_poll_end=0,incoming_max_poll_gap=0,incoming_max_read_ms=0;
    TlsReceiveDiagnostic incoming_diagnostic;
    const TlsReceiveDiagnostic& receive_diagnostic()const{return incoming_diagnostic;}
    ReceiveResult receive_failed(TlsReceiveDiagnostic::Cause,int,uint64_t);
    ReceiveResult receive_expired(TlsReceiveDiagnostic::Cause,int,uint64_t);

};
'''
SUITE = r'''
void lazy_handshake_and_transport_health(){
    reset();TlsLink link;link.handshaken=false;std::string body;
    check(!link.established()&&link.usable(),"configured transport falsely established");
    check(link.poll_receive(body)==TlsLink::ReceiveResult::failed&&handshake_calls==0&&reads==0,"nonblocking poll attempted a blocking handshake");
    append("first response");
    check(link.send("first request")&&link.receive(body)&&body=="first response"&&link.established()&&handshake_calls==1,"first exchange did not establish authenticated TLS once");
    append("second response");check(link.send("second request")&&link.receive(body)&&body=="second response"&&link.handshake()&&handshake_calls==1,"later exchange replaced or repeated the TLS handshake");
    reset();TlsLink rejected;rejected.handshaken=false;verify_flags=1;
    check(!rejected.send("request")&&!rejected.usable()&&handshake_calls==1&&write_calls==0,"unverified handshake admitted application bytes");
    verify_flags=0;check(!rejected.send("request")&&handshake_calls==1&&write_calls==0,"failed transport silently attempted another handshake");
    for(int error:{0,-99}){
        reset();TlsLink broken;read_error=error;
        check(broken.poll_receive(body)==TlsLink::ReceiveResult::failed&&!broken.usable()&&!broken.established(),"fatal receive left transport reusable");
        const auto before=reads;read_error.reset();check(!broken.receive(body)&&!broken.send("later")&&reads==before&&write_calls==0&&handshake_calls==0,"fatal receive reused or reestablished the broken TLS object");
    }
    reset();TlsLink write;write_error=-99;
    check(!write.send("request")&&!write.usable(),"fatal write left transport reusable");
    write_error.reset();const auto before=write_calls;check(!write.send("later")&&write_calls==before&&handshake_calls==0,"failed write silently reused/reestablished TLS");
    reset();TlsLink invalid;invalid.handshaken=false;
    check(!invalid.send("")&&invalid.usable()&&handshake_calls==0&&write_calls==0,"invalid local payload poisoned or opened TLS");
}
void ipc_during_partial_frame(){
    reset();TlsLink link;append("complete owner observation",0,1200);std::string body;
    bool admitted=false,processed=false,complete=false;uint64_t admitted_at=0;
    for(unsigned pass=0;pass<65;++pass){
        const auto before=clock_ms;const auto result=link.poll_receive(body);
        check(clock_ms==before,"poll_receive waited and starved the existing IPC deadline");
        check(result!=TlsLink::ReceiveResult::failed,"valid partial frame failed");
        if(clock_ms==20){admitted=true;admitted_at=clock_ms;}
        // The existing link worker can pump already admitted work on every turn.
        if(admitted&&!processed){check(clock_ms-admitted_at<1000,"accepted work expired behind RX");processed=true;}
        if(result==TlsLink::ReceiveResult::frame){check(clock_ms==1200&&body=="complete owner observation","partial bytes escaped as a frame");complete=true;break;}
        check(body.empty(),"partial input reached the session/application");clock_ms+=20;
    }
    check(admitted&&processed&&complete&&waits.empty(),"RX did not preserve full-duplex progress");
}
void fragments_and_frame_boundaries(){
    reset();TlsLink link;std::string body;const auto bytes=frame("fragmented");
    for(size_t i=0;i<bytes.size();++i)incoming.push_back({i*10,bytes[i]});
    for(size_t i=0;i<bytes.size();++i){clock_ms=i*10;const auto result=link.poll_receive(body);
        check(result==(i+1==bytes.size()?TlsLink::ReceiveResult::frame:TlsLink::ReceiveResult::pending),"fragment framing changed");}
    check(body=="fragmented","fragment body changed");
    reset();TlsLink pair;append("first");append("second");body.clear();
    check(pair.poll_receive(body)==TlsLink::ReceiveResult::frame&&body=="first"&&cursor==9,"RX consumed another frame");
    check(pair.poll_receive(body)==TlsLink::ReceiveResult::frame&&body=="second"&&cursor==19,"next frame state was lost");
    reset();TlsLink maximum;append(std::string(max_frame_bytes,'x'));body.clear();
    check(maximum.poll_receive(body)==TlsLink::ReceiveResult::pending&&reads==16,"poll CPU read budget changed");
    check(maximum.poll_receive(body)==TlsLink::ReceiveResult::frame&&body.size()==max_frame_bytes,"maximum frame did not resume");
    // A mode change must continue the same in-flight framing state; a wrapper
    // cannot mistake a partial prefix/body for the next envelope's prefix.
    for(size_t available:{size_t{2},size_t{6}}){
        reset();TlsLink handover;const auto first=frame("handover");
        for(size_t i=0;i<first.size();++i)incoming.push_back({i<available?uint64_t{0}:uint64_t{100},first[i]});
        append("following",100,100);body.clear();
        check(handover.poll_receive(body)==TlsLink::ReceiveResult::pending&&cursor==available,"handover fixture did not retain partial bytes");
        check(handover.receive(body,500)&&body=="handover"&&clock_ms==100,"blocking wrapper discarded partial state");
        check(handover.poll_receive(body)==TlsLink::ReceiveResult::frame&&body=="following","wrapper consumed or corrupted the next frame");
    }
}
void unchanged_terminal_deadlines_and_errors(){
    reset();TlsLink idle;std::string body;clock_ms=20000;
    check(idle.poll_receive(body)==TlsLink::ReceiveResult::pending,"idle link invented a partial-frame timeout");
    reset();TlsLink progress;append("later",0,3000);
    check(progress.poll_receive(body)==TlsLink::ReceiveResult::pending,"prefix unexpectedly completed");
    clock_ms=FrameReader::progress_timeout_ms-1;check(progress.poll_receive(body)==TlsLink::ReceiveResult::pending,"progress timeout fired early");
    ++clock_ms;check(progress.poll_receive(body)==TlsLink::ReceiveResult::expired&&progress.usable()&&body.empty(),"progress expiry closed TLS or was extended");
    clock_ms=3000;check(progress.poll_receive(body)==TlsLink::ReceiveResult::pending&&body.empty()&&progress.usable(),"timed-out payload resumed instead of draining");
    reset();TlsLink total;const auto bytes=frame(std::string(20,'x'));
    for(size_t i=0;i<bytes.size();++i)incoming.push_back({i<4?0:(i-3)*1000,bytes[i]});
    for(clock_ms=0;clock_ms<FrameReader::frame_timeout_ms;clock_ms+=1000)
        check(total.poll_receive(body)==TlsLink::ReceiveResult::pending,"total deadline fired early");
    check(total.poll_receive(body)==TlsLink::ReceiveResult::expired&&total.usable(),"total frame deadline was extended by progress or closed TLS");
    for(uint32_t length:{0u,static_cast<uint32_t>(max_frame_bytes+1)}){
        reset();TlsLink invalid;for(unsigned shift:{24,16,8,0})incoming.push_back({0,static_cast<uint8_t>(length>>shift)});
        check(invalid.poll_receive(body)==TlsLink::ReceiveResult::failed,"invalid length accepted");
        incoming.clear();cursor=0;append("valid later");check(invalid.poll_receive(body)==TlsLink::ReceiveResult::failed,"invalid frame resumed");
    }
    for(int error:{0,-99}){reset();TlsLink broken;read_error=error;check(broken.poll_receive(body)==TlsLink::ReceiveResult::failed,"EOF/fatal TLS read was ignored");}
    reset();TlsLink write;read_error=MBEDTLS_ERR_SSL_WANT_WRITE;check(write.poll_receive(body)==TlsLink::ReceiveResult::pending,"WANT_WRITE was treated as fatal");
    read_error.reset();append("after want");check(write.poll_receive(body)==TlsLink::ReceiveResult::frame,"WANT_WRITE lost receive progress");
}
void blocking_wrapper_and_nonblocking_socket(){
    reset();TlsLink handshake;std::string body;append("hello",0,1200);
    check(handshake.receive(body,2000)&&clock_ms==1200&&body=="hello","bounded blocking wrapper changed frame/deadline");
    check(!waits.empty()&&std::all_of(waits.begin(),waits.end(),[](unsigned ticks){return ticks>=1;}),"blocking wrapper busy-spun at100Hz");
    for(size_t available:{size_t{0},size_t{2},size_t{6}}){
        reset();TlsLink expired;const auto bytes=frame("late hello");
        for(size_t i=0;i<bytes.size();++i)incoming.push_back({i<available?uint64_t{0}:uint64_t{1200},bytes[i]});
        append("following",1200,1200);
        check(!expired.receive(body,1000)&&clock_ms==1000&&expired.usable()&&expired.established(),"wrapper deadline poisoned healthy TLS or was extended");
        check(expired.receive(body,1000)&&clock_ms==1200&&body=="late hello"&&handshake_calls==0,"next receive lost partial prefix/body or reconnected");
        check(expired.receive_diagnostic().cause==TlsReceiveDiagnostic::Cause::none,"successful receive retained a stale application timeout diagnostic");
        check(expired.poll_receive(body)==TlsLink::ReceiveResult::frame&&body=="following","wrapper timeout corrupted the following frame");
    }
    reset();int fd=3;uint8_t bytes[8];check(receive_socket(&fd,bytes,sizeof(bytes))==MBEDTLS_ERR_SSL_WANT_READ&&recv_flags==MSG_DONTWAIT,"poll socket callback can block for SO_RCVTIMEO");
    check(send_socket(&fd,bytes,sizeof(bytes))==MBEDTLS_ERR_SSL_WANT_WRITE&&send_flags==MSG_DONTWAIT,"TLS read-side write callback can block for SO_SNDTIMEO");
    reset();TlsLink sender;writable_at=1200;check(sender.send("hello")&&clock_ms==1200&&written==frame("hello"),"bounded write lost partial/WANT progress");
    check(!waits.empty()&&std::all_of(waits.begin(),waits.end(),[](unsigned ticks){return ticks>=1;}),"send wrapper busy-spun at100Hz");
    reset();TlsLink blocked;writable_at=3100;check(!blocked.send("late")&&clock_ms==3000&&written.empty()&&!blocked.usable(),"existing three-second write budget was extended or pending write left reusable");
}
void bounded_application_writes(){
    for(size_t size:{size_t{1},size_t{508},size_t{509},max_frame_bytes}){
        reset();TlsLink link;partial_write=97;writable_at=30;const std::string body(size,'x');
        check(link.send(body)&&written==frame(body),"bounded/partial application writes changed frame bytes");
        check(maximum_write<=512&&clock_ms==30,"application write exceeded512 bytes or changed WANT deadline");
    }
    reset();TlsLink want;want_read=true;writable_at=20;
    check(want.send(std::string(600,'x'))&&maximum_write==512&&clock_ms==20,"WANT_READ did not retain original write parameters");
    for(int error:{0,-99}){
        reset();TlsLink failed;write_error=error;error_after=1;partial_write=17;
        const std::string body(1000,'x');const auto bytes=frame(body);
        check(!failed.send(body)&&write_calls==2&&written==std::vector<uint8_t>(bytes.begin(),bytes.begin()+17),"fatal/zero write retried or reported complete");
    }
    reset();TlsLink slow;write_elapsed=20;
    check(!slow.send(std::string(max_frame_bytes,'x'))&&clock_ms==3000&&write_calls==150,"partial progress renewed the three-second budget");
    for(const std::string &body:{std::string{},std::string(max_frame_bytes+1,'x')}){
        reset();TlsLink invalid;check(!invalid.send(body)&&write_calls==0,"invalid/oversized frame reached TLS");
    }
}

void terminal_receive_diagnostics(){
    using Cause=TlsReceiveDiagnostic::Cause;
    reset();TlsLink starved;append("buffered body",0,100);std::string body;
    check(starved.poll_receive(body)==TlsLink::ReceiveResult::pending,"starvation fixture prefix missing");clock_ms=2300;
    check(starved.poll_receive(body)==TlsLink::ReceiveResult::expired&&starved.usable(),"existing progress timeout changed or closed TLS");
    const auto captured=starved.receive_diagnostic();
    check(captured.cause==Cause::progress_timeout&&captured.bytes==4&&captured.expected==17&&captured.progress_ms==2300&&captured.total_ms==2300&&captured.poll_gap_ms==2300&&captured.read_ms==0,"progress/poll-gap diagnostics did not capture exact failure");
    clock_ms=9900;check(starved.poll_receive(body)==TlsLink::ReceiveResult::pending&&starved.receive_diagnostic().progress_ms==captured.progress_ms&&starved.usable(),"expired-frame diagnostics changed during drain");
    reset();TlsLink preempted;append("body after preemption",0,1);
    check(preempted.poll_receive(body)==TlsLink::ReceiveResult::pending,"preemption fixture missing prefix");clock_ms=1;clock_reads=0;jump_on_clock=5;jump_to=2100;
    check(preempted.poll_receive(body)==TlsLink::ReceiveResult::expired&&preempted.usable()&&preempted.receive_diagnostic().cause==Cause::progress_timeout,"preemption before parser feed reused an earlier deadline sample");
    reset();TlsLink blocked;append("blocked inside SDK");read_elapsed=2100;
    check(blocked.poll_receive(body)==TlsLink::ReceiveResult::expired&&blocked.usable()&&blocked.receive_diagnostic().cause==Cause::progress_timeout&&blocked.receive_diagnostic().read_ms==2100&&blocked.receive_diagnostic().poll_gap_ms==0,"SDK read wall time confused with an unserviced worker");
    reset();TlsLink wrapper;check(!wrapper.receive(body,100)&&wrapper.usable()&&wrapper.receive_diagnostic().cause==Cause::wrapper_timeout&&wrapper.receive_diagnostic().bytes==0,"outer timeout poisoned TLS or cause missing");
    read_error=-123;check(wrapper.poll_receive(body)==TlsLink::ReceiveResult::failed&&wrapper.receive_diagnostic().cause==Cause::tls_error&&wrapper.receive_diagnostic().code==-123,"old application timeout concealed a later fatal transport diagnostic");
    reset();TlsLink eof;read_error=0;check(eof.poll_receive(body)==TlsLink::ReceiveResult::failed&&eof.receive_diagnostic().cause==Cause::tls_eof&&eof.receive_diagnostic().code==0,"EOF cause missing");
    reset();TlsLink fatal;read_error=-123;check(fatal.poll_receive(body)==TlsLink::ReceiveResult::failed&&fatal.receive_diagnostic().cause==Cause::tls_error&&fatal.receive_diagnostic().code==-123,"fatal SDK code missing");
    reset();TlsLink invalid;for(unsigned i=0;i<4;++i)incoming.push_back({0,0});check(invalid.poll_receive(body)==TlsLink::ReceiveResult::failed&&invalid.receive_diagnostic().cause==Cause::invalid_length&&invalid.receive_diagnostic().bytes==4,"malformed frame cause missing");
    reset();TlsLink reversed;clock_ms=100;append("later",100,3000);check(reversed.poll_receive(body)==TlsLink::ReceiveResult::pending,"clock fixture missing prefix");clock_ms=99;
    check(reversed.poll_receive(body)==TlsLink::ReceiveResult::expired&&reversed.usable()&&reversed.receive_diagnostic().cause==Cause::clock_reversed,"clock reversal cause missing");
    reset();TlsLink total;const auto bytes=frame(std::string(20,'x'));for(size_t i=0;i<bytes.size();++i)incoming.push_back({i*1000,bytes[i]});
    for(clock_ms=0;clock_ms<10000;clock_ms+=1000)check(total.poll_receive(body)==TlsLink::ReceiveResult::pending,"total timeout fired early");
    check(total.poll_receive(body)==TlsLink::ReceiveResult::expired&&total.usable()&&total.receive_diagnostic().cause==Cause::frame_timeout&&total.receive_diagnostic().total_ms==10000,"total frame deadline cause missing");
}
void expired_frames_drain_without_replacing_tls(){
    using Result=TlsLink::ReceiveResult;using Cause=TlsReceiveDiagnostic::Cause;
    // Reproduce the physical512/582-byte observation, and partial prefixes.
    for(size_t available:{size_t{1},size_t{2},size_t{3},size_t{4},size_t{512}}){
        reset();TlsLink link;std::string body;const auto expired=frame(std::string(578,'x'));
        for(size_t i=0;i<expired.size();++i)incoming.push_back({i<available?uint64_t{0}:uint64_t{3100},expired[i]});
        append("fresh round",3100,3100);
        check(link.poll_receive(body)==Result::pending&&cursor==available,"expiry fixture did not retain exact partial bytes");
        clock_ms=2000;const auto read_before=reads;
        check(link.poll_receive(body)==Result::expired&&reads==read_before&&body.empty()&&link.established(),"deadline read/accepted bytes or discarded healthy TLS");
        const auto diagnostic=link.receive_diagnostic();check(diagnostic.expired()&&!diagnostic.fatal()&&diagnostic.bytes==available&&diagnostic.progress_ms==2000,"expiry metadata missing");
        clock_ms=2500;check(link.poll_receive(body)==Result::pending&&body.empty()&&link.usable(),"expiry was reported repeatedly while draining");
        clock_ms=3100;check(link.poll_receive(body)==Result::pending&&body.empty()&&cursor==expired.size()&&link.usable(),"drain crossed a frame boundary or delivered late payload");
        check(link.receive_diagnostic().progress_ms==2000,"draining reset the expired message's acceptance clock");
        check(link.poll_receive(body)==Result::frame&&body=="fresh round"&&handshake_calls==0&&link.receive_diagnostic().cause==Cause::none,"next frame did not resume on original TLS");
    }
    // An outer budget can expire before an encrypted record yields a prefix.
    reset();TlsLink encrypted;std::string body;append("old encrypted frame",3000,3000);append("after encrypted",3000,3000);
    check(encrypted.expire_receive()==Result::expired&&encrypted.usable()&&encrypted.receive_diagnostic().cause==Cause::operation_timeout,"outer expiry poisoned transport");
    check(encrypted.expire_receive()==Result::pending,"outer expiry was emitted twice");clock_ms=3000;
    check(encrypted.poll_receive(body)==Result::pending&&body.empty(),"expired encrypted frame was accepted");
    check(encrypted.poll_receive(body)==Result::frame&&body=="after encrypted","outer expiry lost following frame");
    // A full-frame expiry cannot be extended by slow continuous progress.
    reset();TlsLink total;const auto slow=frame(std::string(20,'x'));
    for(size_t i=0;i<slow.size();++i)incoming.push_back({i<4?0:(i-3)*1000,slow[i]});append("after total",21000,21000);
    for(clock_ms=0;clock_ms<10000;clock_ms+=1000)check(total.poll_receive(body)==Result::pending,"slow frame expired early");
    check(total.poll_receive(body)==Result::expired&&total.receive_diagnostic().total_ms==10000,"total deadline extended");clock_ms=21000;
    check(total.poll_receive(body)==Result::pending&&body.empty()&&total.usable(),"total-expired payload was accepted");
    check(total.poll_receive(body)==Result::frame&&body=="after total","total expiry corrupted next frame");
    // Fatal reads and an invalid completed prefix remain truly unusable.
    for(int error:{0,-99}){
        reset();TlsLink broken;append("late",0,3000);check(broken.poll_receive(body)==Result::pending,"fatal-drain fixture missing prefix");clock_ms=2000;
        check(broken.poll_receive(body)==Result::expired,"fatal-drain fixture did not expire");read_error=error;
        check(broken.poll_receive(body)==Result::failed&&!broken.usable()&&broken.receive_diagnostic().fatal(),"fatal read while draining remained usable or hid behind expiry");
    }
    reset();TlsLink invalid;incoming={{0,0},{0,0},{3000,0},{3000,0}};check(invalid.poll_receive(body)==Result::pending,"partial invalid prefix fixture failed");clock_ms=2000;
    check(invalid.poll_receive(body)==Result::expired&&invalid.usable(),"partial prefix did not expire");clock_ms=3000;
    check(invalid.poll_receive(body)==Result::failed&&!invalid.usable()&&invalid.receive_diagnostic().cause==Cause::invalid_length,"invalid late prefix gained framing recovery");
    // Blocking callers return on expiry; later waits yield while draining and
    // preserve expiry evidence instead of replacing it with a wrapper timeout.
    reset();TlsLink wrapper;append("too late",0,5000);append("resumed",5000,5000);
    check(!wrapper.receive(body,3000)&&clock_ms==2000&&wrapper.usable()&&wrapper.receive_diagnostic().expired(),"wrapper ignored application-frame expiry");
    check(!wrapper.receive(body,1000)&&clock_ms==3000&&wrapper.receive_diagnostic().progress_ms==2000,"drain wait erased original expiry");
    check(!waits.empty()&&std::all_of(waits.begin(),waits.end(),[](unsigned ticks){return ticks>=1;}),"expired drain busy-spun");clock_ms=5000;
    check(wrapper.receive(body,1000)&&body=="resumed"&&handshake_calls==0,"blocking drain did not continue existing TLS");
    // Draining a maximum frame remains bounded per polling turn.
    reset();TlsLink maximum;append(std::string(max_frame_bytes,'x'),0,3000);append("after maximum",3000,3000);
    check(maximum.poll_receive(body)==Result::pending,"maximum drain fixture missing prefix");clock_ms=2000;check(maximum.poll_receive(body)==Result::expired,"maximum frame did not expire");clock_ms=3000;
    bool resumed=false;for(unsigned turn=0;turn<4;++turn){const auto before=reads;const auto result=maximum.poll_receive(body);check(reads-before<=16,"drain exceeded bounded polling work");check(result!=Result::expired&&result!=Result::failed,"drain repeated expiry or failed");if(result==Result::frame){check(body=="after maximum","maximum late payload was delivered");resumed=true;break;}}
    check(resumed&&maximum.usable()&&waits.empty(),"maximum drain did not resume without blocking");
}
}
int main(){using namespace amity::consortium::platform;try{
    lazy_handshake_and_transport_health();ipc_during_partial_frame();fragments_and_frame_boundaries();unchanged_terminal_deadlines_and_errors();blocking_wrapper_and_nonblocking_socket();bounded_application_writes();terminal_receive_diagnostics();expired_frames_drain_without_replacing_tls();
    std::cout<<"PASS actual TLS lifecycle/receive/send: one lazy handshake reused; expired frames are rejected once and drained without replacing TLS, including512/582/prefix/total/outer deadlines; no late payload executes; fatal errors remain unusable; bounded nonblocking polls,512-byte writes and partial/WANT continuation\n";
    return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT / 'components/consortium/tls_link.cpp')
    source = parser.parse_args().source.read_text()
    actual = (function(source, 'int receive_socket(') + function(source, 'int send_socket(')
              + function(source, 'bool TlsLink::handshake(') + function(source, 'bool TlsLink::receive(') + function(source, 'bool TlsLink::send('))
    if 'TlsLink::ReceiveResult TlsLink::poll_receive(' in source:
        actual += (function(source, 'TlsLink::ReceiveResult TlsLink::receive_failed(') if 'TlsLink::ReceiveResult TlsLink::receive_failed(' in source else '') + function(source, 'TlsLink::ReceiveResult TlsLink::receive_expired(') + function(source, 'TlsLink::ReceiveResult TlsLink::expire_receive(') + function(source, 'TlsLink::ReceiveResult TlsLink::poll_receive(')
    else:
        # Before/after regression: exercise the previous actual blocking read
        # through the same worker-turn seam, rather than fail at extraction.
        actual += 'TlsLink::ReceiveResult TlsLink::poll_receive(std::string& out){return receive(out)?ReceiveResult::frame:ReceiveResult::failed;}\n'
    with tempfile.TemporaryDirectory(prefix='amity-tls-receive-') as directory:
        cpp, binary = Path(directory) / 'test.cpp', Path(directory) / 'test'
        cpp.write_text(timing_support() + PREFIX + actual + SUITE)
        subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + [
            '-std=c++17', '-Wall', '-Wextra', '-Werror', '-g', '-fsanitize=address,undefined',
            '-I' + str(ROOT / 'components/consortium/include'), '-I' + str(ROOT / 'components/consortium'),
            '-I' + str(ROOT / 'components/amity_system/include'), str(cpp),
            str(ROOT / 'components/consortium/json.cpp'), '-o', str(binary)], check=True)
        return subprocess.run([str(binary)], check=False).returncode


if __name__ == '__main__':
    raise SystemExit(main())
