#!/usr/bin/env python3
"""Compile actual TLS rejection/handshake code with bounded network seams."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
from consortium_gatt_callback_test import function
from setup_timing_support import timing_support

ROOT=Path(__file__).resolve().parents[1]
PREFIX=r'''
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>
#define MBEDTLS_PRIVATE(value) value
constexpr int MBEDTLS_ERR_SSL_NO_CLIENT_CERTIFICATE=-0x7480;
constexpr int MBEDTLS_ERR_SSL_WANT_READ=-1,MBEDTLS_ERR_SSL_WANT_WRITE=-2;
constexpr int MBEDTLS_ERR_SSL_TIMEOUT=-5;
constexpr int MBEDTLS_SSL_ALERT_LEVEL_FATAL=2,MBEDTLS_SSL_ALERT_MSG_HANDSHAKE_FAILURE=40;
constexpr int SHUT_WR=1,MSG_DONTWAIT=64,F_GETFL=3,F_SETFL=4,O_NONBLOCK=2048;
struct mbedtls_ssl_context{size_t out_left=0;};
uint64_t clock_ms=0;
uint64_t now_ms(){return clock_ms;}
unsigned pdMS_TO_TICKS(unsigned duration){return duration;}
void vTaskDelay(unsigned duration){clock_ms+=duration;}
int handshake_error=0,handshake_calls=0,verify_flags=0,alerts=0,flushes=0,shutdowns=0,fcntl_flags=0;
size_t available_bytes=0,received_bytes=0;
bool blocked_alert=false,fcntl_failure=false;
int mbedtls_ssl_handshake(mbedtls_ssl_context*){++handshake_calls;return handshake_error;}
uint32_t mbedtls_ssl_get_verify_result(mbedtls_ssl_context*){return verify_flags;}
int mbedtls_ssl_send_alert_message(mbedtls_ssl_context*ssl,int level,int description){
    if(level!=2||description!=40)throw std::runtime_error("wrong fatal alert");
    if(ssl->out_left){++flushes;if(blocked_alert)return MBEDTLS_ERR_SSL_WANT_WRITE;ssl->out_left=0;return 0;}
    ++alerts;if(blocked_alert){ssl->out_left=7;return MBEDTLS_ERR_SSL_WANT_WRITE;}return 0;
}
int fcntl(int,int command,int value){if(fcntl_failure)return -1;if(command==F_SETFL)fcntl_flags=value;return 0;}
int shutdown(int,int direction){if(direction!=SHUT_WR)throw std::runtime_error("rejection closed input instead of half-closing output");++shutdowns;return 0;}
int recv(int,void*,size_t size,int flags){
    if(flags!=MSG_DONTWAIT)throw std::runtime_error("blocking rejection drain");
    if(available_bytes){const auto count=std::min(available_bytes,size);available_bytes-=count;received_bytes+=count;return static_cast<int>(count);}
    errno=EAGAIN;return -1;
}
struct TlsLink{mbedtls_ssl_context ssl;int socket=31;bool server=true,configured=true,handshaken=false,transport_failed=false,incoming_failed=false;amity::system::OctopusId authenticated=*amity::system::OctopusId::parse("00000000000000000000000000000001");uint64_t timing_verify_ms=0;uint32_t timing_verify_calls=0;bool usable()const{return configured&&!transport_failed&&!incoming_failed;}bool handshake();};
void reset(){clock_ms=0;handshake_error=handshake_calls=verify_flags=alerts=flushes=shutdowns=fcntl_flags=0;available_bytes=received_bytes=0;blocked_alert=fcntl_failure=false;}
void check(bool value,const char*message){if(!value)throw std::runtime_error(message);}
'''
SUITE=r'''
int main(){try{
    {reset();TlsLink link;handshake_error=MBEDTLS_ERR_SSL_NO_CLIENT_CERTIFICATE;available_bytes=9000;
     check(!link.handshake(),"missing client cert gained authentication");check(handshake_calls==1&&alerts==1&&shutdowns==1,"missing cert did not send exact fatal alert and stop handshake");check(received_bytes==4096&&fcntl_flags==O_NONBLOCK&&clock_ms<=250,"rejection exceeded drain bounds");}
    {reset();TlsLink link;handshake_error=-0x2700;link.ssl.out_left=7;
     check(!link.handshake()&&alerts==0&&flushes==1,"existing SDK certificate alert was replaced or not flushed");check(clock_ms==250&&received_bytes==0,"empty receive drain exceeded250ms");}
    {reset();TlsLink link;handshake_error=MBEDTLS_ERR_SSL_NO_CLIENT_CERTIFICATE;blocked_alert=true;
     check(!link.handshake()&&handshake_calls==1&&clock_ms<=250,"blocked fatal alert resumed handshake or exceeded deadline");}
    {reset();TlsLink link;link.server=false;handshake_error=MBEDTLS_ERR_SSL_NO_CLIENT_CERTIFICATE;
     check(!link.handshake()&&alerts==0&&shutdowns==0,"server-only certificate response affected client path");}
    {reset();TlsLink link;handshake_error=MBEDTLS_ERR_SSL_NO_CLIENT_CERTIFICATE;fcntl_failure=true;
     check(!link.handshake()&&alerts==0&&shutdowns==0&&clock_ms==0,"failed nonblocking setup allowed potentially blocking reject path");}
    {reset();TlsLink link;check(link.handshake()&&alerts==0&&shutdowns==0,"valid authenticated TLS no longer succeeds");check(link.handshake()&&handshake_calls==1,"healthy TLS repeated its handshake");}
    {reset();TlsLink link;link.authenticated={};check(!link.handshake(),"unrecognized leaf gained authentication");}
    {reset();TlsLink link;verify_flags=1;check(!link.handshake(),"bad verification flags gained authentication");}
    std::cout<<"PASS actual TLS rejection/handshake: exact missing-cert alert, pending-alert flush, nonblocking4KiB/250ms drain, no auth bypass or handshake retry\n";
    return 0;
}catch(const std::exception&e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
'''


def main():
    source=(ROOT/'components/consortium/tls_link.cpp').read_text()
    actual=function(source,'void reject_handshake(mbedtls_ssl_context &ssl,')+function(source,'bool TlsLink::handshake()')
    with tempfile.TemporaryDirectory(prefix='amity-tls-reject-') as directory:
        cpp=Path(directory)/'test.cpp';binary=Path(directory)/'test';cpp.write_text(timing_support()+PREFIX+actual+SUITE)
        subprocess.run(shlex.split(os.environ.get('CXX','c++'))+['-std=c++17','-Wall','-Wextra','-Werror','-g','-fsanitize=address,undefined','-I'+str(ROOT/'components/consortium/include'),'-I'+str(ROOT/'components/amity_system/include'),str(cpp),'-o',str(binary)],check=True)
        return subprocess.run([str(binary)],check=False).returncode


if __name__=='__main__':raise SystemExit(main())
