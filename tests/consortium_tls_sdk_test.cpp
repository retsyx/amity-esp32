/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// Credentials and TLS links, built from credentials.cpp and tls_link.cpp
// against ESP-IDF's pinned Mbed TLS on the host. NVS, random fill and select
// come from tests/consortium_tls_sdk_fakes, and links run over loopback TCP.
#include "platform.hpp"
#include "config_store.hpp"
#include "core_fakes.hpp"
#include "platform_fakes.hpp"
#include "seams.hpp"
#include "mbedtls/oid.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"
#include <poll.h>
#include <sys/ioctl.h>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
// Last: it routes select to the scenario's clock, as tls_link.cpp sees it.
#include "lwip/sockets.h"

namespace {

namespace platform = amity::consortium::platform;
namespace model = amity::system;
using amity_tls_fake::Step;
using platform::ConnectionAdmission;
using platform::Group;
using platform::Identity;
using platform::Peer;
using platform::TlsLink;

template <class Id> Id id(unsigned n) {
    char text[33];
    std::snprintf(text, sizeof(text), "%032x", n);
    return *Id::parse(text);
}

std::string to_hex(std::string_view bytes) {
    static const char digits[] = "0123456789abcdef";
    std::string text;
    for (const unsigned char byte : bytes) {
        text += digits[byte >> 4];
        text += digits[byte & 15];
    }
    return text;
}

// Every scenario initializes PSA, as the runtime does before it uses
// credentials, and the flash broker runs each NVS access inline.
void start() {
    CHECK(psa_crypto_init() == PSA_SUCCESS);
    amity_seam::install<&config_store::with_internal_flash_stack>([](std::function<void()> work) {
        work();
        return true;
    });
}

// A parsed certificate, freed with its scope.
struct Certificate {
    mbedtls_x509_crt crt;
    explicit Certificate(std::string_view der) {
        mbedtls_x509_crt_init(&crt);
        CHECK(mbedtls_x509_crt_parse_der(&crt, reinterpret_cast<const uint8_t *>(der.data()), der.size()) == 0);
    }
    ~Certificate() { mbedtls_x509_crt_free(&crt); }
    Certificate(const Certificate &) = delete;
    Certificate &operator=(const Certificate &) = delete;
};

// A leaf's pin, the SHA-256 of its subjectPublicKeyInfo, taken here from the DER.
std::string spki_pin(std::string_view der) {
    Certificate parsed(der);
    uint8_t digest[32];
    size_t size = 0;
    CHECK(psa_hash_compute(PSA_ALG_SHA_256, parsed.crt.pk_raw.p, parsed.crt.pk_raw.len, digest, sizeof(digest),
                           &size) == PSA_SUCCESS &&
          size == sizeof(digest));
    return to_hex(std::string_view(reinterpret_cast<const char *>(digest), size));
}

// The leaf with the last byte of its signature flipped: well-formed DER with
// the same key and profile, whose self-signature no longer holds.
std::string damaged(std::string der) {
    der.back() ^= 1;
    return der;
}

bool same(const Identity &a, const Identity &b) {
    return a.id == b.id && a.der == b.der && a.key_der == b.key_der && a.spki == b.spki;
}

std::string replace(std::string text, std::string_view before, std::string_view after) {
    const auto at = text.find(before);
    CHECK(at != std::string::npos);
    text.replace(at, before.size(), after);
    return text;
}

// A credential generated the way an unclaimed board generates its own.
Identity generate(unsigned n) {
    amity_tls_fake::erase_partition();
    Identity identity;
    CHECK(platform::load_identity(id<model::OctopusId>(n), identity, true) == ESP_OK);
    return identity;
}

Peer peer(const Identity &identity) { return {identity.id, identity.der, identity.spki, true}; }

// A self-signed leaf with the profile load_identity writes, written here so a
// scenario can change one property at a time.
struct LeafSpec {
    // Pads the leaf with an extension no parser knows to exactly this many bytes.
    std::size_t size = 0;
    const char *not_before = "20240101000000", *not_after = "20500101000000";
    psa_ecc_family_t family = PSA_ECC_FAMILY_SECP_R1;
    std::size_t bits = 256;
    bool server_auth = true;
    unsigned names = 1;
};

std::string write_der(mbedtls_pk_context &key, model::OctopusId octopus, const LeafSpec &spec, std::size_t padding,
                      unsigned variant) {
    mbedtls_x509write_cert cert;
    mbedtls_x509write_crt_init(&cert);
    mbedtls_x509write_crt_set_version(&cert, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&cert, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&cert, &key);
    mbedtls_x509write_crt_set_issuer_key(&cert, &key);
    const std::string dns = "o-" + octopus.str() + ".amity.invalid", subject = "CN=" + dns + ",O=Amity Consortium";
    const uint8_t serial[16] = {0x40, 1, uint8_t(variant >> 8), uint8_t(variant)};
    mbedtls_x509_san_list names[2]{};
    for (auto &name : names) {
        name.node.type = MBEDTLS_X509_SAN_DNS_NAME;
        name.node.san.unstructured_name.p = reinterpret_cast<uint8_t *>(const_cast<char *>(dns.data()));
        name.node.san.unstructured_name.len = dns.size();
    }
    names[0].next = spec.names > 1 ? &names[1] : nullptr;
    mbedtls_asn1_sequence client{}, server{};
    client.buf.tag = server.buf.tag = MBEDTLS_ASN1_OID;
    client.buf.p = reinterpret_cast<uint8_t *>(const_cast<char *>(MBEDTLS_OID_CLIENT_AUTH));
    client.buf.len = MBEDTLS_OID_SIZE(MBEDTLS_OID_CLIENT_AUTH);
    server.buf.p = reinterpret_cast<uint8_t *>(const_cast<char *>(MBEDTLS_OID_SERVER_AUTH));
    server.buf.len = MBEDTLS_OID_SIZE(MBEDTLS_OID_SERVER_AUTH);
    client.next = spec.server_auth ? &server : nullptr;
    // 2.999.1, under the example arc: a non-critical extension parsers skip.
    static const char unknown[] = {char(0x88), 0x37, 0x01};
    const std::string fill(padding, 'p');
    int result = mbedtls_x509write_crt_set_serial_raw(&cert, serial, sizeof(serial));
    if (!result) result = mbedtls_x509write_crt_set_subject_name(&cert, subject.c_str());
    if (!result) result = mbedtls_x509write_crt_set_issuer_name(&cert, subject.c_str());
    if (!result) result = mbedtls_x509write_crt_set_validity(&cert, spec.not_before, spec.not_after);
    if (!result) result = mbedtls_x509write_crt_set_basic_constraints(&cert, 0, -1);
    if (!result) result = mbedtls_x509write_crt_set_key_usage(&cert, MBEDTLS_X509_KU_DIGITAL_SIGNATURE);
    if (!result) result = mbedtls_x509write_crt_set_subject_alternative_name(&cert, &names[0]);
    if (!result) result = mbedtls_x509write_crt_set_ext_key_usage(&cert, &client);
    if (!result && padding)
        result = mbedtls_x509write_crt_set_extension(&cert, unknown, sizeof(unknown), 0,
                                                     reinterpret_cast<const unsigned char *>(fill.data()), fill.size());
    unsigned char der[2048];
    const int length = result ? result : mbedtls_x509write_crt_der(&cert, der, sizeof(der));
    mbedtls_x509write_crt_free(&cert);
    CHECK(length > 0);
    return std::string(reinterpret_cast<char *>(der + sizeof(der) - length), std::size_t(length));
}

Identity write_leaf(model::OctopusId octopus, const LeafSpec &spec = {}) {
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_KEY_PAIR(spec.family));
    psa_set_key_bits(&attributes, spec.bits);
    psa_set_key_algorithm(&attributes, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_EXPORT | PSA_KEY_USAGE_COPY);
    mbedtls_svc_key_id_t key_id = 0;
    CHECK(psa_generate_key(&attributes, &key_id) == PSA_SUCCESS);
    mbedtls_pk_context key;
    mbedtls_pk_init(&key);
    CHECK(mbedtls_pk_copy_from_psa(key_id, &key) == 0);
    CHECK(psa_destroy_key(key_id) == PSA_SUCCESS);
    // A padding byte adds one byte, but the signature's length varies by a
    // byte or two. Signatures are deterministic, so each attempt also changes
    // the serial number, and the padding follows the difference until exact.
    std::size_t padding = 0;
    auto der = write_der(key, octopus, spec, padding, 0);
    for (unsigned attempt = 1; spec.size && der.size() != spec.size && attempt < 1000; ++attempt) {
        padding = std::size_t(
            std::max<std::ptrdiff_t>(1, std::ptrdiff_t(padding + spec.size) - std::ptrdiff_t(der.size())));
        der = write_der(key, octopus, spec, padding, attempt);
    }
    CHECK(!spec.size || der.size() == spec.size);
    unsigned char private_der[512];
    const int length = mbedtls_pk_write_key_der(&key, private_der, sizeof(private_der));
    mbedtls_pk_free(&key);
    CHECK(length > 0);
    return {octopus, der,
            std::string(reinterpret_cast<char *>(private_der + sizeof(private_der) - length), std::size_t(length)),
            spki_pin(der)};
}

// The two ends of a connected loopback TCP pair on an ephemeral port.
// TlsLink::setup takes ownership of an end; a scenario closes any it keeps.
struct Sockets {
    int server = -1, client = -1;
};

Sockets tcp_pair() {
    const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    CHECK(listener >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(::bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0 && ::listen(listener, 1) == 0);
    socklen_t length = sizeof(address);
    CHECK(::getsockname(listener, reinterpret_cast<sockaddr *>(&address), &length) == 0);
    Sockets pair;
    pair.client = ::socket(AF_INET, SOCK_STREAM, 0);
    CHECK(pair.client >= 0 && ::connect(pair.client, reinterpret_cast<sockaddr *>(&address), length) == 0);
    pair.server = ::accept(listener, nullptr, nullptr);
    ::close(listener);
    CHECK(pair.server >= 0);
    return pair;
}

// Waits up to five seconds of real time for input on either socket.
bool readable(int first, int second) {
    pollfd polls[] = {{first, POLLIN, 0}, {second, POLLIN, 0}};
    return ::poll(polls, 2, 5000) > 0;
}

// Whether the scenario's clock follows real time (follow_real_time()).
bool real_time = false;

// Steps both handshakes in turn, as two boards would, until both finish or
// either fails, then completes both links.
bool exchange(TlsLink &server, TlsLink &client) {
    const auto step = [](TlsLink &link, bool &done) {
        if (done) return true;
        const int result = mbedtls_ssl_handshake(&link.ssl);
        done = !result;
        return !result || result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE;
    };
    bool server_done = false, client_done = false;
    for (unsigned round = 0; round < 2000; ++round) {
        if (!step(server, server_done) || !step(client, client_done)) return false;
        if (server_done && client_done) return server.handshake() && client.handshake();
        if (!readable(server.socket, client.socket)) return false;
        if (real_time)
            amity_tls_fake::catch_up();
        else
            amity_fake::advance_ms(1);
    }
    return false;
}

// What each end of a link authenticated.
struct Authenticated {
    model::OctopusId by_server, by_client;
};

// Sets up the authority's server admitting permitted and a member's client
// expecting the authority, then runs their handshake.
bool connect(const Identity &authority, const Identity &member, const std::vector<Peer> &permitted,
             model::OctopusId expected = {}, Authenticated *seen = nullptr) {
    const auto sockets = tcp_pair();
    ConnectionAdmission client_admission;
    TlsLink server, client(client_admission);
    if (!server.setup(authority, permitted, true, sockets.server, expected)) {
        ::close(sockets.client);
        return false;
    }
    if (!client.setup(member, {peer(authority)}, false, sockets.client, authority.id)) return false;
    // Each end requires the other's certificate, and no session resumes.
    CHECK(server.config.MBEDTLS_PRIVATE(authmode) == MBEDTLS_SSL_VERIFY_REQUIRED &&
          client.config.MBEDTLS_PRIVATE(authmode) == MBEDTLS_SSL_VERIFY_REQUIRED);
    CHECK(client.config.MBEDTLS_PRIVATE(session_tickets) == MBEDTLS_SSL_SESSION_TICKETS_DISABLED);
    if (!exchange(server, client)) return false;
    // TLS 1.2, with the one ECDHE-ECDSA AES-128-GCM suite.
    for (const TlsLink *link : {&server, &client}) {
        CHECK(std::string_view(mbedtls_ssl_get_version(&link->ssl)) == "TLSv1.2");
        CHECK(std::string_view(mbedtls_ssl_get_ciphersuite(&link->ssl)) == "TLS-ECDHE-ECDSA-WITH-AES-128-GCM-SHA256");
    }
    if (seen) *seen = {server.authenticated, client.authenticated};
    return true;
}

Group coordinator_group(const Identity &authority) {
    Group group;
    group.role = Group::coordinator;
    group.consortium = id<model::ConsortiumId>(10);
    group.authority = authority.id;
    group.revision = 1;
    return group;
}

// The stored state of a coordinator that admitted member.
std::string group_record(const Identity &authority, const Identity &member) {
    auto group = coordinator_group(authority);
    group.peers.push_back(peer(member));
    amity_tls_fake::erase_partition();
    CHECK(platform::save_group(group) == ESP_OK);
    return amity_tls_fake::blobs().at("amity_group/state");
}

int option(int socket, int level, int name) {
    int value = 0;
    socklen_t size = sizeof(value);
    CHECK(::getsockopt(socket, level, name, &value, &size) == 0);
    return value;
}

int64_t timeout_us(int socket, int name) {
    timeval value{};
    socklen_t size = sizeof(value);
    CHECK(::getsockopt(socket, SOL_SOCKET, name, &value, &size) == 0);
    return int64_t(value.tv_sec) * 1000000 + value.tv_usec;
}

// An unclaimed board generates a self-signed P-256 leaf bound to its id and
// stores it with its key in one record; a board that may not create one
// reports the missing record.
void identity_generation() {
    start();
    const auto octopus = id<model::OctopusId>(2);
    Identity identity;
    CHECK(platform::load_identity(octopus, identity, false) == ESP_ERR_NVS_NOT_FOUND);
    CHECK(amity_tls_fake::blobs().empty() && amity_tls_fake::writes().empty());
    CHECK(platform::load_identity(octopus, identity, true) == ESP_OK);
    CHECK(amity_tls_fake::writes() == std::vector<std::string>{"amity_group/identity"});
    CHECK(amity_tls_fake::blobs().size() == 1);
    CHECK(amity_tls_fake::blobs().at("amity_group/identity") ==
          "{\"version\":1,\"octopus_id\":\"" + octopus.str() + "\",\"leaf\":\"" + to_hex(identity.der) +
              "\",\"key\":\"" + to_hex(identity.key_der) + "\"}");
    CHECK(identity.id == octopus && !identity.der.empty() && identity.der.size() <= 1024 && !identity.key_der.empty());
    CHECK(identity.spki == spki_pin(identity.der));
    std::string pin;
    CHECK(platform::certificate(identity.der, octopus, pin) && pin == identity.spki);
    {
        Certificate leaf(identity.der);
        CHECK(mbedtls_pk_get_bitlen(&leaf.crt.pk) == 256 && leaf.crt.version == 3);
    }
    // The record loads as the same credential, which is never replaced.
    Identity reloaded;
    CHECK(platform::load_identity(octopus, reloaded, false) == ESP_OK && same(reloaded, identity));
    CHECK(platform::load_identity(octopus, reloaded, true) == ESP_OK && same(reloaded, identity));
    CHECK(amity_tls_fake::writes().size() == 1);
    // It belongs to this board alone.
    CHECK(platform::load_identity(id<model::OctopusId>(3), reloaded, true) == ESP_ERR_INVALID_STATE);
    // A credential that cannot be stored is reported.
    amity_tls_fake::erase_partition();
    amity_tls_fake::fail(Step::commit, "amity_group", ESP_ERR_NO_MEM);
    CHECK(platform::load_identity(octopus, reloaded, true) == ESP_ERR_NO_MEM && amity_tls_fake::blobs().empty());
    // The serial number is positive and never zero, whatever the random bytes.
    for (const char fill : {char(0xff), char(0)}) {
        amity_tls_fake::erase_partition();
        amity_tls_fake::script_random(std::string(16, fill));
        CHECK(platform::load_identity(octopus, reloaded, true) == ESP_OK);
        Certificate leaf(reloaded.der);
        const auto expected = (fill ? std::string(1, char(0x7f)) : std::string(1, char(1))) + std::string(15, fill);
        CHECK(std::string(reinterpret_cast<const char *>(leaf.crt.serial.p), leaf.crt.serial.len) == expected);
    }
    CHECK(amity_tls_fake::open_handles() == 0 && amity_tls_fake::scripts_used());
}

// A stored identity loads only as this board's own record, whose key matches
// its leaf and whose leaf passes full validation.
void identity_record() {
    start();
    const auto member = generate(2), outsider = generate(3);
    const auto record = [&](const std::string &leaf, const std::string &key) {
        return "{\"version\":1,\"octopus_id\":\"" + member.id.str() + "\",\"leaf\":\"" + to_hex(leaf) +
               "\",\"key\":\"" + to_hex(key) + "\"}";
    };
    Identity decoded;
    const auto load = [&](const std::string &text) {
        amity_tls_fake::erase_partition();
        amity_tls_fake::blobs()["amity_group/identity"] = text;
        decoded = {};
        return platform::load_identity(member.id, decoded, false);
    };
    CHECK(load(record(member.der, member.key_der)) == ESP_OK && same(decoded, member));
    CHECK(load(record(member.der, outsider.key_der)) == ESP_ERR_INVALID_STATE);
    CHECK(load(record(damaged(member.der), member.key_der)) == ESP_ERR_INVALID_STATE);
    CHECK(load(record(outsider.der, outsider.key_der)) == ESP_ERR_INVALID_STATE);
    const auto valid = record(member.der, member.key_der);
    CHECK(load(replace(valid, member.id.str(), outsider.id.str())) == ESP_ERR_INVALID_STATE);
    CHECK(load(replace(valid, "\"version\":1", "\"version\":2")) == ESP_ERR_INVALID_STATE);
    CHECK(load(replace(valid, "}", ",\"spare\":1}")) == ESP_ERR_INVALID_STATE);
    CHECK(load(replace(valid, to_hex(member.der), "zz")) == ESP_ERR_INVALID_STATE);
    // Nothing is decoded from a rejected record.
    CHECK(decoded.der.empty() && !decoded.id);
}

// An untrusted leaf gets full validation: profile, the identity in its name,
// its self-signature and its size, but not the clock.
void certificate_validation() {
    start();
    const auto authority = generate(1), member = generate(2);
    std::string pin;
    CHECK(platform::certificate(member.der, member.id, pin) && pin == member.spki);
    CHECK(!platform::certificate(damaged(member.der), member.id, pin));
    CHECK(!platform::certificate(member.der, authority.id, pin));
    CHECK(!platform::certificate(member.der, {}, pin));
    CHECK(!platform::certificate("not DER", member.id, pin));
    CHECK(!platform::certificate({}, member.id, pin));
    LeafSpec spec;
    spec.size = 1024;
    const auto largest = write_leaf(member.id, spec);
    CHECK(platform::certificate(largest.der, member.id, pin) && pin == largest.spki);
    spec.size = 1025;
    CHECK(!platform::certificate(write_leaf(member.id, spec).der, member.id, pin));
    LeafSpec expired;
    expired.not_before = "20000101000000";
    expired.not_after = "20010101000000";
    const auto old = write_leaf(member.id, expired);
    CHECK(platform::certificate(old.der, member.id, pin) && pin == old.spki);
    LeafSpec future;
    future.not_before = "20900101000000";
    future.not_after = "20950101000000";
    CHECK(platform::certificate(write_leaf(member.id, future).der, member.id, pin));
}

// certificate_binding requires the whole leaf profile and derives the pin
// from the subjectPublicKeyInfo.
void profile_binding() {
    start();
    const auto member = generate(2), outsider = generate(3);
    const auto bound = [&](const Identity &leaf, model::OctopusId octopus, std::string *pin = nullptr) {
        Certificate parsed(leaf.der);
        std::string derived;
        const bool good = platform::certificate_binding(parsed.crt, octopus, derived);
        if (pin) *pin = derived;
        return good;
    };
    std::string pin;
    CHECK(bound(member, member.id, &pin) && pin == member.spki && pin == spki_pin(member.der));
    CHECK(!bound(member, outsider.id) && !bound(member, {}));
    const auto edited = [&](const std::function<void(mbedtls_x509_crt &)> &edit) {
        Certificate parsed(member.der);
        edit(parsed.crt);
        std::string derived;
        return platform::certificate_binding(parsed.crt, member.id, derived);
    };
    CHECK(!edited([](mbedtls_x509_crt &crt) { crt.MBEDTLS_PRIVATE(ca_istrue) = 1; }));
    CHECK(!edited([](mbedtls_x509_crt &crt) { crt.MBEDTLS_PRIVATE(key_usage) = 0; }));
    CHECK(!edited([](mbedtls_x509_crt &crt) { crt.MBEDTLS_PRIVATE(ext_types) = 0; }));
    CHECK(!edited([](mbedtls_x509_crt &crt) { crt.MBEDTLS_PRIVATE(sig_md) = MBEDTLS_MD_SHA384; }));
    CHECK(!edited([](mbedtls_x509_crt &crt) { crt.version = 2; }));
    CHECK(!edited([](mbedtls_x509_crt &crt) { --crt.issuer_raw.len; }));
    {
        // A chain is not a leaf.
        Certificate chain(member.der);
        CHECK(mbedtls_x509_crt_parse_der(&chain.crt, reinterpret_cast<const uint8_t *>(outsider.der.data()),
                                         outsider.der.size()) == 0);
        std::string derived;
        CHECK(!platform::certificate_binding(chain.crt, member.id, derived));
    }
    // A P-256 key on another curve family, another key size, a leaf for
    // client authentication only, and a second name are each refused.
    CHECK(bound(write_leaf(member.id), member.id));
    LeafSpec spec;
    spec.family = PSA_ECC_FAMILY_BRAINPOOL_P_R1;
    CHECK(!bound(write_leaf(member.id, spec), member.id));
    spec = {};
    spec.bits = 384;
    CHECK(!bound(write_leaf(member.id, spec), member.id));
    spec = {};
    spec.server_auth = false;
    CHECK(!bound(write_leaf(member.id, spec), member.id));
    spec = {};
    spec.names = 2;
    CHECK(!bound(write_leaf(member.id, spec), member.id));
}

// The TLS verify callback accepts only the exact allowed leaf at depth zero,
// for the expected peer, and forgives nothing but the clock.
void verify_callback() {
    start();
    const auto authority = generate(1), member = generate(2), outsider = generate(3);
    Certificate leaf(member.der);
    TlsLink link;
    link.allowed = {peer(member)};
    uint32_t flags = MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE;
    CHECK(TlsLink::verify(&link, &leaf.crt, 0, &flags) == 0 && flags == 0 && link.authenticated == member.id);
    for (const uint32_t other : {uint32_t(MBEDTLS_X509_BADCERT_BAD_KEY), uint32_t(MBEDTLS_X509_BADCERT_NOT_TRUSTED),
                                 uint32_t(MBEDTLS_X509_BADCERT_CN_MISMATCH)}) {
        flags = other | MBEDTLS_X509_BADCERT_EXPIRED;
        CHECK(TlsLink::verify(&link, &leaf.crt, 0, &flags) == 0 && flags == other);
    }
    flags = 0;
    CHECK(TlsLink::verify(&link, &leaf.crt, 1, &flags) == 0 && flags == MBEDTLS_X509_BADCERT_OTHER);
    leaf.crt.next = &leaf.crt;
    flags = 0;
    CHECK(TlsLink::verify(&link, &leaf.crt, 0, &flags) == 0 && flags == MBEDTLS_X509_BADCERT_OTHER);
    leaf.crt.next = nullptr;
    link.authenticated = authority.id;
    flags = 0;
    CHECK(TlsLink::verify(&link, &leaf.crt, 0, &flags) == 0 && flags == MBEDTLS_X509_BADCERT_OTHER &&
          link.authenticated == authority.id);
    link.authenticated = {};
    link.allowed = {peer(outsider)};
    flags = 0;
    CHECK(TlsLink::verify(&link, &leaf.crt, 0, &flags) == 0 && flags == MBEDTLS_X509_BADCERT_OTHER &&
          !link.authenticated);
    // Every call is timed.
    CHECK(link.timing_verify_calls == 8);
}

// A stored leaf was validated before it was saved, so loading binds it by its
// pinned SPKI without repeating its self-signature check, and TLS accepts its
// exact DER: a stored leaf whose signature no longer holds still loads and
// authenticates, while full validation rejects it. A damaged pin, id or leaf
// is refused.
void stored_roster() {
    start();
    const auto authority = generate(1), member = generate(2);
    const auto clean = group_record(authority, member);
    Group restored;
    CHECK(platform::load_group(restored) == ESP_OK && restored.peers.size() == 1 && restored.peers[0].id == member.id &&
          restored.peers[0].der == member.der && restored.peers[0].pin == member.spki && restored.peers[0].active);
    const auto corrupted = damaged(member.der);
    std::string pin;
    CHECK(!platform::certificate(corrupted, member.id, pin));
    amity_tls_fake::blobs()["amity_group/state"] = replace(clean, to_hex(member.der), to_hex(corrupted));
    CHECK(platform::load_group(restored) == ESP_OK && restored.peers.size() == 1 && restored.peers[0].der == corrupted);
    Authenticated seen;
    const Identity presenting{member.id, corrupted, member.key_der, member.spki};
    CHECK(connect(authority, presenting, restored.peers, {}, &seen) && seen.by_server == member.id &&
          seen.by_client == authority.id);
    for (const auto &bad : {replace(clean, member.spki, std::string(64, 'f')),
                            replace(clean, member.id.str(), id<model::OctopusId>(99).str()),
                            replace(clean, to_hex(member.der), "00")}) {
        amity_tls_fake::blobs()["amity_group/state"] = bad;
        CHECK(platform::load_group(restored) == ESP_ERR_INVALID_STATE);
    }
}

// Whether a server link refuses these peers on a real TCP socket, with its
// own admission budget, where only the peers can make setup fail.
bool refuses(const Identity &identity, const std::vector<Peer> &peers) {
    const auto sockets = tcp_pair();
    ConnectionAdmission admission;
    TlsLink link(admission);
    const bool refused = !link.setup(identity, peers, true, sockets.server) && !link.usable();
    ::close(sockets.client);
    return refused;
}

// TlsLink::setup binds every allowed leaf, of at most 1024 bytes, to its id
// and pin, and refuses an empty allowlist, a client that expects no one, its
// own unusable credential and a second setup, even after a failed one.
void setup_peers() {
    start();
    const auto authority = generate(1), member = generate(2), outsider = generate(3);
    CHECK(!refuses(authority, {peer(member)}) && !refuses(authority, {peer(member), peer(outsider)}));
    auto wrong_pin = peer(member), wrong_id = peer(member), not_der = peer(member);
    wrong_pin.pin = std::string(64, 'f');
    wrong_id.id = outsider.id;
    not_der.der = "not DER";
    LeafSpec oversized;
    oversized.size = 1025;
    for (const auto &bad : {wrong_pin, wrong_id, not_der, peer(write_leaf(member.id, oversized))})
        CHECK(refuses(authority, {bad}) && refuses(authority, {peer(outsider), bad}));
    CHECK(refuses(authority, {}));
    auto broken = authority;
    broken.key_der = "not a key";
    CHECK(refuses(broken, {peer(member)}));
    const auto sockets = tcp_pair();
    {
        ConnectionAdmission admission;
        TlsLink client(admission);
        CHECK(!client.setup(member, {peer(authority)}, false, sockets.client, {}) && !client.usable());
    }
    TlsLink server;
    CHECK(server.setup(authority, {peer(member)}, true, sockets.server) && server.usable() && !server.established());
    const auto other = tcp_pair();
    CHECK(!server.setup(authority, {peer(member)}, true, other.server) && server.usable());
    {
        ConnectionAdmission admission;
        TlsLink failed(admission);
        CHECK(!failed.setup(authority, {}, true, other.client) && !failed.usable());
        const auto again = tcp_pair();
        CHECK(!failed.setup(authority, {peer(member)}, true, again.server) && !failed.usable());
        ::close(again.server);
        ::close(again.client);
    }
    ::close(other.server);
}

// All three secondary peers persist and authenticate; a fourth is refused by
// save_group, leaving the committed roster, and by TLS setup.
void roster_capacity() {
    start();
    const auto authority = generate(1), member = generate(2), second = generate(3), third = generate(4);
    auto group = coordinator_group(authority);
    group.peers = {peer(member), peer(second), peer(third)};
    amity_tls_fake::erase_partition();
    CHECK(platform::save_group(group) == ESP_OK);
    Group restored;
    CHECK(platform::load_group(restored) == ESP_OK && restored.peers.size() == 3);
    for (const Identity *identity : {&member, &second, &third}) {
        Authenticated seen;
        CHECK(connect(authority, *identity, restored.peers, {}, &seen) && seen.by_server == identity->id);
    }
    const auto committed = amity_tls_fake::blobs().at("amity_group/state");
    group.peers.push_back(peer(authority));
    CHECK(platform::save_group(group) == ESP_ERR_INVALID_ARG &&
          amity_tls_fake::blobs().at("amity_group/state") == committed);
    CHECK(refuses(authority, group.peers));
}

// One pending handshake per board, and four retained links: a silent socket
// holds the pending budget until its 10 s deadline, a refused link parses
// nothing, a failed link counts until it is destroyed, and a full roster
// still admits a replacement for a rebooted peer.
void connection_admission() {
    start();
    const auto authority = generate(1), member = generate(2), second = generate(3), third = generate(4);
    ConnectionAdmission main_admission;
    const std::vector<Peer> permitted{peer(member), peer(second), peer(third)};
    const auto refused = [&] {
        TlsLink blocked(main_admission);
        return !blocked.setup(authority, permitted, true, -1) && !blocked.own.raw.p;
    };
    const auto silent_pair = tcp_pair();
    auto silent = std::make_unique<TlsLink>(main_admission);
    CHECK(silent->setup(authority, permitted, true, silent_pair.server));
    CHECK(refused());
    // The handshake waits for input in 100 ms steps until its deadline.
    const auto began = amity_fake::now_us();
    const auto waited = amity_tls_fake::waits().size();
    CHECK(!silent->handshake() && amity_fake::now_us() - began == 10000000 && !silent->usable());
    const auto waits = amity_tls_fake::waits();
    CHECK(waits.size() - waited == 100);
    for (auto wait = waits.begin() + std::ptrdiff_t(waited); wait != waits.end(); ++wait)
        CHECK(wait->readable && !wait->writable && wait->timeout_us == 100000 && !wait->ready);
    CHECK(refused());
    silent.reset();
    ::close(silent_pair.client);
    // Each member board has its own budget; the main board's links share one.
    ConnectionAdmission client_admission[4];
    std::vector<std::unique_ptr<TlsLink>> servers, clients;
    const Identity *identities[] = {&member, &second, &third, &member};
    for (unsigned i = 0; i < 4; ++i) {
        const auto sockets = tcp_pair();
        servers.push_back(std::make_unique<TlsLink>(main_admission));
        clients.push_back(std::make_unique<TlsLink>(client_admission[i]));
        CHECK(servers.back()->setup(authority, permitted, true, sockets.server) &&
              clients.back()->setup(*identities[i], {peer(authority)}, false, sockets.client, authority.id));
        CHECK(exchange(*servers.back(), *clients.back()));
    }
    CHECK(servers[0]->authenticated == member.id && servers[3]->authenticated == member.id);
    for (const auto &server : servers) CHECK(server->established());
    CHECK(refused());
    servers.pop_back();
    clients.pop_back();
    const auto sockets = tcp_pair();
    TlsLink next(main_admission);
    CHECK(next.setup(authority, permitted, true, sockets.server));
    ::close(sockets.client);
}

// Real mutual TLS admits a permitted peer, and refuses a new certificate
// under the same id, an unknown peer, a peer other than the expected one and
// a peer without the matching key. The allowlist is captured at setup.
void mutual_tls() {
    start();
    const auto authority = generate(1), member = generate(2), outsider = generate(3);
    Authenticated seen;
    CHECK(connect(authority, member, {peer(member)}, {}, &seen) && seen.by_server == member.id &&
          seen.by_client == authority.id);
    CHECK(!connect(authority, generate(2), {peer(member)}));
    {
        const auto sockets = tcp_pair();
        ConnectionAdmission client_admission;
        TlsLink server, client(client_admission);
        std::vector<Peer> roster{peer(member)};
        CHECK(server.setup(authority, roster, true, sockets.server));
        roster[0] = peer(outsider);
        CHECK(client.setup(member, {peer(authority)}, false, sockets.client, authority.id) &&
              exchange(server, client) && server.authenticated == member.id);
    }
    CHECK(!connect(authority, outsider, {peer(member)}));
    CHECK(!connect(authority, member, {peer(member)}, outsider.id));
    auto wrong_key = member;
    wrong_key.key_der = outsider.key_der;
    CHECK(!connect(authority, wrong_key, {peer(member)}));
}

// setup tunes both TCP ends: no Nagle delay, keepalive after 10 s idle with
// three probes 3 s apart, and 100 ms socket timeouts.
void socket_options() {
    start();
    const auto main = generate(1), member = generate(2);
    const auto sockets = tcp_pair();
    ConnectionAdmission client_admission;
    TlsLink server, client(client_admission);
    CHECK(server.setup(main, {peer(member)}, true, sockets.server) &&
          client.setup(member, {peer(main)}, false, sockets.client, main.id));
    for (const int socket : {sockets.server, sockets.client}) {
        CHECK(option(socket, IPPROTO_TCP, TCP_NODELAY) != 0);
        CHECK(option(socket, SOL_SOCKET, SO_KEEPALIVE) != 0);
        CHECK(option(socket, IPPROTO_TCP, TCP_KEEPIDLE) == 10);
        CHECK(option(socket, IPPROTO_TCP, TCP_KEEPINTVL) == 3);
        CHECK(option(socket, IPPROTO_TCP, TCP_KEEPCNT) == 3);
        CHECK(timeout_us(socket, SO_RCVTIMEO) == 100000 && timeout_us(socket, SO_SNDTIMEO) == 100000);
    }
}

// A socket that cannot take the TCP options leaves the link unconfigured.
void non_tcp_socket() {
    start();
    const auto main = generate(1), member = generate(2);
    int ends[2];
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, ends) == 0);
    TlsLink link;
    CHECK(!link.setup(main, {peer(member)}, true, ends[0]) && !link.usable());
    ::close(ends[1]);
}

// Waits up to two seconds of real time until a socket holds at least bytes
// of input, without reading any.
bool arrived(int socket, std::size_t bytes) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    int available = 0;
    while (::ioctl(socket, FIONREAD, &available) == 0 && std::size_t(available) < bytes) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        ::poll(nullptr, 0, 1);
    }
    return std::size_t(available) >= bytes;
}

// TLS 1.2 AES-GCM adds a 5-byte header, an 8-byte nonce and a 16-byte tag to
// each record.
constexpr std::size_t record_overhead = 29;

// The plaintext sizes of the application records waiting on a socket that
// carry a frame of frame_bytes, once all of them have arrived.
std::vector<std::size_t> records(int socket, std::size_t frame_bytes, std::size_t limit) {
    const std::size_t count = (frame_bytes + limit - 1) / limit, wire = frame_bytes + count * record_overhead;
    CHECK(arrived(socket, wire));
    std::string bytes(wire, '\0');
    CHECK(::recv(socket, bytes.data(), wire, MSG_PEEK) == ssize_t(wire));
    std::vector<std::size_t> sizes;
    for (std::size_t at = 0; at + 5 <= wire;) {
        const auto *header = reinterpret_cast<const unsigned char *>(bytes.data() + at);
        const std::size_t length = std::size_t(header[3]) << 8 | header[4];
        CHECK(header[0] == 23 && header[1] == 3 && header[2] == 3 && length > 24);
        sizes.push_back(length - 24);
        at += 5 + length;
    }
    return sizes;
}

// Over real TCP, in real time: mutual authentication, then ordinary and bulk
// frames in both directions on one stream. The four-byte prefix puts 508 and
// 509, and 2044 and 2045, byte bodies on either side of the 512-byte ordinary
// and 2048-byte bulk record limits; each frame fills records to its limit and
// arrives whole, with its class.
void tcp_frames() {
    start();
    amity_tls_fake::follow_real_time();
    real_time = true;
    const auto main = generate(1), member = generate(2);
    const auto sockets = tcp_pair();
    ConnectionAdmission client_admission;
    TlsLink server, client(client_admission);
    CHECK(server.setup(main, {peer(member)}, true, sockets.server) &&
          client.setup(member, {peer(main)}, false, sockets.client, main.id));
    CHECK(exchange(server, client));
    CHECK(server.authenticated == member.id && client.authenticated == main.id &&
          !mbedtls_ssl_get_verify_result(&server.ssl) && !mbedtls_ssl_get_verify_result(&client.ssl));
    // An idle link has nothing to receive, and a frame whose records arrive
    // apart is assembled across polls.
    std::string partial;
    CHECK(server.poll_receive(partial) == TlsLink::ReceiveResult::pending && partial.empty());
    const auto split = amity::consortium::frame(std::string(1000, 'p'));
    CHECK(mbedtls_ssl_write(&client.ssl, split.data(), 100) == 100);
    CHECK(arrived(server.socket, 100 + record_overhead));
    CHECK(server.poll_receive(partial) == TlsLink::ReceiveResult::pending && partial.empty() &&
          !server.receiving_bulk());
    CHECK(mbedtls_ssl_write(&client.ssl, split.data() + 100, split.size() - 100) == int(split.size() - 100));
    CHECK(server.receive(partial, 2000) && partial == std::string(1000, 'p') && !server.received_bulk());
    unsigned frames = 0;
    for (const std::size_t bytes : {64u, 508u, 509u, 800u, 2044u, 2045u, 2048u, 4775u}) {
        for (const bool reverse : {false, true}) {
            for (const bool bulk : {false, true}) {
                auto &sender = reverse ? server : client;
                auto &receiver = reverse ? client : server;
                const std::string body(bytes, reverse ? 's' : 'c');
                CHECK(sender.send(body, bulk));
                const std::size_t limit = bulk ? 2048 : 512, frame = bytes + 4;
                std::vector<std::size_t> expected(frame / limit, limit);
                if (frame % limit) expected.push_back(frame % limit);
                CHECK(records(receiver.socket, frame, limit) == expected);
                std::string received;
                CHECK(receiver.receive(received, 2000) && received == body);
                CHECK(receiver.received_bulk() == bulk && !receiver.receiving_bulk());
                CHECK(server.established() && client.established());
                ++frames;
            }
        }
    }
    CHECK(frames == 32);
}

// The primitives the runtime uses: random hex, the clock and SHA-256.
void primitives() {
    start();
    amity_tls_fake::script_random(std::string("\x01\x23\xab\xff", 4));
    CHECK(platform::random_hex(4) == "0123abff");
    CHECK(platform::random_hex(0).empty() && platform::random_hex(33).empty());
    CHECK(platform::random_hex(32).size() == 64);
    amity_fake::advance_us(1999);
    CHECK(platform::now_ms() == 1);
    amity_fake::advance_us(1);
    CHECK(platform::now_ms() == 2);
    CHECK(platform::sha256("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(amity_tls_fake::scripts_used());
}

// Records live in NVS on the amity_cfg partition, read and written through
// the flash broker: a read takes the whole record up to the caller's limit,
// a write stores 1 to 32768 bytes and commits, and every error reaches the
// caller.
void record_storage() {
    start();
    auto &blobs = amity_tls_fake::blobs();
    std::string out;
    blobs["space/key"] = std::string(64, 'r');
    CHECK(platform::read_record("space", "key", out, 64) == ESP_OK && out == std::string(64, 'r'));
    CHECK(platform::read_record("space", "key", out, 63) == ESP_ERR_INVALID_SIZE);
    blobs["space/empty"] = "";
    CHECK(platform::read_record("space", "empty", out, 64) == ESP_ERR_INVALID_SIZE);
    CHECK(platform::read_record("space", "missing", out, 64) == ESP_ERR_NVS_NOT_FOUND);
    CHECK(platform::read_record("elsewhere", "key", out, 64) == ESP_ERR_NVS_NOT_FOUND);
    // A record that changes between its size and its read is refused.
    amity_tls_fake::on_size([&](const std::string &name) {
        if (name == "space/key") blobs[name] = "short";
    });
    CHECK(platform::read_record("space", "key", out, 64) == ESP_ERR_INVALID_SIZE);
    amity_tls_fake::on_size(nullptr);
    amity_tls_fake::fail(Step::open, "space", ESP_ERR_NVS_NOT_INITIALIZED);
    CHECK(platform::read_record("space", "key", out, 64) == ESP_ERR_NVS_NOT_INITIALIZED);
    amity_tls_fake::fail(Step::size, "space/key", ESP_ERR_NVS_TYPE_MISMATCH);
    CHECK(platform::read_record("space", "key", out, 64) == ESP_ERR_NVS_TYPE_MISMATCH);
    amity_tls_fake::fail(Step::read, "space/key", ESP_ERR_NO_MEM);
    CHECK(platform::read_record("space", "key", out, 64) == ESP_ERR_NO_MEM);
    const std::string largest(32768, 'w');
    CHECK(platform::write_record("space", "big", largest) == ESP_OK && blobs.at("space/big") == largest);
    CHECK(platform::write_record("space", "big", largest + "w") == ESP_ERR_INVALID_SIZE);
    CHECK(platform::write_record("space", "none", "") == ESP_ERR_INVALID_SIZE && !blobs.count("space/none"));
    amity_tls_fake::fail(Step::open, "space", ESP_ERR_NVS_NOT_INITIALIZED);
    CHECK(platform::write_record("space", "big", "new") == ESP_ERR_NVS_NOT_INITIALIZED);
    amity_tls_fake::fail(Step::write, "space/big", ESP_ERR_NO_MEM);
    CHECK(platform::write_record("space", "big", "new") == ESP_ERR_NO_MEM);
    amity_tls_fake::fail(Step::commit, "space", ESP_ERR_INVALID_STATE);
    CHECK(platform::write_record("space", "big", "new") == ESP_ERR_INVALID_STATE && blobs.at("space/big") == largest);
    CHECK(platform::write_record("space", "big", "new") == ESP_OK && blobs.at("space/big") == "new");
    CHECK(amity_tls_fake::open_handles() == 0);
    // Every access went through the broker, which may refuse the work.
    CHECK(amity_seam::count("config_store::with_internal_flash_stack") == 16);
    amity_seam::install<&config_store::with_internal_flash_stack>([](std::function<void()>) { return false; });
    const auto writes = amity_tls_fake::writes().size();
    CHECK(platform::read_record("space", "big", out, 64) == ESP_FAIL);
    CHECK(platform::write_record("space", "big", "other") == ESP_FAIL && blobs.at("space/big") == "new");
    CHECK(amity_tls_fake::writes().size() == writes && amity_tls_fake::scripts_used());
}

}  // namespace

int main(int argc, char **argv) {
    return amity_test::run(argc, argv,
                           {{"identity-generation", identity_generation},
                            {"identity-record", identity_record},
                            {"certificate", certificate_validation},
                            {"profile-binding", profile_binding},
                            {"verify-callback", verify_callback},
                            {"stored-roster", stored_roster},
                            {"setup-peers", setup_peers},
                            {"roster-capacity", roster_capacity},
                            {"connection-admission", connection_admission},
                            {"mutual-tls", mutual_tls},
                            {"socket-options", socket_options},
                            {"non-tcp-socket", non_tcp_socket},
                            {"tcp-frames", tcp_frames},
                            {"primitives", primitives},
                            {"record-storage", record_storage}});
}
