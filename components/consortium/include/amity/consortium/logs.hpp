// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "amity/consortium/wire.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <memory>

namespace amity::consortium {
inline constexpr size_t log_capacity=24576;
inline constexpr size_t log_chunk_bytes=4096;
struct LogSnapshot {
    std::shared_ptr<const char> bytes;
    size_t size=0;
    explicit operator bool() const{return bool(bytes);}
};
// Frozen with the update messages, so a member running other firmware stays
// readable. The main sends log_request {"transfer_id"}; the member answers
// with ordered log_result chunks of its log as it was at that moment, sent as
// bulk frames like firmware chunks. Integers are big-endian.
// Chunk: transfer ID (16), offset (4), total (4), status (1: 0 carries log
// bytes, 1 reports the log unavailable), then up to log_chunk_bytes bytes.
// Frame: ALG1 (4), configuration revision (8), TTL (4), chunk.
inline bool is_log_kind(Kind kind){return kind==Kind::log_request||kind==Kind::log_result;}
inline bool valid_log_request(const Json& value){model::MessageId transfer;return closed(value,{"transfer_id"})&&id(value.get("transfer_id"),transfer);}
inline std::string log_request_json(model::MessageId transfer){return transfer?"{\"transfer_id\":\""+transfer.str()+"\"}":std::string{};}
inline bool valid_log_chunk(const LogChunk& chunk){
    if(!chunk.transfer||chunk.total>log_capacity||chunk.bytes.size()>log_chunk_bytes)return false;
    if(chunk.unavailable)return !chunk.offset&&!chunk.total&&chunk.bytes.empty();
    return chunk.offset<=chunk.total&&chunk.bytes.size()<=chunk.total-chunk.offset&&(!chunk.bytes.empty()||chunk.offset==chunk.total);
}
namespace log_detail {
inline void append(std::string& out,uint64_t value,size_t size){for(size_t n=size;n>0;--n)out+=char(value>>((n-1)*8));}
inline uint64_t read(std::string_view bytes){uint64_t value=0;for(unsigned char byte:bytes)value=(value<<8)|byte;return value;}
inline constexpr size_t chunk_header=25,frame_header=16;
}
inline std::string encode_log_chunk(const LogChunk& chunk){
    if(!valid_log_chunk(chunk))return {};
    std::string out;out.reserve(log_detail::chunk_header+chunk.bytes.size());
    out.append(reinterpret_cast<const char*>(chunk.transfer.bytes().data()),chunk.transfer.bytes().size());
    log_detail::append(out,chunk.offset,4);log_detail::append(out,chunk.total,4);out+=char(chunk.unavailable?1:0);
    return out+chunk.bytes;
}
inline bool decode_log_chunk(std::string_view encoded,LogChunk& out){
    if(encoded.size()<log_detail::chunk_header||encoded.size()>log_detail::chunk_header+log_chunk_bytes)return false;
    std::array<uint8_t,16> raw;std::memcpy(raw.data(),encoded.data(),raw.size());const auto transfer=model::MessageId::from_bytes(raw);
    const auto status=static_cast<unsigned char>(encoded[24]);if(!transfer||status>1)return false;
    LogChunk chunk;chunk.transfer=*transfer;chunk.offset=static_cast<uint32_t>(log_detail::read(encoded.substr(16,4)));
    chunk.total=static_cast<uint32_t>(log_detail::read(encoded.substr(20,4)));chunk.unavailable=status==1;chunk.bytes=encoded.substr(log_detail::chunk_header);
    if(!valid_log_chunk(chunk))return false;
    out=std::move(chunk);return true;
}
inline std::string log_chunk_envelope(const Envelope& envelope,std::string_view encoded_chunk){
    LogChunk chunk;
    if(envelope.kind!=Kind::log_result||!envelope.ttl_ms||envelope.ttl_ms>10000||!decode_log_chunk(encoded_chunk,chunk))return {};
    std::string out="ALG1";out.reserve(log_detail::frame_header+encoded_chunk.size());
    log_detail::append(out,envelope.config_revision,8);log_detail::append(out,envelope.ttl_ms,4);
    out.append(encoded_chunk);return out;
}
inline Error parse_log_chunk_envelope(std::string_view bytes,Envelope& out){
    if(bytes.size()<log_detail::frame_header||bytes.substr(0,4)!="ALG1")return Error::invalid_message;
    Envelope envelope;envelope.config_revision=log_detail::read(bytes.substr(4,8));envelope.ttl_ms=static_cast<uint32_t>(log_detail::read(bytes.substr(12,4)));
    LogChunk chunk;
    if(!envelope.ttl_ms||envelope.ttl_ms>10000||!decode_log_chunk(bytes.substr(log_detail::frame_header),chunk))return Error::invalid_message;
    envelope.kind=Kind::log_result;envelope.log_chunk=std::move(chunk);out=std::move(envelope);return Error::none;
}
}
