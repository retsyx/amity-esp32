/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// Strict seams for the admission server of components/consortium/admission_crypto.cpp,
// for GATT harnesses that script the channel instead of running its crypto.
// Never link them with admission_crypto.cpp. The constructor and destructor
// only record their calls.
#include "admission_crypto.hpp"
#include "seams.hpp"

using amity::consortium::platform::AdmissionServer;

struct AdmissionServer::State {};

AdmissionServer::AdmissionServer(std::string) : state_(new State) {
    amity_seam::record("AdmissionServer::AdmissionServer");
}
AdmissionServer::~AdmissionServer() { amity_seam::record("AdmissionServer::~AdmissionServer"); }
bool AdmissionServer::handshake(std::string_view input, std::string &output) {
    return AMITY_SEAM(AdmissionServer::handshake, *this, input, output);
}
bool AdmissionServer::authenticated() const { return AMITY_SEAM(AdmissionServer::authenticated, *this); }
bool AdmissionServer::encrypt(std::string_view input, std::string &output) {
    return AMITY_SEAM(AdmissionServer::encrypt, *this, input, output);
}
bool AdmissionServer::decrypt(std::string_view input, std::string &output) {
    return AMITY_SEAM(AdmissionServer::decrypt, *this, input, output);
}
