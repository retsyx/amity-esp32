// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#ifdef ESP_PLATFORM
#include "platform.hpp"
#include "mbedtls/ssl_ciphersuites.h"
#include "mbedtls/net_sockets.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <cerrno>
#include <algorithm>
#include <cstring>
#include <fcntl.h>
namespace amity::consortium::platform {
void emit_setup_timing(const char *area,const SetupTimingEntry& entry,model::OctopusId peer,model::LinkId link,model::MessageId scan){
    if(scan){
        ESP_LOGI("setup_timing","area=%s stage=%s at_ms=%llu elapsed_ms=%llu result=%s scan=%s peer=%s code=%d",area,entry.stage,
            static_cast<unsigned long long>(entry.at_ms),static_cast<unsigned long long>(entry.elapsed_ms),entry.result,scan.str().c_str(),peer.str().c_str(),entry.code);
        return;
    }
    ESP_LOGI("setup_timing","area=%s stage=%s at_ms=%llu elapsed_ms=%llu result=%s peer=%s link=%s count=%u call_ms=%llu wait_ms=%llu code=%d metric=%s",area,entry.stage,
        static_cast<unsigned long long>(entry.at_ms),static_cast<unsigned long long>(entry.elapsed_ms),entry.result,peer.str().c_str(),link.str().c_str(),unsigned(entry.count),
        static_cast<unsigned long long>(entry.call_ms),static_cast<unsigned long long>(entry.wait_ms),entry.code,entry.summed?"sum":"span");
}
namespace {
int send_socket(void *ctx,const uint8_t *data,size_t size){int fd=*static_cast<int*>(ctx);int n=::send(fd,data,size,MSG_DONTWAIT);if(n<0&&(errno==EWOULDBLOCK||errno==EAGAIN||errno==EINTR))return MBEDTLS_ERR_SSL_WANT_WRITE;return n<0?MBEDTLS_ERR_NET_SEND_FAILED:n;}
int receive_socket(void *ctx,uint8_t *data,size_t size){int fd=*static_cast<int*>(ctx);int n=::recv(fd,data,size,MSG_DONTWAIT);if(n<0&&(errno==EWOULDBLOCK||errno==EAGAIN||errno==EINTR))return MBEDTLS_ERR_SSL_WANT_READ;return n<0?MBEDTLS_ERR_NET_RECV_FAILED:n;}
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
    while(ssl.MBEDTLS_PRIVATE(out_left)&&now_ms()<deadline){const int result=mbedtls_ssl_send_alert_message(&ssl,MBEDTLS_SSL_ALERT_LEVEL_FATAL,MBEDTLS_SSL_ALERT_MSG_HANDSHAKE_FAILURE);if(result!=MBEDTLS_ERR_SSL_WANT_WRITE)break;vTaskDelay(std::max<unsigned>(1,pdMS_TO_TICKS(2)));}
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
        vTaskDelay(std::max<unsigned>(1,pdMS_TO_TICKS(2)));
    }
}
}
TlsLink::TlsLink(){mbedtls_ssl_init(&ssl);mbedtls_ssl_config_init(&config);mbedtls_x509_crt_init(&own);mbedtls_x509_crt_init(&anchors);mbedtls_pk_init(&key);}
TlsLink::~TlsLink(){if(socket>=0){::shutdown(socket,SHUT_RDWR);::close(socket);}mbedtls_ssl_free(&ssl);mbedtls_ssl_config_free(&config);mbedtls_x509_crt_free(&own);mbedtls_x509_crt_free(&anchors);mbedtls_pk_free(&key);}
int TlsLink::verify(void *ctx,mbedtls_x509_crt *crt,int depth,uint32_t *flags){
    auto &self=*static_cast<TlsLink*>(ctx);
    struct TimedVerify{TlsLink& link;uint64_t began;~TimedVerify(){++link.timing_verify_calls;link.timing_verify_ms+=now_ms()-began;}} timed{self,now_ms()};
    if(depth!=0||crt->next){*flags|=MBEDTLS_X509_BADCERT_OTHER;return 0;}
    const std::string_view der(reinterpret_cast<const char*>(crt->raw.p),crt->raw.len);
    // setup captured the exact validated membership leaf and bound its SPKI
    // pin/profile. The standard TLS verifier and CertificateVerify still run;
    // repeating the same leaf's self-signatures here adds no authentication.
    for(const auto &peer:self.allowed){if(der!=peer.der)continue;if(self.authenticated&&self.authenticated!=peer.id){*flags|=MBEDTLS_X509_BADCERT_OTHER;return 0;}self.authenticated=peer.id;*flags&=~uint32_t(MBEDTLS_X509_BADCERT_EXPIRED|MBEDTLS_X509_BADCERT_FUTURE);return 0;}
    *flags|=MBEDTLS_X509_BADCERT_OTHER;return 0;
}
bool TlsLink::setup(const Identity &identity,const std::vector<Peer> &peers,bool is_server,int fd,model::OctopusId expected){
    if(configured||socket>=0)return false;
    BoundedSetupTimings<5> timing("tls_setup",now_ms);timing.peer=expected;timing.begin("total");
    socket=fd;server=is_server;if(peers.empty()||peers.size()>maximum_roster_peers)return false;allowed=peers;authenticated=expected;
    if(!timing.step("own_credentials",[&]{return !mbedtls_x509_crt_parse_der(&own,reinterpret_cast<const uint8_t*>(identity.der.data()),identity.der.size())&&!mbedtls_pk_parse_key(&key,reinterpret_cast<const uint8_t*>(identity.key_der.data()),identity.key_der.size(),nullptr,0);}))return timing.done(false);
    // The only callers copy Group peers, validated by load_group or the
    // authenticated bootstrap ingress. Retain local profile/id/pin binding on
    // these exact DER bytes, without redoing their verified self-signatures.
    uint64_t binding_ms=0,anchor_ms=0;uint32_t bindings=0,anchors_parsed=0;
    for(const auto &peer:allowed){
        auto began=now_ms();const auto parsed=mbedtls_x509_crt_parse_der(&anchors,reinterpret_cast<const uint8_t*>(peer.der.data()),peer.der.size());anchor_ms+=now_ms()-began;++anchors_parsed;
        if(parsed){timing.sum("peer_validation",binding_ms,bindings);timing.sum("anchor_parse",anchor_ms,anchors_parsed,false);return timing.done(false);}
        const auto *anchor=&anchors;while(anchor->next)anchor=anchor->next;
        std::string pin;began=now_ms();const bool valid=peer.der.size()<=1024&&certificate_binding(*anchor,peer.id,pin)&&pin==peer.pin;binding_ms+=now_ms()-began;++bindings;
        if(!valid){timing.sum("peer_validation",binding_ms,bindings,false);timing.sum("anchor_parse",anchor_ms,anchors_parsed);return timing.done(false);}
    }
    timing.sum("peer_validation",binding_ms,bindings);timing.sum("anchor_parse",anchor_ms,anchors_parsed);
    const auto configured=timing.begin("configure");
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
    // Application frames span short TLS records. Send each record immediately
    // instead of waiting for a delayed ACK to flush the rest of the frame.
    const int no_delay=1;
    if(setsockopt(socket,IPPROTO_TCP,TCP_NODELAY,&no_delay,sizeof(no_delay)))return timing.done(false);
    struct timeval timeout{0,100000};setsockopt(socket,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));setsockopt(socket,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));mbedtls_ssl_set_bio(&ssl,&socket,send_socket,receive_socket,nullptr);timing.finish(configured,true);this->configured=true;return timing.done(true);
}
bool TlsLink::handshake(){
    if(!usable())return false;
    if(handshaken)return true;
    BoundedSetupTimings<3> timing("tls_handshake",now_ms);const auto total=timing.begin("handshake");
    timing_verify_ms=0;timing_verify_calls=0;uint64_t call_ms=0,wait_ms=0;uint32_t calls=0;
    auto finished=[&](bool good,int code){handshaken=good;transport_failed=!good;timing.peer=authenticated;timing.finish(total,good,calls,call_ms,wait_ms,code);timing.sum("verify_callback",timing_verify_ms,timing_verify_calls,good);return good;};
    const auto deadline=now_ms()+10000;
    while(now_ms()<deadline){
        auto began=now_ms();const int r=mbedtls_ssl_handshake(&ssl);call_ms+=now_ms()-began;++calls;
        if(!r)return finished(authenticated&&mbedtls_ssl_get_verify_result(&ssl)==0,r);
        if(r!=MBEDTLS_ERR_SSL_WANT_READ&&r!=MBEDTLS_ERR_SSL_WANT_WRITE){const auto rejection=timing.begin("reject_alert");reject_handshake(ssl,socket,server,r);timing.finish(rejection,false,0,0,0,r);return finished(false,r);}
        began=now_ms();vTaskDelay(std::max<unsigned>(1,pdMS_TO_TICKS(2)));wait_ms+=now_ms()-began;
    }
    return finished(false,MBEDTLS_ERR_SSL_TIMEOUT);
}
bool TlsLink::extended_profile()const{const auto *profile=mbedtls_ssl_get_alpn_protocol(&ssl);return profile&&(strcmp(profile,"amity-consortium-v1.extended")==0||strcmp(profile,"amity-consortium-v1.hardware")==0);}
bool TlsLink::hardware_profile()const{const auto *profile=mbedtls_ssl_get_alpn_protocol(&ssl);return profile&&strcmp(profile,"amity-consortium-v1.hardware")==0;}
bool TlsLink::send(std::string_view body){
    auto bytes=frame(body);if(bytes.empty())return false;
    if(!handshake())return false;
    size_t offset=0;const auto deadline=now_ms()+3000;
    // Bound PSRAM-to-DMA AES bounce allocations for application data without
    // reducing the TLS buffers used by certificates or incoming records.
    while(offset<bytes.size()&&now_ms()<deadline){
        const auto count=std::min(size_t{512},bytes.size()-offset);
        const int n=mbedtls_ssl_write(&ssl,bytes.data()+offset,count);
        if(n>0)offset+=n;
        else if(n!=MBEDTLS_ERR_SSL_WANT_READ&&n!=MBEDTLS_ERR_SSL_WANT_WRITE){transport_failed=true;return false;}
        else vTaskDelay(std::max<unsigned>(1,pdMS_TO_TICKS(2)));
    }
    // A timed-out write may retain a pending TLS record or an incomplete
    // application frame. Its buffer cannot be replaced by another exchange.
    if(offset!=bytes.size()){transport_failed=true;return false;}
    return true;
}
TlsLink::ReceiveResult TlsLink::receive_failed(TlsReceiveDiagnostic::Cause cause,int code,uint64_t now){
    incoming_failed=true;
    if(!incoming_diagnostic.fatal()){
        const auto progress=incoming_reader.progress(now);
        incoming_diagnostic={cause,code,static_cast<uint32_t>(incoming_size),static_cast<uint32_t>(incoming_expected),
            progress.idle_ms,progress.total_ms,incoming_max_poll_gap,incoming_max_read_ms};
    }
    return ReceiveResult::failed;
}
TlsLink::ReceiveResult TlsLink::receive_expired(TlsReceiveDiagnostic::Cause cause,int code,uint64_t now){
    if(incoming_expired)return ReceiveResult::pending;
    incoming_expired=true;
    const auto progress=incoming_reader.progress(now);
    incoming_diagnostic={cause,code,static_cast<uint32_t>(incoming_size),static_cast<uint32_t>(incoming_expected),
        progress.idle_ms,progress.total_ms,incoming_max_poll_gap,incoming_max_read_ms};
    return ReceiveResult::expired;
}
TlsLink::ReceiveResult TlsLink::expire_receive(){
    if(!established())return ReceiveResult::failed;
    return receive_expired(TlsReceiveDiagnostic::Cause::operation_timeout,0,now_ms());
}
TlsLink::ReceiveResult TlsLink::poll_receive(std::string &out){
    if(!established())return ReceiveResult::failed;
    out.clear();
    const auto began=now_ms();
    if(incoming_size&&began>=incoming_last_poll_end)incoming_max_poll_gap=std::max(incoming_max_poll_gap,began-incoming_last_poll_end);
    struct PollFinished{TlsLink& link;~PollFinished(){link.incoming_last_poll_end=now_ms();}} finished{*this};
    uint8_t bytes[512];
    // Bound CPU work even when an entire maximum-size frame is buffered. Never
    // consume bytes from the next frame or wait for another network fragment.
    for(unsigned turn=0;turn<16;++turn){
        auto now=now_ms();
        // Expiry rejects this message once. Keep its original deadline data
        // while draining exactly its bounded bytes, never accepting late work
        // or interpreting its tail as another frame's length prefix.
        if(incoming_expired&&incoming_size==incoming_expected&&incoming_size>=4){
            incoming_reader.reset();incoming_size=0;incoming_expected=4;incoming_expired=false;
            incoming_max_poll_gap=incoming_max_read_ms=0;return ReceiveResult::pending;
        }
        if(!incoming_expired&&incoming_reader.check_deadline(now)!=FrameReader::Result::incomplete){
            const auto progress=incoming_reader.progress(now);
            return receive_expired(progress.clock_reversed?TlsReceiveDiagnostic::Cause::clock_reversed:
                progress.total_ms>=FrameReader::frame_timeout_ms?TlsReceiveDiagnostic::Cause::frame_timeout:TlsReceiveDiagnostic::Cause::progress_timeout,0,now);
        }
        const auto request=std::min(incoming_expected-incoming_size,sizeof(bytes));
        const auto read_at=now_ms();const int count=mbedtls_ssl_read(&ssl,bytes,request);now=now_ms();
        if(now>=read_at)incoming_max_read_ms=std::max(incoming_max_read_ms,now-read_at);
        if(count==MBEDTLS_ERR_SSL_WANT_READ||count==MBEDTLS_ERR_SSL_WANT_WRITE)return ReceiveResult::pending;
        if(count<=0||static_cast<size_t>(count)>request)return receive_failed(count==0?TlsReceiveDiagnostic::Cause::tls_eof:TlsReceiveDiagnostic::Cause::tls_error,count,now);
        if(incoming_size<4)std::copy(bytes,bytes+count,incoming_prefix+incoming_size);
        incoming_size+=count;
        if(incoming_size==4&&incoming_expected==4){
            const uint32_t length=(uint32_t(incoming_prefix[0])<<24)|(uint32_t(incoming_prefix[1])<<16)|
                (uint32_t(incoming_prefix[2])<<8)|incoming_prefix[3];
            if(!length||length>max_frame_bytes)return receive_failed(TlsReceiveDiagnostic::Cause::invalid_length,0,now);
            incoming_expected=4+length;
        }
        if(incoming_expired)continue;
        size_t used=0;const auto fed_at=now_ms();const auto result=incoming_reader.feed(bytes,count,used,fed_at);
        if(used!=static_cast<size_t>(count)||(result!=FrameReader::Result::incomplete&&result!=FrameReader::Result::frame)){
            const auto progress=incoming_reader.progress(fed_at);
            if(result==FrameReader::Result::timed_out)return receive_expired(progress.clock_reversed?TlsReceiveDiagnostic::Cause::clock_reversed:
                progress.total_ms>=FrameReader::frame_timeout_ms?TlsReceiveDiagnostic::Cause::frame_timeout:TlsReceiveDiagnostic::Cause::progress_timeout,static_cast<int>(result),fed_at);
            return receive_failed(TlsReceiveDiagnostic::Cause::parser_rejected,static_cast<int>(result),fed_at);
        }
        if(result==FrameReader::Result::frame){
            out=std::string(incoming_reader.body());incoming_reader.reset();incoming_size=0;incoming_expected=4;
            incoming_max_poll_gap=incoming_max_read_ms=0;
            incoming_diagnostic={};
            return ReceiveResult::frame;
        }
    }
    return ReceiveResult::pending;
}
bool TlsLink::receive(std::string &out,uint32_t duration){
    if(!handshake())return false;
    const auto deadline=now_ms()+duration;
    while(now_ms()<deadline){
        const auto result=poll_receive(out);
        if(result==ReceiveResult::frame)return true;
        if(result==ReceiveResult::failed||result==ReceiveResult::expired)return false;
        // The configured 100 Hz tick would round a 2 ms delay to zero. Blocking
        // handshake/snapshot callers must let other tasks run while waiting.
        vTaskDelay(std::max<unsigned>(1,pdMS_TO_TICKS(2)));
    }
    // An application wait expiring says nothing about transport health. Keep
    // its exact partial frame so the next exchange continues at the same byte.
    if(!incoming_diagnostic.expired()){
        const auto now=now_ms();const auto progress=incoming_reader.progress(now);
        incoming_diagnostic={TlsReceiveDiagnostic::Cause::wrapper_timeout,0,static_cast<uint32_t>(incoming_size),static_cast<uint32_t>(incoming_expected),
            progress.idle_ms,progress.total_ms,incoming_max_poll_gap,incoming_max_read_ms};
    }
    return false;
}
std::string envelope_json(const Envelope &e,std::string_view payload){return "{\"protocol_version\":1,\"consortium_id\":\""+e.consortium_id.str()+"\",\"origin_octopus_id\":\""+e.origin_octopus_id.str()+"\",\"origin_boot_id\":\""+e.origin_boot_id.str()+"\",\"destination_octopus_id\":\""+e.destination_octopus_id.str()+"\",\"destination_boot_id\":\""+e.destination_boot_id.str()+"\",\"coordinator_epoch\":\""+std::to_string(e.coordinator_epoch)+"\",\"config_revision\":\""+std::to_string(e.config_revision)+"\",\"kind\":\""+kind_name(e.kind)+"\",\"ttl_ms\":"+std::to_string(e.ttl_ms)+",\"payload\":"+std::string(payload)+"}";}
} // namespace
#endif
