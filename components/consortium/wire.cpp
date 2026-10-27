// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#include "amity/consortium/wire.hpp"
#include "amity/consortium/relay.hpp"
#include "amity/consortium/control.hpp"
#include "amity/consortium/transfer.hpp"
#include "amity/consortium/update.hpp"
#include "amity/consortium/hostname.hpp"
#include "amity/consortium/settings.hpp"
#include <algorithm>

namespace amity::consortium {
namespace {
bool text_is(const Json *v, std::initializer_list<std::string_view> allowed) {return v&&v->type==Json::Type::string&&std::find(allowed.begin(),allowed.end(),v->string)!=allowed.end();}
bool hex(const Json *v,size_t length) {return v&&v->type==Json::Type::string&&v->string.size()==length&&std::all_of(v->string.begin(),v->string.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');});}
bool generic_id(const Json *v) { model::MessageId ignored;return id(v,ignored); }
bool uint_field(const Json &v,std::string_view key,uint64_t low,uint64_t high) {uint64_t n;return json_uint(v.get(key),n,low,high);}
bool bool_field(const Json &v,std::string_view key) {bool b;return json_bool(v.get(key),b);}
bool decimal_field(const Json &v,std::string_view key) {uint64_t n;return decimal(v.get(key),n);}
bool ids(const Json &v,std::initializer_list<std::string_view> keys) {for(auto key:keys)if(!generic_id(v.get(key)))return false;return true;}
bool binding(const Json &v) {
    if(!closed(v,{"binding_id","component_id","capability","binding_kind","endpoint","owner_octopus_id","domain_id","domain_revision","binding_revision","role","physical_address_evidence_ref","status"},{"proxy_identity_resource_id","device_name"}))return false;
    if(!ids(v,{"binding_id","component_id","owner_octopus_id","domain_id","physical_address_evidence_ref"})||!decimal_field(v,"domain_revision")||!decimal_field(v,"binding_revision"))return false;
    if(!text_is(v.get("capability"),{"power","navigation","audio","routing","query_state","proxy_activation"})||!text_is(v.get("role"),{"playback_proxy","tv_controller","monitor","disabled"})||!text_is(v.get("status"),{"unresolved","tentative","verified","quarantined","retired"}))return false;
    model::FullyQualifiedCecAddress endpoint;if(!parse_address(*v.get("endpoint"),endpoint))return false;
    model::OctopusId owner;if(!id(v.get("owner_octopus_id"),owner)||owner!=endpoint.port.octopus_id)return false;
    if(text_is(v.get("binding_kind"),{"proxy_representation"}))return generic_id(v.get("proxy_identity_resource_id"))&&!v.get("device_name")&&text_is(v.get("role"),{"playback_proxy"})&&text_is(v.get("capability"),{"proxy_activation"});
    if(!text_is(v.get("binding_kind"),{"native_appliance"})||v.get("proxy_identity_resource_id")||!v.get("device_name"))return false;
    model::QualifiedDeviceName name;return parse_name(*v.get("device_name"),name)&&name.port==endpoint.port;
}
bool binding_update(const Json &v) {
    if(!closed(v,{"resolution_id","device_name","observation_boot_id","observation_sequence","bindings"})||!ids(v,{"resolution_id","observation_boot_id"})||!decimal_field(v,"observation_sequence"))return false;
    model::QualifiedDeviceName name;if(!parse_name(*v.get("device_name"),name))return false;
    const auto *entries=v.get("bindings");if(entries->type!=Json::Type::array||entries->array.empty()||entries->array.size()>8)return false;
    std::vector<model::BindingId> seen;
    for(const auto &entry:entries->array) {
        if(!closed(entry,{"expected_binding_revision","binding"})||!decimal_field(entry,"expected_binding_revision")||!binding(*entry.get("binding")))return false;
        const auto &next=*entry.get("binding");model::QualifiedDeviceName next_name;if(!next.get("device_name")||!parse_name(*next.get("device_name"),next_name)||next_name!=name)return false;
        model::BindingId bid;id(next.get("binding_id"),bid);if(std::find(seen.begin(),seen.end(),bid)!=seen.end())return false;seen.push_back(bid);
        uint64_t before=0,after=0;decimal(entry.get("expected_binding_revision"),before);decimal(next.get("binding_revision"),after);if(after<=before)return false;
    }
    return true;
}
bool binding_result(const Json &v) {
    if(!closed(v,{"resolution_id","result","bindings"},{"error"})||!generic_id(v.get("resolution_id"))||!text_is(v.get("result"),{"applied","rejected"}))return false;
    const auto *entries=v.get("bindings");if(entries->type!=Json::Type::array||entries->array.size()>8)return false;
    std::vector<model::BindingId> seen;
    for(const auto &entry:entries->array){
        model::BindingId binding_id;
        if(!closed(entry,{"binding_id","binding_revision"})||!id(entry.get("binding_id"),binding_id)||!decimal_field(entry,"binding_revision"))return false;
        if(std::find(seen.begin(),seen.end(),binding_id)!=seen.end())return false;
        seen.push_back(binding_id);
    }
    if(text_is(v.get("result"),{"applied"}))return !entries->array.empty()&&!v.get("error");
    return entries->array.empty()&&text_is(v.get("error"),{"STALE_BINDING","STALE_DOMAIN","STALE_BOOT","STALE_CONFIG","INVALID_MESSAGE","NOT_READY"});
}
bool enrollment(const Json &v) {
    if(!ids(v,{"join_id"})||!hex(v.get("join_digest"),64))return false;
    const auto *phase=v.get("phase");
    if(text_is(phase,{"challenge","prove"}))return closed(v,{"phase","join_id","join_digest","challenge_nonce"})&&generic_id(v.get("challenge_nonce"));
    if(text_is(phase,{"commit"}))return closed(v,{"phase","join_id","join_digest","membership_revision","ready"})&&decimal_field(v,"membership_revision")&&bool_field(v,"ready")&&!v.get("ready")->boolean;
    if(text_is(phase,{"ack"}))return closed(v,{"phase","join_id","join_digest","membership_revision","state"})&&decimal_field(v,"membership_revision")&&text_is(v.get("state"),{"active"});
    if(text_is(phase,{"abort"}))return closed(v,{"phase","join_id","join_digest","reason"})&&text_is(v.get("reason"),{"cancelled","incompatible","network_failed","internal_error"});
    if(!text_is(phase,{"status"})||!closed(v,{"phase","join_id","join_digest","request"},{"state","membership_revision"})||!bool_field(v,"request"))return false;
    if(v.get("request")->boolean)return !v.get("state")&&!v.get("membership_revision");
    if(!text_is(v.get("state"),{"prepared","committed","active","aborted","unknown"}))return false;
    return text_is(v.get("state"),{"committed","active"})?decimal_field(v,"membership_revision"):!v.get("membership_revision");
}
bool membership(const Json &v) {
    return closed(v,{"mutation_id","proposal_digest","phase","membership_revision","expected_boot_id"},{"result"})&&generic_id(v.get("expected_boot_id"))&&generic_id(v.get("mutation_id"))&&hex(v.get("proposal_digest"),64)&&decimal_field(v,"membership_revision")&&text_is(v.get("phase"),{"factory_reset"})&&(!v.get("result")||text_is(v.get("result"),{"reset_scheduled","rejected","unknown"}));
}
bool payload(Kind kind,const Json &v) {
    switch(kind) {
    case Kind::relay_reserve:case Kind::relay_record:case Kind::relay_close:case Kind::relay_result:return validate_relay_payload(kind,v);
    case Kind::settings_read:case Kind::settings_write:case Kind::settings_result:return valid_settings_payload(kind,v);
    case Kind::device_status:return closed(v,{"hostname"})&&v.get("hostname")->type==Json::Type::string&&valid_hostname(v.get("hostname")->string);
    case Kind::heartbeat:return closed(v,{"sequence","observation_cursor","ready"})&&decimal_field(v,"sequence")&&decimal_field(v,"observation_cursor")&&bool_field(v,"ready");
    case Kind::operation:{Operation out;return parse_operation(v,out)==Error::none;}
    case Kind::execution_lease:return closed(v,{"lease_id","link_id","class","validity_ms"})&&ids(v,{"lease_id","link_id"})&&text_is(v.get("class"),{"control","realtime_input"})&&uint_field(v,"validity_ms",1,text_is(v.get("class"),{"realtime_input"})?2000:10000);
    case Kind::hold_release:return closed(v,{"plan_id","session_id","hold_token","route_generation"})&&ids(v,{"plan_id","session_id","hold_token"})&&decimal_field(v,"route_generation");
    case Kind::binding_update:return binding_update(v);
    case Kind::binding_result:return binding_result(v);
    case Kind::enrollment_control:return enrollment(v);
    case Kind::membership_event:return membership(v)&&!v.get("result");
    case Kind::membership_result:return membership(v)&&v.get("result");
    case Kind::snapshot_request:{SnapshotManifest manifest;std::string phase;uint32_t next=0;return parse_snapshot_manifest(v,manifest,phase,next)==Error::none;}
    case Kind::snapshot_chunk:{
        if(!closed(v,{"transfer_id","index","total_length","payload_hash","data"})||!generic_id(v.get("transfer_id"))||!uint_field(v,"index",0,15)||!uint_field(v,"total_length",1,32768)||!hex(v.get("payload_hash"),64))return false;
        const auto *data=v.get("data");return data&&data->type==Json::Type::string&&!data->string.empty()&&data->string.size()<=4096&&data->string.size()%2==0&&hex(data,data->string.size());
    }
    case Kind::config_prepare:case Kind::config_activate:case Kind::config_result:return valid_config_control(kind,v);
    case Kind::plan_prepare:{PlanPrepare p;return parse_plan_prepare(v,p)==Error::none;}
    case Kind::plan_barrier:{PlanBarrier p;return parse_plan_barrier(v,p)==Error::none;}
    case Kind::plan_abort:{PlanContext p;return parse_plan_context(v,p)==Error::none;}
    case Kind::plan_result:{PlanResult p;return parse_plan_result(v,p)==Error::none;}
    case Kind::operation_result:return valid_operation_result(v);
    case Kind::scan_request:return closed(v,{"request_id","execution_lease_id"})&&ids(v,{"request_id","execution_lease_id"});
    case Kind::scan_result:return closed(v,{"request_id","result","error"})&&generic_id(v.get("request_id"))&&text_is(v.get("result"),{"admitted","rejected"})&&text_is(v.get("error"),{"","NOT_READY","LEASE_EXPIRED"});
    case Kind::update_begin:case Kind::update_chunk:case Kind::update_install:case Kind::update_status:case Kind::update_abort:case Kind::update_result:return valid_update(kind,v);
    }
    return false;
}
}
const char *error_name(Error v) {
    switch(v) {
#define E(a,b) case Error::a:return b
    E(none,"OK");E(invalid_message,"INVALID_MESSAGE");E(unsupported_kind,"UNSUPPORTED_CAPABILITY");E(protocol_mismatch,"PROTOCOL_MISMATCH");E(unauthorized_role,"UNAUTHORIZED_ROLE");E(wrong_consortium,"WRONG_CONSORTIUM");E(stale_boot,"STALE_BOOT");E(stale_epoch,"STALE_EPOCH");E(stale_config,"STALE_CONFIG");E(stale_binding,"STALE_BINDING");E(stale_domain,"STALE_DOMAIN");E(stale_route,"STALE_ROUTE");E(lease_expired,"LEASE_EXPIRED");E(target_not_local,"TARGET_NOT_LOCAL");E(not_ready,"NOT_READY");E(queue_full,"QUEUE_FULL");E(duplicate,"DUPLICATE");E(identity_conflict,"IDENTITY_CONFLICT");E(unknown_outcome,"UNKNOWN_OUTCOME");E(storage_failure,"STORAGE_FAILURE");E(transaction_conflict,"TRANSACTION_CONFLICT");
#undef E
    }return "INVALID_MESSAGE";
}
const char *kind_name(Kind k) {switch(k) {
#define K(a) case Kind::a:return #a
K(relay_reserve);K(relay_record);K(relay_close);K(relay_result);K(heartbeat);K(execution_lease);K(operation);K(hold_release);K(enrollment_control);K(binding_update);K(binding_result);K(membership_event);K(membership_result);
K(snapshot_request);K(snapshot_chunk);K(config_prepare);K(config_activate);K(config_result);K(plan_prepare);K(plan_barrier);K(plan_abort);K(plan_result);K(operation_result);K(update_begin);K(update_chunk);K(update_install);K(update_status);K(update_abort);K(update_result);K(scan_request);K(scan_result);K(device_status);K(settings_read);K(settings_write);K(settings_result);
#undef K
}return "";}
bool decimal(const Json *v,uint64_t &out) {if(!v||v->type!=Json::Type::string)return false;const auto parsed=model::parse_uint64(v->string);if(!parsed)return false;out=*parsed;return true;}
bool parse_address(const Json &v,model::FullyQualifiedCecAddress &out) {
    if(!closed(v,{"consortium_id","octopus_id","tentacle_id","side","logical_address"}))return false;
    uint64_t tentacle=0,la=0;
    if(!id(v.get("consortium_id"),out.port.consortium_id)||!id(v.get("octopus_id"),out.port.octopus_id)||!json_uint(v.get("tentacle_id"),tentacle,0,amity::last_tentacle_id)||!json_uint(v.get("logical_address"),la,0,14)||!text_is(v.get("side"),{"front","back"}))return false;
    out.port.tentacle_id=tentacle;out.logical_address=la;out.port.side=text_is(v.get("side"),{"front"})?model::Side::front:model::Side::back;return out.valid();
}
bool parse_name(const Json &v,model::QualifiedDeviceName &out) {
    if(!closed(v,{"consortium_id","octopus_id","tentacle_id","side","osd_name"}))return false;
    uint64_t tentacle=0;if(!id(v.get("consortium_id"),out.port.consortium_id)||!id(v.get("octopus_id"),out.port.octopus_id)||!json_uint(v.get("tentacle_id"),tentacle,0,amity::last_tentacle_id)||!text_is(v.get("side"),{"front","back"})||!json_string(v.get("osd_name"),out.osd_name,1,14))return false;
    out.port.tentacle_id=tentacle;out.port.side=text_is(v.get("side"),{"front"})?model::Side::front:model::Side::back;return out.valid();
}
Error parse_operation(const Json &v,Operation &out) {
    if(!closed(v,{"command_id","plan_id","session_id","component_id","binding_id","domain_id","execution_lease_id","route_generation","binding_revision","domain_revision","target","operation","arguments","command_sequence"}))return Error::invalid_message;
    Operation x;
    if(!decimal(v.get("command_sequence"),x.command_sequence)||!x.command_sequence)return Error::invalid_message;
    if(!id(v.get("command_id"),x.command_id)||!id(v.get("plan_id"),x.plan_id)||!id(v.get("session_id"),x.session_id)||!id(v.get("component_id"),x.component_id)||!id(v.get("binding_id"),x.binding_id)||!id(v.get("domain_id"),x.domain_id)||!id(v.get("execution_lease_id"),x.execution_lease_id)||!decimal(v.get("route_generation"),x.route_generation)||!decimal(v.get("binding_revision"),x.binding_revision)||!decimal(v.get("domain_revision"),x.domain_revision)||!parse_address(*v.get("target"),x.target))return Error::invalid_message;
    const auto &a=*v.get("arguments");const auto *op=v.get("operation");
    if(text_is(op,{"ensure_power"})) {x.operation=SemanticOperation::ensure_power;if(!closed(a,{"on"})||!json_bool(a.get("on"),x.on))return Error::invalid_message;}
    else if(text_is(op,{"select_route"})) {x.operation=SemanticOperation::select_route;if(!closed(a,{"route_id"})||!id(a.get("route_id"),x.route_id))return Error::invalid_message;}
    else if(text_is(op,{"announce_active_source"})) {x.operation=SemanticOperation::announce_active_source;if(!closed(a,{}))return Error::invalid_message;}
    else if(text_is(op,{"key_press"})) {x.operation=SemanticOperation::key_press;uint64_t code=0;if(!closed(a,{"key_code","hold_token","repeat"})||!json_uint(a.get("key_code"),code,0,255)||!id(a.get("hold_token"),x.hold_token)||!json_bool(a.get("repeat"),x.repeat))return Error::invalid_message;x.key_code=code;}
    else if(text_is(op,{"query_state"})) {x.operation=SemanticOperation::query_state;if(!closed(a,{"field"})||!text_is(a.get("field"),{"power","audio"}))return Error::invalid_message;x.query_field=a.get("field")->string;}
    else return Error::invalid_message;
    out=std::move(x);return Error::none;
}
Error parse_envelope(std::string_view body,Envelope &out,JsonError *detail) {
    Json v;JsonError e;if(!parse_json(body,v,e)){if(detail)*detail=e;return Error::invalid_message;}
    if(!closed(v,{"protocol_version","consortium_id","message_id","origin_octopus_id","origin_boot_id","destination_octopus_id","destination_boot_id","coordinator_epoch","config_revision","kind","ttl_ms","payload"}))return Error::invalid_message;
    uint64_t version=0,ttl=0;if(!json_uint(v.get("protocol_version"),version,1,1))return Error::protocol_mismatch;
    Envelope x;
    if(!id(v.get("consortium_id"),x.consortium_id)||!id(v.get("message_id"),x.message_id)||!id(v.get("origin_octopus_id"),x.origin_octopus_id)||!id(v.get("origin_boot_id"),x.origin_boot_id)||!id(v.get("destination_octopus_id"),x.destination_octopus_id)||!id(v.get("destination_boot_id"),x.destination_boot_id)||!decimal(v.get("coordinator_epoch"),x.coordinator_epoch)||!decimal(v.get("config_revision"),x.config_revision)||!json_uint(v.get("ttl_ms"),ttl,1,10000))return Error::invalid_message;
    bool recognized=false;for(auto kind:{Kind::relay_reserve,Kind::relay_record,Kind::relay_close,Kind::relay_result,Kind::heartbeat,Kind::execution_lease,Kind::operation,Kind::hold_release,Kind::enrollment_control,Kind::binding_update,Kind::binding_result,Kind::membership_event,Kind::membership_result,Kind::snapshot_request,Kind::snapshot_chunk,Kind::config_prepare,Kind::config_activate,Kind::config_result,Kind::plan_prepare,Kind::plan_barrier,Kind::plan_abort,Kind::plan_result,Kind::operation_result,Kind::update_begin,Kind::update_chunk,Kind::update_install,Kind::update_status,Kind::update_abort,Kind::update_result,Kind::scan_request,Kind::scan_result,Kind::device_status,Kind::settings_read,Kind::settings_write,Kind::settings_result})if(text_is(v.get("kind"),{kind_name(kind)})){x.kind=kind;recognized=true;break;}
    if(!recognized||retired_kind(x.kind))return Error::unsupported_kind;
    if((x.kind==Kind::membership_event||x.kind==Kind::membership_result)&&ttl>2000)return Error::invalid_message;
    if(!payload(x.kind,*v.get("payload")))return Error::invalid_message;
    x.ttl_ms=ttl;x.payload=std::move(*v.get("payload"));
    if(x.kind==Kind::operation){Operation op;parse_operation(x.payload,op);if(op.target.port.consortium_id!=x.consortium_id||op.target.port.octopus_id!=x.destination_octopus_id)return Error::target_not_local;}
    out=std::move(x);return Error::none;
}
std::string hello_json(const HelloFrame &h){
    // Firmware is the fixed lowercase ELF digest supplied by runtime. The
    // optional transaction pair never authorizes an ordinary execution session.
    if(h.firmware_identity.size()!=64||!std::all_of(h.firmware_identity.begin(),h.firmware_identity.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');}))return {};
    std::string out="{\"type\":\"hello\",\"protocol_min\":1,\"protocol_max\":1,\"consortium_id\":\""+h.consortium_id.str()+"\",\"octopus_id\":\""+h.octopus_id.str()+"\",\"boot_id\":\""+h.boot_id.str()+"\",\"coordinator_epoch\":\""+std::to_string(h.coordinator_epoch)+"\",\"config_revision\":\""+std::to_string(h.config_revision)+"\",\"config_hash\":\""+h.config_hash+"\",\"firmware_identity\":\""+h.firmware_identity+"\",\"required_capabilities\":"+std::to_string(h.required_capabilities)+",\"optional_capabilities\":"+std::to_string(h.optional_capabilities)+",\"frame_cap\":8192";
    if(h.pending_join_id)out+=",\"pending_join_id\":\""+h.pending_join_id->str()+"\",\"join_digest\":\""+h.join_digest+"\"";
    if(h.hardware_profile){if(!h.supported_tentacles||h.supported_tentacles>amity::max_tentacles||h.configured_tentacles_mask>=(1u<<h.supported_tentacles))return {};out+=",\"supported_tentacles\":"+std::to_string(h.supported_tentacles)+",\"configured_tentacles_mask\":"+std::to_string(h.configured_tentacles_mask);}
    return out+"}";
}
Error parse_hello(std::string_view body,HelloFrame &out,JsonError *detail) {
    Json v;JsonError e;if(!parse_json(body,v,e)){if(detail)*detail=e;return Error::invalid_message;}
    if(!closed(v,{"type","protocol_min","protocol_max","consortium_id","octopus_id","boot_id","coordinator_epoch","config_revision","config_hash","firmware_identity","required_capabilities","optional_capabilities","frame_cap"},{"pending_join_id","join_digest","supported_tentacles","configured_tentacles_mask"})||!text_is(v.get("type"),{"hello"}))return Error::invalid_message;
    HelloFrame x;uint64_t low=0,high=0,required=0,optional=0,cap=0;
    if(!json_uint(v.get("protocol_min"),low,1,255)||!json_uint(v.get("protocol_max"),high,low,255)||!id(v.get("consortium_id"),x.consortium_id)||!id(v.get("octopus_id"),x.octopus_id)||!id(v.get("boot_id"),x.boot_id)||!decimal(v.get("coordinator_epoch"),x.coordinator_epoch)||!decimal(v.get("config_revision"),x.config_revision)||!hex(v.get("config_hash"),64)||!json_string(v.get("firmware_identity"),x.firmware_identity,1,96)||!json_uint(v.get("required_capabilities"),required,0,UINT32_MAX)||(required&~known_capabilities)||!json_uint(v.get("optional_capabilities"),optional,0,UINT32_MAX)||!json_uint(v.get("frame_cap"),cap,512,max_frame_bytes))return Error::invalid_message;
    if(v.get("pending_join_id")||v.get("join_digest")){model::JoinId join;if(!id(v.get("pending_join_id"),join)||!hex(v.get("join_digest"),64))return Error::invalid_message;x.pending_join_id=join;x.join_digest=v.get("join_digest")->string;}
    if(v.get("supported_tentacles")||v.get("configured_tentacles_mask")){uint64_t count=0,mask=0;if(!json_uint(v.get("supported_tentacles"),count,1,amity::max_tentacles)||!json_uint(v.get("configured_tentacles_mask"),mask,0,(1u<<count)-1))return Error::invalid_message;x.supported_tentacles=count;x.configured_tentacles_mask=mask;x.hardware_profile=true;}
    if(low>1||high<1)return Error::protocol_mismatch;
    x.protocol_min=low;x.protocol_max=high;x.required_capabilities=required;x.optional_capabilities=optional;x.frame_cap=cap;x.config_hash=v.get("config_hash")->string;out=std::move(x);return Error::none;
}
} // namespace amity::consortium
