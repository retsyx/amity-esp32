// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#include "amity/consortium/update.hpp"
#include <algorithm>
#include <cstring>
namespace amity::consortium {
namespace {
bool text(const Json* v, std::initializer_list<std::string_view> values) {
    return v && v->type == Json::Type::string && std::find(values.begin(), values.end(), v->string) != values.end();
}
bool hex(const Json* v, size_t length) {
    return v && v->type == Json::Type::string && v->string.size() == length &&
        std::all_of(v->string.begin(), v->string.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
bool release(const Json* v) {
    return v && v->type == Json::Type::string && !v->string.empty() && v->string.size() <= 96 &&
        std::all_of(v->string.begin(), v->string.end(), [](char c) { return (c >= '0' && c <= '9') ||
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '_' || c == '-'; });
}
void append_number(std::string& out,uint64_t value,size_t size) {
    for(size_t n=size;n>0;--n)out+=char(value>>((n-1)*8));
}
uint64_t read_number(std::string_view bytes) {
    uint64_t value=0;for(unsigned char byte:bytes)value=(value<<8)|byte;return value;
}
template<class Id>void append_id(std::string& out,const Id& value) {
    out.append(reinterpret_cast<const char*>(value.bytes().data()),value.bytes().size());
}
template<class Id>bool read_id(std::string_view bytes,Id& out) {
    if(bytes.size()!=16)return false;
    std::array<uint8_t,16> raw;std::memcpy(raw.data(),bytes.data(),raw.size());
    const auto parsed=Id::from_bytes(raw);if(!parsed)return false;out=*parsed;return true;
}
}
bool valid_update_chunk(const UpdateChunk& chunk) {
    return chunk.begin_id&&!chunk.bytes.empty()&&chunk.bytes.size()<=update_chunk_bytes&&
        chunk.offset<=UINT64_MAX-chunk.bytes.size();
}
std::string encode_update_chunk(model::MessageId begin,uint64_t offset,std::string_view bytes) {
    if(!begin||bytes.empty()||bytes.size()>update_chunk_bytes||offset>UINT64_MAX-bytes.size())return {};
    std::string out;out.reserve(24+bytes.size());append_id(out,begin);append_number(out,offset,8);out.append(bytes);
    return out;
}
bool decode_update_chunk(std::string_view encoded,UpdateChunk& out) {
    if(encoded.size()<=24||encoded.size()>24+update_chunk_bytes)return false;
    UpdateChunk chunk;if(!read_id(encoded.substr(0,16),chunk.begin_id))return false;
    chunk.offset=read_number(encoded.substr(16,8));chunk.bytes=encoded.substr(24);
    if(!valid_update_chunk(chunk))return false;
    out=std::move(chunk);return true;
}
std::string update_chunk_envelope(const Envelope& envelope,std::string_view encoded_chunk) {
    UpdateChunk chunk;
    if(envelope.kind!=Kind::update_chunk||!envelope.ttl_ms||envelope.ttl_ms>10000||!decode_update_chunk(encoded_chunk,chunk))return {};
    std::string out="AFU1";out.reserve(16+encoded_chunk.size());
    append_number(out,envelope.config_revision,8);append_number(out,envelope.ttl_ms,4);
    out.append(encoded_chunk);return out;
}
Error parse_update_chunk_envelope(std::string_view bytes,Envelope& out) {
    if(bytes.size()<=40||bytes.size()>40+update_chunk_bytes||bytes.substr(0,4)!="AFU1")return Error::invalid_message;
    Envelope envelope;
    envelope.config_revision=read_number(bytes.substr(4,8));
    envelope.ttl_ms=read_number(bytes.substr(12,4));
    UpdateChunk chunk;
    if(!envelope.ttl_ms||envelope.ttl_ms>10000||!decode_update_chunk(bytes.substr(16),chunk))return Error::invalid_message;
    envelope.kind=Kind::update_chunk;envelope.update_chunk=std::move(chunk);out=std::move(envelope);return Error::none;
}
bool is_update_kind(Kind k) {
    return k == Kind::update_begin || k == Kind::update_chunk || k == Kind::update_install ||
        k == Kind::update_abort || k == Kind::update_result;
}
bool valid_update(Kind k, const Json& v) {
    model::MessageId request;
    if (!id(v.get("request_id"), request)) return false;
    if (k == Kind::update_begin) {
        std::string manifest;
        return closed(v, {"request_id", "canonical_manifest", "signature_hex"}) &&
            json_string(v.get("canonical_manifest"), manifest, 1, 4096) && (hex(v.get("signature_hex"), 128) || hex(v.get("signature_hex"), 0));
    }
    if (k == Kind::update_install) {
        if (text(v.get("phase"), {"stage"})) return closed(v, {"request_id", "release_id", "phase"}) && release(v.get("release_id"));
        uint64_t delay = 0;
        return text(v.get("phase"), {"activate"}) && closed(v, {"request_id", "release_id", "phase", "reboot_delay_ms"}) &&
            release(v.get("release_id")) && decimal(v.get("reboot_delay_ms"), delay) && delay <= 15000;
    }
    if (k == Kind::update_abort) return closed(v, {"request_id", "release_id"}) && release(v.get("release_id"));
    if (k == Kind::update_result)
        return closed(v, {"request_id", "stage"}) && text(v.get("stage"), {"ready", "staged", "installing", "aborted", "rejected"});
    return false; // Image chunks have no JSON representation.
}
}
