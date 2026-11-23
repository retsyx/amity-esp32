/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// Scripted Mbed TLS sessions with the SDK's constants and signatures,
// checked against the pinned SDK by sdk_parity/tls_link.def. A configuration
// records what each setter set, in amity_ fields. A session runs over the
// amity_fake::tls wire of the socket its bio names: the handshake, records
// and alerts follow that wire's script.
#include "mbedtls/pk.h"
#include "mbedtls/x509_crt.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// As in the SDK, private members carry a prefix outside Mbed TLS itself.
#define MBEDTLS_PRIVATE(member) private_##member

// The Amity build enables session tickets (CONFIG_MBEDTLS_CLIENT_SSL_SESSION_TICKETS).
#define MBEDTLS_SSL_SESSION_TICKETS

#define MBEDTLS_ERR_SSL_NO_CLIENT_CERTIFICATE -0x7480
#define MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE -0x7780
#define MBEDTLS_ERR_SSL_WANT_READ -0x6900
#define MBEDTLS_ERR_SSL_WANT_WRITE -0x6880
#define MBEDTLS_ERR_SSL_TIMEOUT -0x6800
#define MBEDTLS_ERR_SSL_BAD_INPUT_DATA -135  // PSA_ERROR_INVALID_ARGUMENT

#define MBEDTLS_SSL_TRANSPORT_STREAM 0
#define MBEDTLS_SSL_IS_CLIENT 0
#define MBEDTLS_SSL_IS_SERVER 1
#define MBEDTLS_SSL_VERIFY_NONE 0
#define MBEDTLS_SSL_VERIFY_OPTIONAL 1
#define MBEDTLS_SSL_VERIFY_REQUIRED 2
#define MBEDTLS_SSL_SESSION_TICKETS_DISABLED 0
#define MBEDTLS_SSL_SESSION_TICKETS_ENABLED 1
#define MBEDTLS_SSL_PRESET_DEFAULT 0
#define MBEDTLS_SSL_ALERT_LEVEL_FATAL 2
#define MBEDTLS_SSL_ALERT_MSG_HANDSHAKE_FAILURE 40

typedef enum {
    MBEDTLS_SSL_VERSION_UNKNOWN,
    MBEDTLS_SSL_VERSION_TLS1_2 = 0x0303,
    MBEDTLS_SSL_VERSION_TLS1_3 = 0x0304,
} mbedtls_ssl_protocol_version;

typedef int mbedtls_ssl_send_t(void *ctx, const unsigned char *buf, size_t len);
typedef int mbedtls_ssl_recv_t(void *ctx, unsigned char *buf, size_t len);
typedef int mbedtls_ssl_recv_timeout_t(void *ctx, unsigned char *buf, size_t len, uint32_t timeout);

typedef struct mbedtls_ssl_config {
    int amity_initialized;
    int amity_endpoint, amity_transport, amity_preset;  // -1 until mbedtls_ssl_config_defaults
    mbedtls_ssl_protocol_version amity_min_tls_version, amity_max_tls_version;
    const int *amity_ciphersuites;
    int amity_authmode;  // -1 until set
    mbedtls_x509_crt *amity_ca_chain;
    mbedtls_x509_crl *amity_ca_crl;
    int (*amity_verify)(void *, mbedtls_x509_crt *, int, uint32_t *);
    void *amity_verify_context;
    mbedtls_x509_crt *amity_own_cert;
    mbedtls_pk_context *amity_own_key;
    int amity_session_tickets;  // -1 until set
} mbedtls_ssl_config;

typedef struct mbedtls_ssl_context {
    // Bytes of an alert or record still waiting for the socket.
    size_t MBEDTLS_PRIVATE(out_left);
    int amity_initialized;
    const mbedtls_ssl_config *amity_config;  // set by mbedtls_ssl_setup
    char amity_hostname[96];
    int amity_hostname_set;
    void *amity_bio;
    mbedtls_ssl_send_t *amity_send;
    mbedtls_ssl_recv_t *amity_recv;
    mbedtls_ssl_recv_timeout_t *amity_recv_timeout;
    int amity_bio_set;
    int amity_handshaken;          // a handshake call returned 0
    uint32_t amity_verify_result;  // UINT32_MAX until the handshake verified the peer
} mbedtls_ssl_context;

void mbedtls_ssl_init(mbedtls_ssl_context *ssl);
void mbedtls_ssl_free(mbedtls_ssl_context *ssl);
void mbedtls_ssl_config_init(mbedtls_ssl_config *conf);
void mbedtls_ssl_config_free(mbedtls_ssl_config *conf);
int mbedtls_ssl_config_defaults(mbedtls_ssl_config *conf, int endpoint, int transport, int preset);
void mbedtls_ssl_conf_min_tls_version(mbedtls_ssl_config *conf, mbedtls_ssl_protocol_version tls_version);
void mbedtls_ssl_conf_max_tls_version(mbedtls_ssl_config *conf, mbedtls_ssl_protocol_version tls_version);
void mbedtls_ssl_conf_ciphersuites(mbedtls_ssl_config *conf, const int *ciphersuites);
void mbedtls_ssl_conf_authmode(mbedtls_ssl_config *conf, int authmode);
void mbedtls_ssl_conf_ca_chain(mbedtls_ssl_config *conf, mbedtls_x509_crt *ca_chain, mbedtls_x509_crl *ca_crl);
void mbedtls_ssl_conf_verify(mbedtls_ssl_config *conf, int (*f_vrfy)(void *, mbedtls_x509_crt *, int, uint32_t *),
                             void *p_vrfy);
void mbedtls_ssl_conf_session_tickets(mbedtls_ssl_config *conf, int use_tickets);
int mbedtls_ssl_conf_own_cert(mbedtls_ssl_config *conf, mbedtls_x509_crt *own_cert, mbedtls_pk_context *pk_key);
int mbedtls_ssl_setup(mbedtls_ssl_context *ssl, const mbedtls_ssl_config *conf);
int mbedtls_ssl_set_hostname(mbedtls_ssl_context *ssl, const char *hostname);
void mbedtls_ssl_set_bio(mbedtls_ssl_context *ssl, void *p_bio, mbedtls_ssl_send_t *f_send, mbedtls_ssl_recv_t *f_recv,
                         mbedtls_ssl_recv_timeout_t *f_recv_timeout);
int mbedtls_ssl_handshake(mbedtls_ssl_context *ssl);
uint32_t mbedtls_ssl_get_verify_result(const mbedtls_ssl_context *ssl);
int mbedtls_ssl_read(mbedtls_ssl_context *ssl, unsigned char *buf, size_t len);
int mbedtls_ssl_write(mbedtls_ssl_context *ssl, const unsigned char *buf, size_t len);
int mbedtls_ssl_check_pending(const mbedtls_ssl_context *ssl);
int mbedtls_ssl_send_alert_message(mbedtls_ssl_context *ssl, unsigned char level, unsigned char message);

#ifdef __cplusplus
}
#endif
