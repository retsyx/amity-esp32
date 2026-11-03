#!/usr/bin/env python3
"""Exercise actual pinned-Mbed-TLS setup/framing over native loopback TCP."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

import consortium_tls_credentials_test as credentials_test
from consortium_gatt_callback_test import function
from setup_timing_support import timing_support

ROOT = Path(__file__).resolve().parents[1]
SUITE = r'''
void real_tcp_records(const Identity& main,const Identity& member){
 int fd[2];tcp_pair(fd);TlsLink server,client;
 check(server.setup(main,{peer(member)},true,fd[0])&&client.setup(member,{peer(main)},false,fd[1],main.id),"real TCP setup failed");
 for(int socket:fd){
  int value=0;socklen_t size=sizeof(value);
  check(!::getsockopt(socket,IPPROTO_TCP,TCP_NODELAY,&value,&size)&&value!=0,"TLS socket retained Nagle coalescing");
  check(!::getsockopt(socket,SOL_SOCKET,SO_KEEPALIVE,&value,&size)&&value!=0,"TLS socket did not enable TCP keepalive");
  check(!::getsockopt(socket,IPPROTO_TCP,TCP_KEEPIDLE,&value,&size)&&value==10,"TLS socket keepalive idle delay changed");
  check(!::getsockopt(socket,IPPROTO_TCP,TCP_KEEPINTVL,&value,&size)&&value==3,"TLS socket keepalive probe interval changed");
  check(!::getsockopt(socket,IPPROTO_TCP,TCP_KEEPCNT,&value,&size)&&value==3,"TLS socket keepalive probe count changed");
 }
 bool sh=false,ch=false;const auto deadline=now_ms()+5000;
 while(now_ms()<deadline&&!(sh&&ch)){
  if(!sh){const auto r=mbedtls_ssl_handshake(&server.ssl);if(!r)sh=true;else check(r==MBEDTLS_ERR_SSL_WANT_READ||r==MBEDTLS_ERR_SSL_WANT_WRITE,"server handshake failed");}
  if(!ch){const auto r=mbedtls_ssl_handshake(&client.ssl);if(!r)ch=true;else check(r==MBEDTLS_ERR_SSL_WANT_READ||r==MBEDTLS_ERR_SSL_WANT_WRITE,"client handshake failed");}
  std::this_thread::sleep_for(std::chrono::milliseconds(1));
 }
 check(sh&&ch&&server.handshake()&&client.handshake(),"mutual TCP handshake failed");
 check(server.authenticated==member.id&&client.authenticated==main.id&&!mbedtls_ssl_get_verify_result(&server.ssl)&&!mbedtls_ssl_get_verify_result(&client.ssl),"TCP lost exact mutual peer authentication");
 uint64_t slowest=0;unsigned frames=0;
 // The four-byte prefix puts 508/509-byte bodies on opposite sides of the
 // production 512-byte TLS-record boundary. Both directions reuse one stream.
 for(unsigned bytes:{64u,508u,509u,800u,2048u})for(bool reverse:{false,true}){
  auto& sender=reverse?server:client;auto& receiver=reverse?client:server;
  const std::string body(bytes,reverse?'s':'c');const auto began=now_ms();check(sender.send(body),"TCP frame send failed");
  std::string received;auto result=TlsLink::ReceiveResult::pending;
  while(now_ms()-began<2000&&(result=receiver.poll_receive(received))==TlsLink::ReceiveResult::pending)std::this_thread::sleep_for(std::chrono::milliseconds(1));
  check(result==TlsLink::ReceiveResult::frame&&received==body,"TCP split records did not yield one exact frame");
  check(server.established()&&client.established(),"frame exchange lost the authenticated transport");
  slowest=std::max(slowest,now_ms()-began);++frames;
 }
 std::cout<<"PASS actual pinned-Mbed-TLS TCP: checked TCP_NODELAY and TCP keepalive enabled with 10s idle/3s interval/3 probes on both peers, exact mutual authentication, "<<frames<<" framed exchanges across 512-byte records on the same stream; native maximum="<<slowest<<"ms\n";
}
void unsupported_socket_fails_setup(const Identity& main,const Identity& member){
 int fd[2];check(!::socketpair(AF_UNIX,SOCK_STREAM,0,fd),"local socketpair failed");
 TlsLink link;check(!link.setup(main,{peer(member)},true,fd[0])&&!link.usable(),"TCP_NODELAY failure was accepted as configured");::close(fd[1]);
}
}
int main(){using namespace amity::consortium::platform;try{
 check(psa_crypto_init()==PSA_SUCCESS,"PSA initialization failed");const auto main=make_identity(1),member=make_identity(2);
 unsupported_socket_fails_setup(main,member);real_tcp_records(main,member);return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
'''


def main():
    source, build = credentials_test.native_library()
    credentials = (ROOT / "components/consortium/credentials.cpp").read_text()
    tls = (ROOT / "components/consortium/tls_link.cpp").read_text()
    prefix = credentials_test.PREFIX.replace("#include <sys/socket.h>",
                                            "#include <sys/socket.h>\n#include <chrono>\n#include <thread>")
    prefix = prefix.replace("uint64_t now_ms(){return clock_ms;}",
                            "uint64_t now_ms(){return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}")
    prefix = prefix.replace("void vTaskDelay(unsigned ticks){clock_ms+=std::max(1u,ticks)*10;}",
                            "void vTaskDelay(unsigned ticks){std::this_thread::sleep_for(std::chrono::milliseconds(std::max(1u,ticks)*10));}")
    actual = "}\nnamespace amity::consortium {\n" + function(
        (ROOT / "components/consortium/wire.cpp").read_text(), "bool decimal(")
    actual += "}\nnamespace amity::consortium::platform {\n"
    actual += "".join(function(credentials, name) for name in (
        "std::string hex(", "bool unhex(", "std::string sha256(", "bool certificate_binding(",
        "bool parse_certificate(", "bool decode_identity(", "bool certificate(",
        "esp_err_t load_identity(", "esp_err_t load_group(", "esp_err_t save_group("))
    actual += "".join(function(tls, name) for name in (
        "int send_socket(", "int receive_socket(", "void reject_handshake(",
        "TlsLink::TlsLink()", "TlsLink::~TlsLink()", "int TlsLink::verify(", "bool TlsLink::setup(",
        "bool TlsLink::handshake(", "bool TlsLink::send(", "TlsLink::ReceiveResult TlsLink::receive_failed(",
        "TlsLink::ReceiveResult TlsLink::receive_expired(", "TlsLink::ReceiveResult TlsLink::poll_receive("))
    helpers = credentials_test.SUITE[:credentials_test.SUITE.index("bool connect(")]
    with tempfile.TemporaryDirectory(prefix="amity-tls-tcp-") as directory:
        cpp, binary = Path(directory) / "test.cpp", Path(directory) / "test"
        cpp.write_text(timing_support() + prefix + actual + helpers + SUITE)
        include = [ROOT / "components/consortium", ROOT / "components/consortium/include",
                   ROOT / "components/amity_system/include", ROOT / "tests/reset_fakes", source / "include",
                   source / "tf-psa-crypto/include", source / "tf-psa-crypto/drivers/builtin/include",
                   build / "tf-psa-crypto/include"]
        command = shlex.split(os.environ.get("CXX", "c++")) + [
            "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-misleading-indentation", "-g",
            "-fsanitize=address,undefined", *["-I" + str(path) for path in include], str(cpp),
            str(ROOT / "components/consortium/json.cpp"), str(ROOT / "components/amity_system/model.cpp"),
            *[str(build / "library" / name) for name in ("libmbedtls.a", "libmbedx509.a", "libtfpsacrypto.a")],
            "-o", str(binary)]
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
