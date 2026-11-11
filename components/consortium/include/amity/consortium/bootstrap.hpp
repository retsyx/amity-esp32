// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "amity/consortium/wire.hpp"
#include <array>
namespace amity::consortium {
// Bounded shared-host record layer for the authenticated bootstrap exchange.
class BootstrapRecords {
public:
    static constexpr size_t capacity=4096;
    enum class Result { incomplete, complete, duplicate, stale, invalid, conflicting, busy };
    void connect(uint64_t generation);
    void disconnect();
    Result fragment(uint64_t generation,model::MessageId record_id,size_t total,size_t offset,std::string_view bytes);
    std::string_view request()const{return request_;}
    std::string_view cached_response()const{return response_;}
    bool finish(model::MessageId record_id,std::string_view ciphertext_response);
private:
    uint64_t generation_=0;model::MessageId record_id_,cached_id_;
    std::string request_,response_,cached_request_;
    std::array<bool,capacity> received_{};size_t received_count_=0;
    bool connected_=false,processing_=false;
};
inline constexpr uint64_t enrollment_window_ms=180000;
class CipherCounter {
public:
    explicit CipherCounter(uint32_t value=1):value_(value){}
    // Consume only for a complete serialized crypto operation. Duplicate
    // records use BootstrapRecords' cached ciphertext and never consume again.
    std::optional<uint32_t> consume(){if(!value_||value_==UINT32_MAX)return {};return value_++;}
private:uint32_t value_;
};
} // namespace amity::consortium
