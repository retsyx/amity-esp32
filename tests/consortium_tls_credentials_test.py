#!/usr/bin/env python3
"""Run actual credential ingress/TLS code against the pinned Mbed TLS library.

The native library uses its upstream software crypto configuration, not ESP32
accelerators. NVS, random-fill and task clock are host seams; certificate parsing,
self signatures, key pairing, mutual TLS and the firmware callbacks are real.
"""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
from consortium_gatt_callback_test import function
from setup_timing_support import timing_support

ROOT=Path(__file__).resolve().parents[1]
IDF=Path(os.environ.get('IDF_PATH',Path.home()/'.espressif/v6.1/esp-idf'))
PREFIX=r'''
#define ESP_PLATFORM
#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
#include "platform.hpp"
#include "amity/consortium/hostname.hpp"
#include "mbedtls/ssl_ciphersuites.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/oid.h"
#include "psa/crypto.h"
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <map>
#include <stdexcept>
constexpr int ESP_ERR_NVS_NOT_FOUND=22;
namespace amity::consortium::platform {
uint64_t clock_ms=0;unsigned full_checks=0;
#ifndef OBSERVE_REDUNDANT_VALIDATION
#define OBSERVE_REDUNDANT_VALIDATION false
#endif
uint64_t now_ms(){return clock_ms;}
unsigned pdMS_TO_TICKS(unsigned n){return n/10;}
void vTaskDelay(unsigned ticks){clock_ms+=std::max(1u,ticks)*10;}
void esp_fill_random(void* bytes,size_t size){assert(psa_generate_random(static_cast<uint8_t*>(bytes),size)==PSA_SUCCESS);}
std::map<std::string,std::string> records;
esp_err_t read_record(const char* space,const char* key,std::string& out,size_t cap){auto it=records.find(std::string(space)+"/"+key);if(it==records.end())return ESP_ERR_NVS_NOT_FOUND;if(it->second.size()>cap)return ESP_ERR_INVALID_SIZE;out=it->second;return ESP_OK;}
esp_err_t write_record(const char* space,const char* key,std::string_view value){records[std::string(space)+"/"+key]=value;return ESP_OK;}
'''
SUITE=r'''
template<class T>T ident(unsigned n){char text[33];snprintf(text,sizeof(text),"%032x",n);return *T::parse(text);}
void check(bool good,const char* why){if(!good)throw std::runtime_error(why);}
Peer peer(const Identity& identity){return {identity.id,identity.der,identity.spki,true};}
Identity make_identity(unsigned n){records.clear();Identity identity;check(load_identity(ident<model::OctopusId>(n),identity)==ESP_OK,"real credential generation/validation failed");return identity;}
std::string replace(std::string value,std::string_view before,std::string_view after){const auto at=value.find(before);check(at!=std::string::npos,"missing fixture field");value.replace(at,before.size(),after);return value;}
std::string group_record(const Identity& authority,const Identity& member){
 Group group;group.role=Group::coordinator;group.consortium=ident<model::ConsortiumId>(10);group.authority=authority.id;group.epoch=group.revision=1;group.peers.push_back(peer(member));records.clear();check(save_group(group)==ESP_OK,"save_group fixture failed");return records.at("amity_group/state");
}
bool exchange(TlsLink& server,TlsLink& client){
 bool s=false,c=false;for(unsigned i=0;i<2000;++i){
  if(!s){const auto r=mbedtls_ssl_handshake(&server.ssl);if(!r)s=true;else if(r!=MBEDTLS_ERR_SSL_WANT_READ&&r!=MBEDTLS_ERR_SSL_WANT_WRITE)return false;}
  if(!c){const auto r=mbedtls_ssl_handshake(&client.ssl);if(!r)c=true;else if(r!=MBEDTLS_ERR_SSL_WANT_READ&&r!=MBEDTLS_ERR_SSL_WANT_WRITE)return false;}
  if(s&&c)return server.authenticated&&client.authenticated&&!mbedtls_ssl_get_verify_result(&server.ssl)&&!mbedtls_ssl_get_verify_result(&client.ssl);
  ++clock_ms;
 }return false;
}
bool connect(const Identity& authority,const Identity& member,const std::vector<Peer>& permitted,model::OctopusId expected={}){
 int fd[2];check(!socketpair(AF_UNIX,SOCK_STREAM,0,fd),"socketpair failed");TlsLink server,client;
 if(!server.setup(authority,permitted,true,fd[0],expected)){::close(fd[1]);return false;}
 if(!client.setup(member,{peer(authority)},false,fd[1],authority.id))return false;
 check(server.config.MBEDTLS_PRIVATE(authmode)==MBEDTLS_SSL_VERIFY_REQUIRED&&client.config.MBEDTLS_PRIVATE(authmode)==MBEDTLS_SSL_VERIFY_REQUIRED,"mutual TLS authentication requirement changed");
 return exchange(server,client);
}
void roster_capacity(const Identity& authority,const Identity& member,const Identity& second,const Identity& third){
 Group group;group.role=Group::coordinator;group.consortium=ident<model::ConsortiumId>(10);group.authority=authority.id;group.epoch=group.revision=1;
 group.peers={peer(member),peer(second),peer(third)};records.clear();
 check(save_group(group)==ESP_OK,"three-secondary roster could not be saved");Group restored;
 check(load_group(restored)==ESP_OK&&restored.peers.size()==3,"three-secondary roster did not survive reload");
 for(const auto& allowed:group.peers){
  const auto& identity=allowed.id==member.id?member:allowed.id==second.id?second:third;
  check(connect(authority,identity,restored.peers),"a permitted secondary could not authenticate with a full roster");
 }
 group.peers.push_back(peer(authority));const auto committed=records.at("amity_group/state");
 check(save_group(group)==ESP_ERR_INVALID_ARG&&records.at("amity_group/state")==committed,"fourth secondary replaced committed membership");
 TlsLink oversized;check(!oversized.setup(authority,group.peers,true,-1),"four-secondary TLS allowlist admitted");
 std::cout<<"PASS actual credentials/TLS capacity: all three secondary peers persist and authenticate; fourth rejected\n";
}
void binding_and_callback(const Identity& authority,const Identity& member,const Identity& outsider){
 mbedtls_x509_crt crt;mbedtls_x509_crt_init(&crt);check(!mbedtls_x509_crt_parse_der(&crt,reinterpret_cast<const uint8_t*>(member.der.data()),member.der.size()),"fixture parse failed");std::string pin;
 check(certificate_binding(crt,member.id,pin)&&pin==member.spki,"valid profile binding changed");
 for(auto mutation:{0,1,2,3}){const auto version=crt.version;const auto extensions=crt.MBEDTLS_PRIVATE(ext_types);const auto usage=crt.MBEDTLS_PRIVATE(key_usage);const auto ca=crt.MBEDTLS_PRIVATE(ca_istrue);const auto md=crt.MBEDTLS_PRIVATE(sig_md);
  if(mutation==0)crt.MBEDTLS_PRIVATE(ca_istrue)=1;if(mutation==1)crt.MBEDTLS_PRIVATE(key_usage)=0;if(mutation==2)crt.MBEDTLS_PRIVATE(ext_types)=0;if(mutation==3)crt.MBEDTLS_PRIVATE(sig_md)=MBEDTLS_MD_SHA384;
  check(!certificate_binding(crt,member.id,pin),"leaf profile constraint lost");crt.version=version;crt.MBEDTLS_PRIVATE(ext_types)=extensions;crt.MBEDTLS_PRIVATE(key_usage)=usage;crt.MBEDTLS_PRIVATE(ca_istrue)=ca;crt.MBEDTLS_PRIVATE(sig_md)=md;
 }
 TlsLink link;link.allowed={peer(member)};uint32_t flags=MBEDTLS_X509_BADCERT_EXPIRED|MBEDTLS_X509_BADCERT_FUTURE;
 full_checks=0;TlsLink::verify(&link,&crt,0,&flags);std::cout<<"trusted_verify_full_validations="<<full_checks<<"\n";check(!flags&&link.authenticated==member.id&&(full_checks==0||OBSERVE_REDUNDANT_VALIDATION),"trusted callback repeated full validation or lost clock policy");
 for(const auto initial:{uint32_t(MBEDTLS_X509_BADCERT_BAD_KEY),uint32_t(MBEDTLS_X509_BADCERT_NOT_TRUSTED),uint32_t(MBEDTLS_X509_BADCERT_CN_MISMATCH)}){
  flags=initial|MBEDTLS_X509_BADCERT_EXPIRED;TlsLink::verify(&link,&crt,0,&flags);check(flags==initial,"callback erased non-clock verification failure");
 }
 flags=0;TlsLink::verify(&link,&crt,1,&flags);check(flags&MBEDTLS_X509_BADCERT_OTHER,"intermediate certificate admitted");
 crt.next=&crt;flags=0;TlsLink::verify(&link,&crt,0,&flags);check(flags&MBEDTLS_X509_BADCERT_OTHER,"chain admitted");crt.next=nullptr;
 link.authenticated=authority.id;flags=0;TlsLink::verify(&link,&crt,0,&flags);check(flags&MBEDTLS_X509_BADCERT_OTHER,"unexpected peer identity admitted");
 link.authenticated={};link.allowed={peer(outsider)};flags=0;TlsLink::verify(&link,&crt,0,&flags);check(flags&MBEDTLS_X509_BADCERT_OTHER,"unknown exact DER admitted");
 mbedtls_x509_crt_free(&crt);
 for(auto invalid:{peer(member),peer(member),peer(member)}){static unsigned scenario=0;if(scenario==0)invalid.pin=std::string(64,'f');if(scenario==1)invalid.id=outsider.id;if(scenario==2)invalid.der="not DER";++scenario;TlsLink invalid_link;check(!invalid_link.setup(authority,{invalid},true,-1),"setup accepted mismatched id/pin/DER");}
}
}
int main(){using namespace amity::consortium;using namespace amity::consortium::platform;try{
 check(psa_crypto_init()==PSA_SUCCESS,"PSA init failed");const auto authority=make_identity(1),member=make_identity(2),outsider=make_identity(3);
 const auto clean=group_record(authority,member);Group restored;full_checks=0;check(load_group(restored)==ESP_OK&&full_checks==1,"stored peer bypassed full validation");
 auto damaged=member.der;damaged.back()^=1;std::string pin;check(!certificate(damaged,member.id,pin),"invalid untrusted self-signature accepted");
 for(const auto& invalid:{replace(clean,hex(member.der),hex(damaged)),replace(clean,member.spki,std::string(64,'f')),replace(clean,member.id.str(),ident<model::OctopusId>(99).str()),replace(clean,hex(member.der),"00")}){records["amity_group/state"]=invalid;check(load_group(restored)!=ESP_OK,"damaged stored certificate/id/pin admitted");}
 check(!certificate(member.der,authority.id,pin),"wrong untrusted SAN identity admitted");
 const auto identity_record="{\"version\":1,\"octopus_id\":\""+member.id.str()+"\",\"leaf\":\""+hex(member.der)+"\",\"key\":\""+hex(member.key_der)+"\"}";
 Identity decoded;check(decode_identity(identity_record,member.id,decoded),"valid identity record rejected");
 check(!decode_identity(replace(identity_record,hex(member.key_der),hex(outsider.key_der)),member.id,decoded),"mismatched boot private key accepted");
 check(!decode_identity(replace(identity_record,hex(member.der),hex(damaged)),member.id,decoded),"invalid boot self-signature accepted");
 binding_and_callback(authority,member,outsider);
 roster_capacity(authority,member,outsider,make_identity(4));
 full_checks=0;check(connect(authority,member,{peer(member)}),"valid real mutual TLS failed");std::cout<<"mutual_tls_full_validations="<<full_checks<<"\n";check(full_checks==0||OBSERVE_REDUNDANT_VALIDATION,"TLS setup/verify repeated full trusted leaf signatures");
 // A changed certificate with the same stable ID is still an unknown leaf.
 const auto changed_member=make_identity(2);check(!connect(authority,changed_member,{peer(member)}),"unlisted same-ID certificate completed TLS");
 int fds[2];check(!socketpair(AF_UNIX,SOCK_STREAM,0,fds),"snapshot socketpair failed");
 {TlsLink server,client;std::vector<Peer> original{peer(member)};check(server.setup(authority,original,true,fds[0]),"snapshot server setup failed");original[0]=peer(outsider);
  check(client.setup(member,{peer(authority)},false,fds[1],authority.id)&&exchange(server,client),"mutable source roster changed captured TLS allowlist");}
 check(!connect(authority,outsider,{peer(member)}),"unknown peer completed mutual TLS");
 check(!connect(authority,member,{peer(member)},outsider.id),"unexpected peer completed mutual TLS");
 auto wrong_key=member;wrong_key.key_der=outsider.key_der;check(!connect(authority,wrong_key,{peer(member)}),"peer without matching private key completed TLS");
 std::cout<<"PASS actual pinned-Mbed-TLS credential ingress/setup/verify: full untrusted signature validation, profile/SAN/EKU/SPKI binding, exact DER/identity allowlist, preserved SDK flags and mutual key possession\n";
 return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
'''
# Keep the real generation/parser functions while substituting only durable storage.

def native_library():
    source=IDF/'components/mbedtls/mbedtls'
    build=ROOT/'test-results/host/consortium-mbedtls'
    shim=build/'host-include/mbedtls';shim.mkdir(parents=True,exist_ok=True)
    # Espressif source includes public compatibility wrappers during the native
    # software build. The wrappers only expose the same upstream private types.
    for name in ('bignum','ecp'):
        header=shim/(name+'.h');content='#include "mbedtls/private/'+name+'.h"\n'
        if not header.exists() or header.read_text()!=content:header.write_text(content)
    configure=['cmake','-S',str(source),'-B',str(build),'-DGEN_FILES=OFF','-DENABLE_PROGRAMS=OFF','-DENABLE_TESTING=OFF','-DCMAKE_BUILD_TYPE=Debug','-DCMAKE_C_FLAGS=-I'+str(shim.parent)]
    for command in (configure,['cmake','--build',str(build),'-j','8']):
        result=subprocess.run(command,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
        if result.returncode:
            print(result.stdout);result.check_returncode()
    return source,build

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--tls-source',type=Path,default=ROOT/'components/consortium/tls_link.cpp');parser.add_argument('--observe-redundant-validation',action='store_true',help='Record the old validation count for before/after evidence while keeping all security assertions');args=parser.parse_args()
    source,build=native_library();credentials=(ROOT/'components/consortium/credentials.cpp').read_text();tls=args.tls_source.read_text()
    actual='}\nnamespace amity::consortium {\n'+function((ROOT/'components/consortium/wire.cpp').read_text(),'bool decimal(')+'}\nnamespace amity::consortium::platform {\n'+''.join(function(credentials,name) for name in ['std::string hex(', 'bool unhex(', 'std::string sha256(', 'bool certificate_binding(', 'bool parse_certificate(', 'bool decode_identity(', 'bool certificate(', 'esp_err_t load_identity(', 'esp_err_t load_group(', 'esp_err_t save_group('])
    actual=actual.replace('bool certificate(std::string_view der,model::OctopusId id,std::string &pin){','bool certificate(std::string_view der,model::OctopusId id,std::string &pin){++full_checks;')
    actual+=''.join(function(tls,name) for name in ['int send_socket(', 'int receive_socket(', 'TlsLink::TlsLink()', 'TlsLink::~TlsLink()', 'int TlsLink::verify(', 'bool TlsLink::setup('])
    suite=SUITE
    with tempfile.TemporaryDirectory(prefix='amity-tls-credentials-') as directory:
        cpp,binary=Path(directory)/'test.cpp',Path(directory)/'test';cpp.write_text(timing_support()+PREFIX+actual+suite)
        include=[ROOT/'components/consortium',ROOT/'components/consortium/include',ROOT/'components/amity_system/include',ROOT/'tests/reset_fakes',source/'include',source/'tf-psa-crypto/include',source/'tf-psa-crypto/drivers/builtin/include',build/'tf-psa-crypto/include']
        command=shlex.split(os.environ.get('CXX','c++'))+['-std=c++17','-DOBSERVE_REDUNDANT_VALIDATION='+str(args.observe_redundant_validation).lower(),'-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-g','-fsanitize=address,undefined',*['-I'+str(path)for path in include],str(cpp),str(ROOT/'components/consortium/json.cpp'),str(ROOT/'components/amity_system/model.cpp'),*[str(build/'library'/name)for name in ['libmbedtls.a','libmbedx509.a','libtfpsacrypto.a']],'-o',str(binary)]
        subprocess.run(command,check=True);return subprocess.run([str(binary)],check=False).returncode
if __name__=='__main__':raise SystemExit(main())
