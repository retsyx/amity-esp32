/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// The HTTPS identity, components/web/tls_identity.cpp, built whole against
// ESP-IDF's pinned Mbed TLS. Configuration storage and the random fill are
// strict seams: saved records in a map and scripted 16-byte draws. OpenSSL
// independently parses every certificate the identity code saves or keeps; it
// rejects the nonminimal serials Mbed TLS accepts.
#include "tls_identity.hpp"
#include "config_store.hpp"
#include "esp_random.h"
#include "scenario_main.hpp"
#include "seams.hpp"
#include "mbedtls/pk.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"
#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/obj_mac.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <initializer_list>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

esp_err_t config_store::read(const char *key, std::string &value) { return AMITY_SEAM(config_store::read, key, value); }
esp_err_t config_store::write(const char *key, const std::string &value) {
    return AMITY_SEAM(config_store::write, key, value);
}
void esp_fill_random(void *out, size_t size) { AMITY_SEAM(esp_fill_random, out, size); }

namespace {

using Draw = std::array<uint8_t, 16>;

// Saved configuration records. A key in read_errors or write_errors fails
// its reads or writes with that error. writes lists every write, failed or
// not, in call order.
struct Store {
    std::map<std::string, std::string> records;
    std::map<std::string, esp_err_t> read_errors, write_errors;
    std::vector<std::string> writes;
};
Store store;

// The random fills the scenario scripted, taken in order.
std::vector<Draw> draws;
size_t drawn = 0;

// Every scenario initializes PSA, as the firmware does before it starts the
// HTTPS server, and runs over an empty store.
void start() {
    CHECK(psa_crypto_init() == PSA_SUCCESS);
    amity_seam::install<&config_store::read>([](const char *key, std::string &value) -> esp_err_t {
        if (const auto error = store.read_errors.find(key); error != store.read_errors.end()) return error->second;
        const auto found = store.records.find(key);
        if (found == store.records.end()) return ESP_ERR_NVS_NOT_FOUND;
        value = found->second;
        return ESP_OK;
    });
    amity_seam::install<&config_store::write>([](const char *key, const std::string &value) -> esp_err_t {
        store.writes.emplace_back(key);
        if (const auto error = store.write_errors.find(key); error != store.write_errors.end()) return error->second;
        store.records[key] = value;
        return ESP_OK;
    });
    amity_seam::install<&esp_fill_random>([](void *out, size_t size) {
        CHECK(size == sizeof(Draw) && drawn < draws.size());
        std::memcpy(out, draws[drawn++].data(), size);
    });
}

Draw filled(uint8_t byte) {
    Draw draw;
    draw.fill(byte);
    return draw;
}

// A draw of zeros apart from the given octets.
Draw with(std::initializer_list<std::pair<size_t, uint8_t>> octets) {
    Draw draw{};
    for (const auto &[index, value] : octets) draw[index] = value;
    return draw;
}

std::string bytes(const Draw &draw) { return std::string(reinterpret_cast<const char *>(draw.data()), draw.size()); }

// The serial as Mbed TLS reads it: the content octets of its DER INTEGER.
std::string serial(const std::string &pem) {
    mbedtls_x509_crt crt;
    mbedtls_x509_crt_init(&crt);
    CHECK(mbedtls_x509_crt_parse(&crt, reinterpret_cast<const uint8_t *>(pem.c_str()), pem.size() + 1) == 0);
    std::string octets(reinterpret_cast<const char *>(crt.serial.p), crt.serial.len);
    mbedtls_x509_crt_free(&crt);
    return octets;
}

// A positive DER INTEGER in its one valid encoding: no sign bit, and a
// leading zero only where the next octet needs it.
bool canonical(std::string_view octets) {
    const auto at = [&](size_t i) { return static_cast<uint8_t>(octets[i]); };
    return !octets.empty() && !(at(0) & 0x80) && (at(0) != 0 || (octets.size() > 1 && (at(1) & 0x80)));
}

struct OpenSslFree {
    void operator()(BIO *bio) const { BIO_free(bio); }
    void operator()(X509 *certificate) const { X509_free(certificate); }
    void operator()(EVP_PKEY *key) const { EVP_PKEY_free(key); }
    void operator()(BIGNUM *number) const { BN_free(number); }
};
using OpenSslCertificate = std::unique_ptr<X509, OpenSslFree>;
using OpenSslKey = std::unique_ptr<EVP_PKEY, OpenSslFree>;

std::unique_ptr<BIO, OpenSslFree> memory(const std::string &pem) {
    std::unique_ptr<BIO, OpenSslFree> bio(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())));
    CHECK(bio != nullptr);
    return bio;
}

// OpenSSL's own parse of a PEM certificate, or null when it rejects it.
OpenSslCertificate openssl_certificate(const std::string &pem) {
    return OpenSslCertificate(PEM_read_bio_X509(memory(pem).get(), nullptr, nullptr, nullptr));
}

bool at_midnight(const ASN1_TIME *time, int year) {
    std::tm parts{};
    return ASN1_TIME_to_tm(time, &parts) == 1 && parts.tm_year + 1900 == year && parts.tm_mon == 0 &&
           parts.tm_mday == 1 && parts.tm_hour == 0 && parts.tm_min == 0 && parts.tm_sec == 0;
}

// Checks a generated identity with OpenSSL: a version 3, self-signed ECDSA
// P-256 SHA-256 certificate for CN=<hostname>,O=Amity, valid from 2024 to
// 2050, for digital signatures only and not a CA, carrying expected_serial and
// matching the saved private key.
void check_generated(const std::string &certificate, const std::string &key, const std::string &hostname,
                     const std::string &expected_serial) {
    CHECK(serial(certificate) == expected_serial);
    CHECK(canonical(serial(certificate)));
    const auto parsed = openssl_certificate(certificate);
    CHECK(parsed != nullptr);
    X509 *const x = parsed.get();
    CHECK(X509_get_version(x) == X509_VERSION_3);
    const std::unique_ptr<BIGNUM, OpenSslFree> number(ASN1_INTEGER_to_BN(X509_get0_serialNumber(x), nullptr));
    std::string openssl_serial(expected_serial.size(), '\0');
    CHECK(number != nullptr && !BN_is_negative(number.get()) &&
          BN_num_bytes(number.get()) == static_cast<int>(expected_serial.size()) &&
          BN_bn2bin(number.get(), reinterpret_cast<unsigned char *>(openssl_serial.data())) ==
              static_cast<int>(expected_serial.size()) &&
          openssl_serial == expected_serial);
    char subject[256], issuer[256];
    X509_NAME_oneline(X509_get_subject_name(x), subject, sizeof(subject));
    X509_NAME_oneline(X509_get_issuer_name(x), issuer, sizeof(issuer));
    CHECK(std::string(subject) == "/CN=" + hostname + "/O=Amity");
    CHECK(std::string(issuer) == subject);
    CHECK(at_midnight(X509_get0_notBefore(x), 2024));
    CHECK(at_midnight(X509_get0_notAfter(x), 2050));
    CHECK(X509_get_signature_nid(x) == NID_ecdsa_with_SHA256);
    EVP_PKEY *const public_key = X509_get0_pubkey(x);
    char group[32]{};
    CHECK(public_key != nullptr && EVP_PKEY_get_base_id(public_key) == EVP_PKEY_EC &&
          EVP_PKEY_get_utf8_string_param(public_key, OSSL_PKEY_PARAM_GROUP_NAME, group, sizeof(group), nullptr) == 1 &&
          std::string(group) == "prime256v1");
    CHECK(X509_verify(x, public_key) == 1);
    const uint32_t extensions = X509_get_extension_flags(x);
    CHECK((extensions & EXFLAG_BCONS) && !(extensions & EXFLAG_CA));
    CHECK((extensions & EXFLAG_KUSAGE) && X509_get_key_usage(x) == KU_DIGITAL_SIGNATURE);
    const OpenSslKey private_key(PEM_read_bio_PrivateKey(memory(key).get(), nullptr, nullptr, nullptr));
    CHECK(private_key != nullptr && X509_check_private_key(x, private_key.get()) == 1);
}

// Generates an identity from draw over an empty store, then checks that the
// saved identity is reused byte for byte without another draw or write.
void generate_and_reuse(const Draw &draw, const Draw &expected_serial) {
    start();
    draws = {draw};
    std::string certificate, key;
    CHECK(web::tls_identity("amity", certificate, key) == ESP_OK);
    CHECK(drawn == 1);
    CHECK((store.writes == std::vector<std::string>{"tls_key", "tls_cert"}));
    CHECK(store.records.at("tls_key") == key && store.records.at("tls_cert") == certificate);
    check_generated(certificate, key, "amity", bytes(expected_serial));
    const auto saved = store.records;
    std::string reused_certificate, reused_key;
    CHECK(web::tls_identity("amity", reused_certificate, reused_key) == ESP_OK);
    CHECK(drawn == 1 && store.writes.size() == 2 && store.records == saved);
    CHECK(reused_certificate == certificate && reused_key == key);
}

// The draw that the identity being repaired came from, and the one the
// repair takes.
const Draw first_draw = with({{0, 0x11}, {15, 0x22}});
const Draw repair_draw = with({{0, 0x33}, {15, 0x44}});

// Saves an identity, generated by the firmware as on an earlier boot, and
// returns its private key.
std::string saved_identity() {
    draws.push_back(first_draw);
    std::string certificate, key;
    CHECK(web::tls_identity("amity", certificate, key) == ESP_OK);
    store.writes.clear();
    return key;
}

// A certificate signed by key with this raw serial, written by Mbed TLS's
// raw serial setter, which keeps leading zero octets and adds a zero before
// a first octet with its top bit set.
std::string certificate_der(const std::string &key_pem, std::string_view raw_serial) {
    mbedtls_pk_context key;
    mbedtls_pk_init(&key);
    CHECK(mbedtls_pk_parse_key(&key, reinterpret_cast<const uint8_t *>(key_pem.c_str()), key_pem.size() + 1,
                               nullptr, 0) == 0);
    mbedtls_x509write_cert writer;
    mbedtls_x509write_crt_init(&writer);
    mbedtls_x509write_crt_set_version(&writer, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&writer, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&writer, &key);
    mbedtls_x509write_crt_set_issuer_key(&writer, &key);
    CHECK(mbedtls_x509write_crt_set_serial_raw(&writer, reinterpret_cast<const uint8_t *>(raw_serial.data()),
                                               raw_serial.size()) == 0);
    CHECK(mbedtls_x509write_crt_set_subject_name(&writer, "CN=amity,O=Amity") == 0);
    CHECK(mbedtls_x509write_crt_set_issuer_name(&writer, "CN=amity,O=Amity") == 0);
    CHECK(mbedtls_x509write_crt_set_validity(&writer, "20240101000000", "20500101000000") == 0);
    uint8_t buffer[1600];
    const int size = mbedtls_x509write_crt_der(&writer, buffer, sizeof(buffer));
    CHECK(size > 0);
    mbedtls_x509write_crt_free(&writer);
    mbedtls_pk_free(&key);
    // The writer fills the buffer from its end.
    return std::string(reinterpret_cast<const char *>(buffer + sizeof(buffer) - size), size);
}

std::string pem(const std::string &der) {
    std::string base64(4 * ((der.size() + 2) / 3) + 1, '\0');
    const int size = EVP_EncodeBlock(reinterpret_cast<unsigned char *>(base64.data()),
                                     reinterpret_cast<const unsigned char *>(der.data()), static_cast<int>(der.size()));
    CHECK(size > 0);
    base64.resize(size);
    std::string text = "-----BEGIN CERTIFICATE-----\n";
    for (size_t at = 0; at < base64.size(); at += 64) text += base64.substr(at, 64) + "\n";
    return text + "-----END CERTIFICATE-----\n";
}

// Replaces a saved certificate with bad and checks that the firmware
// discards the whole saved identity: it draws again and saves a new key and
// a new certificate.
void repair(const std::string &bad) {
    const auto old_key = store.records.at("tls_key");
    store.records["tls_cert"] = bad;
    draws.push_back(repair_draw);
    std::string certificate, key;
    CHECK(web::tls_identity("amity", certificate, key) == ESP_OK);
    CHECK(drawn == 2);
    CHECK((store.writes == std::vector<std::string>{"tls_key", "tls_cert"}));
    CHECK(certificate != bad && key != old_key);
    CHECK(store.records.at("tls_cert") == certificate && store.records.at("tls_key") == key);
    check_generated(certificate, key, "amity", bytes(repair_draw));
}

// Each draw gives a 16-octet serial with the top bit of its first octet
// cleared and the bottom bit set, so it needs no sign octet and is never zero.
void redundant_zero_draw() { generate_and_reuse(with({{1, 0x01}}), with({{0, 0x01}, {1, 0x01}})); }
void masked_draw() { generate_and_reuse(with({{0, 0x80}, {1, 0x01}}), with({{0, 0x01}, {1, 0x01}})); }
void necessary_zero_draw() { generate_and_reuse(with({{1, 0x80}}), with({{0, 0x01}, {1, 0x80}})); }
void all_ones_draw() {
    Draw expected = filled(0xff);
    expected[0] = 0x7f;
    generate_and_reuse(filled(0xff), expected);
}
void all_zero_draw() { generate_and_reuse(filled(0), with({{0, 0x01}})); }

// Saved certificates whose serials Mbed TLS parses but browsers reject, or
// that do not parse, are replaced together with their key.
void repair_redundant_zero() {
    start();
    const auto key = saved_identity();
    repair(pem(certificate_der(key, std::string("\x00\x01", 2))));
}
void repair_zero_serial() {
    start();
    const auto key = saved_identity();
    repair(pem(certificate_der(key, std::string("\x00", 1))));
}
void repair_negative_serial() {
    start();
    const auto key = saved_identity();
    // The writer cannot encode a negative serial; set the sign bit in the DER.
    auto der = certificate_der(key, "\x5a\xa5\x5a\xa5");
    const auto at = der.find("\x02\x04\x5a\xa5\x5a\xa5");
    CHECK(at != std::string::npos && der.find("\x02\x04\x5a\xa5\x5a\xa5", at + 1) == std::string::npos);
    der[at + 2] = static_cast<char>(0xda);
    const auto negative = pem(der);
    CHECK(serial(negative) == "\xda\xa5\x5a\xa5");
    repair(negative);
}
void repair_garbage() {
    start();
    saved_identity();
    repair("not a certificate");
}

// A serial whose first octet has its top bit set needs its leading zero, so
// the saved identity is kept: no draw, no write, and the saved PEMs returned.
void preserve_necessary_zero() {
    start();
    const auto key = saved_identity();
    const auto valid = pem(certificate_der(key, "\x80"));
    CHECK(serial(valid) == std::string("\x00\x80", 2));
    CHECK(openssl_certificate(valid) != nullptr);
    store.records["tls_cert"] = valid;
    const auto saved = store.records;
    std::string certificate, private_key;
    CHECK(web::tls_identity("amity", certificate, private_key) == ESP_OK);
    CHECK(drawn == 1 && store.writes.empty() && store.records == saved);
    CHECK(certificate == valid && private_key == key);
}

// A saved certificate without its key, or a key without its certificate, is
// replaced by a new identity.
void missing_key() {
    start();
    saved_identity();
    const auto old_certificate = store.records.at("tls_cert");
    store.records.erase("tls_key");
    draws.push_back(repair_draw);
    std::string certificate, key;
    CHECK(web::tls_identity("amity", certificate, key) == ESP_OK);
    CHECK(drawn == 2 && (store.writes == std::vector<std::string>{"tls_key", "tls_cert"}));
    CHECK(certificate != old_certificate && store.records.at("tls_cert") == certificate &&
          store.records.at("tls_key") == key);
    check_generated(certificate, key, "amity", bytes(repair_draw));
}
void missing_certificate() {
    start();
    const auto old_key = saved_identity();
    store.records.erase("tls_cert");
    draws.push_back(repair_draw);
    std::string certificate, key;
    CHECK(web::tls_identity("amity", certificate, key) == ESP_OK);
    CHECK(drawn == 2 && (store.writes == std::vector<std::string>{"tls_key", "tls_cert"}));
    CHECK(key != old_key && store.records.at("tls_cert") == certificate && store.records.at("tls_key") == key);
    check_generated(certificate, key, "amity", bytes(repair_draw));
}

// A read that fails for any reason but a missing record fails the identity
// without drawing or writing, so a storage fault never replaces a saved one.
void certificate_read_error() {
    start();
    saved_identity();
    const auto saved = store.records;
    store.read_errors["tls_cert"] = ESP_FAIL;
    std::string certificate, key;
    CHECK(web::tls_identity("amity", certificate, key) == ESP_FAIL);
    CHECK(drawn == 1 && store.writes.empty() && store.records == saved);
}
void key_read_error() {
    start();
    // Nothing is saved, but the key cannot be read.
    store.read_errors["tls_key"] = ESP_ERR_INVALID_SIZE;
    std::string certificate, key;
    CHECK(web::tls_identity("amity", certificate, key) == ESP_FAIL);
    CHECK(drawn == 0 && store.writes.empty() && store.records.empty());
}

// The key is saved before the certificate. A failed write is the result,
// and a failed key write leaves the certificate unwritten.
void key_write_error() {
    start();
    draws = {first_draw};
    store.write_errors["tls_key"] = ESP_ERR_INVALID_SIZE;
    std::string certificate, key;
    CHECK(web::tls_identity("amity", certificate, key) == ESP_ERR_INVALID_SIZE);
    CHECK(drawn == 1 && (store.writes == std::vector<std::string>{"tls_key"}) && store.records.empty());
}
void certificate_write_error() {
    start();
    draws = {first_draw};
    store.write_errors["tls_cert"] = ESP_ERR_INVALID_SIZE;
    std::string certificate, key;
    CHECK(web::tls_identity("amity", certificate, key) == ESP_ERR_INVALID_SIZE);
    CHECK(drawn == 1 && (store.writes == std::vector<std::string>{"tls_key", "tls_cert"}));
    CHECK(store.records.size() == 1 && store.records.at("tls_key") == key);
}

// The certificate names the board's hostname.
void hostname_subject() {
    start();
    draws = {first_draw};
    std::string certificate, key;
    CHECK(web::tls_identity("living-room", certificate, key) == ESP_OK);
    CHECK(drawn == 1);
    check_generated(certificate, key, "living-room", bytes(first_draw));
}

// The PSA key generated for the certificate is destroyed once the PEMs are
// written. PSA hands out the volatile slot freed last, so the generated key
// takes the slot of a probe key destroyed just before, which must be free
// again afterwards.
void key_released() {
    start();
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_RAW_DATA);
    psa_set_key_bits(&attributes, 128);
    mbedtls_svc_key_id_t probe = 0;
    CHECK(psa_generate_key(&attributes, &probe) == PSA_SUCCESS && psa_destroy_key(probe) == PSA_SUCCESS);
    draws = {first_draw};
    std::string certificate, key;
    CHECK(web::tls_identity("amity", certificate, key) == ESP_OK);
    psa_key_attributes_t found = PSA_KEY_ATTRIBUTES_INIT;
    CHECK(psa_get_key_attributes(probe, &found) == PSA_ERROR_INVALID_HANDLE);
}

}  // namespace

int main(int argc, char **argv) {
    return amity_test::run(argc, argv,
                           {{"redundant-zero-draw", redundant_zero_draw},
                            {"masked-draw", masked_draw},
                            {"necessary-zero-draw", necessary_zero_draw},
                            {"all-ones-draw", all_ones_draw},
                            {"all-zero-draw", all_zero_draw},
                            {"repair-redundant-zero", repair_redundant_zero},
                            {"repair-zero-serial", repair_zero_serial},
                            {"repair-negative-serial", repair_negative_serial},
                            {"repair-garbage", repair_garbage},
                            {"preserve-necessary-zero", preserve_necessary_zero},
                            {"missing-key", missing_key},
                            {"missing-certificate", missing_certificate},
                            {"certificate-read-error", certificate_read_error},
                            {"key-read-error", key_read_error},
                            {"key-write-error", key_write_error},
                            {"certificate-write-error", certificate_write_error},
                            {"hostname-subject", hostname_subject},
                            {"key-released", key_released}});
}
