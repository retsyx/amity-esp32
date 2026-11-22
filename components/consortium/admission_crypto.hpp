// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <string_view>
namespace amity::consortium::platform {
class AdmissionClient {
public:
    AdmissionClient();
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
    explicit AdmissionServer(std::string own_id);
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
} // namespace amity::consortium::platform
