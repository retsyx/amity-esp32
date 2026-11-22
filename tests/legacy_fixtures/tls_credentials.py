"""Credential and TLS fixture for the tests that link the pinned Mbed TLS.

PREFIX holds the includes and the NVS, random-fill and task-clock seams. HELPERS makes
identities and rosters and runs handshakes over loopback TCP.
"""
PREFIX = r'''
#define ESP_PLATFORM
#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
#include "platform.hpp"
#include "amity/consortium/hostname.hpp"
#include "mbedtls/ssl_ciphersuites.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/oid.h"
#include "psa/crypto.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#ifdef __APPLE__
#define TCP_KEEPIDLE TCP_KEEPALIVE
#endif
#include <unistd.h>
#include <fcntl.h>
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
constexpr int ESP_ERR_NVS_NOT_FOUND=22;
namespace amity::consortium::platform {
uint64_t clock_ms=0;unsigned full_checks=0;
uint64_t now_ms(){return clock_ms;}
unsigned pdMS_TO_TICKS(unsigned n){return n/10;}
void vTaskDelay(unsigned ticks){clock_ms+=std::max(1u,ticks)*10;}
void wait_socket(int,bool,uint64_t deadline){clock_ms=std::min<uint64_t>(deadline,clock_ms+10);} // Scripted readiness: one tick per wait.
void esp_fill_random(void* bytes,size_t size){assert(psa_generate_random(static_cast<uint8_t*>(bytes),size)==PSA_SUCCESS);}
std::map<std::string,std::string> records;
esp_err_t read_record(const char* space,const char* key,std::string& out,size_t cap){auto it=records.find(std::string(space)+"/"+key);if(it==records.end())return ESP_ERR_NVS_NOT_FOUND;if(it->second.size()>cap)return ESP_ERR_INVALID_SIZE;out=it->second;return ESP_OK;}
esp_err_t write_record(const char* space,const char* key,std::string_view value){records[std::string(space)+"/"+key]=value;return ESP_OK;}
'''
HELPERS = r'''
template<class T>T ident(unsigned n){char text[33];snprintf(text,sizeof(text),"%032x",n);return *T::parse(text);}
void check(bool good,const char* why){if(!good)throw std::runtime_error(why);}
Peer peer(const Identity& identity){return {identity.id,identity.der,identity.spki,true};}
Identity make_identity(unsigned n){records.clear();Identity identity;check(load_identity(ident<model::OctopusId>(n),identity,false)!=ESP_OK&&records.empty(),"missing identity was generated without create");check(load_identity(ident<model::OctopusId>(n),identity,true)==ESP_OK,"real credential generation/validation failed");return identity;}
std::string replace(std::string value,std::string_view before,std::string_view after){const auto at=value.find(before);check(at!=std::string::npos,"missing fixture field");value.replace(at,before.size(),after);return value;}
std::string group_record(const Identity& authority,const Identity& member){
 Group group;group.role=Group::coordinator;group.consortium=ident<model::ConsortiumId>(10);group.authority=authority.id;group.revision=1;group.peers.push_back(peer(member));records.clear();check(save_group(group)==ESP_OK,"save_group fixture failed");return records.at("amity_group/state");
}
bool exchange(TlsLink& server,TlsLink& client){
 bool s=false,c=false;for(unsigned i=0;i<2000;++i){
  if(!s){const auto r=mbedtls_ssl_handshake(&server.ssl);if(!r)s=true;else if(r!=MBEDTLS_ERR_SSL_WANT_READ&&r!=MBEDTLS_ERR_SSL_WANT_WRITE)return false;}
  if(!c){const auto r=mbedtls_ssl_handshake(&client.ssl);if(!r)c=true;else if(r!=MBEDTLS_ERR_SSL_WANT_READ&&r!=MBEDTLS_ERR_SSL_WANT_WRITE)return false;}
  if(s&&c)return server.handshake()&&client.handshake();
  ++clock_ms;
 }return false;
}
void tcp_pair(int (&fd)[2]){
 int listener=::socket(AF_INET,SOCK_STREAM,0);check(listener>=0,"TCP listener failed");
 sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
 if(::bind(listener,reinterpret_cast<sockaddr*>(&address),sizeof(address))||::listen(listener,1)){::close(listener);throw std::runtime_error("TCP loopback listen failed");}
 socklen_t length=sizeof(address);check(!::getsockname(listener,reinterpret_cast<sockaddr*>(&address),&length),"TCP listener address failed");
 fd[1]=::socket(AF_INET,SOCK_STREAM,0);check(fd[1]>=0&&!::connect(fd[1],reinterpret_cast<sockaddr*>(&address),length),"TCP loopback connect failed");
 fd[0]=::accept(listener,nullptr,nullptr);::close(listener);check(fd[0]>=0,"TCP loopback accept failed");
}
'''
