#pragma once
#ifdef ESP_PLATFORM
#include "amity/consortium/runtime.hpp"
#include "amity/consortium/credential_types.hpp"
#include "setup_timing.hpp"
#include "mbedtls/ssl.h"
#include "mbedtls/pk.h"
#include "mbedtls/x509_crt.h"
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
    bool handshake();
    bool extended_profile()const;
    bool hardware_profile()const;
    bool send(std::string_view);
    enum class ReceiveResult {pending,frame,failed};
    // One bounded nonblocking turn; partial framing belongs to this TLS link.
    ReceiveResult poll_receive(std::string &);
    bool receive(std::string &,uint32_t timeout_ms=5000);
    static int verify(void *,mbedtls_x509_crt *,int,uint32_t *);
private:
    FrameReader incoming_reader;
    uint8_t incoming_prefix[4]{};
    size_t incoming_size=0,incoming_expected=4;
    bool incoming_failed=false;
};
std::string envelope_json(const Envelope &,std::string_view payload);
} // namespace
#endif
#ifdef ESP_PLATFORM
namespace amity::consortium::platform {
using BootstrapHandler=std::function<esp_err_t(std::string_view,std::string&)>;
esp_err_t gatt_register();
esp_err_t gatt_server_open(uint64_t generation,model::OctopusId,BootstrapHandler);
void gatt_server_close();
// A durable prepared decision permits only this already connected workflow.
void gatt_server_seal();
esp_err_t gatt_client_discover(uint64_t generation,std::vector<RuntimeAdmissionCandidate>&);
esp_err_t gatt_client_connect_candidate(std::string_view token,uint64_t generation);
bool gatt_client_exchange(bool encrypted,std::string_view,std::string &);
void gatt_client_disconnect();
bool gatt_quiescent();
bool gatt_candidate_transport_idle();
}
#endif
