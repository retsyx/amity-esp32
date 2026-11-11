// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "amity/consortium/wire.hpp"
#include <algorithm>
#include <memory>

namespace amity::consortium {
inline constexpr size_t log_capacity=24576;
inline constexpr size_t log_chunk_bytes=1024;
struct LogSnapshot {
    std::shared_ptr<const char> bytes;
    size_t size=0;
    explicit operator bool() const{return bool(bytes);}
};
namespace log_detail {
inline bool error_name(std::string_view value){return value.empty()||value=="busy"||value=="unavailable"||value=="expired";}
inline bool valid_result(size_t offset,size_t total,size_t bytes,std::string_view error){
    if(offset>log_capacity||total>log_capacity||bytes>log_chunk_bytes||!error_name(error))return false;
    if(!error.empty())return !total&&!bytes;
    return offset<=total&&bytes<=total-offset&&(bytes||offset==total);
}
}
inline bool is_log_kind(Kind kind){return kind==Kind::log_request||kind==Kind::log_result;}
inline bool valid_log_payload(Kind kind,const Json& value){
    model::MessageId transfer;uint64_t offset=0;
    if(!id(value.get("transfer_id"),transfer)||!json_uint(value.get("offset"),offset,0,log_capacity))return false;
    if(kind==Kind::log_request){bool close=false;return closed(value,{"transfer_id","offset","close"})&&json_bool(value.get("close"),close);}
    if(kind!=Kind::log_result||!closed(value,{"transfer_id","offset","total","data","error"}))return false;
    uint64_t total=0;
    const auto* data=value.get("data");const auto* error=value.get("error");
    if(!json_uint(value.get("total"),total,0,log_capacity)||data->type!=Json::Type::string||error->type!=Json::Type::string||
       data->string.size()>log_chunk_bytes*2||data->string.size()%2||
       !std::all_of(data->string.begin(),data->string.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');}))return false;
    return log_detail::valid_result(offset,total,data->string.size()/2,error->string);
}
inline std::string log_request_json(model::MessageId transfer,size_t offset,bool close=false){
    if(!transfer||offset>log_capacity)return {};
    return "{\"transfer_id\":\""+transfer.str()+"\",\"offset\":"+std::to_string(offset)+",\"close\":"+(close?"true":"false")+"}";
}
inline std::string log_result_json(model::MessageId transfer,size_t offset,size_t total,std::string_view bytes,std::string_view error=""){
    if(!transfer||!log_detail::valid_result(offset,total,bytes.size(),error))return {};
    constexpr char digits[]="0123456789abcdef";
    std::string data;data.reserve(bytes.size()*2);
    for(unsigned char byte:bytes){data+=digits[byte>>4];data+=digits[byte&15];}
    return "{\"transfer_id\":\""+transfer.str()+"\",\"offset\":"+std::to_string(offset)+",\"total\":"+std::to_string(total)+",\"data\":\""+data+"\",\"error\":\""+std::string(error)+"\"}";
}
}
