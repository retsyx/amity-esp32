// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#include "amity/consortium/bootstrap.hpp"
#include <algorithm>
namespace amity::consortium {
void BootstrapRecords::connect(uint64_t generation){disconnect();generation_=generation;connected_=true;}
void BootstrapRecords::disconnect(){connected_=processing_=false;record_id_={};cached_id_={};std::fill(request_.begin(),request_.end(),0);std::fill(response_.begin(),response_.end(),0);std::fill(cached_request_.begin(),cached_request_.end(),0);request_.clear();response_.clear();cached_request_.clear();received_.fill(false);received_count_=0;}
BootstrapRecords::Result BootstrapRecords::fragment(uint64_t generation,model::MessageId rid,size_t total,size_t offset,std::string_view bytes){
    if(!connected_||generation!=generation_)return Result::stale;
    if(!rid||!total||total>capacity||bytes.empty()||offset>total||bytes.size()>total-offset)return Result::invalid;
    if(processing_)return Result::busy;
    if(!record_id_){record_id_=rid;request_.assign(total,0);received_.fill(false);received_count_=0;}
    if(rid!=record_id_)return Result::busy;
    if(total!=request_.size())return Result::conflicting;
    if(rid==cached_id_&&(total!=cached_request_.size()||cached_request_.compare(offset,bytes.size(),bytes)!=0))return Result::conflicting;
    for(size_t i=0;i<bytes.size();++i){const auto at=offset+i;if(received_[at]&&request_[at]!=bytes[i])return Result::conflicting;}
    for(size_t i=0;i<bytes.size();++i){const auto at=offset+i;if(!received_[at]){request_[at]=bytes[i];received_[at]=true;++received_count_;}}
    if(received_count_!=request_.size())return Result::incomplete;
    if(rid==cached_id_){record_id_={};request_.clear();received_.fill(false);received_count_=0;return Result::duplicate;}
    processing_=true;return Result::complete;
}
bool BootstrapRecords::finish(model::MessageId rid,std::string_view response){
    if(!connected_||!processing_||rid!=record_id_||response.empty()||response.size()>capacity)return false;
    cached_id_=rid;cached_request_=std::move(request_);response_.assign(response);record_id_={};processing_=false;received_.fill(false);received_count_=0;return true;
}
bool EnrollmentWindow::open(bool physical,uint64_t now){if(!physical||open_||now>UINT64_MAX-duration_ms)return false;opened_=now;deadline_=now+duration_ms;failed_proofs_=0;open_=true;session_=false;return true;}
bool EnrollmentWindow::available(uint64_t now)const{return open_&&now>=opened_&&now<deadline_&&failed_proofs_<5;}
bool EnrollmentWindow::admit(uint64_t now){if(!available(now)||session_)return false;session_=true;return true;}
void EnrollmentWindow::failed_proof(){if(!open_)return;++failed_proofs_;session_=false;if(failed_proofs_>=5)close();}
void EnrollmentWindow::close(){open_=session_=false;}
} // namespace amity::consortium
