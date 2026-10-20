// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#ifdef ESP_PLATFORM
#include "platform.hpp"
#include "mbedtls/ssl_ciphersuites.h"
#include "mbedtls/net_sockets.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cerrno>
#include <algorithm>
#include <cstring>
#include <fcntl.h>
namespace amity::consortium::platform {
namespace {
int send_socket(void *ctx,const uint8_t *data,size_t size){int fd=*static_cast<int*>(ctx);int n=::send(fd,data,size,0);if(n<0&&(errno==EWOULDBLOCK||errno==EAGAIN||errno==EINTR))return MBEDTLS_ERR_SSL_WANT_WRITE;return n<0?MBEDTLS_ERR_NET_SEND_FAILED:n;}
int receive_socket(void *ctx,uint8_t *data,size_t size){int fd=*static_cast<int*>(ctx);int n=::recv(fd,data,size,0);if(n<0&&(errno==EWOULDBLOCK||errno==EAGAIN||errno==EINTR))return MBEDTLS_ERR_SSL_WANT_READ;return n<0?MBEDTLS_ERR_NET_RECV_FAILED:n;}
void reject_handshake(mbedtls_ssl_context &ssl,int socket,bool server,int reason){
    if(!server||socket<0)return;
    const auto flags=::fcntl(socket,F_GETFL,0);
    if(flags<0||::fcntl(socket,F_SETFL,flags|O_NONBLOCK)<0)return;
    const auto deadline=now_ms()+250;
    if(reason==MBEDTLS_ERR_SSL_NO_CLIENT_CERTIFICATE){
        // This pinned SDK returns the exact missing-certificate error without
        // emitting an alert. Preserve VERIFY_REQUIRED and the fatal decision.
        mbedtls_ssl_send_alert_message(&ssl,MBEDTLS_SSL_ALERT_LEVEL_FATAL,MBEDTLS_SSL_ALERT_MSG_HANDSHAKE_FAILURE);
    }
    // The SDK may already have queued a certificate-specific alert. Its public
    // send-alert API flushes an outstanding record before creating another.
    while(ssl.MBEDTLS_PRIVATE(out_left)&&now_ms()<deadline){const int result=mbedtls_ssl_send_alert_message(&ssl,MBEDTLS_SSL_ALERT_LEVEL_FATAL,MBEDTLS_SSL_ALERT_MSG_HANDSHAKE_FAILURE);if(result!=MBEDTLS_ERR_SSL_WANT_WRITE)break;vTaskDelay(pdMS_TO_TICKS(2));}
    // A fatal certificate alert can otherwise be hidden by a TCP reset when
    // the already-sent client key-exchange flight remains unread. Half-close
    // output and discard a bounded amount only; no handshake resumes here.
    ::shutdown(socket,SHUT_WR);
    uint8_t bytes[256];size_t drained=0;
    while(drained<4096&&now_ms()<deadline){
        const int count=::recv(socket,bytes,std::min(sizeof(bytes),4096-drained),MSG_DONTWAIT);
        if(count>0){drained+=static_cast<size_t>(count);continue;}
        if(!count)break;
        if(errno!=EWOULDBLOCK&&errno!=EAGAIN&&errno!=EINTR)break;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}
}
TlsLink::TlsLink(){mbedtls_ssl_init(&ssl);mbedtls_ssl_config_init(&config);mbedtls_x509_crt_init(&own);mbedtls_x509_crt_init(&anchors);mbedtls_pk_init(&key);}
TlsLink::~TlsLink(){if(socket>=0){::shutdown(socket,SHUT_RDWR);::close(socket);}mbedtls_ssl_free(&ssl);mbedtls_ssl_config_free(&config);mbedtls_x509_crt_free(&own);mbedtls_x509_crt_free(&anchors);mbedtls_pk_free(&key);}
int TlsLink::verify(void *ctx,mbedtls_x509_crt *crt,int depth,uint32_t *flags){
    auto &self=*static_cast<TlsLink*>(ctx);
    if(depth!=0||crt->next){*flags|=MBEDTLS_X509_BADCERT_OTHER;return 0;}
    const std::string_view der(reinterpret_cast<const char*>(crt->raw.p),crt->raw.len);
    for(const auto &peer:self.allowed){if(der!=peer.der)continue;std::string pin;if(!certificate(der,peer.id,pin)||pin!=peer.pin){*flags|=MBEDTLS_X509_BADCERT_OTHER;return 0;}if(self.authenticated&&self.authenticated!=peer.id){*flags|=MBEDTLS_X509_BADCERT_OTHER;return 0;}self.authenticated=peer.id;*flags&=~uint32_t(MBEDTLS_X509_BADCERT_EXPIRED|MBEDTLS_X509_BADCERT_FUTURE);return 0;}
    *flags|=MBEDTLS_X509_BADCERT_OTHER;return 0;
}
bool TlsLink::setup(const Identity &identity,const std::vector<Peer> &peers,bool is_server,int fd,model::OctopusId expected){
    socket=fd;server=is_server;if(peers.empty()||peers.size()>7)return false;allowed=peers;authenticated=expected;
    if(mbedtls_x509_crt_parse_der(&own,reinterpret_cast<const uint8_t*>(identity.der.data()),identity.der.size())||mbedtls_pk_parse_key(&key,reinterpret_cast<const uint8_t*>(identity.key_der.data()),identity.key_der.size(),nullptr,0))return false;
    for(const auto &peer:allowed){std::string pin;if(!certificate(peer.der,peer.id,pin)||pin!=peer.pin||mbedtls_x509_crt_parse_der(&anchors,reinterpret_cast<const uint8_t*>(peer.der.data()),peer.der.size()))return false;}
    if(mbedtls_ssl_config_defaults(&config,is_server?MBEDTLS_SSL_IS_SERVER:MBEDTLS_SSL_IS_CLIENT,MBEDTLS_SSL_TRANSPORT_STREAM,MBEDTLS_SSL_PRESET_DEFAULT))return false;
    static const int suites[]={MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,0};
    mbedtls_ssl_conf_min_tls_version(&config,MBEDTLS_SSL_VERSION_TLS1_2);mbedtls_ssl_conf_max_tls_version(&config,MBEDTLS_SSL_VERSION_TLS1_2);mbedtls_ssl_conf_ciphersuites(&config,suites);mbedtls_ssl_conf_authmode(&config,MBEDTLS_SSL_VERIFY_REQUIRED);mbedtls_ssl_conf_ca_chain(&config,&anchors,nullptr);mbedtls_ssl_conf_verify(&config,verify,this);
#if defined(MBEDTLS_SSL_SESSION_TICKETS)
    mbedtls_ssl_conf_session_tickets(&config,MBEDTLS_SSL_SESSION_TICKETS_DISABLED);
#endif
    static const char *profiles[]={"amity-consortium-v1.hardware","amity-consortium-v1.extended",nullptr};
    if(mbedtls_ssl_conf_alpn_protocols(&config,profiles))return false;
    if(mbedtls_ssl_conf_own_cert(&config,&own,&key)||mbedtls_ssl_setup(&ssl,&config))return false;
    if(!is_server){if(!expected)return false;const std::string dns="o-"+expected.str()+".amity.invalid";if(mbedtls_ssl_set_hostname(&ssl,dns.c_str()))return false;}
    struct timeval timeout{0,100000};setsockopt(socket,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));setsockopt(socket,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));mbedtls_ssl_set_bio(&ssl,&socket,send_socket,receive_socket,nullptr);return true;
}
bool TlsLink::handshake(){const auto deadline=now_ms()+10000;while(now_ms()<deadline){int r=mbedtls_ssl_handshake(&ssl);if(!r)return authenticated&&mbedtls_ssl_get_verify_result(&ssl)==0;if(r!=MBEDTLS_ERR_SSL_WANT_READ&&r!=MBEDTLS_ERR_SSL_WANT_WRITE){reject_handshake(ssl,socket,server,r);return false;}vTaskDelay(pdMS_TO_TICKS(2));}return false;}
bool TlsLink::extended_profile()const{const auto *profile=mbedtls_ssl_get_alpn_protocol(&ssl);return profile&&(strcmp(profile,"amity-consortium-v1.extended")==0||strcmp(profile,"amity-consortium-v1.hardware")==0);}
bool TlsLink::hardware_profile()const{const auto *profile=mbedtls_ssl_get_alpn_protocol(&ssl);return profile&&strcmp(profile,"amity-consortium-v1.hardware")==0;}
bool TlsLink::send(std::string_view body){auto bytes=frame(body);if(bytes.empty())return false;size_t offset=0;const auto deadline=now_ms()+3000;while(offset<bytes.size()&&now_ms()<deadline){int n=mbedtls_ssl_write(&ssl,bytes.data()+offset,bytes.size()-offset);if(n>0)offset+=n;else if(n!=MBEDTLS_ERR_SSL_WANT_READ&&n!=MBEDTLS_ERR_SSL_WANT_WRITE)return false;else vTaskDelay(pdMS_TO_TICKS(2));}return offset==bytes.size();}
bool TlsLink::receive(std::string &out,uint32_t duration){FrameReader reader;uint8_t bytes[512];size_t expected=4;std::string packet;const auto deadline=now_ms()+duration;while(now_ms()<deadline){size_t request=expected-packet.size();if(request>sizeof(bytes))request=sizeof(bytes);int n=mbedtls_ssl_read(&ssl,bytes,request);if(n>0){packet.append(reinterpret_cast<const char*>(bytes),n);if(packet.size()==4&&expected==4){uint32_t len=(uint32_t(uint8_t(packet[0]))<<24)|(uint32_t(uint8_t(packet[1]))<<16)|(uint32_t(uint8_t(packet[2]))<<8)|uint8_t(packet[3]);if(!len||len>max_frame_bytes)return false;expected=4+len;}size_t used=0;auto result=reader.feed(bytes,n,used,now_ms());if(result==FrameReader::Result::frame){out=std::string(reader.body());return true;}if(result!=FrameReader::Result::incomplete)return false;}else if(n!=MBEDTLS_ERR_SSL_WANT_READ&&n!=MBEDTLS_ERR_SSL_WANT_WRITE)return false;else vTaskDelay(pdMS_TO_TICKS(2));if(reader.check_deadline(now_ms())==FrameReader::Result::timed_out)return false;}return false;}
std::string envelope_json(const Envelope &e,std::string_view payload){return "{\"protocol_version\":1,\"consortium_id\":\""+e.consortium_id.str()+"\",\"message_id\":\""+e.message_id.str()+"\",\"origin_octopus_id\":\""+e.origin_octopus_id.str()+"\",\"origin_boot_id\":\""+e.origin_boot_id.str()+"\",\"destination_octopus_id\":\""+e.destination_octopus_id.str()+"\",\"destination_boot_id\":\""+e.destination_boot_id.str()+"\",\"coordinator_epoch\":\""+std::to_string(e.coordinator_epoch)+"\",\"config_revision\":\""+std::to_string(e.config_revision)+"\",\"kind\":\""+kind_name(e.kind)+"\",\"ttl_ms\":"+std::to_string(e.ttl_ms)+",\"payload\":"+std::string(payload)+"}";}
} // namespace
#endif
