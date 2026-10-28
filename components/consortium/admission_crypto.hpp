// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
namespace amity::consortium::platform {
enum class AdmissionMode : uint8_t { button=0, setup_code=1 };
AdmissionMode configured_admission_mode();
bool parse_admission_setup_code(std::string_view,uint32_t&);
class AdmissionClient {
public:
    AdmissionClient(AdmissionMode,uint32_t pin=0);
    ~AdmissionClient();
    AdmissionClient(const AdmissionClient&)=delete;
    AdmissionClient& operator=(const AdmissionClient&)=delete;
    bool handshake(const std::function<bool(std::string_view,std::string&)>&);
    const std::string& peer_id()const;
    bool encrypt(std::string_view,std::string&);
    bool decrypt(std::string_view,std::string&);
private:
    struct State;std::unique_ptr<State> state_;
};
class AdmissionServer {
public:
    AdmissionServer(AdmissionMode,std::string own_id,uint32_t pin=0);
    ~AdmissionServer();
    AdmissionServer(const AdmissionServer&)=delete;
    AdmissionServer& operator=(const AdmissionServer&)=delete;
    bool handshake(std::string_view,std::string&);
    bool authenticated()const;
    bool encrypt(std::string_view,std::string&);
    bool decrypt(std::string_view,std::string&);
private:
    struct State;std::unique_ptr<State> state_;
};
bool admission_crypto_self_test();
} // namespace amity::consortium::platform
