#pragma once
#include "amity/system/coordinator.hpp"
#include "system_configuration.hpp"
#include "esp_err.h"
namespace amity {
extern uint64_t synchronization_test_now;
class SystemControl {
public:
    config_store::ConfigurationV2 configuration;
    system::OctopusId identity;
    system::Coordinator owner{[](uint8_t* data,size_t size){static unsigned next=100;std::fill(data,data+size,0);data[size-1]=++next;return true;}};
    std::vector<std::pair<system::SessionId,uint64_t>> route_fences;bool reject_fence=false;std::optional<system::AdmissionCode> cleanup_admission;
    std::function<std::optional<bool>(const system::AuthorizedCecOperation&)> classify_remote_key;
    unsigned admissions=0; bool published=true;uint64_t publication_at_ms=0;std::optional<system::InputIntent> last_input;
    system::ConsortiumId consortium_id()const{return configuration.consortium_id;}
    system::OctopusId octopus_id()const{return identity;}
    uint8_t supported_tentacles()const{return 1;}
    uint8_t configured_tentacles_mask()const{return 1;}
    system::BootId boot_identity()const{return {};}
    std::optional<config_store::ConfigurationV2> active_configuration()const{return configuration;}
    std::vector<system::PortPolicySnapshot> local_policy_snapshot()const{return owner.policies();}
    system::ExecutionSnapshot execution_snapshot()const{auto snapshot=owner.snapshot();if(published&&synchronization_test_now>=publication_at_ms){snapshot.applied_config_revision=configuration.config_revision;snapshot.applied_presentation_revision=configuration.presentation_revision;snapshot.applied_policy_hash=config_store::executable_policy_hash(configuration);}return snapshot;}
    esp_err_t configure_member_slice(config_store::ConfigurationV2 c,system::OctopusId,uint64_t){configuration=std::move(c);return ESP_OK;}
    system::AdmissionResult prepare_member_configuration(config_store::ConfigurationV2,system::OctopusId,uint64_t,std::string){return {system::AdmissionCode::accepted,{}};}
    esp_err_t activate_member_configuration(const std::string&){return ESP_OK;}
    system::AdmissionResult prepare_remote_plan(system::Plan p,std::vector<system::PortPolicySnapshot> policies){return owner.prepare_remote_plan(p,policies,synchronization_test_now);}
    system::AdmissionResult fence_remote_route(system::SessionId s,uint64_t g){route_fences.push_back({s,g});return reject_fence?system::AdmissionResult{system::AdmissionCode::overloaded,"injected owner queue full"}:owner.fence_remote_route(s,g);}
    bool remote_route_quiescent(system::SessionId s,uint64_t g)const{return owner.remote_route_quiescent(s,g);}
    system::AdmissionResult activate_remote_plan(system::PlanId id,uint32_t ttl){return owner.activate_remote_plan(id,ttl,synchronization_test_now);}
    std::vector<system::CecObservation> native_observations()const{return {};}
    bool discover(){return true;}
    system::AdmissionResult discover_authorized(uint32_t){return {system::AdmissionCode::accepted,{}};}
    system::AdmissionResult renew_remote_authority(system::PlanId,system::SessionId,uint64_t,uint32_t){return {system::AdmissionCode::accepted,{}};}
    system::AdmissionResult abort_remote_plan(system::PlanId id){owner.abort_remote_plan(id);return {system::AdmissionCode::accepted,{}};}
    system::AdmissionResult admit_authorized(system::AuthorizedCecOperation op){++admissions;if(op.kind==system::OperationKind::release_key&&cleanup_admission)return {*cleanup_admission,"injected cleanup owner admission"};return owner.execute_authorized(op,synchronization_test_now);}
    std::optional<bool> remote_key_active(const system::AuthorizedCecOperation& op,bool synchronously_admitted=false)const{return synchronously_admitted&&classify_remote_key?classify_remote_key(op):std::nullopt;}
    system::AdmissionResult update_remote_owner(system::OctopusId,std::vector<system::PortPolicySnapshot>,bool){return {system::AdmissionCode::accepted,{}};}
    system::AdmissionResult remote_plan_result(system::OctopusId,system::PlanId,system::RemotePlanPhase,bool,std::string){return {system::AdmissionCode::accepted,{}};}
    system::AdmissionResult remote_operation_result(system::OctopusId,system::CommandId,system::StepResult,std::string){return {system::AdmissionCode::accepted,{}};}
    void set_remote_execution_hooks(system::RemoteExecutionHooks hooks){owner.remote_execution_hooks(std::move(hooks));}
    system::InputContext input_context()const{return {};}
    system::AdmissionResult submit_input_intent(system::InputIntent input){last_input=input;return {system::AdmissionCode::accepted,{}};}
    esp_err_t retire_membership(){return ESP_OK;}
    std::optional<system::CommandOutcome> command_outcome(system::CommandId id)const{for(const auto& result:owner.snapshot().command_outcomes)if(result.command_id==id)return result;return {};}
    esp_err_t configure_v2(config_store::ConfigurationV2 c,std::function<esp_err_t()> commit){auto result=commit();if(result==ESP_OK)configuration=std::move(c);return result;}
};
}
