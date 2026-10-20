#include "amity/consortium/relay.hpp"
#include "amity/consortium/enrollment.hpp"
#include <cassert>
#include <iostream>
using namespace amity::consortium;
template<class T>T ident(char c){return *T::parse(std::string(32,c));}
int main(){
 LinkScope scope;scope.link_id=ident<model::LinkId>('1');scope.local_id=ident<model::OctopusId>('2');scope.peer_id=ident<model::OctopusId>('3');scope.local_boot_id=ident<model::BootId>('4');scope.peer_boot_id=ident<model::BootId>('5');scope.coordinator_id=scope.peer_id;scope.consortium_id=ident<model::ConsortiumId>('6');scope.epoch=8;scope.config_revision=9;
 auto reservation=ident<model::MessageId>('7');auto join=ident<model::JoinId>('8');OpaqueRelay relay;assert(relay.reserve(scope,reservation,join,ident<model::OctopusId>('9'),180000,100)==Error::none);assert(relay.reserve(scope,reservation,join,ident<model::OctopusId>('9'),180000,101)==Error::duplicate);assert(!relay.live(scope,99));
 RelayFragment f;f.reservation_id=reservation;f.join_id=join;f.request_id=ident<model::MessageId>('a');f.sequence=1;f.total=4096;f.encrypted=true;f.data=std::string(2048,'\0');
 assert(relay.append(scope,f,102)==Error::none);assert(!relay.take_request());assert(relay.append(scope,f,103)==Error::duplicate);
 auto wrong=f;wrong.data[0]='x';assert(relay.append(scope,wrong,104)==Error::transaction_conflict);wrong=f;wrong.offset=2049;assert(relay.append(scope,wrong,104)==Error::invalid_message);
 f.offset=2048;f.data=std::string(2048,'\xff');assert(relay.append(scope,f,105)==Error::none);auto request=relay.take_request();assert(request&&request->data==std::string(2048,'\0')+std::string(2048,'\xff'));assert(!relay.take_request());
 std::string ciphertext(4096,'\x81');assert(relay.finish(scope,f.request_id,1,ciphertext,106));assert(relay.cached()&&relay.response()==ciphertext);assert(relay.append(scope,f,107)==Error::duplicate);assert(!relay.take_request()); // Cached replay never calls PAKE/GCM again.
 auto changed=scope;changed.peer_boot_id=ident<model::BootId>('b');assert(relay.append(changed,f,108)==Error::lease_expired);
 for(uint64_t n=2;n<=100;++n){f.sequence=n;f.request_id=*model::MessageId::generate([n](uint8_t *bytes,size_t count){for(size_t i=0;i<count;++i)bytes[i]=uint8_t(n+i);return true;});f.offset=0;f.total=3;f.data="\x01\x02\x03";assert(relay.append(scope,f,100+n)==Error::none);assert(relay.take_request());assert(relay.finish(scope,f.request_id,n,"opaque encrypted response",101+n));}
 auto old=f;old.sequence=1;assert(relay.append(scope,old,250)==Error::duplicate);assert(!relay.take_request());assert(relay.append(scope,f,180100)==Error::lease_expired);
 RelayBuffer buffer;f.offset=0;f.total=3;f.data="abc";assert(buffer.append(f)==Error::none&&buffer.complete());auto json=relay_fragment_json(f,true);Json parsed;JsonError error;assert(parse_json(json,parsed,error)&&validate_relay_payload(Kind::relay_result,parsed));parsed.object.emplace_back("password",Json{});assert(!validate_relay_payload(Kind::relay_result,parsed));
 JoinProposal p;p.join_id=join;p.consortium_id=scope.consortium_id;p.coordinator_id=scope.peer_id;p.candidate_id=ident<model::OctopusId>('c');p.epoch=1;p.revision=3;p.coordinator_leaf="public coordinator leaf";p.candidate_leaf="public candidate leaf";p.coordinator_pin=std::string(64,'a');p.candidate_pin=std::string(64,'b');p.host="192.0.2.9";p.candidate_hostname="media-room";
 const auto proposal=encode_join_proposal(p);JoinProposal decoded;assert(!proposal.empty()&&decode_join_proposal(proposal,decoded)&&decoded.candidate_pin==p.candidate_pin);
 for(const auto &field:{std::string("\"bootstrap_profile\":2"),std::string("\"protocol_min\":1"),std::string("\"protocol_max\":1")}){auto invalid=proposal;auto at=invalid.find(field);assert(at!=std::string::npos);invalid[at+field.size()-1]='3';assert(!decode_join_proposal(invalid,decoded));}
 auto invalid=proposal;invalid.replace(invalid.find("actuator"),8,"coordinator");assert(!decode_join_proposal(invalid,decoded));assert(!decode_join_proposal(proposal+" ",decoded));
 std::cout<<"opaque relay reservation/framing/replay and immutable enrollment tests passed\n";
}
