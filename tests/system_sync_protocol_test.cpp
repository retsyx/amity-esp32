#define main configuration_contract_tests
#include "system_configuration_test.cpp"
#undef main
#include "system_wire.hpp"
#include "../components/system_sync/system_sync.cpp"
namespace amity { uint64_t synchronization_test_now=100; }
int64_t esp_timer_get_time(){return static_cast<int64_t>(amity::synchronization_test_now*1000);}
void esp_fill_random(void* target,size_t size){static unsigned next=100;auto* bytes=static_cast<uint8_t*>(target);std::fill(bytes,bytes+size,0);bytes[size-1]=++next;}
int xTaskCreate(TaskFunction_t,const char*,unsigned,void*,unsigned,TaskHandle_t*){return pdPASS;}
void vTaskDelete(TaskHandle_t){}
void vTaskDelay(unsigned ticks){amity::synchronization_test_now+=ticks;}
namespace amity::consortium {
bool replace_link_before_operation=false;RuntimeControlHooks installed;RuntimeOperation actuator;std::vector<RuntimePeerStatus> peers;std::vector<Operation> sent_operations;std::vector<uint32_t> sent_operation_ttls;
struct Packet {model::OctopusId owner;Kind kind;std::string payload;uint32_t ttl=0;uint64_t at=0;};std::vector<Packet> packets;
bool member_mode=true;RuntimeLifecycleHooks registered_lifecycle;
RuntimeStatus runtime_status(){RuntimeStatus status;status.member=member_mode;status.coordinator=!member_mode;status.authority_ready=!member_mode;status.epoch=1;return status;}
std::vector<RuntimePeerStatus> runtime_peers(){return peers;}
model::LinkId current_test_link=*model::LinkId::parse("00000000000000000000000000000070");
std::optional<model::LinkId> runtime_link_id(model::OctopusId,model::BootId){return current_test_link;}
void runtime_control_hooks(RuntimeControlHooks hooks){installed=std::move(hooks);}
void runtime_operation_handler(RuntimeOperation fn){actuator=std::move(fn);}
void runtime_lifecycle_hooks(RuntimeLifecycleHooks hooks){registered_lifecycle=std::move(hooks);}
esp_err_t runtime_send_control(model::OctopusId owner,Kind kind,std::string payload,uint32_t ttl){packets.push_back({owner,kind,std::move(payload),ttl,amity::synchronization_test_now});return ESP_OK;}
esp_err_t runtime_send_cleanup(model::OctopusId owner,std::string payload,model::LinkId expected,uint32_t ttl){if(expected!=current_test_link)return ESP_ERR_INVALID_STATE;return runtime_send_control(owner,Kind::hold_release,std::move(payload),ttl);}
esp_err_t runtime_send_operation(model::OctopusId,const Operation& op,model::LinkId expected,uint32_t ttl){if(replace_link_before_operation){replace_link_before_operation=false;current_test_link=*model::LinkId::parse("00000000000000000000000000000071");}if(expected!=current_test_link)return ESP_ERR_INVALID_STATE;sent_operations.push_back(op);sent_operation_ttls.push_back(ttl);return ESP_OK;}
void runtime_configuration_changed(uint64_t,const std::string&){}
}
namespace config_store {
Memory configuration_memory;ConfigurationStore production(configuration_memory);
esp_err_t stage_system_configuration(ConfigurationV2 config,RevisionPair expected,std::string& error){return production.stage(std::move(config),expected,error)==ConfigResult::ok?ESP_OK:ESP_FAIL;}
esp_err_t staged_system_configuration(ConfigurationV2& out){if(!production.staged())return ESP_FAIL;out=*production.staged();return ESP_OK;}
std::string staged_configuration_digest(){return production.candidate_digest();}
std::vector<model::OctopusId> required_staged_owners(){return production.staged_owners();}
esp_err_t cancel_staged_configuration(){production.cancel_stage();return ESP_OK;}
esp_err_t mark_configuration_ready(model::OctopusId owner,std::string_view hash){return production.ready(owner,hash)==ConfigResult::ok?ESP_OK:ESP_FAIL;}
esp_err_t activate_system_configuration(ConfigurationV2& out,std::string& error){return production.commit(out,error)==ConfigResult::ok?ESP_OK:ESP_FAIL;}
esp_err_t configuration_applied(model::OctopusId owner,RevisionPair pair,std::string_view hash){return production.applied(owner,pair,hash)==ConfigResult::ok?ESP_OK:ESP_FAIL;}
}
int main(){
    namespace wire=amity::consortium;namespace sync=amity::synchronization;namespace sys=amity::system;
    amity::SystemControl control;control.configuration=direct_configuration();control.identity=id<sys::OctopusId>(2);const auto boot=id<sys::BootId>(4),coordinator_boot=id<sys::BootId>(5);const auto coordinator=id<sys::OctopusId>(3);control.configuration.controller_octopus_id=coordinator;control.configuration.octopuses.push_back({coordinator,"Controller","Controller",false,1});
    const auto second_zone=id<sys::ZoneId>(60);control.configuration.zones.push_back({second_zone,"Other screen","Other screen",false,{id<sys::ActivityId>(61)}});auto second_activity=control.configuration.activities[0];second_activity.activity_id=id<sys::ActivityId>(61);second_activity.zone_id=second_zone;control.configuration.activities.push_back(second_activity);
    auto topology=configured_topology(control.configuration);topology.ready_owners={control.identity};std::vector<sys::PortPolicySnapshot> policies;
    for(const auto& d:control.configuration.domains)for(const auto& p:d.ports){sys::PortPolicySnapshot policy;policy.port=p.port;policy.owner_boot_id=boot;policy.domain_id=d.domain_id;policy.domain_revision=d.domain_revision;policy.coordinator_epoch=1;policy.config_revision=1;policy.policy_generation=1;policy.authority_expires_at_ms=5100;policy.role=p.role;policy.owned_logical_address=p.role==sys::PortRole::playback_proxy?4:0;policy.physical_address=sys::PhysicalAddressEvidence{p.port,d.domain_id,d.domain_revision,0x1000,sys::PaProvenance::user_confirmed_fallback,{},true};policy.components=topology.components;
        for(const auto& b:control.configuration.binding_policies)if(b.port==p.port){sys::ComponentBinding binding;binding.binding_id=b.binding_id;binding.component_id=b.component_id;binding.capability=b.capability;binding.kind=b.kind;binding.device_name=b.device_name;binding.proxy_identity_resource_id=b.proxy_identity_resource_id;binding.endpoint={p.port,static_cast<uint8_t>(b.kind==sys::BindingKind::proxy_representation?*policy.owned_logical_address:p.port.side==sys::Side::front?0:4)};binding.owner_octopus_id=control.identity;binding.domain_id=d.domain_id;binding.domain_revision=d.domain_revision;binding.binding_revision=1;binding.role=p.role;binding.status=sys::BindingStatus::verified;binding.physical_address_evidence_ref=id<sys::EvidenceId>(20);policy.bindings.push_back({binding,b.discrete_power_on,b.discrete_power_off,b.image_view_on,b.exact_path_selection});topology.bindings.push_back(binding);}policies.push_back(policy);}
    assert(control.owner.install(topology,policies,control.identity,100));
    std::vector<TestWire> wires(policies.size());for(size_t i=0;i<policies.size();++i)wires[i].bind(control.owner,policies[i].port);
    const auto complete_wires=[&]{for(auto& wire:wires)while(wire.take(amity::synchronization_test_now))wire.complete(true,amity::synchronization_test_now);};
    assert(amity::start_configuration_synchronization(control)==ESP_OK);assert(!wire::registered_lifecycle.retire&&wire::registered_lifecycle.revoked);
    sys::Coordinator availability([](uint8_t* data,size_t size){std::fill(data,data+size,1);return true;});auto monitor=policies[0];monitor.port.octopus_id=id<sys::OctopusId>(99);monitor.role=sys::PortRole::monitor;monitor.owned_logical_address.reset();monitor.bindings.clear();monitor.physical_address.reset();assert(availability.install(topology,{monitor},id<sys::OctopusId>(99),100));auto partially_resolved=policies;auto unresolved=partially_resolved[0].bindings[0];unresolved.binding.binding_id=id<sys::BindingId>(98);unresolved.binding.status=sys::BindingStatus::unresolved;unresolved.binding.endpoint.logical_address=15;partially_resolved[0].bindings.push_back(unresolved);assert(availability.update_remote_owner(control.identity,partially_resolved,true,5000,100));
    wire::PlanPrepare prepare;prepare.context={id<sys::PlanId>(30),id<sys::SessionId>(31),1,2,std::string(64,'0'),executable_policy_hash(control.configuration)};prepare.activity_id=control.configuration.activities[0].activity_id;prepare.zone_id=control.configuration.primary_zone_id;const auto& route=control.configuration.routes[0];prepare.route_id=route.route_id;prepare.operation_capacity=route.steps.size();
    for(const auto& policy:policies)for(const auto& permission:policy.bindings){const auto& b=permission.binding;if(std::find(route.required_bindings.begin(),route.required_bindings.end(),b.binding_id)==route.required_bindings.end())continue;prepare.bindings.push_back({b.binding_id,b.component_id,b.domain_id,b.binding_revision,b.domain_revision,b.endpoint});wire::PreparedGrant grant;grant.binding_id=b.binding_id;for(const auto& step:route.steps)if(step.binding_id==b.binding_id&&step.operation==sys::OperationKind::select_path)grant.permitted_paths.push_back(step.arguments.physical_path);prepare.grants.push_back(grant);}
    prepare.context.plan_digest=amity::plan_digest(prepare);
    wire::Envelope envelope;envelope.origin_octopus_id=coordinator;envelope.destination_octopus_id=control.identity;envelope.origin_boot_id=coordinator_boot;envelope.destination_boot_id=boot;envelope.coordinator_epoch=1;envelope.config_revision=1;wire::JsonError detail;
    envelope.kind=wire::Kind::plan_prepare;assert(wire::parse_json(wire::plan_prepare_json(prepare),envelope.payload,detail));assert(wire::installed.control(coordinator,envelope,0,id<sys::LinkId>(0x70))==wire::Error::none);assert(control.admissions==0);
    wire::PlanBarrier barrier;barrier.context=prepare.context;barrier.phase="activate";barrier.authority_ttl_ms=5000;envelope.kind=wire::Kind::plan_barrier;assert(wire::parse_json(wire::plan_barrier_json(barrier),envelope.payload,detail));assert(wire::installed.control(coordinator,envelope,0,id<sys::LinkId>(0x70))==wire::Error::not_ready);
    barrier.phase="fence";assert(wire::parse_json(wire::plan_barrier_json(barrier),envelope.payload,detail));assert(wire::installed.control(coordinator,envelope,0,id<sys::LinkId>(0x70))==wire::Error::none);barrier.phase="activate";assert(wire::parse_json(wire::plan_barrier_json(barrier),envelope.payload,detail));assert(wire::installed.control(coordinator,envelope,0,id<sys::LinkId>(0x70))==wire::Error::none);
    const auto& binding=prepare.bindings[0];wire::Operation op;op.command_id=id<sys::CommandId>(40);op.plan_id=prepare.context.plan_id;op.session_id=prepare.context.session_id;op.route_generation=1;op.binding_id=binding.binding_id;op.component_id=binding.component_id;op.domain_id=binding.domain_id;op.binding_revision=1;op.domain_revision=binding.domain_revision;op.target=binding.target;op.operation=wire::SemanticOperation::ensure_power;op.on=true;
    op.command_sequence=1;assert(wire::actuator(op,envelope,600,id<sys::LinkId>(0x70))==wire::Error::none);const auto admitted=control.admissions;assert(wire::actuator(op,envelope,600,id<sys::LinkId>(0x70))==wire::Error::duplicate&&control.admissions==admitted);
    complete_wires();
    // A second independent logical context on the same authenticated owner must
    // retain the first context's ledger and exact grants. Global resource
    // arbitration happens before these owner-side reservations.
    auto next=prepare;next.context.plan_id=id<sys::PlanId>(62);next.context.session_id=id<sys::SessionId>(63);next.context.policy_generation=3;next.activity_id=second_activity.activity_id;next.zone_id=second_zone;next.context.plan_digest=amity::plan_digest(next);
    envelope.kind=wire::Kind::plan_prepare;assert(wire::parse_json(wire::plan_prepare_json(next),envelope.payload,detail));assert(wire::installed.control(coordinator,envelope,0,id<sys::LinkId>(0x70))==wire::Error::none);
    auto next_barrier=barrier;next_barrier.context=next.context;next_barrier.phase="fence";envelope.kind=wire::Kind::plan_barrier;assert(wire::parse_json(wire::plan_barrier_json(next_barrier),envelope.payload,detail));assert(wire::installed.control(coordinator,envelope,0,id<sys::LinkId>(0x70))==wire::Error::none);next_barrier.phase="activate";assert(wire::parse_json(wire::plan_barrier_json(next_barrier),envelope.payload,detail));assert(wire::installed.control(coordinator,envelope,0,id<sys::LinkId>(0x70))==wire::Error::none);
    assert(amity::state.received_plans.size()==2);assert(wire::actuator(op,envelope,600,id<sys::LinkId>(0x70))==wire::Error::duplicate&&control.admissions==admitted);
    auto refreshed=control.owner.policies();for(auto& policy:refreshed)policy.route_grants.clear();assert(control.owner.refresh_bindings(topology,refreshed,100));
    op.command_id=id<sys::CommandId>(64);op.command_sequence=2;assert(wire::actuator(op,envelope,600,id<sys::LinkId>(0x70))==wire::Error::none);const auto after_refresh=control.admissions;
    complete_wires();
    ++op.binding_revision;op.command_id=id<sys::CommandId>(65);assert(wire::actuator(op,envelope,600,id<sys::LinkId>(0x70))==wire::Error::stale_binding&&control.admissions==after_refresh);--op.binding_revision;
    // Exhausting the transport outcome reservation never starts another frame,
    // and retries retain a rejected result rather than a false admission.
    amity::outcomes.resize(32);op.command_id=id<sys::CommandId>(41);op.command_sequence=3;assert(wire::actuator(op,envelope,600,id<sys::LinkId>(0x70))==wire::Error::queue_full&&control.admissions==after_refresh);assert(wire::actuator(op,envelope,600,id<sys::LinkId>(0x70))==wire::Error::duplicate&&wire::packets.back().payload.find("\"result\":\"rejected\"")!=std::string::npos);amity::outcomes.clear();
    amity::received_locked(op.plan_id)->commands.result(op.command_id,wire::Outcome::failed,100);assert(wire::actuator(op,envelope,600,id<sys::LinkId>(0x70))==wire::Error::duplicate&&wire::packets.back().payload.find("timed_out")!=std::string::npos);
    // All exact-token cleanup slots are reserved before admission.
    amity::received_holds.resize(16);auto nav=std::find_if(topology.bindings.begin(),topology.bindings.end(),[](const auto& b){return b.capability==sys::Capability::navigation;});assert(nav!=topology.bindings.end());op.command_id=id<sys::CommandId>(42);op.command_sequence=4;op.operation=wire::SemanticOperation::key_press;op.binding_id=nav->binding_id;op.component_id=nav->component_id;op.domain_id=nav->domain_id;op.domain_revision=nav->domain_revision;op.target=nav->endpoint;op.hold_token=id<sys::HoldToken>(45);op.repeat=true;assert(wire::actuator(op,envelope,600,id<sys::LinkId>(0x70))==wire::Error::queue_full&&control.admissions==after_refresh);
    auto* peer=amity::peer_locked(coordinator);peer->outgoing={id<sys::MessageId>(50),control.identity,boot,1,10,std::string(64,'a'),"configuration"};auto wrong=peer->outgoing;++wrong.total_length;envelope.kind=wire::Kind::snapshot_request;assert(wire::parse_json(wire::snapshot_manifest_json(wrong,"ack",0),envelope.payload,detail));assert(wire::installed.control(coordinator,envelope,0,id<sys::LinkId>(0x70))==wire::Error::invalid_message);
    // Completed correlations may rotate indefinitely in one active plan. Only
    // the coordinator's fresh-command hook allocates a sequence; retransmitting
    // a retained operation preserves that exact number.
    peer->policies=control.owner.policies();for(auto& policy:peer->policies){policy.port.octopus_id=coordinator;for(auto& permission:policy.bindings){permission.binding.owner_octopus_id=coordinator;permission.binding.endpoint.port.octopus_id=coordinator;}}
    for(auto& domain:control.configuration.domains){domain.owner_octopus_id=coordinator;for(auto& p:domain.ports)p.port.octopus_id=coordinator;}for(auto& item:control.configuration.components)for(auto& name:item.record.qualified_names)name.port.octopus_id=coordinator;for(auto& policy:control.configuration.binding_policies){policy.port.octopus_id=coordinator;if(policy.device_name)policy.device_name->port.octopus_id=coordinator;}for(auto& resource:control.configuration.resources)resource.owner_octopus_id=coordinator;
    peer->execution_ready=true;wire::RuntimePeerStatus live;live.id=coordinator;live.boot_id=boot;live.config_revision=1;live.ready=true;live.control_lease=id<sys::ExecutionLeaseId>(70);wire::peers={live};amity::SentPlan sent;sent.owner=coordinator;sent.boot=boot;sent.link=id<sys::LinkId>(0x70);sent.prepare=prepare;sent.active=true;amity::state.sent_plans.push_back(sent);auto hooks=amity::remote_hooks();
    sys::AuthorizedCecOperation outgoing;outgoing.kind=sys::OperationKind::ensure_power;outgoing.binding_id=binding.binding_id;outgoing.component_id=binding.component_id;outgoing.target=binding.target;outgoing.target.port.octopus_id=coordinator;outgoing.execution_expires_at_ms=600;outgoing.fence={boot,1,1,binding.domain_revision,1,prepare.context.session_id,1,peer->policies[0].policy_generation};
    envelope.origin_boot_id=boot;
    for(unsigned n=0;n<1000;++n){outgoing.command_id=id<sys::CommandId>(1000+n);assert(hooks.send(coordinator,prepare.context.plan_id,outgoing));assert(wire::sent_operations.back().command_sequence==n+1);envelope.kind=wire::Kind::operation_result;assert(wire::parse_json(amity::operation_reply(wire::sent_operations.back(),"applied"),envelope.payload,detail));assert(wire::installed.control(coordinator,envelope,0,id<sys::LinkId>(0x70))==wire::Error::none);}
    envelope.origin_boot_id=coordinator_boot;
    auto raced_operation=outgoing;raced_operation.command_id=id<sys::CommandId>(9999);const auto before_link_race=wire::sent_operations.size();wire::replace_link_before_operation=true;assert(!hooks.send(coordinator,prepare.context.plan_id,raced_operation)&&wire::sent_operations.size()==before_link_race);wire::current_test_link=id<sys::LinkId>(0x70);
    auto late_up=outgoing;late_up.kind=sys::OperationKind::release_key;late_up.arguments.hold_token=id<sys::HoldToken>(777);const auto sent_before_stale_up=wire::packets.size();wire::current_test_link=id<sys::LinkId>(0x71);assert(!hooks.send(coordinator,prepare.context.plan_id,late_up)&&wire::packets.size()==sent_before_stale_up);wire::current_test_link=id<sys::LinkId>(0x70);
    assert(wire::sent_operation_ttls.back()==500);auto stale_authority=outgoing;++stale_authority.fence.config_revision;assert(!hooks.send(coordinator,prepare.context.plan_id,stale_authority));stale_authority=outgoing;stale_authority.execution_expires_at_ms=amity::synchronization_test_now;assert(!hooks.send(coordinator,prepare.context.plan_id,stale_authority));
    const auto sequence=wire::sent_operations.back().command_sequence;assert(hooks.send(coordinator,prepare.context.plan_id,outgoing)&&wire::sent_operations.back().command_sequence==sequence);assert(amity::state.sent_plans.back().commands.size()==32);
    // A command outcome does not generate another network key transition.
    // The emitting executor owns stream termination and any final operand-free
    // CEC release; a later physical up still carries only its exact token.
    auto state_down=outgoing;state_down.command_id=id<sys::CommandId>(10001);state_down.kind=sys::OperationKind::press_key;
    state_down.component_id=nav->component_id;state_down.binding_id=nav->binding_id;state_down.target=nav->endpoint;state_down.target.port.octopus_id=coordinator;
    state_down.fence.domain_revision=nav->domain_revision;state_down.fence.binding_revision=nav->binding_revision;
    state_down.arguments.key=1;state_down.arguments.hold_token=id<sys::HoldToken>(10002);state_down.arguments.repeat=true;
    wire::peers[0].realtime_lease=id<sys::ExecutionLeaseId>(10003);assert(hooks.send(coordinator,prepare.context.plan_id,state_down));
    const auto transitions=wire::sent_operations.size(),messages=wire::packets.size();
    envelope.origin_boot_id=boot;envelope.kind=wire::Kind::operation_result;
    assert(wire::parse_json(amity::operation_reply(wire::sent_operations.back(),"unknown","nack"),envelope.payload,detail));
    assert(wire::installed.control(coordinator,envelope,0,id<sys::LinkId>(0x70))==wire::Error::none);
    assert(wire::sent_operations.size()==transitions&&wire::packets.size()==messages);
    auto state_up=state_down;state_up.kind=sys::OperationKind::release_key;assert(hooks.send(coordinator,prepare.context.plan_id,state_up));
    wire::Json key_up;assert(wire::packets.size()==messages+1&&wire::packets.back().kind==wire::Kind::hold_release&&wire::parse_json(wire::packets.back().payload,key_up,detail));
    assert(key_up.object.size()==4&&key_up.get("hold_token")->string==state_down.arguments.hold_token->str()&&!key_up.get("key_code")&&!key_up.get("arguments"));
    envelope.origin_boot_id=coordinator_boot;
    // Added devices expose neither input ingress nor remote metadata, even if
    // a caller bypasses transport admission and invokes a registered hook.
    for(auto value:{2,4,6,26,35,36}){envelope.kind=static_cast<wire::Kind>(value);assert(wire::retired_kind(envelope.kind));assert(wire::installed.control(coordinator,envelope,12345,id<sys::LinkId>(0x70))==wire::Error::unauthorized_role);}
    assert(!control.last_input);
    control.published=false;wire::Json document;document.type=wire::Json::Type::string;document.string=serialize_configuration_v2(control.configuration);assert(wire::installed.synchronize(coordinator,document)==wire::Error::storage_failure);
    // Real owner installation may return while bounded native PA preflight is
    // still collecting. The active marker is durable, but application is ACKed
    // only when the exact full configuration appears in the owner snapshot.
    auto activation=std::make_shared<amity::Activation>();activation->candidate=direct_configuration();activation->expected={};
    auto unused=activation->candidate.components.front();unused.record.component_id=id<sys::ComponentId>(600);unused.record.qualified_names.clear();unused.kind="placeholder";unused.capabilities.clear();activation->candidate.components.push_back(unused);
    assert(executable_policy_hash(activation->candidate)!=executable_policy_hash(sync::owner_slice(activation->candidate,control.identity)));
    control.published=true;control.publication_at_ms=amity::synchronization_test_now+4500;wire::peers.clear();
    const auto began=amity::synchronization_test_now;amity::activation(activation);
    assert(amity::synchronization_test_now-began>=4500&&amity::configuration_activation_status().phase=="applied"&&config_store::production.activation_complete());
    auto mismatched_configuration=control.configuration;++mismatched_configuration.presentation_revision;std::string correlation;
    assert(!amity::wait_installed(mismatched_configuration,40,&correlation)&&correlation.find("Expected ")!=std::string::npos);
    mismatched_configuration=control.configuration;++mismatched_configuration.config_revision;assert(!amity::wait_installed(mismatched_configuration,40));
    mismatched_configuration=control.configuration;mismatched_configuration.components.back().kind="audio";assert(!amity::wait_installed(mismatched_configuration,40));
    control.publication_at_ms=amity::synchronization_test_now+4500;document.string=serialize_configuration_v2(control.configuration);
    const auto member_began=amity::synchronization_test_now;assert(wire::installed.synchronize(coordinator,document)==wire::Error::none&&amity::synchronization_test_now-member_began>=4500);
    auto* applied_peer=amity::peer_locked(coordinator);applied_peer->configuration_payload=document.string;
    envelope.kind=wire::Kind::config_activate;assert(wire::parse_json(wire::config_control_json(control.configuration.config_revision,control.configuration.presentation_revision,boot,config_store::sha256_hex(document.string),executable_policy_hash(control.configuration),"activate"),envelope.payload,detail));
    control.publication_at_ms=amity::synchronization_test_now+4500;const auto member_activation_began=amity::synchronization_test_now;
    assert(wire::installed.control(coordinator,envelope,0,id<sys::LinkId>(0x70))==wire::Error::none&&amity::synchronization_test_now-member_activation_began>=4500);
    assert(wire::packets.back().kind==wire::Kind::config_result);wire::Json applied_reply;assert(wire::parse_json(wire::packets.back().payload,applied_reply,detail)&&applied_reply.get("result")->string=="applied");
    // Healthy control-lease rollover is general route authority, never per-key
    // traffic. Deliberately deliver each new4s lease20ms after the regular1s
    // send tick: the old conservative deadline must still never be crossed.
    wire::member_mode=false;wire::packets.clear();amity::state.sent_plans.clear();sent.active=true;sent.last_control_lease={};amity::state.sent_plans.push_back(sent);
    const auto rollover_began=amity::synchronization_test_now;uint64_t next_authority=rollover_began,route_until=rollover_began+5000;unsigned delivered=0,rollovers=0;live.control_lease=id<sys::ExecutionLeaseId>(5000);live.control_usable_until_ms=rollover_began+4900;wire::peers={live};
    for(uint64_t elapsed=0;elapsed<=14000;elapsed+=20){
        amity::synchronization_test_now=rollover_began+elapsed;
        if(elapsed>=4000&&elapsed%4000==20){++rollovers;wire::peers[0].control_lease=id<sys::ExecutionLeaseId>(5000+rollovers);wire::peers[0].control_usable_until_ms=rollover_began+(elapsed/4000)*4000+4900;}
        assert(route_until>amity::synchronization_test_now);amity::renew_authority_if_due(next_authority);
        while(delivered<wire::packets.size()){const auto& packet=wire::packets[delivered++];assert(packet.kind==wire::Kind::plan_barrier&&packet.ttl==5000);wire::PlanBarrier renewal;wire::Json value;assert(wire::parse_json(packet.payload,value,detail)&&wire::parse_plan_barrier(value,renewal)==wire::Error::none&&renewal.phase=="renew"&&renewal.execution_lease_id==wire::peers[0].control_lease);route_until=packet.at+renewal.authority_ttl_ms;assert(route_until<=wire::peers[0].control_usable_until_ms);}
    }
    assert(rollovers==3&&wire::packets.size()<24);wire::member_mode=true;std::cout<<"PASS healthy14s general authority across3 control-lease rollovers; no per-key traffic\n";
    // Link teardown fences only contexts admitted on that exact old TLS link.
    // A failed owner fence remains retryable and cannot approve replacement.
    amity::state.received_plans.clear();amity::received_holds.clear();
    amity::ReceivedPlan old_context;old_context.prepare=prepare;old_context.coordinator=coordinator;old_context.boot=coordinator_boot;old_context.link=id<sys::LinkId>(0x70);old_context.activated=true;
    auto new_context=old_context;new_context.prepare.context.plan_id=id<sys::PlanId>(900);new_context.prepare.context.session_id=id<sys::SessionId>(901);new_context.link=id<sys::LinkId>(0x71);
    amity::state.received_plans={old_context,new_context};control.route_fences.clear();
    assert(wire::installed.disconnected(coordinator,coordinator_boot,id<sys::LinkId>(0x99))&&control.route_fences.empty());
    control.reject_fence=true;assert(!wire::installed.disconnected(coordinator,coordinator_boot,id<sys::LinkId>(0x70)));
    assert(amity::state.received_plans[0].aborted&&!amity::state.received_plans[1].aborted);
    control.reject_fence=false;assert(wire::installed.disconnected(coordinator,coordinator_boot,id<sys::LinkId>(0x70)));
    assert(control.route_fences.size()==2&&control.route_fences[0].first==prepare.context.session_id&&control.route_fences[1].first==prepare.context.session_id);
    amity::RemoteHold prior_hold;prior_hold.coordinator=coordinator;prior_hold.boot=coordinator_boot;prior_hold.link=id<sys::LinkId>(0x70);prior_hold.plan=prepare.context.plan_id;prior_hold.operation.arguments.hold_token=id<sys::HoldToken>(902);prior_hold.operation.fence.session_id=prepare.context.session_id;prior_hold.operation.fence.route_generation=1;
    auto fresh_hold=prior_hold;fresh_hold.link=id<sys::LinkId>(0x71);fresh_hold.plan=new_context.prepare.context.plan_id;fresh_hold.operation.fence.session_id=new_context.prepare.context.session_id;amity::received_holds={prior_hold,fresh_hold};
    assert(wire::installed.disconnected(coordinator,coordinator_boot,id<sys::LinkId>(0x70))&&amity::received_holds.size()==1&&amity::received_holds.front().link==id<sys::LinkId>(0x71));
    assert(wire::installed.disconnected(coordinator,coordinator_boot,id<sys::LinkId>(0x71))&&amity::received_holds.empty());
    // An active matched cleanup overload retains its exact token for link
    // fencing. Stale A-up cannot remove the current B token or close its link.
    fresh_hold.operation.target=binding.target;fresh_hold.operation.binding_id=binding.binding_id;fresh_hold.operation.component_id=binding.component_id;fresh_hold.operation.arguments.hold_token=id<sys::HoldToken>(903);amity::received_holds={fresh_hold};envelope.kind=wire::Kind::hold_release;envelope.origin_boot_id=coordinator_boot;
    const auto cleanup=[&](sys::HoldToken token){wire::Json value;assert(wire::parse_json("{\"plan_id\":\""+fresh_hold.plan.str()+"\",\"session_id\":\""+fresh_hold.operation.fence.session_id.str()+"\",\"route_generation\":\"1\",\"hold_token\":\""+token.str()+"\"}",value,detail));return value;};
    envelope.payload=cleanup(id<sys::HoldToken>(902));assert(wire::installed.control(coordinator,envelope,0,id<sys::LinkId>(0x71))==wire::Error::stale_route&&amity::received_holds.size()==1);
    envelope.payload=cleanup(id<sys::HoldToken>(903));control.cleanup_admission=sys::AdmissionCode::overloaded;assert(wire::installed.control(coordinator,envelope,0,id<sys::LinkId>(0x71))==wire::Error::queue_full&&amity::received_holds.size()==1);
    control.cleanup_admission=sys::AdmissionCode::expired;assert(wire::installed.control(coordinator,envelope,0,id<sys::LinkId>(0x71))==wire::Error::lease_expired&&amity::received_holds.size()==1);
    control.cleanup_admission.reset();assert(wire::installed.control(coordinator,envelope,0,id<sys::LinkId>(0x71))==wire::Error::stale_binding&&amity::received_holds.empty()); // Owner already stopped; harmless cleanup.
    // A failed A may outlive its bounded outcome record. Only exact published
    // owner inactivity can make its late up harmless under a full actor IPC;
    // never infer this from a timer or the absence of an unprocessed down.
    auto ended_a=fresh_hold;ended_a.operation.command_id=id<sys::CommandId>(1200);ended_a.operation.arguments.hold_token=id<sys::HoldToken>(1201);
    auto current_b=fresh_hold;current_b.operation.command_id=id<sys::CommandId>(1202);current_b.operation.arguments.hold_token=id<sys::HoldToken>(1203);
    amity::received_holds={ended_a,current_b};control.cleanup_admission=sys::AdmissionCode::overloaded;
    control.classify_remote_key=[&](const sys::AuthorizedCecOperation& original)->std::optional<bool>{
        if(original.command_id==ended_a.operation.command_id&&original.arguments.hold_token==ended_a.operation.arguments.hold_token&&original.fence==ended_a.operation.fence)return false;
        return true;
    };
    const auto before_inactive_up=control.admissions;envelope.payload=cleanup(*ended_a.operation.arguments.hold_token);
    assert(wire::installed.control(coordinator,envelope,0,id<sys::LinkId>(0x71))==wire::Error::stale_route&&control.admissions==before_inactive_up);
    assert(amity::received_holds.size()==1&&amity::received_holds.front().operation.command_id==current_b.operation.command_id);
    envelope.payload=cleanup(*current_b.operation.arguments.hold_token);
    assert(wire::installed.control(coordinator,envelope,0,id<sys::LinkId>(0x71))==wire::Error::queue_full&&amity::received_holds.size()==1);
    // Also cover A ending between the first snapshot read and actor rejection.
    amity::received_holds={ended_a,current_b};unsigned reads=0;
    control.classify_remote_key=[&](const sys::AuthorizedCecOperation& original)->std::optional<bool>{return original.command_id==ended_a.operation.command_id?std::optional<bool>{++reads==1}:std::optional<bool>{true};};
    envelope.payload=cleanup(*ended_a.operation.arguments.hold_token);const auto before_racing_up=control.admissions;
    assert(wire::installed.control(coordinator,envelope,0,id<sys::LinkId>(0x71))==wire::Error::stale_route&&reads==2&&control.admissions==before_racing_up+1);
    assert(amity::received_holds.size()==1&&amity::received_holds.front().operation.command_id==current_b.operation.command_id);
    // Unknown/unprocessed down coverage must still attempt owner handoff and
    // retain the token on rejection; it may never silently swallow its up.
    amity::received_holds={ended_a,current_b};control.classify_remote_key={};const auto before_unknown_up=control.admissions;
    assert(wire::installed.control(coordinator,envelope,0,id<sys::LinkId>(0x71))==wire::Error::queue_full&&control.admissions==before_unknown_up+1&&amity::received_holds.size()==2);
    control.cleanup_admission.reset();
    std::cout<<"system sync: exact owner snapshots, prepare/fence/activate, bounded command dedup, reserved token cleanup, ordered manifest ACKs, applied ACK gating, and exact old-link owner fencing passed\n";
}
