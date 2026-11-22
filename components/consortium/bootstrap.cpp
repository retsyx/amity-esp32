// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#include "amity/consortium/bootstrap.hpp"
#include <algorithm>
namespace amity::consortium {
void BootstrapRecords::connect(uint64_t generation){disconnect();generation_=generation;connected_=true;}
void BootstrapRecords::disconnect(){finish();connected_=false;}
BootstrapRecords::Result BootstrapRecords::fragment(uint64_t generation,uint32_t rid,size_t total,size_t offset,std::string_view bytes){
    if(!connected_||generation!=generation_)return Result::stale;
    if(processing_||!rid||!total||total>capacity||bytes.empty()||offset>total||bytes.size()>total-offset)return Result::invalid;
    if(!record_id_){if(offset)return Result::invalid;record_id_=rid;total_=total;}
    if(rid!=record_id_||total!=total_||offset!=request_.size())return Result::invalid;
    request_.append(bytes);
    if(request_.size()!=total_)return Result::incomplete;
    processing_=true;return Result::complete;
}
void BootstrapRecords::finish(){std::fill(request_.begin(),request_.end(),0);request_.clear();record_id_=0;total_=0;processing_=false;}
} // namespace amity::consortium
