// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
namespace amity::consortium {
// Assembles one request record at a time for the candidate's GATT server.
// The only client writes each record's fragments in order and never retries,
// so a fragment out of sequence ends the record as invalid.
class BootstrapRecords {
public:
    static constexpr size_t capacity=4096;
    enum class Result { incomplete, complete, stale, invalid };
    void connect(uint64_t generation);
    void disconnect();
    Result fragment(uint64_t generation,uint32_t record_id,size_t total,size_t offset,std::string_view bytes);
    std::string_view request()const{return request_;}
    void finish();
private:
    uint64_t generation_=0;uint32_t record_id_=0;size_t total_=0;
    std::string request_;
    bool connected_=false,processing_=false;
};
inline constexpr uint64_t enrollment_window_ms=180000;
} // namespace amity::consortium
