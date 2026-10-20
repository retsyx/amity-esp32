// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "amity/consortium/session.hpp"
namespace amity::consortium {
struct RelayFragment {
    model::MessageId reservation_id,request_id;model::JoinId join_id;
    uint64_t sequence=0;uint32_t total=0,offset=0;bool encrypted=false;std::string data;
};
bool is_relay_kind(Kind);
Error parse_relay_fragment(const Json &,RelayFragment &);
bool validate_relay_payload(Kind,const Json &);
std::string relay_fragment_json(const RelayFragment &,bool response);
// One bounded opaque record, with identical-prefix duplicates accepted. Scope
// and reservation authorization are checked independently at both endpoints.
class RelayBuffer {
public:
    Error append(const RelayFragment &);
    bool complete()const{return initialized_&&bytes_.size()==fragment_.total;}
    const RelayFragment &fragment()const{return fragment_;}
    const std::string &bytes()const{return bytes_;}
    void reset(){initialized_=false;fragment_={};bytes_.clear();}
private:bool initialized_=false;RelayFragment fragment_;std::string bytes_;
};
class OpaqueRelay {
public:
    Error reserve(const LinkScope &,model::MessageId,model::JoinId,model::OctopusId,uint32_t duration,uint64_t now);
    Error append(const LinkScope &,const RelayFragment &,uint64_t now);
    std::optional<RelayFragment> take_request();
    bool finish(const LinkScope &,model::MessageId,uint64_t,std::string,uint64_t now);
    bool cached()const{return cached_;}
    const std::string &response()const{return response_;}
    const RelayFragment &request()const{return input_.fragment();}
    bool live(const LinkScope &,uint64_t now)const;
    void cancel();
private:
    LinkScope scope_;model::MessageId reservation_;model::JoinId join_;model::OctopusId candidate_;
    uint64_t issued_=0,deadline_=0,highwater_=0;bool forwarded_=false,cached_=false;
    RelayBuffer input_;std::string response_;
};
}
