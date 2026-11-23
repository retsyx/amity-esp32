/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// The admission handshake and its encrypted records, built from
// admission_crypto.cpp over OpenSSL-backed PSA.
#include "admission_crypto.hpp"
#include "crypto_fakes.hpp"
#include "scenario_main.hpp"
#include <openssl/evp.h>
#include <functional>
#include <string>
#include <string_view>

namespace {

using amity::consortium::platform::AdmissionClient;
using amity::consortium::platform::AdmissionServer;

const std::string id = "00000000000000000000000000000001";

// The hello and welcome a handshake exchanged, and any edit to the server's replies.
struct Exchange {
    std::string hello, welcome;
    std::function<void(std::string &)> edit;
};

bool connect(AdmissionClient &client, AdmissionServer &server, Exchange *seen = nullptr) {
    return client.handshake([&](std::string_view in, std::string &out) {
        if (!server.handshake(in, out)) return false;
        if (seen && seen->hello.empty()) {
            seen->hello = std::string(in);
            seen->welcome = out;
        }
        if (seen && seen->edit) seen->edit(out);
        return true;
    });
}

// Every key and derivation a channel made is gone once its endpoints are.
void check_released() { CHECK(amity_fake::psa_keys() == 0 && amity_fake::psa_derivations() == 0); }

void round_trip() {
    {
        AdmissionServer server(id);
        AdmissionClient client;
        CHECK(connect(client, server) && client.peer_id() == id && server.authenticated());
        // Each side keeps its two directional keys; the ephemeral keys are gone.
        CHECK(amity_fake::psa_keys() == 4 && amity_fake::psa_derivations() == 0);
        std::string request, response, plain;
        CHECK(client.encrypt("client message", request) && server.decrypt(request, plain) && plain == "client message");
        CHECK(server.encrypt("server message", response) && client.decrypt(response, plain) && plain == "server message");
        CHECK(client.encrypt("again", request) && server.decrypt(request, plain) && plain == "again");
    }
    check_released();
}

void replay() {
    {
        AdmissionServer server(id);
        AdmissionClient client;
        CHECK(connect(client, server));
        std::string request, plain;
        CHECK(client.encrypt("client message", request) && server.decrypt(request, plain));
        CHECK(!server.decrypt(request, plain) && !server.authenticated());
        CHECK(!server.decrypt(request, plain));
    }
    check_released();
}

void key_generation_failure() {
    {
        amity_fake::fail_key_generations(1);
        AdmissionServer server(id);
        AdmissionClient client;
        CHECK(!connect(client, server) && !server.authenticated());
    }
    check_released();
}

// One record from the client, altered before the server opens it.
void attack(void (*alter)(std::string &)) {
    {
        AdmissionServer server(id);
        AdmissionClient client;
        CHECK(connect(client, server));
        std::string packet, plain = "stale";
        CHECK(client.encrypt("command", packet));
        alter(packet);
        CHECK(!server.decrypt(packet, plain) && plain.empty() && !server.authenticated());
        std::string next;
        CHECK(client.encrypt("next", next) && !server.decrypt(next, plain));
    }
    check_released();
}

void reflected_direction() { attack([](std::string &packet) { packet[3] ^= 1; }); }
void skipped_sequence() { attack([](std::string &packet) { packet[11] += 1; }); }
void tampered_ciphertext() { attack([](std::string &packet) { packet[12] ^= 1; }); }
void tampered_tag() { attack([](std::string &packet) { packet.back() ^= 1; }); }
void malformed_header() { attack([](std::string &packet) { packet.erase(0, 1); }); }

void cross_session() {
    {
        AdmissionServer first(id), second(id);
        AdmissionClient client, other;
        CHECK(connect(client, first) && connect(other, second));
        std::string packet, plain;
        CHECK(client.encrypt("command", packet) && !second.decrypt(packet, plain) && plain.empty());
    }
    check_released();
}

// A welcome altered in transit gives the two ends different transcripts.
void edited_transcript() {
    {
        AdmissionServer server(id);
        AdmissionClient client;
        Exchange seen;
        seen.edit = [](std::string &out) {
            if (out.size() == 138 && out.compare(0, 8, "AMADMIT4") == 0) out.back() ^= 1;
        };
        CHECK(!connect(client, server, &seen) && seen.welcome.size() == 138);
    }
    check_released();
}

void tampered_confirmation() {
    {
        AdmissionServer server(id);
        AdmissionClient client;
        Exchange seen;
        seen.edit = [](std::string &out) {
            if (out.size() != 138) out.back() ^= 1;
        };
        CHECK(!connect(client, server, &seen) && server.authenticated());
    }
    check_released();
}

void invalid_server_id() {
    for (const std::string &invalid : {std::string(32, '0'), std::string("0000000000000000000000000000000A"),
                                      std::string("000000000000000000000000000000001"), std::string()}) {
        AdmissionServer server(invalid);
        AdmissionClient client;
        CHECK(!connect(client, server) && !server.authenticated());
    }
    check_released();
}

void one_handshake() {
    {
        AdmissionServer server(id), again(id);
        AdmissionClient client;
        CHECK(connect(client, server) && !connect(client, again) && client.peer_id() == id);
        std::string request, plain;
        CHECK(client.encrypt("still", request) && server.decrypt(request, plain) && plain == "still");
    }
    check_released();
}

void record_size_limit() {
    {
        AdmissionServer server(id);
        AdmissionClient client;
        CHECK(connect(client, server));
        const std::string largest(4000, 'x');
        std::string record, plain;
        CHECK(client.encrypt(largest, record) && record.size() == 4028 && server.decrypt(record, plain) && plain == largest);
        CHECK(!client.encrypt(largest + "x", record) && record.empty() && !client.encrypt("", record));
        CHECK(client.encrypt("small", record) && server.decrypt(record, plain) && plain == "small");
    }
    check_released();
}

std::string sha256(std::string_view bytes) {
    unsigned char digest[32];
    unsigned size = 0;
    CHECK(EVP_Digest(bytes.data(), bytes.size(), digest, &size, EVP_sha256(), nullptr) == 1 && size == 32);
    return std::string(reinterpret_cast<const char *>(digest), size);
}

// After confirmation the server takes no further handshake message, even a
// well-formed finish.
void repeated_finish() {
    {
        AdmissionServer server(id);
        AdmissionClient client;
        Exchange seen;
        CHECK(connect(client, server, &seen));
        std::string record, reply;
        const std::string binding = "Amity admission client binding v1:" + sha256(seen.hello + seen.welcome);
        CHECK(client.encrypt(binding, record));
        CHECK(!server.handshake(std::string("AMADMIT4") + char(3) + record, reply) && reply.empty());
        CHECK(!server.authenticated());
    }
    check_released();
}

}  // namespace

int main(int argc, char **argv) {
    return amity_test::run(argc, argv,
                           {{"round-trip", round_trip},
                            {"replay", replay},
                            {"key-generation-failure", key_generation_failure},
                            {"reflected-direction", reflected_direction},
                            {"skipped-sequence", skipped_sequence},
                            {"tampered-ciphertext", tampered_ciphertext},
                            {"tampered-tag", tampered_tag},
                            {"malformed-header", malformed_header},
                            {"cross-session", cross_session},
                            {"edited-transcript", edited_transcript},
                            {"tampered-confirmation", tampered_confirmation},
                            {"invalid-server-id", invalid_server_id},
                            {"one-handshake", one_handshake},
                            {"record-size-limit", record_size_limit},
                            {"repeated-finish", repeated_finish}});
}
