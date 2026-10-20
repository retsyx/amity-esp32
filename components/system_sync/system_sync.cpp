#include "amity/system_sync.hpp"
#include "amity/system_control.hpp"
#include "amity/consortium/runtime.hpp"
#include "amity/consortium/control.hpp"
#include "amity/consortium/transfer.hpp"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <mutex>
#include <set>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "freertos/idf_additions.h"
#endif

namespace amity {
namespace s=system; namespace c=consortium; namespace cs=config_store;
namespace {
uint64_t now_ms(){return static_cast<uint64_t>(esp_timer_get_time()/1000);}
template<class T>T fresh(){return T::generate([](uint8_t* data,size_t size){esp_fill_random(data,size);return true;}).value_or(T{});}
struct Peer {
    s::OctopusId id; s::BootId boot; std::vector<s::PortPolicySnapshot> policies; std::vector<s::CecObservation> observations;
    std::string policy_hash,configuration_payload; uint64_t config_revision=0,presentation_revision=0,seen_at=0,maintenance_generation=0;
    c::SnapshotTransfer incoming{[](std::string_view text){return cs::sha256_hex(text);}};
    c::SnapshotManifest outgoing; uint32_t next=0; bool transfer_ack=false,transfer_complete=false,ready=false,applied=false,rejected=false,maintenance=false,execution_ready=false;
};
struct SentCommand {s::CommandId id;uint64_t sequence=0,at=0;bool terminal=false;};
struct SentPlan {s::OctopusId owner;s::BootId boot;s::LinkId link;s::ExecutionLeaseId last_control_lease;c::PlanPrepare prepare;bool active=false;std::optional<s::SessionId> fenced_session;uint64_t fenced_generation=0,retired_at=0,next_sequence=0;std::vector<SentCommand> commands;};
struct ReceivedPlan {c::PlanPrepare prepare; s::OctopusId coordinator;s::BootId boot;s::LinkId link; bool activated=false,fenced=false,aborted=false; c::CommandLedger commands; std::optional<c::PlanBarrier> pending_fence;uint64_t retired_at=0;};
struct State {
    std::mutex mutex; SystemControl* system=nullptr; std::vector<Peer> peers; ConfigurationActivationStatus status;
    std::vector<SentPlan> sent_plans; std::vector<ReceivedPlan> received_plans;
    bool activating=false; uint64_t reconfigure_at=0; uint64_t new_revision=0;std::string new_policy;
} state;
ReceivedPlan* received_locked(s::PlanId id){for(auto& plan:state.received_plans)if(plan.prepare.context.plan_id==id)return &plan;return nullptr;}
void forget_received_holds(s::SessionId,uint64_t);
void retire_received_locked(s::SessionId session,uint64_t generation){forget_received_holds(session,generation);for(auto& plan:state.received_plans)if(plan.prepare.context.session_id==session&&plan.prepare.context.route_generation<=generation){plan.aborted=true;plan.retired_at=now_ms();}}
Peer* peer_locked(s::OctopusId id){for(auto& p:state.peers)if(p.id==id)return &p;if(state.peers.size()>=7)return nullptr;state.peers.emplace_back();state.peers.back().id=id;return &state.peers.back();}
s::BootId local_boot(){for(const auto& p:state.system->local_policy_snapshot())if(p.owner_boot_id)return p.owner_boot_id;return state.system->boot_identity();}
std::optional<c::RuntimePeerStatus> runtime_peer(s::OctopusId id){for(const auto& p:c::runtime_peers())if(p.id==id)return p;return {};}
std::optional<cs::ConfigurationV2> current(){return state.system->active_configuration();}
cs::ConfigurationV2 scoped_configuration(const cs::ConfigurationV2& source,s::OctopusId owner){
    auto slice=synchronization::owner_slice(source,owner);
    // Capacity is authenticated hardware metadata. It only permits passive
    // observation on unused connectors; roles/bindings still require config.
    const auto peer=runtime_peer(owner);
    if(peer&&peer->hardware_profile)for(auto& board:slice.octopuses)if(board.octopus_id==owner&&board.supported_tentacles<peer->supported_tentacles)board.supported_tentacles=peer->supported_tentacles;
    return slice;
}
std::string plan_digest(c::PlanPrepare p){p.context.plan_digest=std::string(64,'0');return cs::sha256_hex(c::plan_prepare_json(p));}
bool same_context(const c::PlanContext& a,const c::PlanContext& b){return a.plan_id==b.plan_id&&a.session_id==b.session_id&&a.route_generation==b.route_generation&&a.policy_generation==b.policy_generation&&a.plan_digest==b.plan_digest&&a.policy_hash==b.policy_hash;}
c::Error error_of(const s::AdmissionResult& result){switch(result.code){case s::AdmissionCode::accepted:return c::Error::none;case s::AdmissionCode::stale:return c::Error::stale_binding;case s::AdmissionCode::expired:return c::Error::lease_expired;case s::AdmissionCode::unsupported:return c::Error::unsupported_kind;case s::AdmissionCode::overloaded:return c::Error::queue_full;case s::AdmissionCode::unauthorized:return c::Error::unauthorized_role;case s::AdmissionCode::held_key_conflict:return c::Error::not_ready;case s::AdmissionCode::invalid:return c::Error::invalid_message;}return c::Error::invalid_message;}
void plan_reply(s::OctopusId to,const c::PlanContext& context,std::string phase,bool accepted,std::string result,c::Error error=c::Error::not_ready){c::PlanResult reply{context,std::move(phase),accepted?std::move(result):"rejected",accepted?std::string{}:c::error_name(error)};c::runtime_send_control(to,c::Kind::plan_result,c::plan_result_json(reply),2000);}
std::string config_reply(const c::Json& payload,std::string result,std::string error={}){
    uint64_t cfg,pres;s::BootId boot;std::string hash,policy,phase;
    if(!c::decimal(payload.get("config_revision"),cfg)||!c::decimal(payload.get("presentation_revision"),pres)||!c::id(payload.get("owner_boot_id"),boot)||!c::json_string(payload.get("payload_hash"),hash,64,64)||!c::json_string(payload.get("policy_hash"),policy,64,64)||!c::json_string(payload.get("phase"),phase,1,16))return {};
    auto out=c::config_control_json(cfg,pres,boot,hash,policy,phase);if(out.empty())return {};out.pop_back();out+=",\"result\":\""+result+"\"";if(!error.empty())out+=",\"error\":\""+error+"\"";return out+"}";
}
bool wait_installed(const cs::ConfigurationV2& configuration,uint32_t timeout=35000,std::string* detail=nullptr){
    const auto hash=cs::executable_policy_hash(configuration);const auto started=now_ms();
    for(;;){
        const auto snapshot=state.system->execution_snapshot();
        if(snapshot.applied_config_revision==configuration.config_revision&&snapshot.applied_presentation_revision==configuration.presentation_revision&&snapshot.applied_policy_hash==hash)return true;
        const auto now=now_ms();if(now<started||now-started>=timeout){
            if(detail)*detail="Expected "+s::format_uint64(configuration.config_revision)+"/"+s::format_uint64(configuration.presentation_revision)+"/"+hash+", observed "+s::format_uint64(snapshot.applied_config_revision)+"/"+s::format_uint64(snapshot.applied_presentation_revision)+"/"+snapshot.applied_policy_hash+(snapshot.error.empty()?"":" ("+snapshot.error+")");
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
bool update_owner(s::OctopusId owner,s::BootId boot,std::string_view bytes){
    auto config=current();if(!config)return false;auto slice=scoped_configuration(*config,owner);bool maintenance=false;std::vector<s::PortPolicySnapshot> policies;std::vector<s::CecObservation> observations;std::string error;
    if(!synchronization::parse_owner_state(bytes,slice,owner,boot,c::runtime_status().epoch,policies,error,&observations,now_ms()))return false;
    uint64_t maintenance_generation=0;{std::lock_guard<std::mutex> lock(state.mutex);auto* p=peer_locked(owner);if(!p)return false;maintenance=p->maintenance;maintenance_generation=p->maintenance_generation;}
    // The owner queue receives a snapshot only after its exact authenticated boot,
    // configured selectors, domain revisions and executable slice hash validate.
    const auto applied=state.system->update_remote_owner(owner,policies,!maintenance);
    if(!applied)state.system->update_remote_owner(owner,{},false);
    bool changed=false;{std::lock_guard<std::mutex> lock(state.mutex);auto* p=peer_locked(owner);if(!p)return false;p->boot=boot;p->config_revision=slice.config_revision;p->presentation_revision=slice.presentation_revision;p->policies=policies;p->observations=std::move(observations);p->policy_hash=cs::executable_policy_hash(slice);p->seen_at=now_ms();changed=p->maintenance_generation!=maintenance_generation;p->execution_ready=bool(applied)&&!p->maintenance&&!changed;}
    if(changed)state.system->update_remote_owner(owner,{},false);
    const bool passive=std::all_of(policies.begin(),policies.end(),[](const auto& policy){return policy.role==s::PortRole::monitor&&policy.bindings.empty()&&!policy.owned_logical_address;});
    return bool(applied)||passive;
}
c::Error synchronize(s::OctopusId owner,const c::Json& json){
    if(json.type!=c::Json::Type::string)return c::Error::invalid_message;
    const auto status=c::runtime_status();
    if(status.coordinator){auto p=runtime_peer(owner);if(!p)return c::Error::not_ready;return update_owner(owner,p->boot_id,json.string)?c::Error::none:c::Error::not_ready;}
    if(!status.member)return c::Error::unauthorized_role;
    cs::ConfigurationV2 config;std::string error;if(!cs::parse_configuration_v2(json.string,config,error)||config.consortium_id!=state.system->consortium_id())return c::Error::invalid_message;
    const auto result=state.system->configure_member_slice(config,owner,status.epoch);return result==ESP_OK&&wait_installed(config)?c::Error::none:c::Error::storage_failure;
}
std::string snapshot(s::OctopusId owner){
    auto config=current();if(!config)return {};
    auto status=c::runtime_status();if(status.coordinator)return cs::serialize_configuration_v2(scoped_configuration(*config,owner));
    if(status.member)return synchronization::owner_state_json(*config,state.system->octopus_id(),local_boot(),status.epoch,state.system->local_policy_snapshot(),state.system->native_observations(),now_ms());
    return {};
}
c::Error receive_transfer(s::OctopusId from,const c::Envelope& envelope){
    const auto now=now_ms();const auto local=state.system->octopus_id();const bool coordinator=c::runtime_status().coordinator;
    if(envelope.kind==c::Kind::snapshot_request){
        c::SnapshotManifest manifest;std::string phase;uint32_t next=0;auto result=c::parse_snapshot_manifest(envelope.payload,manifest,phase,next);if(result!=c::Error::none)return result;
        std::lock_guard<std::mutex> lock(state.mutex);auto* peer=peer_locked(from);if(!peer)return c::Error::queue_full;
        if(phase!="begin"){
            const auto& expected=peer->outgoing;
            if(manifest.transfer_id!=expected.transfer_id||manifest.payload_hash!=expected.payload_hash||manifest.owner_boot_id!=expected.owner_boot_id||manifest.owner_octopus_id!=expected.owner_octopus_id||manifest.config_revision!=expected.config_revision||manifest.document_kind!=expected.document_kind||manifest.total_length!=expected.total_length||manifest.chunk_count()!=expected.chunk_count()||next<peer->next||next>peer->next+1||(phase=="complete"&&next!=expected.chunk_count()))return c::Error::invalid_message;
            peer->next=next;peer->transfer_ack=phase=="ack";peer->transfer_complete=phase=="complete";peer->rejected=phase=="reject";return c::Error::none;
        }
        if((coordinator&&manifest.document_kind!="owner_state")||(!coordinator&&manifest.document_kind!="configuration"))return c::Error::unauthorized_role;
        if(!coordinator&&manifest.config_revision<state.system->execution_snapshot().applied_config_revision)return c::Error::stale_config;
        if(peer->incoming.manifest().transfer_id!=manifest.transfer_id){peer->incoming.reset();result=peer->incoming.begin(manifest,coordinator?from:local,coordinator?envelope.origin_boot_id:envelope.destination_boot_id,manifest.config_revision,now);}else result=c::Error::none;
        if(result==c::Error::none)c::runtime_send_control(from,c::Kind::snapshot_request,c::snapshot_manifest_json(manifest,"ack",peer->incoming.next_index()),2000);
        return result;
    }
    std::string complete;c::SnapshotManifest manifest;uint32_t next=0;c::Error accepted;
    {std::lock_guard<std::mutex> lock(state.mutex);auto* p=peer_locked(from);if(!p)return c::Error::queue_full;accepted=p->incoming.accept(envelope.payload,now);manifest=p->incoming.manifest();next=p->incoming.next_index();if(accepted!=c::Error::none&&accepted!=c::Error::duplicate)return accepted;if(p->incoming.complete())complete=p->incoming.payload();}
    const bool finished=!complete.empty();
    if(finished){
        if(coordinator){if(!update_owner(from,envelope.origin_boot_id,complete))return c::Error::not_ready;}
        else{cs::ConfigurationV2 candidate;std::string error;if(!cs::parse_configuration_v2(complete,candidate,error)||candidate.config_revision!=manifest.config_revision)return c::Error::invalid_message;std::lock_guard<std::mutex> lock(state.mutex);peer_locked(from)->configuration_payload=std::move(complete);}
    }
    c::runtime_send_control(from,c::Kind::snapshot_request,c::snapshot_manifest_json(manifest,finished?"complete":"ack",next),2000);return c::Error::none;
}
c::Error receive_configuration(s::OctopusId from,const c::Envelope& envelope){
    if(envelope.kind==c::Kind::config_result){
        uint64_t cfg,pres;s::BootId boot;std::string hash,policy,phase,result;
        if(!c::decimal(envelope.payload.get("config_revision"),cfg)||!c::decimal(envelope.payload.get("presentation_revision"),pres)||!c::id(envelope.payload.get("owner_boot_id"),boot)||!c::json_string(envelope.payload.get("payload_hash"),hash,64,64)||!c::json_string(envelope.payload.get("policy_hash"),policy,64,64)||!c::json_string(envelope.payload.get("phase"),phase,1,16)||!c::json_string(envelope.payload.get("result"),result,1,16))return c::Error::invalid_message;
        std::lock_guard<std::mutex> lock(state.mutex);auto* p=peer_locked(from);if(!state.activating||!p||p->boot!=boot||boot!=envelope.origin_boot_id||p->outgoing.payload_hash!=hash||p->policy_hash!=policy||state.status.config_revision!=cfg||state.status.presentation_revision!=pres)return c::Error::stale_config;
        if(result=="rejected")p->rejected=true;else if(phase=="prepare"&&result=="ready")p->ready=true;else if(phase=="activate"&&result=="applied")p->applied=true;else return c::Error::invalid_message;return c::Error::none;
    }
    if(!c::runtime_status().member)return c::Error::unauthorized_role;
    uint64_t cfg,pres;s::BootId boot;std::string hash,policy,payload;
    if(!c::decimal(envelope.payload.get("config_revision"),cfg)||!c::decimal(envelope.payload.get("presentation_revision"),pres)||!c::id(envelope.payload.get("owner_boot_id"),boot)||boot!=local_boot()||!c::json_string(envelope.payload.get("payload_hash"),hash,64,64)||!c::json_string(envelope.payload.get("policy_hash"),policy,64,64))return c::Error::invalid_message;
    {std::lock_guard<std::mutex> lock(state.mutex);auto* p=peer_locked(from);if(!p)return c::Error::queue_full;payload=p->configuration_payload;}
    cs::ConfigurationV2 candidate;std::string error;bool valid=cs::sha256_hex(payload)==hash&&cs::parse_configuration_v2(payload,candidate,error)&&candidate.config_revision==cfg&&candidate.presentation_revision==pres&&cs::executable_policy_hash(candidate)==policy;
    bool accepted=false;
    if(valid){if(envelope.kind==c::Kind::config_prepare)accepted=bool(state.system->prepare_member_configuration(candidate,from,envelope.coordinator_epoch,hash));else accepted=state.system->activate_member_configuration(hash)==ESP_OK&&wait_installed(candidate);}
    const bool activate=envelope.kind==c::Kind::config_activate;
    c::runtime_send_control(from,c::Kind::config_result,config_reply(envelope.payload,accepted?(activate?"applied":"ready"):"rejected",accepted?"":"NOT_READY"),2000);
    if(accepted&&activate){std::lock_guard<std::mutex> lock(state.mutex);state.reconfigure_at=now_ms()+250;state.new_revision=cfg;state.new_policy=policy;}
    return accepted?c::Error::none:c::Error::not_ready;
}
c::Error receive_plan(s::OctopusId from,const c::Envelope& envelope,uint64_t receiver_lease_deadline,s::LinkId link){
    if(envelope.kind==c::Kind::plan_result){
        c::PlanResult response;auto parsed=c::parse_plan_result(envelope.payload,response);if(parsed!=c::Error::none)return parsed;
        bool matched=false;{std::lock_guard<std::mutex> lock(state.mutex);for(const auto& sent:state.sent_plans)if(sent.owner==from&&sent.boot==envelope.origin_boot_id&&sent.link==link&&same_context(sent.prepare.context,response.context))matched=true;}
        if(!matched)return c::Error::stale_route;
        if(response.phase=="activate"&&response.result=="applied"){std::lock_guard<std::mutex> lock(state.mutex);for(auto& sent:state.sent_plans)if(sent.owner==from&&sent.boot==envelope.origin_boot_id&&sent.link==link&&same_context(sent.prepare.context,response.context))sent.active=true;}
        if(response.phase=="fence"&&response.result=="quiescent"){
            std::lock_guard<std::mutex> lock(state.mutex);std::optional<s::SessionId> retired;uint64_t generation=0;
            for(const auto& sent:state.sent_plans)if(sent.owner==from&&sent.boot==envelope.origin_boot_id&&sent.link==link&&same_context(sent.prepare.context,response.context)){retired=sent.fenced_session;generation=sent.fenced_generation;}
            if(retired)for(auto& sent:state.sent_plans)if(sent.owner==from&&sent.prepare.context.session_id==*retired&&sent.prepare.context.route_generation<=generation){sent.active=false;sent.retired_at=now_ms();}
        }
        if(response.result=="pending")return c::Error::none;
        if(response.phase=="abort")return c::Error::none;
        const auto phase=response.phase=="prepare"?s::RemotePlanPhase::prepare:response.phase=="fence"?s::RemotePlanPhase::fence:s::RemotePlanPhase::activate;
        state.system->remote_plan_result(from,response.context.plan_id,phase,response.result!="rejected",response.error);return c::Error::none;
    }
    if(!c::runtime_status().member)return c::Error::unauthorized_role;
    auto config=current();if(!config)return c::Error::not_ready;
    if(envelope.kind==c::Kind::plan_prepare){
        c::PlanPrepare request;auto parsed=c::parse_plan_prepare(envelope.payload,request);if(parsed!=c::Error::none)return parsed;
        if(request.context.policy_hash!=cs::executable_policy_hash(*config)||request.context.plan_digest!=plan_digest(request))return c::Error::stale_config;
        bool duplicate=false;{std::lock_guard<std::mutex> lock(state.mutex);if(auto* old=received_locked(request.context.plan_id)){if(old->coordinator!=from||old->boot!=envelope.origin_boot_id||old->link!=link||!same_context(old->prepare.context,request.context)||old->aborted)return c::Error::stale_route;duplicate=true;}else if(state.received_plans.size()>=64)return c::Error::queue_full;}if(duplicate){plan_reply(from,request.context,"prepare",true,"ready");return c::Error::none;}
        const auto route=std::find_if(config->routes.begin(),config->routes.end(),[&](const auto& r){return r.route_id==request.route_id;});
        if(route==config->routes.end()||request.operation_capacity!=route->steps.size())return c::Error::invalid_message;
        auto policies=state.system->local_policy_snapshot();std::set<s::BindingId> checked;
        for(auto& policy:policies){
            if(policy.policy_generation==UINT64_MAX||request.context.policy_generation!=policy.policy_generation+1)return c::Error::stale_route;
            ++policy.policy_generation;policy.route_grants.clear();
            for(const auto& permission:policy.bindings){const auto& b=permission.binding;
                if(std::find(route->required_bindings.begin(),route->required_bindings.end(),b.binding_id)==route->required_bindings.end())continue;
                auto required=std::find_if(request.bindings.begin(),request.bindings.end(),[&](const auto& r){return r.binding_id==b.binding_id;});
                if(required==request.bindings.end()||required->component_id!=b.component_id||required->domain_id!=b.domain_id||required->binding_revision!=b.binding_revision||required->domain_revision!=b.domain_revision||required->target!=b.endpoint||b.status!=s::BindingStatus::verified)return c::Error::stale_binding;
                checked.insert(b.binding_id);
                auto grant=std::find_if(request.grants.begin(),request.grants.end(),[&](const auto& g){return g.binding_id==b.binding_id;});if(grant==request.grants.end())return c::Error::invalid_message;
                policy.route_grants.push_back({b.binding_id,request.context.session_id,request.context.route_generation,0,grant->permitted_paths});
            }
        }
        if(checked.size()!=request.bindings.size()||checked.size()!=request.grants.size())return c::Error::unauthorized_role;
        s::Plan plan;plan.plan_id=request.context.plan_id;plan.config_revision=config->config_revision;plan.steps=route->steps;plan.owners={state.system->octopus_id()};plan.proposed_session={request.context.session_id,request.activity_id,request.zone_id,request.route_id,request.context.plan_id,request.context.route_generation,s::SessionStatus::preparing,route->demands};
        auto accepted=state.system->prepare_remote_plan(plan,std::move(policies));
        if(accepted){std::lock_guard<std::mutex> lock(state.mutex);state.received_plans.emplace_back();state.received_plans.back().prepare=request;state.received_plans.back().coordinator=from;state.received_plans.back().boot=envelope.origin_boot_id;state.received_plans.back().link=link;}
        plan_reply(from,request.context,"prepare",bool(accepted),"ready",error_of(accepted));return error_of(accepted);
    }
    c::PlanContext context;c::PlanBarrier barrier;
    if(envelope.kind==c::Kind::plan_abort){auto result=c::parse_plan_context(envelope.payload,context);if(result!=c::Error::none)return result;}else{auto result=c::parse_plan_barrier(envelope.payload,barrier);if(result!=c::Error::none)return result;context=barrier.context;}
    {std::lock_guard<std::mutex> lock(state.mutex);auto* plan=received_locked(context.plan_id);if(!plan||plan->coordinator!=from||plan->boot!=envelope.origin_boot_id||plan->link!=link||!same_context(plan->prepare.context,context)||plan->aborted)return c::Error::stale_route;}
    if(barrier.phase=="renew"){if(!receiver_lease_deadline||receiver_lease_deadline<=now_ms())return c::Error::lease_expired;const auto ttl=static_cast<uint32_t>(std::min<uint64_t>(barrier.authority_ttl_ms,receiver_lease_deadline-now_ms()));return error_of(state.system->renew_remote_authority(context.plan_id,context.session_id,context.route_generation,ttl));}
    if(envelope.kind==c::Kind::plan_abort){auto result=state.system->abort_remote_plan(context.plan_id);{std::lock_guard<std::mutex> lock(state.mutex);retire_received_locked(context.session_id,context.route_generation);}plan_reply(from,context,"abort",bool(result),"aborted",error_of(result));return error_of(result);}
    if(barrier.phase=="fence"){
        if(!barrier.old_session_id){{std::lock_guard<std::mutex> lock(state.mutex);auto* plan=received_locked(context.plan_id);if(!plan)return c::Error::stale_route;plan->fenced=true;}plan_reply(from,context,"fence",true,"quiescent");return c::Error::none;}
        auto result=state.system->fence_remote_route(*barrier.old_session_id,barrier.old_route_generation);
        bool quiescent=result&&state.system->remote_route_quiescent(*barrier.old_session_id,barrier.old_route_generation);
        if(result){std::lock_guard<std::mutex> lock(state.mutex);auto* plan=received_locked(context.plan_id);if(!plan)return c::Error::stale_route;plan->fenced=quiescent;if(!quiescent)plan->pending_fence=barrier;else retire_received_locked(*barrier.old_session_id,barrier.old_route_generation);}
        plan_reply(from,context,"fence",bool(result),quiescent?"quiescent":"pending",error_of(result));return error_of(result);
    }
    bool already=false;{std::lock_guard<std::mutex> lock(state.mutex);auto* plan=received_locked(context.plan_id);if(!plan||!plan->fenced||plan->pending_fence||plan->aborted)return c::Error::not_ready;already=plan->activated;}
    auto result=already?s::AdmissionResult{s::AdmissionCode::accepted,{}}:state.system->activate_remote_plan(context.plan_id,barrier.authority_ttl_ms);
    if(result&&!already){std::lock_guard<std::mutex> lock(state.mutex);auto* plan=received_locked(context.plan_id);if(!plan)return c::Error::stale_route;plan->activated=true;plan->commands.activate(context.plan_id,context.session_id);}
    plan_reply(from,context,"activate",bool(result),"applied",error_of(result));return error_of(result);
}
struct PendingOutcome {c::Operation operation;s::OctopusId coordinator; s::StepResult last=s::StepResult::admitted;uint64_t deadline;};
std::vector<PendingOutcome> outcomes;
struct RemoteHold {s::AuthorizedCecOperation operation;s::PlanId plan;s::OctopusId coordinator;s::BootId boot;s::LinkId link;};
std::vector<RemoteHold> received_holds;
void forget_received_holds(s::SessionId session,uint64_t generation){received_holds.erase(std::remove_if(received_holds.begin(),received_holds.end(),[&](const auto& held){return held.operation.fence.session_id==session&&held.operation.fence.route_generation<=generation;}),received_holds.end());}
void forget_received_hold(const RemoteHold& held){
    received_holds.erase(std::remove_if(received_holds.begin(),received_holds.end(),[&](const auto& current){
        return current.coordinator==held.coordinator&&current.boot==held.boot&&current.link==held.link&&current.plan==held.plan&&
            current.operation.command_id==held.operation.command_id&&current.operation.arguments.hold_token==held.operation.arguments.hold_token&&
            current.operation.target==held.operation.target&&current.operation.binding_id==held.operation.binding_id&&current.operation.fence==held.operation.fence;
    }),received_holds.end());
}
std::string operation_reply(const c::Operation& op,std::string_view result,std::string_view error={}){auto out="{\"command_id\":\""+op.command_id.str()+"\",\"plan_id\":\""+op.plan_id.str()+"\",\"session_id\":\""+op.session_id.str()+"\",\"route_generation\":\""+s::format_uint64(op.route_generation)+"\",\"result\":\""+std::string(result)+"\"";if(!error.empty())out+=",\"error\":\""+std::string(error)+"\"";return out+"}";}
std::string outcome_error(const s::CommandOutcome& outcome){
    if(outcome.result!=s::StepResult::rejected&&outcome.result!=s::StepResult::timed_out&&outcome.result!=s::StepResult::unknown)return {};
    for(const auto* code:{"nack","arbitration_lost","canceled","expired","line_error","admission_rejected"})if(outcome.error==code)return code;
    return outcome.result==s::StepResult::timed_out?"expired":outcome.result==s::StepResult::unknown?"UNKNOWN_OUTCOME":"NOT_READY";
}
const char* step_name(s::StepResult value){switch(value){case s::StepResult::not_sent:return "unknown";case s::StepResult::admitted:return "admitted";case s::StepResult::applied:return "applied";case s::StepResult::observed:return "observed";case s::StepResult::rejected:return "rejected";case s::StepResult::timed_out:return "timed_out";case s::StepResult::unknown:return "unknown";}return "unknown";}
c::Error operation(const c::Operation& op,const c::Envelope& envelope,uint64_t deadline,s::LinkId link){
    c::PlanContext context;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        const auto* plan=received_locked(op.plan_id);
        if(!plan||!plan->activated||plan->aborted||plan->coordinator!=envelope.origin_octopus_id||plan->boot!=envelope.origin_boot_id||plan->link!=link)return c::Error::not_ready;
        context=plan->prepare.context;
        const auto& requirements=plan->prepare.bindings;
        auto binding=std::find_if(requirements.begin(),requirements.end(),[&](const auto& b){return b.binding_id==op.binding_id;});
        if(binding==requirements.end()||binding->domain_id!=op.domain_id)return c::Error::stale_domain;
        if(context.plan_id!=op.plan_id||context.session_id!=op.session_id||context.route_generation!=op.route_generation)return c::Error::stale_route;
    }
    s::AuthorizedCecOperation authorized;
    authorized.command_id=op.command_id;authorized.component_id=op.component_id;authorized.binding_id=op.binding_id;authorized.target=op.target;authorized.execution_expires_at_ms=deadline;
    // Routine native refresh advances the owner policy generation while keeping
    // exact bindings and independent route grants. Resolve only that local
    // generation from the current committed grant; every wire-supplied binding,
    // endpoint, domain, boot, revision and route fence remains unchanged.
    uint64_t generation=0;
    for(const auto& policy:state.system->local_policy_snapshot()){
        const auto binding=std::find_if(policy.bindings.begin(),policy.bindings.end(),[&](const auto& b){return b.binding.binding_id==op.binding_id&&b.binding.component_id==op.component_id&&b.binding.endpoint==op.target&&b.binding.binding_revision==op.binding_revision&&b.binding.domain_id==op.domain_id&&b.binding.domain_revision==op.domain_revision;});
        if(binding==policy.bindings.end())continue;
        if(std::any_of(policy.route_grants.begin(),policy.route_grants.end(),[&](const auto& grant){return grant.binding_id==op.binding_id&&grant.session_id==op.session_id&&grant.route_generation==op.route_generation;}))generation=policy.policy_generation;
    }
    if(!generation)return c::Error::stale_binding;
    authorized.fence={envelope.destination_boot_id,envelope.coordinator_epoch,envelope.config_revision,op.domain_revision,op.binding_revision,op.session_id,op.route_generation,generation};
    switch(op.operation){
    case c::SemanticOperation::ensure_power:authorized.kind=s::OperationKind::ensure_power;authorized.arguments.on=op.on;break;
    case c::SemanticOperation::announce_active_source:authorized.kind=s::OperationKind::announce_active_source;break;
    case c::SemanticOperation::query_state:authorized.kind=s::OperationKind::query_state;authorized.arguments.query_opcode=op.query_field=="audio"?0x71:0x8f;break;
    case c::SemanticOperation::key_press:authorized.kind=s::OperationKind::press_key;authorized.arguments.key=op.key_code;authorized.arguments.hold_token=op.hold_token;authorized.arguments.repeat=op.repeat;break;
    case c::SemanticOperation::select_route:{
        auto config=current();bool found=false;
        if(config)for(const auto& route:config->routes)if(route.route_id==op.route_id)for(const auto& step:route.steps)if(step.binding_id==op.binding_id&&step.operation==s::OperationKind::select_path){
            if(found)return c::Error::invalid_message;
            authorized.kind=s::OperationKind::select_path;authorized.arguments=step.arguments;found=true;
        }
        if(!found)return c::Error::invalid_message;
        break;
    }}
    s::AdmissionResult result{s::AdmissionCode::overloaded,"transport outcome/hold reservation is full"};
    {
        // Reserve both outcome and exact-token cleanup space before admitting a
        // positive operation. The owner call never invokes transport callbacks
        // on a member, so this lock also serializes concurrent link replacement.
        std::lock_guard<std::mutex> lock(state.mutex);
        auto* plan=received_locked(context.plan_id);if(!plan||!same_context(context,plan->prepare.context)||plan->aborted)return c::Error::stale_route;
        auto& ledger=plan->commands;
        const auto received=ledger.receive(op,now_ms());
        if(received==c::Error::duplicate){
            const auto outcome=ledger.query(op.command_id,now_ms());const char* value="unknown";
            if(outcome)switch(*outcome){case c::Outcome::received:value="admitted";break;case c::Outcome::applied:value="applied";break;case c::Outcome::observed:value="observed";break;case c::Outcome::rejected:value="rejected";break;case c::Outcome::failed:value="timed_out";break;case c::Outcome::unknown:break;}
            c::runtime_send_control(envelope.origin_octopus_id,c::Kind::operation_result,operation_reply(op,value,std::string_view(value)=="rejected"?"NOT_READY":""),1000);
            return received;
        }
        if(received!=c::Error::none)return received;
        const auto held=std::find_if(received_holds.begin(),received_holds.end(),[&](const auto& h){return h.operation.arguments.hold_token==authorized.arguments.hold_token;});
        const bool cleanup_space=op.operation!=c::SemanticOperation::key_press||!op.repeat||held!=received_holds.end()||received_holds.size()<16;
        if(outcomes.size()<32&&cleanup_space)result=state.system->admit_authorized(authorized);
        ledger.result(op.command_id,result?c::Outcome::received:c::Outcome::rejected,now_ms());
        if(result){
            outcomes.push_back({op,envelope.origin_octopus_id,s::StepResult::admitted,deadline});
            if(op.operation==c::SemanticOperation::key_press&&op.repeat){
                if(held!=received_holds.end())held->operation=authorized;
                else received_holds.push_back({authorized,op.plan_id,envelope.origin_octopus_id,envelope.origin_boot_id,link});
            }
        }
    }
    const auto error=error_of(result);
    c::runtime_send_control(envelope.origin_octopus_id,c::Kind::operation_result,operation_reply(op,result?"admitted":"rejected",result?"":c::error_name(error)),1000);
    return error;
}
c::Error hold_control(s::OctopusId from,const c::Envelope& envelope,s::LinkId link){
    s::PlanId plan;s::SessionId session;s::HoldToken token;uint64_t generation;
    if(envelope.kind!=c::Kind::hold_release||!c::id(envelope.payload.get("plan_id"),plan)||!c::id(envelope.payload.get("session_id"),session)||!c::id(envelope.payload.get("hold_token"),token)||!c::decimal(envelope.payload.get("route_generation"),generation))return c::Error::invalid_message;
    RemoteHold held;{std::lock_guard<std::mutex> lock(state.mutex);auto it=std::find_if(received_holds.begin(),received_holds.end(),[&](const auto& h){return h.coordinator==from&&h.boot==envelope.origin_boot_id&&h.link==link&&h.plan==plan&&h.operation.arguments.hold_token==token&&h.operation.fence.session_id==session&&h.operation.fence.route_generation==generation;});if(it==received_holds.end())return c::Error::stale_route;held=*it;}
    // This record exists only after synchronous down validation and owner
    // publication completed; absence from that scoped snapshot is meaningful.
    const auto inactive=[&]{const auto active=state.system->remote_key_active(held.operation,true);return active&&!*active;};
    if(inactive()){std::lock_guard<std::mutex> lock(state.mutex);forget_received_hold(held);return c::Error::stale_route;}
    auto op=held.operation;op.command_id=fresh<s::CommandId>();op.kind=s::OperationKind::release_key;
    const auto result=state.system->admit_authorized(op);
    // A key can end while its actor handoff is blocked. Only an exact owner-
    // published inactive result may turn that late up into a harmless no-op.
    if(!result&&(result.code==s::AdmissionCode::overloaded||result.code==s::AdmissionCode::expired)&&inactive()){
        std::lock_guard<std::mutex> lock(state.mutex);forget_received_hold(held);return c::Error::stale_route;
    }
    if(result||(result.code!=s::AdmissionCode::overloaded&&result.code!=s::AdmissionCode::expired)){std::lock_guard<std::mutex> lock(state.mutex);forget_received_hold(held);}
    return error_of(result);
}
bool link_disconnected(s::OctopusId peer,s::BootId boot,s::LinkId link){
    std::vector<c::PlanContext> closing;
    {std::lock_guard<std::mutex> lock(state.mutex);
        for(auto& plan:state.received_plans)if(plan.coordinator==peer&&plan.boot==boot&&plan.link==link){plan.aborted=true;plan.retired_at=now_ms();closing.push_back(plan.prepare.context);}
        for(auto& plan:state.sent_plans)if(plan.owner==peer&&plan.boot==boot&&plan.link==link){plan.active=false;plan.retired_at=now_ms();}
        received_holds.erase(std::remove_if(received_holds.begin(),received_holds.end(),[&](const auto& h){return h.coordinator==peer&&h.boot==boot&&h.link==link;}),received_holds.end());
    }
    // Synchronous owner admission fences all old queued/repeated work and
    // reserves matching release. A replacement TLS link waits for this call.
    bool fenced=true;for(const auto& context:closing)if(!state.system->fence_remote_route(context.session_id,context.route_generation))fenced=false;
    const auto current_link=c::runtime_link_id(peer,boot);
    if(c::runtime_status().coordinator&&(!current_link||*current_link==link))if(!state.system->update_remote_owner(peer,{},false))fenced=false;
    return fenced;
}
void refresh_remote_equipment_if_due(){
    const auto status=c::runtime_status();if(!status.coordinator||!status.authority_ready)return;
    const auto config=current();if(!config)return;
    struct Due {s::OctopusId owner;uint64_t at;};static std::vector<Due> schedule;
    std::set<s::OctopusId> owners;for(const auto& domain:config->domains)if(domain.owner_octopus_id!=state.system->octopus_id())owners.insert(domain.owner_octopus_id);
    schedule.erase(std::remove_if(schedule.begin(),schedule.end(),[&](const auto& due){return !owners.count(due.owner);}),schedule.end());
    for(auto owner:owners){
        auto due=std::find_if(schedule.begin(),schedule.end(),[&](const auto& entry){return entry.owner==owner;});
        if(due==schedule.end()){schedule.push_back({owner,now_ms()+20000});continue;}
        const auto now=now_ms();if(now<due->at)continue;
        bool maintenance=false;{std::lock_guard<std::mutex> lock(state.mutex);for(const auto& peer:state.peers)if(peer.id==owner)maintenance=peer.maintenance;}
        const auto peer=runtime_peer(owner);
        // Wait for a newly issued lease rather than starting a bounded scan at
        // the end of an old lease. Missing authority never grants fallback.
        if(maintenance||!peer||!peer->ready||!peer->control_lease||peer->control_usable_until_ms<now+4000)continue;
        const auto request=fresh<s::MessageId>();const auto body="{\"request_id\":\""+request.str()+"\",\"execution_lease_id\":\""+peer->control_lease.str()+"\"}";
        if(c::runtime_send_control(owner,c::Kind::scan_request,body,1000)==ESP_OK)due->at=now+20000;
    }
}
c::Error control_message(s::OctopusId from,const c::Envelope& envelope,uint64_t receiver_lease_deadline,s::LinkId link){
    if(c::retired_kind(envelope.kind))return c::Error::unauthorized_role;
    if(envelope.kind==c::Kind::scan_result)return c::Error::none;
    if(envelope.kind==c::Kind::scan_request){
        const auto now=now_ms();const bool fresh=receiver_lease_deadline>now&&receiver_lease_deadline-now<=5000;
        const auto result=fresh?state.system->discover_authorized(static_cast<uint32_t>(receiver_lease_deadline-now)):s::AdmissionResult{s::AdmissionCode::expired,"receiver discovery lease expired"};
        const auto reply="{\"request_id\":\""+envelope.payload.get("request_id")->string+"\",\"result\":\""+(result?"admitted":"rejected")+"\",\"error\":\""+(result?"":fresh?"NOT_READY":"LEASE_EXPIRED")+"\"}";
        c::runtime_send_control(from,c::Kind::scan_result,reply,1000);return result?c::Error::none:fresh?c::Error::not_ready:c::Error::lease_expired;
    }
    switch(envelope.kind){case c::Kind::hold_release:return hold_control(from,envelope,link);case c::Kind::snapshot_request:case c::Kind::snapshot_chunk:return receive_transfer(from,envelope);case c::Kind::config_prepare:case c::Kind::config_activate:case c::Kind::config_result:return receive_configuration(from,envelope);case c::Kind::plan_prepare:case c::Kind::plan_barrier:case c::Kind::plan_abort:case c::Kind::plan_result:return receive_plan(from,envelope,receiver_lease_deadline,link);case c::Kind::operation_result:{s::CommandId id;s::PlanId plan;s::SessionId session;uint64_t generation;std::string result,error;if(!c::id(envelope.payload.get("command_id"),id)||!c::id(envelope.payload.get("plan_id"),plan)||!c::id(envelope.payload.get("session_id"),session)||!c::decimal(envelope.payload.get("route_generation"),generation)||!c::json_string(envelope.payload.get("result"),result,1,16))return c::Error::invalid_message;bool matched=false;{std::lock_guard<std::mutex> lock(state.mutex);for(const auto& sent:state.sent_plans)if(sent.owner==from&&sent.boot==envelope.origin_boot_id&&sent.link==link&&sent.prepare.context.plan_id==plan&&sent.prepare.context.session_id==session&&sent.prepare.context.route_generation==generation)matched=true;}if(!matched)return c::Error::stale_route;if(envelope.payload.get("error"))error=envelope.payload.get("error")->string;const auto value=result=="admitted"?s::StepResult::admitted:result=="applied"?s::StepResult::applied:result=="observed"?s::StepResult::observed:result=="rejected"?s::StepResult::rejected:result=="timed_out"?s::StepResult::timed_out:s::StepResult::unknown;if(value!=s::StepResult::admitted){std::lock_guard<std::mutex> lock(state.mutex);for(auto& sent:state.sent_plans)if(sent.owner==from&&sent.boot==envelope.origin_boot_id&&sent.link==link&&sent.prepare.context.plan_id==plan)for(auto& command:sent.commands)if(command.id==id){command.at=now_ms();command.terminal=true;}}state.system->remote_operation_result(from,id,value,error);return c::Error::none;}default:return c::Error::unsupported_kind;}
}
s::RemoteExecutionHooks remote_hooks(){
    s::RemoteExecutionHooks hooks;
    hooks.prepare=[](s::OctopusId owner,const s::Plan& plan){
        const auto live=runtime_peer(owner);if(!live||!live->ready)return false;const auto link=c::runtime_link_id(owner,live->boot_id);if(!link)return false;
        c::PlanPrepare request;request.context.plan_id=plan.plan_id;request.context.session_id=plan.proposed_session.session_id;request.context.route_generation=plan.proposed_session.route_generation;request.activity_id=plan.proposed_session.activity_id;request.zone_id=plan.proposed_session.zone_id;request.route_id=plan.proposed_session.route_id;request.operation_capacity=static_cast<uint16_t>(plan.steps.size());
        auto config=current();if(!config)return false;const auto route=std::find_if(config->routes.begin(),config->routes.end(),[&](const auto& r){return r.route_id==request.route_id;});if(route==config->routes.end())return false;
        {std::lock_guard<std::mutex> lock(state.mutex);auto* peer=peer_locked(owner);if(!peer||peer->maintenance||!peer->execution_ready||peer->policies.empty()||now_ms()-peer->seen_at>=5000)return false;request.context.policy_hash=peer->policy_hash;
            for(const auto& p:peer->policies){if(p.policy_generation==UINT64_MAX)return false;if(request.context.policy_generation&&request.context.policy_generation!=p.policy_generation+1)return false;request.context.policy_generation=p.policy_generation+1;
                for(const auto& permission:p.bindings){const auto& b=permission.binding;if(std::find(route->required_bindings.begin(),route->required_bindings.end(),b.binding_id)==route->required_bindings.end())continue;if(b.status!=s::BindingStatus::verified)return false;
                    request.bindings.push_back({b.binding_id,b.component_id,b.domain_id,b.binding_revision,b.domain_revision,b.endpoint});c::PreparedGrant grant;grant.binding_id=b.binding_id;for(const auto& step:plan.steps)if(step.binding_id==b.binding_id&&step.operation==s::OperationKind::select_path)grant.permitted_paths.push_back(step.arguments.physical_path);request.grants.push_back(std::move(grant));
                }
            }
            if(request.bindings.empty()||request.bindings.size()>8)return false;
            request.context.plan_digest=plan_digest(request);
            if(std::any_of(state.sent_plans.begin(),state.sent_plans.end(),[&](const auto& sent){return sent.owner==owner&&sent.prepare.context.plan_id==plan.plan_id;})||state.sent_plans.size()>=64)return false;
            state.sent_plans.emplace_back();state.sent_plans.back().owner=owner;state.sent_plans.back().boot=live->boot_id;state.sent_plans.back().link=*link;state.sent_plans.back().prepare=request;
        }
        return c::runtime_send_control(owner,c::Kind::plan_prepare,c::plan_prepare_json(request),2000)==ESP_OK;
    };
    hooks.fence=[](s::OctopusId owner,const s::Plan& plan,const std::optional<s::Session>& old){
        c::PlanBarrier barrier;{std::lock_guard<std::mutex> lock(state.mutex);auto it=std::find_if(state.sent_plans.begin(),state.sent_plans.end(),[&](const auto& sent){return sent.owner==owner&&sent.prepare.context.plan_id==plan.plan_id;});if(it==state.sent_plans.end())return false;barrier.context=it->prepare.context;it->active=false;if(old){it->fenced_session=old->session_id;it->fenced_generation=old->route_generation;for(auto& previous:state.sent_plans)if(previous.owner==owner&&previous.prepare.context.session_id==old->session_id&&previous.prepare.context.route_generation<=old->route_generation)previous.active=false;}}
        barrier.phase="fence";if(old){barrier.old_session_id=old->session_id;barrier.old_route_generation=old->route_generation;}return c::runtime_send_control(owner,c::Kind::plan_barrier,c::plan_barrier_json(barrier),2000)==ESP_OK;
    };
    hooks.activate=[](s::OctopusId owner,const s::Plan& plan,uint32_t ttl){c::PlanBarrier barrier;{std::lock_guard<std::mutex> lock(state.mutex);auto it=std::find_if(state.sent_plans.begin(),state.sent_plans.end(),[&](const auto& sent){return sent.owner==owner&&sent.prepare.context.plan_id==plan.plan_id;});if(it==state.sent_plans.end())return false;barrier.context=it->prepare.context;}barrier.phase="activate";barrier.authority_ttl_ms=ttl;return c::runtime_send_control(owner,c::Kind::plan_barrier,c::plan_barrier_json(barrier),2000)==ESP_OK;};
    hooks.send=[](s::OctopusId owner,s::PlanId id,const s::AuthorizedCecOperation& authorized){
        if(authorized.kind==s::OperationKind::release_key){
            if(!authorized.arguments.hold_token)return false;
            const auto token=*authorized.arguments.hold_token;
            s::LinkId link;{std::lock_guard<std::mutex> lock(state.mutex);const auto plan=std::find_if(state.sent_plans.begin(),state.sent_plans.end(),[&](const auto& sent){return sent.owner==owner&&sent.prepare.context.plan_id==id&&sent.prepare.context.session_id==authorized.fence.session_id&&sent.prepare.context.route_generation==authorized.fence.route_generation&&sent.boot==authorized.fence.owner_boot_id;});if(plan==state.sent_plans.end())return false;link=plan->link;}
            const auto body="{\"plan_id\":\""+id.str()+"\",\"session_id\":\""+authorized.fence.session_id.str()+"\",\"hold_token\":\""+token.str()+"\",\"route_generation\":\""+s::format_uint64(authorized.fence.route_generation)+"\"}";
            return c::runtime_send_cleanup(owner,body,link,2000)==ESP_OK;
        }
        const auto active=current();const auto admitted_at=now_ms();if(!active||authorized.fence.config_revision!=active->config_revision||authorized.execution_expires_at_ms<=admitted_at)return false;
        c::Operation op;op.command_id=authorized.command_id;op.plan_id=id;op.session_id=authorized.fence.session_id;op.component_id=authorized.component_id;op.binding_id=authorized.binding_id;op.target=authorized.target;op.route_generation=authorized.fence.route_generation;op.binding_revision=authorized.fence.binding_revision;op.domain_revision=authorized.fence.domain_revision;
        {std::lock_guard<std::mutex> lock(state.mutex);auto* p=peer_locked(owner);if(!p||p->maintenance||!p->execution_ready)return false;bool found=false;for(const auto& policy:p->policies)for(const auto& permission:policy.bindings){const auto& binding=permission.binding;if(binding.binding_id==op.binding_id&&binding.component_id==op.component_id&&binding.endpoint==op.target&&binding.owner_octopus_id==owner&&binding.binding_revision==authorized.fence.binding_revision&&binding.domain_revision==authorized.fence.domain_revision&&binding.status==s::BindingStatus::verified&&policy.owner_boot_id==authorized.fence.owner_boot_id&&policy.coordinator_epoch==authorized.fence.coordinator_epoch&&policy.config_revision==authorized.fence.config_revision&&policy.policy_generation==authorized.fence.policy_generation){op.domain_id=binding.domain_id;found=true;}}if(!found)return false;}
        auto runtime=runtime_peer(owner);if(!runtime||!runtime->ready||runtime->boot_id!=authorized.fence.owner_boot_id||runtime->config_revision!=authorized.fence.config_revision)return false;
        switch(authorized.kind){case s::OperationKind::ensure_power:op.operation=c::SemanticOperation::ensure_power;op.on=authorized.arguments.on;break;case s::OperationKind::announce_active_source:op.operation=c::SemanticOperation::announce_active_source;break;case s::OperationKind::query_state:op.operation=c::SemanticOperation::query_state;op.query_field=authorized.arguments.query_opcode==0x71?"audio":"power";break;case s::OperationKind::press_key:op.operation=c::SemanticOperation::key_press;op.key_code=authorized.arguments.key;op.hold_token=authorized.arguments.hold_token.value_or(s::HoldToken{});op.repeat=authorized.arguments.repeat;break;case s::OperationKind::select_path:{op.operation=c::SemanticOperation::select_route;std::lock_guard<std::mutex> lock(state.mutex);for(const auto& sent:state.sent_plans)if(sent.owner==owner&&sent.prepare.context.plan_id==id)op.route_id=sent.prepare.route_id;break;}case s::OperationKind::release_key:return false;}
        op.execution_lease_id=op.operation==c::SemanticOperation::key_press?runtime->realtime_lease:runtime->control_lease;
        s::LinkId expected_link;
        {
            std::lock_guard<std::mutex> lock(state.mutex);auto plan=std::find_if(state.sent_plans.begin(),state.sent_plans.end(),[&](const auto& p){return p.owner==owner&&p.prepare.context.plan_id==id&&p.active;});
            if(plan==state.sent_plans.end()||plan->prepare.context.session_id!=op.session_id||plan->prepare.context.route_generation!=op.route_generation)return false;
            const auto link=c::runtime_link_id(owner,runtime->boot_id);if(!link||*link!=plan->link||runtime->boot_id!=plan->boot)return false;expected_link=*link;
            const auto now=now_ms();auto known=std::find_if(plan->commands.begin(),plan->commands.end(),[&](const auto& command){return command.id==op.command_id;});
            if(known!=plan->commands.end())op.command_sequence=known->sequence;
            else {
                // This owner hook is called once for each fresh coordinator
                // command. Network retries retain their serialized sequence;
                // existing correlations reuse it during the bounded query window.
                plan->commands.erase(std::remove_if(plan->commands.begin(),plan->commands.end(),[&](const auto& command){return now>=command.at&&now-command.at>=c::CommandLedger::query_window_ms;}),plan->commands.end());
                if(plan->commands.size()>=32){const auto old=std::min_element(plan->commands.begin(),plan->commands.end(),[](const auto& a,const auto& b){return a.terminal!=b.terminal?a.terminal:a.at<b.at;});if(old==plan->commands.end()||!old->terminal)return false;plan->commands.erase(old);}
                if(plan->next_sequence==UINT64_MAX)return false;
                op.command_sequence=++plan->next_sequence;plan->commands.push_back({op.command_id,op.command_sequence,now,false});
            }
        }
        const auto send_at=now_ms();if(send_at>=authorized.execution_expires_at_ms)return false;
        const bool queued=c::runtime_send_operation(owner,op,expected_link,static_cast<uint32_t>(std::min<uint64_t>(op.operation==c::SemanticOperation::key_press?500:1000,authorized.execution_expires_at_ms-send_at)))==ESP_OK;
        if(!queued){std::lock_guard<std::mutex> lock(state.mutex);for(auto& sent:state.sent_plans)if(sent.owner==owner&&sent.prepare.context.plan_id==id)for(auto& command:sent.commands)if(command.id==op.command_id){command.terminal=true;command.at=now_ms();}}
        return queued;
    };
    hooks.abort=[](s::OctopusId owner,const s::Plan& plan){c::PlanContext context;{std::lock_guard<std::mutex> lock(state.mutex);for(const auto& sent:state.sent_plans)if(sent.owner==owner&&sent.prepare.context.plan_id==plan.plan_id)context=sent.prepare.context;}if(context.plan_id)c::runtime_send_control(owner,c::Kind::plan_abort,c::plan_abort_json(context),2000);{std::lock_guard<std::mutex> lock(state.mutex);state.sent_plans.erase(std::remove_if(state.sent_plans.begin(),state.sent_plans.end(),[&](const auto& sent){return sent.owner==owner&&sent.prepare.context.plan_id==plan.plan_id;}),state.sent_plans.end());}};return hooks;
}
struct Activation {cs::ConfigurationV2 candidate;cs::RevisionPair expected;};
std::shared_ptr<Activation> pending;
bool wait_peer(s::OctopusId owner,const std::function<bool(const Peer&)>& ready,uint64_t deadline){while(now_ms()<deadline){{std::lock_guard<std::mutex> lock(state.mutex);auto* p=peer_locked(owner);if(!p||p->rejected)return false;if(ready(*p))return true;}vTaskDelay(pdMS_TO_TICKS(20));}return false;}
bool send_transfer(s::OctopusId owner,s::BootId boot,const std::string& payload,uint64_t revision,std::string kind){
    c::SnapshotManifest manifest;manifest.transfer_id=fresh<s::MessageId>();manifest.owner_octopus_id=kind=="configuration"?owner:state.system->octopus_id();manifest.owner_boot_id=kind=="configuration"?boot:local_boot();manifest.config_revision=revision;manifest.total_length=payload.size();manifest.payload_hash=cs::sha256_hex(payload);manifest.document_kind=std::move(kind);
    {std::lock_guard<std::mutex> lock(state.mutex);auto* p=peer_locked(owner);if(!p)return false;p->outgoing=manifest;p->next=0;p->transfer_ack=p->transfer_complete=p->rejected=false;}
    if(c::runtime_send_control(owner,c::Kind::snapshot_request,c::snapshot_manifest_json(manifest,"begin",0),2000)!=ESP_OK||!wait_peer(owner,[](const Peer& p){return p.transfer_ack;},now_ms()+3000))return false;
    for(uint32_t n=0;n<manifest.chunk_count();++n){
        if(c::runtime_send_control(owner,c::Kind::snapshot_chunk,c::snapshot_chunk_json(manifest,n,payload),2000)!=ESP_OK||!wait_peer(owner,[n](const Peer& p){return p.next>=n+1;},now_ms()+3000))return false;
    }
    return wait_peer(owner,[](const Peer& p){return p.transfer_complete;},now_ms()+3000);
}
void activation(std::shared_ptr<Activation> request){
    bool committed=false;std::vector<s::OctopusId> remote;std::string reason;cs::ConfigurationV2 candidate;
    auto finish=[&](bool ok,std::string error){if(!ok&&!committed)cs::cancel_staged_configuration();if(committed)c::runtime_configuration_changed(candidate.config_revision,cs::executable_policy_hash(candidate));std::lock_guard<std::mutex> lock(state.mutex);state.status.phase=ok?"applied":"failed";state.status.error=std::move(error);state.activating=false;};
    if(cs::stage_system_configuration(std::move(request->candidate),request->expected,reason)!=ESP_OK||cs::staged_system_configuration(candidate)!=ESP_OK){finish(false,reason.empty()?"Configuration staging failed":reason);return;}
    const auto digest=cs::staged_configuration_digest();const auto required=cs::required_staged_owners();const auto local=state.system->octopus_id();
    std::set<s::OctopusId> targets;
    for(auto owner:required)if(owner!=local)targets.insert(owner);
    // Presentation changes also reach connected owners, without installing new
    // execution grants or changing an executable policy generation.
    if(required.empty())for(const auto& p:c::runtime_peers())if(p.ready)targets.insert(p.id);
    {std::lock_guard<std::mutex> lock(state.mutex);state.status.phase="preparing";state.status.config_revision=candidate.config_revision;state.status.presentation_revision=candidate.presentation_revision;state.status.pending_owners.assign(targets.begin(),targets.end());}
    for(auto owner:targets){
        auto runtime=runtime_peer(owner);if(!runtime||!runtime->ready){finish(false,"Required owner is offline or has not synchronized");return;}
        const auto slice=scoped_configuration(candidate,owner);const auto payload=cs::serialize_configuration_v2(slice);const auto policy=cs::executable_policy_hash(slice);
        bool capacity=true;{std::lock_guard<std::mutex> lock(state.mutex);auto* p=peer_locked(owner);if(!p)capacity=false;else{p->boot=runtime->boot_id;p->policy_hash=policy;p->ready=p->applied=p->rejected=false;}}if(!capacity){finish(false,"Owner synchronization capacity exceeded");return;}
        if(!send_transfer(owner,runtime->boot_id,payload,candidate.config_revision,"configuration")){finish(false,"Owner candidate transfer did not complete");return;}
        const auto hash=cs::sha256_hex(payload);
        if(c::runtime_send_control(owner,c::Kind::config_prepare,c::config_control_json(candidate.config_revision,candidate.presentation_revision,runtime->boot_id,hash,policy,"prepare"),2000)!=ESP_OK||!wait_peer(owner,[](const Peer& p){return p.ready;},now_ms()+5000)){finish(false,"Owner rejected or timed out preparing configuration");return;}
        if(std::find(required.begin(),required.end(),owner)!=required.end()&&cs::mark_configuration_ready(owner,digest)!=ESP_OK){finish(false,"Owner prepare correlation was lost");return;}
        remote.push_back(owner);
    }
    {std::lock_guard<std::mutex> lock(state.mutex);state.status.phase="activating";}
    const bool local_required=std::find(required.begin(),required.end(),local)!=required.end();
    auto result=state.system->configure_v2(candidate,[local,local_required,digest]{if(local_required&&cs::mark_configuration_ready(local,digest)!=ESP_OK)return ESP_ERR_INVALID_STATE;cs::ConfigurationV2 activated;std::string error;return cs::activate_system_configuration(activated,error);});
    if(result!=ESP_OK){finish(false,"Local owner rejected configuration before activation");return;}committed=true;
    // Installation may start the qualified attachment's bounded 30-second
    // native scan before publishing applied metadata. Queue acceptance alone
    // is not an ACK; allow that scan plus a publication margin to finish.
    std::string application_detail;
    if(!wait_installed(candidate,35000,&application_detail)){finish(false,"Configuration committed; local owner application timed out. "+application_detail);return;}
    cs::configuration_applied(local,candidate.revisions(),cs::executable_policy_hash(candidate));
    for(auto owner:remote){s::BootId boot;std::string hash,policy;{std::lock_guard<std::mutex> lock(state.mutex);auto* p=peer_locked(owner);boot=p->boot;hash=p->outgoing.payload_hash;policy=p->policy_hash;}
        if(c::runtime_send_control(owner,c::Kind::config_activate,c::config_control_json(candidate.config_revision,candidate.presentation_revision,boot,hash,policy,"activate"),2000)!=ESP_OK||!wait_peer(owner,[](const Peer& p){return p.applied;},now_ms()+40000)){finish(false,"Configuration committed; an owner has not acknowledged application");return;}
        cs::configuration_applied(owner,candidate.revisions(),cs::executable_policy_hash(candidate));
        {std::lock_guard<std::mutex> lock(state.mutex);auto& owners=state.status.pending_owners;owners.erase(std::remove(owners.begin(),owners.end(),owner),owners.end());}
    }
    finish(true,{});
}
void peer_revoked(s::OctopusId owner){
    {std::lock_guard<std::mutex> lock(state.mutex);state.peers.erase(std::remove_if(state.peers.begin(),state.peers.end(),[&](const auto& p){return p.id==owner;}),state.peers.end());state.sent_plans.erase(std::remove_if(state.sent_plans.begin(),state.sent_plans.end(),[&](const auto& plan){return plan.owner==owner;}),state.sent_plans.end());received_holds.erase(std::remove_if(received_holds.begin(),received_holds.end(),[&](const auto& h){return h.coordinator==owner;}),received_holds.end());outcomes.erase(std::remove_if(outcomes.begin(),outcomes.end(),[&](const auto& o){return o.coordinator==owner;}),outcomes.end());state.received_plans.erase(std::remove_if(state.received_plans.begin(),state.received_plans.end(),[&](const auto& plan){return plan.coordinator==owner;}),state.received_plans.end());}
    state.system->update_remote_owner(owner,{},false);
}
c::RuntimeLifecycleHooks lifecycle_hooks(){
    // Main owns retirement of the local controller services. Do not replace its
    // web/home/input shutdown callback when attaching synchronization.
    c::RuntimeLifecycleHooks hooks;hooks.revoked=peer_revoked;return hooks;
}
void adopt_enrolled_owners(){
    if(!c::runtime_status().coordinator)return;
    {std::lock_guard<std::mutex> lock(state.mutex);if(state.activating)return;}
    auto config=current();if(!config)return;
    auto local=*config;bool local_changed=false;std::string local_error;
    if(cs::reconcile_hardware_profile(local,state.system->octopus_id(),state.system->supported_tentacles(),state.system->configured_tentacles_mask(),[](uint8_t* data,size_t size){esp_fill_random(data,size);return true;},local_changed,local_error)&&local_changed){activate_distributed_configuration(std::move(local),config->revisions(),fresh<s::MessageId>());return;}
    for(const auto& peer:c::runtime_peers()){
        if(!peer.ready||!peer.enrolled)continue;
        const auto known=std::find_if(config->octopuses.begin(),config->octopuses.end(),[&](const auto& b){return b.octopus_id==peer.id;});
        if(known!=config->octopuses.end()&&known->supported_tentacles>=peer.supported_tentacles)continue;
        auto candidate=*config;
        if(known==config->octopuses.end()){
            const auto generated="Amity "+std::to_string(config->octopuses.size()+1);std::string label=generated;
            candidate.octopuses.push_back({peer.id,label,generated,label!=generated,peer.supported_tentacles});
        }else for(auto& board:candidate.octopuses)if(board.octopus_id==peer.id)board.supported_tentacles=peer.supported_tentacles;
        activate_distributed_configuration(std::move(candidate),config->revisions(),fresh<s::MessageId>());return;
    }
}
void renew_authority_if_due(uint64_t& next_authority){
    const auto now=now_ms();const bool periodic=now>=next_authority;
    struct Renewal{s::OctopusId owner;s::BootId boot;s::LinkId link;c::PlanContext context;s::ExecutionLeaseId previous;};
    std::vector<Renewal> active;
    {std::lock_guard<std::mutex> lock(state.mutex);for(const auto& sent:state.sent_plans)if(sent.active){auto* peer=peer_locked(sent.owner);if(peer&&!peer->maintenance)active.push_back({sent.owner,sent.boot,sent.link,sent.prepare.context,sent.last_control_lease});}}
    for(const auto& sent:active){
        const auto peer=runtime_peer(sent.owner);if(!peer||!peer->ready||peer->boot_id!=sent.boot||!peer->control_lease||peer->control_usable_until_ms<=now+100||(!periodic&&peer->control_lease==sent.previous))continue;
        const auto link=c::runtime_link_id(sent.owner,sent.boot);if(!link||*link!=sent.link)continue;
        c::PlanBarrier renewal;renewal.context=sent.context;renewal.phase="renew";renewal.execution_lease_id=peer->control_lease;renewal.authority_ttl_ms=static_cast<uint32_t>(std::min<uint64_t>(5000,peer->control_usable_until_ms-now));
        if(c::runtime_send_control(sent.owner,c::Kind::plan_barrier,c::plan_barrier_json(renewal),5000)==ESP_OK){std::lock_guard<std::mutex> lock(state.mutex);for(auto& current:state.sent_plans)if(current.owner==sent.owner&&current.boot==sent.boot&&current.link==sent.link&&same_context(current.prepare.context,sent.context))current.last_control_lease=peer->control_lease;}
    }
    // The existing general1s cadence stays unchanged. A newly issued control
    // lease is also an authority state change: forward it immediately, so its
    // rollover cannot leave a healthy active route with a scheduling gap.
    if(periodic)next_authority=now+1000;
}
void worker(void*){
    uint64_t next_state=0,next_authority=0;
    for(;;){
        refresh_remote_equipment_if_due();
        std::shared_ptr<Activation> request;std::vector<std::pair<s::OctopusId,c::PlanBarrier>> fences;uint64_t revision=0;std::string policy;
        {std::lock_guard<std::mutex> lock(state.mutex);request=std::move(pending);for(const auto& plan:state.received_plans)if(plan.pending_fence)fences.push_back({plan.coordinator,*plan.pending_fence});if(state.reconfigure_at&&now_ms()>=state.reconfigure_at){revision=state.new_revision;policy=state.new_policy;state.reconfigure_at=0;}}
        if(revision)c::runtime_configuration_changed(revision,policy);
        if(request){activation(std::move(request));continue;}
        for(const auto& item:fences){const auto& fence=item.second;if(state.system->remote_route_quiescent(*fence.old_session_id,fence.old_route_generation)){plan_reply(item.first,fence.context,"fence",true,"quiescent");std::lock_guard<std::mutex> lock(state.mutex);auto* plan=received_locked(fence.context.plan_id);if(plan&&same_context(plan->prepare.context,fence.context)){plan->pending_fence.reset();plan->fenced=true;retire_received_locked(*fence.old_session_id,fence.old_route_generation);}}}
        std::vector<PendingOutcome> pending_outcomes;{std::lock_guard<std::mutex> lock(state.mutex);pending_outcomes=outcomes;}
        for(const auto& item:pending_outcomes){auto outcome=state.system->command_outcome(item.operation.command_id);if(!outcome||outcome->result==item.last)continue;const auto result=outcome->result;c::runtime_send_control(item.coordinator,c::Kind::operation_result,operation_reply(item.operation,step_name(result),outcome_error(*outcome)),1000);std::lock_guard<std::mutex> lock(state.mutex);if(auto* plan=received_locked(item.operation.plan_id))plan->commands.result(item.operation.command_id,result==s::StepResult::observed?c::Outcome::observed:result==s::StepResult::applied?c::Outcome::applied:result==s::StepResult::rejected?c::Outcome::rejected:result==s::StepResult::timed_out?c::Outcome::failed:c::Outcome::unknown,now_ms());for(auto& entry:outcomes)if(entry.operation.command_id==item.operation.command_id)entry.last=result;}
        {std::lock_guard<std::mutex> lock(state.mutex);outcomes.erase(std::remove_if(outcomes.begin(),outcomes.end(),[](const auto& item){return now_ms()>=item.deadline+5000||item.last==s::StepResult::observed||item.last==s::StepResult::rejected||item.last==s::StepResult::timed_out||item.last==s::StepResult::unknown;}),outcomes.end());}
        {std::lock_guard<std::mutex> lock(state.mutex);
            // Retain terminal correlations beyond every admitted message TTL;
            // never evict a live route or a plan with a pending wire outcome.
            state.received_plans.erase(std::remove_if(state.received_plans.begin(),state.received_plans.end(),[](const auto& plan){return plan.retired_at&&now_ms()-plan.retired_at>=6000&&std::none_of(outcomes.begin(),outcomes.end(),[&](const auto& outcome){return outcome.operation.plan_id==plan.prepare.context.plan_id;});}),state.received_plans.end());
            state.sent_plans.erase(std::remove_if(state.sent_plans.begin(),state.sent_plans.end(),[](const auto& plan){return plan.retired_at&&now_ms()-plan.retired_at>=6000;}),state.sent_plans.end());
        }
        renew_authority_if_due(next_authority);
        if(now_ms()>=next_state){
            const auto status=c::runtime_status();if(status.member){auto config=current();if(config){for(const auto& peer:c::runtime_peers())if(peer.ready){const auto body=synchronization::owner_state_json(*config,state.system->octopus_id(),local_boot(),status.epoch,state.system->local_policy_snapshot(),state.system->native_observations(),now_ms());if(!body.empty())send_transfer(peer.id,peer.boot_id,body,config->config_revision,"owner_state");}}}
            else if(status.coordinator){for(const auto& peer:c::runtime_peers())if(!peer.ready)state.system->update_remote_owner(peer.id,{},false);adopt_enrolled_owners();}
            next_state=now_ms()+1500;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
} // namespace
esp_err_t start_configuration_synchronization(SystemControl& control){
    {std::lock_guard<std::mutex> lock(state.mutex);if(state.system)return state.system==&control?ESP_OK:ESP_ERR_INVALID_STATE;state.system=&control;}
    control.set_remote_execution_hooks(remote_hooks());c::runtime_control_hooks({snapshot,synchronize,control_message,link_disconnected});c::runtime_operation_handler(operation);
    // Retirement fences the native owner and clears cached peer execution.
    c::runtime_lifecycle_hooks(lifecycle_hooks());
    #ifdef ESP_PLATFORM
    const auto created=xTaskCreateWithCaps(worker,"system_sync",12288,nullptr,2,nullptr,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
#else
    const auto created=xTaskCreate(worker,"system_sync",12288,nullptr,2,nullptr);
#endif
    if(created!=pdPASS){std::lock_guard<std::mutex> lock(state.mutex);state.system=nullptr;return ESP_ERR_NO_MEM;}return ESP_OK;
}
esp_err_t activate_distributed_configuration(cs::ConfigurationV2 candidate,cs::RevisionPair expected,s::MessageId request){
    std::string error;if(!request||!cs::validate_configuration_v2(candidate,error))return ESP_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lock(state.mutex);if(!state.system||state.activating||c::runtime_status().member)return ESP_ERR_INVALID_STATE;
    state.status={request,0,0,"admitted",{}, {}};state.activating=true;pending=std::make_shared<Activation>(Activation{std::move(candidate),expected});return ESP_OK;
}
void set_owner_maintenance(s::OctopusId owner,bool maintenance){
    SystemControl* system=nullptr;{std::lock_guard<std::mutex> lock(state.mutex);auto* peer=peer_locked(owner);if(!peer)return;if(peer->maintenance_generation==UINT64_MAX)maintenance=true;else ++peer->maintenance_generation;peer->maintenance=maintenance;peer->execution_ready=false;system=state.system;if(maintenance)for(auto& sent:state.sent_plans)if(sent.owner==owner)sent.active=false;}
    // Clearing the ban never restores cached grants. A subsequent exact owner
    // snapshot must revalidate boot/config/policy/bindings before readiness.
    if(system)system->update_remote_owner(owner,{},false);
}
esp_err_t request_equipment_scan(){
    SystemControl* system=nullptr;{std::lock_guard<std::mutex> lock(state.mutex);system=state.system;}
    const auto runtime=c::runtime_status();if(!system||runtime.member||(runtime.coordinator&&!runtime.authority_ready))return ESP_ERR_INVALID_STATE;
    const auto config=system->active_configuration();if(!config)return ESP_ERR_INVALID_STATE;
    bool admitted=system->discover();std::set<s::OctopusId> owners;
    for(const auto& domain:config->domains)if(domain.owner_octopus_id!=system->octopus_id())owners.insert(domain.owner_octopus_id);
    for(auto owner:owners){
        bool maintenance=false;{std::lock_guard<std::mutex> lock(state.mutex);for(const auto& peer:state.peers)if(peer.id==owner)maintenance=peer.maintenance;}
        const auto peer=runtime_peer(owner);const auto now=now_ms();
        if(maintenance||!peer||!peer->ready||!peer->control_lease||peer->control_usable_until_ms<=now+100){admitted=false;continue;}
        const auto request=fresh<s::MessageId>();const auto body="{\"request_id\":\""+request.str()+"\",\"execution_lease_id\":\""+peer->control_lease.str()+"\"}";
        if(c::runtime_send_control(owner,c::Kind::scan_request,body,1000)!=ESP_OK)admitted=false;
    }
    return admitted?ESP_OK:ESP_ERR_INVALID_STATE;
}
std::vector<s::CecObservation> discovered_equipment(){
    if(!state.system)return {};
    auto result=state.system->native_observations();const auto now=now_ms();const auto peers=c::runtime_peers();
    std::lock_guard<std::mutex> lock(state.mutex);for(const auto& p:state.peers)for(auto observation:p.observations){const bool current=std::any_of(peers.begin(),peers.end(),[&](const auto& peer){return peer.id==p.id&&peer.boot_id==p.boot&&peer.ready;});observation.stale=observation.stale||p.maintenance||!current||now<observation.received_at_ms||now-observation.received_at_ms>=30000;result.push_back(std::move(observation));}
    return result;
}
ConfigurationActivationStatus configuration_activation_status(){std::lock_guard<std::mutex> lock(state.mutex);return state.status;}
} // namespace amity
