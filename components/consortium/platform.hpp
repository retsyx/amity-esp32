#pragma once
#ifdef ESP_PLATFORM
#include "amity/consortium/runtime.hpp"
#include "amity/consortium/credential_types.hpp"
#include "setup_timing.hpp"
#include "tls_receive_trace.hpp"
#include "mbedtls/ssl.h"
#include "mbedtls/pk.h"
#include "mbedtls/x509_crt.h"
#include <atomic>
#include <mutex>
namespace amity::consortium::platform {
inline constexpr std::string_view origin_record_prefix="1:unprovisioned:";
std::string hex(std::string_view);
bool unhex(std::string_view,std::string &,size_t limit);
std::string sha256(std::string_view);
std::string random_hex(size_t);
uint64_t now_ms();
esp_err_t read_record(const char *space,const char *key,std::string &,size_t cap);
esp_err_t write_record(const char *space,const char *key,std::string_view);
esp_err_t erase_record(const char *space,const char *key);
esp_err_t erase_namespace(const char *space);
esp_err_t load_identity(model::OctopusId,Identity &);
bool certificate(std::string_view der,model::OctopusId,std::string &pin);
// Structural identity/pin binding only; untrusted leaves still use certificate().
bool certificate_binding(const mbedtls_x509_crt &,model::OctopusId,std::string &pin);
esp_err_t load_group(Group &);
esp_err_t save_group(const Group &);
struct TlsLink {
    int socket=-1;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config config;
    mbedtls_x509_crt own,anchors;
    mbedtls_pk_context key;
    std::vector<Peer> allowed;
    model::OctopusId authenticated;
    bool server=false;
    uint64_t timing_verify_ms=0;uint32_t timing_verify_calls=0;
    TlsLink();~TlsLink();
    bool setup(const Identity &,const std::vector<Peer>&,bool is_server,int fd,model::OctopusId expected={});
    // The first blocking I/O establishes this transport. Subsequent exchanges
    // reuse it; application synchronization has no ownership of TLS lifetime.
    bool handshake();
    bool usable()const{return configured&&!transport_failed&&!incoming_failed;}
    bool established()const{return handshaken&&usable();}
    bool extended_profile()const;
    bool hardware_profile()const;
    bool send(std::string_view);
    enum class ReceiveResult {pending,frame,expired,failed};
    // One bounded nonblocking turn on an established transport; partial
    // framing belongs to this TLS link, including across wrapper timeouts.
    ReceiveResult poll_receive(std::string &);
    // Reject the in-flight application frame after an outer receive budget.
    // Its remaining bytes are drained; the authenticated TLS stays available.
    ReceiveResult expire_receive();
    bool receive(std::string &,uint32_t timeout_ms=5000);
    const TlsReceiveDiagnostic& receive_diagnostic()const{return incoming_diagnostic;}
    static int verify(void *,mbedtls_x509_crt *,int,uint32_t *);
private:
    bool configured=false;
    std::atomic_bool handshaken{false},transport_failed{false};
    FrameReader incoming_reader;
    uint8_t incoming_prefix[4]{};
    size_t incoming_size=0,incoming_expected=4;
    std::atomic_bool incoming_failed{false};
    bool incoming_expired=false;
    uint64_t incoming_last_poll_end=0,incoming_max_poll_gap=0,incoming_max_read_ms=0;
    TlsReceiveDiagnostic incoming_diagnostic;
    ReceiveResult receive_failed(TlsReceiveDiagnostic::Cause,int,uint64_t);
    ReceiveResult receive_expired(TlsReceiveDiagnostic::Cause,int,uint64_t);
};
std::string envelope_json(const Envelope &,std::string_view payload);
} // namespace
#endif
#ifdef ESP_PLATFORM
namespace amity::consortium::platform {
using BootstrapHandler=std::function<esp_err_t(std::string_view,std::string&)>;
enum class BootstrapFailure : uint8_t { none=0, crypto_self_test_failed=1 };
esp_err_t gatt_register();
esp_err_t gatt_server_open(uint64_t generation,model::OctopusId,BootstrapHandler);
void gatt_server_close();
// A durable prepared decision permits only this already connected workflow.
void gatt_server_seal();
esp_err_t gatt_client_discover(uint64_t generation,std::vector<RuntimeAdmissionCandidate>&);
esp_err_t gatt_client_candidates(uint64_t generation,std::vector<RuntimeAdmissionCandidate>&);
esp_err_t gatt_client_connect_candidate(std::string_view token,uint64_t generation);
bool gatt_client_exchange(bool encrypted,std::string_view,std::string &,BootstrapFailure* failure=nullptr);
void gatt_client_disconnect();
bool gatt_quiescent();
bool gatt_candidate_transport_idle();
}
#endif
