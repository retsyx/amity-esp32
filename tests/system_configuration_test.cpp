#include "system_configuration.hpp"
#include "coordinator_intent.hpp"
#include "candidate_network.hpp"
#include "amity/system_sync.hpp"
#include "amity/consortium/transfer.hpp"
#include "amity/consortium/control.hpp"
#include <cassert>
#include <cstdio>
#include <iostream>
#include <map>

using namespace config_store;
template<class T> T id(unsigned n) { char value[33]; std::snprintf(value, sizeof(value), "%032x", n); return T::parse(value).value(); }
struct PowerCut {};
struct Memory : ConfigBackend {
    std::map<std::string, std::string> values;
    unsigned operations = 0, cut_at = 0, writes = 0;
    void step() { if (++operations == cut_at) throw PowerCut{}; }
    ReadResult read(const char* key, std::string& value) override {
        auto it = values.find(key); if (it == values.end()) { step(); return ReadResult::missing; }
        value = it->second; step(); return ReadResult::found;
    }
    bool write(const char* key, const std::string& value) override { values[key] = value; ++writes; step(); return true; }
    bool erase(const char* key) override { values.erase(key); step(); return true; }
};
auto random_source() {
    return [counter = uint64_t{1000}](uint8_t* bytes, size_t size) mutable {
        ++counter; std::fill(bytes, bytes + size, 0); for (size_t n = 0; n < std::min<size_t>(size, 8); ++n) bytes[size - 1 - n] = static_cast<uint8_t>(counter >> (n * 8)); return true;
    };
}
ConfigurationV2 empty_configuration() {
    ConfigurationV2 c; c.consortium_id = id<model::ConsortiumId>(1); c.primary_zone_id = id<model::ZoneId>(10);
    c.controller_octopus_id = id<model::OctopusId>(2);
    c.octopuses.push_back({id<model::OctopusId>(2), "Amity 1", "Amity 1", false, 1});
    c.zones.push_back({c.primary_zone_id, "TV", "TV", false, {}}); return c;
}
std::vector<LegacyEvidence> evidence() {
    model::CecPortRef back{id<model::ConsortiumId>(1), id<model::OctopusId>(2), 0, model::Side::back};
    auto front = back; front.side = model::Side::front;
    return {{{back, "Source"}, true, true, 0x1000, true, true, false, 1}, {{front, "TV"}, true, true, 0, true, true, false, 1}};
}
ConfigurationV2 direct_configuration() {
    ConfigurationV2 c; std::vector<std::string> tasks; std::string error;
    assert(propose_local_activity(empty_configuration(), {"Watch", "TV", "Source", "TV", "", 0}, evidence(), random_source(), c, tasks, error));
    assert(tasks.empty() && c.activities[0].enabled); assert(c.activities[0].audio == c.activities[0].display);
    return c;
}
void ready_all(ConfigurationStore& store) { auto owners = store.staged_owners(); for (auto owner : owners) assert(store.ready(owner, store.candidate_digest()) == ConfigResult::ok); }
void commit(ConfigurationStore& store, ConfigurationV2 candidate, RevisionPair expected, ConfigurationV2& active) {
    std::string error; assert(store.stage(std::move(candidate), expected, error) == ConfigResult::ok); ready_all(store); assert(store.commit(active, error) == ConfigResult::ok);
}
void codecs_and_semantics() {
    assert(sha256_hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    assert(sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    auto c = direct_configuration(); auto json = serialize_configuration_v2(c); ConfigurationV2 decoded; std::string error;
    assert(parse_configuration_v2(json, decoded, error)); assert(serialize_configuration_v2(decoded) == json);
    assert(json.find("logical_address") == std::string::npos && json.find("execution_lease") == std::string::npos);
    for (const auto& d : c.domains) for (const auto& p : d.ports) if (p.physical_address) assert(!p.physical_address->confirmed && p.physical_address->provenance == model::PaProvenance::legacy_minimum_hint);
    auto reject = [&](std::string input) { ConfigurationV2 ignored; assert(!parse_configuration_v2(input, ignored, error)); };
    reject(json + " trailing"); reject(std::string(9, '[') + std::string(9, ']')); reject(std::string(32769, ' '));
    reject(json.substr(0, json.size() - 1) + ",\"version\":2}");
    auto bad = json; auto pos = bad.find("\"config_revision\":\"1\""); bad.replace(pos, std::strlen("\"config_revision\":\"1\""), "\"config_revision\":1"); reject(bad);
    bad = json; pos = bad.find("\"osd_name\":\"Source\""); bad.replace(pos, std::strlen("\"osd_name\":\"Source\""), "\"logical_address\":4"); reject(bad);
    bad = json; pos = bad.find("Source"); bad.replace(pos, 6, "Source\\u0000suffix"); reject(bad);
    auto changed = c; changed.config_revision = UINT64_MAX; changed.presentation_revision = UINT64_MAX;
    changed.activities[0].label = "Cinema"; changed.octopuses[0].label = "Shelf"; changed.components[0].record.label = "Player";
    assert(executable_policy_hash(changed) == executable_policy_hash(c));
    changed = c; changed.binding_policies[0].discrete_power_off = !changed.binding_policies[0].discrete_power_off; assert(executable_policy_hash(changed) != executable_policy_hash(c));
    changed = c; changed.components[0].record.qualified_names[0].port.tentacle_id = 1; assert(!validate_configuration_v2(changed, error));
    changed = c; changed.domains[0].ports.push_back(changed.domains[0].ports[0]); assert(!validate_configuration_v2(changed, error));
}
void journal_and_revisions() {
    Memory memory; ConfigurationStore store(memory); ConfigurationV2 active; std::string error;
    auto initial = direct_configuration(); assert(store.stage(initial, {}, error) == ConfigResult::ok);
    assert(store.commit(active, error) == ConfigResult::not_ready); assert(store.ready(id<model::OctopusId>(99), store.candidate_digest()) == ConfigResult::not_ready);
    assert(store.ready(id<model::OctopusId>(2), "wrong") == ConfigResult::not_ready); ready_all(store);
    assert(store.commit(active, error) == ConfigResult::ok && active.revisions() == RevisionPair({1, 1}));
    assert(!store.activation_complete()); assert(store.applied(id<model::OctopusId>(2), active.revisions(), executable_policy_hash(active)) == ConfigResult::ok); assert(store.activation_complete());
    auto presentation = active; presentation.activities[0].label = "Cinema"; presentation.octopuses[0].label = "Rack";
    assert(store.stage(presentation, active.revisions(), error) == ConfigResult::ok && store.staged_owners().empty());
    assert(store.staged()->revisions() == RevisionPair({1, 2})); assert(store.commit(active, error) == ConfigResult::ok); assert(store.activation_complete());
    auto semantic = active; semantic.binding_policies[0].discrete_power_off = !semantic.binding_policies[0].discrete_power_off;
    assert(store.stage(semantic, {1, 1}, error) == ConfigResult::conflict); assert(store.stage(semantic, active.revisions(), error) == ConfigResult::ok);
    assert(store.staged()->revisions() == RevisionPair({2, 2})); store.cancel_stage();
    ConfigurationStore reboot(memory); ConfigurationV2 recovered; assert(reboot.recover(recovered, error) == ConfigResult::ok); assert(recovered.revisions() == RevisionPair({1, 2}));
    assert(!reboot.activation_complete()); // Durable config never restores live execution readiness.
    memory.values["d0"]="obsolete nonsecret draft";memory.values["hprepare"]="obsolete nonsecret promotion";memory.values["legacy_backup"]="original backup";memory.values["migration_ids"]="original identity seed";
    ConfigurationStore cleaned(memory);assert(cleaned.recover(recovered,error)==ConfigResult::ok&&!memory.values.count("d0")&&!memory.values.count("hprepare"));assert(memory.values.at("legacy_backup")=="original backup"&&memory.values.at("migration_ids")=="original identity seed");
    const auto baseline = memory.values;
    // Cut power after each backend operation in a complete semantic stage and
    // decision. Recovery must produce exactly the old or the committed new blob.
    for (unsigned cut = 1; cut <= 64; ++cut) {
        Memory interrupted; interrupted.values = baseline; ConfigurationStore transaction(interrupted); ConfigurationV2 old;
        assert(transaction.recover(old, error) == ConfigResult::ok); interrupted.operations = 0; interrupted.cut_at = cut;
        try { auto status = transaction.stage(semantic, old.revisions(), error); if (status == ConfigResult::ok) { ready_all(transaction); transaction.commit(recovered, error); } } catch (const PowerCut&) {}
        interrupted.cut_at = 0; ConfigurationStore after(interrupted); assert(after.recover(recovered, error) == ConfigResult::ok);
        assert(recovered.config_revision == 1 || recovered.config_revision == 2);
        if (recovered.config_revision == 1) assert(serialize_configuration_v2(recovered) == serialize_configuration_v2(old));
        else assert(recovered.presentation_revision == 2 && recovered.binding_policies[0].discrete_power_off == semantic.binding_policies[0].discrete_power_off);
    }
    memory.values["c1"] = "corrupt"; // c1 is the second committed generation.
    ConfigurationStore corrupt(memory); assert(corrupt.recover(recovered, error) == ConfigResult::recovery_required);
}
void optional_hardware_capacity() {
    auto c=direct_configuration();const auto before=serialize_configuration_v2(c);const auto domains=c.domains.size();const auto owner=c.octopuses.front().octopus_id;bool changed=false;std::string error;
    assert(reconcile_hardware_profile(c,owner,4,0,random_source(),changed,error)&&changed);
    assert(c.octopuses.front().supported_tentacles==4&&c.domains.size()==domains); // Unpopulated connectors consume no journal payload.
    auto original=c;original.octopuses.front().supported_tentacles=1;assert(serialize_configuration_v2(original)==before);
    assert(serialize_configuration_v2(c).size()==before.size()); // Capacity metadata does not crowd out retained activities/slots.
    auto unsupported=c;unsupported.octopuses.front().supported_tentacles=5;ConfigurationV2 rejected;
    assert(!validate_configuration_v2(unsupported,error)&&!parse_configuration_v2(serialize_configuration_v2(unsupported),rejected,error));
    unsupported=c;unsupported.components[0].record.qualified_names[0].port.tentacle_id=4;
    assert(!validate_configuration_v2(unsupported,error)&&!parse_configuration_v2(serialize_configuration_v2(unsupported),rejected,error));
    assert(!reconcile_hardware_profile(c,owner,5,0,random_source(),changed,error));
    assert(reconcile_hardware_profile(c,owner,4,8,random_source(),changed,error)&&!changed); // Sparse configured index3 never renumbers saved index0.
    ConfigurationV2 decoded;assert(parse_configuration_v2(serialize_configuration_v2(c),decoded,error));
    for(auto& component:c.components)for(auto& n:component.record.qualified_names)n.port.tentacle_id=3;
    for(auto& d:c.domains)for(auto& p:d.ports){p.port.tentacle_id=3;if(p.physical_address)p.physical_address->attachment=p.port;}
    for(auto& b:c.binding_policies){b.port.tentacle_id=3;if(b.device_name)b.device_name->port.tentacle_id=3;}
    assert(validate_configuration_v2(c,error));c.octopuses.front().supported_tentacles=1;assert(!validate_configuration_v2(c,error));
    assert(!reconcile_hardware_profile(decoded,owner,4,16,random_source(),changed,error));
    auto passive=empty_configuration();passive.octopuses.front().supported_tentacles=4;const auto boot=id<model::BootId>(77);std::vector<model::PortPolicySnapshot> snapshots;
    const auto zero=amity::synchronization::owner_state_json(passive,owner,boot,1,{});assert(!zero.empty()&&amity::synchronization::parse_owner_state(zero,passive,owner,boot,1,snapshots,error)&&snapshots.empty());
    ConfigurationV2 connected;assert(propose_adapter_connection(passive,owner,3,{},random_source(),connected,error));
    assert(connected.domains.size()==2&&connected.activities.empty()&&connected.binding_policies.empty()&&connected.resources.empty()&&connected.routes.empty()&&connected.external_slots.empty());
    for(const auto& domain:connected.domains){assert(domain.confirmed&&domain.ports.size()==1&&domain.ports[0].port.tentacle_id==3);assert(domain.ports[0].role==(domain.ports[0].port.side==model::Side::front?model::PortRole::playback_proxy:model::PortRole::tv_controller));assert(!domain.ports[0].physical_address);}
    ConfigurationV2 fallback;assert(propose_adapter_connection(connected,owner,3,0x2000,random_source(),fallback,error));
    for(size_t n=0;n<fallback.domains.size();++n){const auto& domain=fallback.domains[n];assert(domain.domain_id==connected.domains[n].domain_id);if(domain.ports[0].port.side==model::Side::front){assert(domain.domain_revision==2&&domain.ports[0].physical_address->confirmed&&domain.ports[0].physical_address->provenance==model::PaProvenance::user_confirmed_fallback);}else assert(domain.domain_revision==1);}
    assert(propose_adapter_connection(fallback,owner,3,{},random_source(),connected,error)&&serialize_configuration_v2(fallback)==serialize_configuration_v2(connected));
    assert(!propose_adapter_connection(passive,owner,4,{},random_source(),connected,error));
    assert(!propose_adapter_connection(passive,owner,3,uint16_t{0},random_source(),connected,error));
    assert(!propose_adapter_connection(decoded,owner,0,{},random_source(),connected,error)); // Assigned logical equipment cannot be repurposed.
}
void external_slots() {
    Memory memory; ConfigurationStore store(memory); ConfigurationV2 active; commit(store, direct_configuration(), {}, active); std::string error;
    auto deletion = active; auto removed = deletion.activities[0]; deletion.activities.clear(); deletion.zones[0].activity_order.clear();
    assert(reconcile_external_slots(active, deletion, error)); assert(deletion.external_slots[0].tombstone);
    auto restoration = deletion; restoration.activities.push_back(removed); restoration.zones[0].activity_order.push_back(removed.activity_id); restoration.external_slots[0].tombstone = false;
    assert(reconcile_external_slots(deletion, restoration, error)); assert(!restoration.external_slots[0].tombstone && restoration.external_slots[0].activity_id == removed.activity_id);
    auto theft = deletion; theft.external_slots[0].activity_id = id<model::ActivityId>(999); assert(!reconcile_external_slots(deletion, theft, error));
    auto expanded = empty_configuration();
    for (unsigned n = 0; n < 17; ++n) { ConfiguredActivity a; a.activity_id = id<model::ActivityId>(200 + n); a.route_id = id<model::RouteId>(300 + n); a.zone_id = expanded.primary_zone_id; a.label = "Activity"; a.enabled = false; expanded.activities.push_back(a); expanded.zones[0].activity_order.push_back(a.activity_id); }
    assert(reconcile_external_slots(empty_configuration(), expanded, error) && expanded.external_slots.size() == 16);
    assert(!activity_for_slot(expanded, 16));
}
void migration() {
    Memory memory; ConfigurationStore store(memory); LegacyConfiguration legacy;
    for (unsigned n = 0; n < 8; ++n) legacy.activities.push_back({"Fixture " + std::to_string(n), "TV", "Source", "Source", "", 0});
    legacy.aliases.push_back({0, true, "Cinema"}); ConfigurationV2 migrated; std::vector<std::string> tasks; std::string error; auto random = random_source();
    assert(migrate_legacy(store, memory, "{\"version\":1,\"activities\":[]}", legacy, id<model::ConsortiumId>(1), id<model::OctopusId>(2), evidence(), random, migrated, tasks, error) == ConfigResult::ok);
    assert(migrated.activities.size() == 8 && migrated.components.size() == 2 && migrated.external_slots[0].alias == "Cinema" && migrated.external_slots[0].hidden);
    for (const auto& activity : migrated.activities) assert(activity.enabled);
    const auto before = serialize_configuration_v2(migrated); assert(before.size() <= 16384); assert(memory.values.at("migration_ids").size() < 256);
    // Restart with changed evidence cannot silently allocate a new mapping.
    ConfigurationStore retry(memory); ConfigurationV2 same;
    assert(migrate_legacy(retry, memory, "{\"version\":1,\"activities\":[]}", legacy, id<model::ConsortiumId>(1), id<model::OctopusId>(2), {}, random, same, tasks, error) == ConfigResult::ok);
    assert(serialize_configuration_v2(same) == before); ready_all(retry); assert(retry.commit(same, error) == ConfigResult::ok);
    assert(memory.values.at("legacy_backup") == "{\"version\":1,\"activities\":[]}"); assert(!memory.values.count("migration_map"));
    Memory ambiguous_memory; ConfigurationStore ambiguous_store(ambiguous_memory); auto ambiguous = evidence(); auto second = ambiguous[0]; second.name.port.tentacle_id = 1; ambiguous.push_back(second);
    assert(migrate_legacy(ambiguous_store, ambiguous_memory, "legacy", legacy, id<model::ConsortiumId>(1), id<model::OctopusId>(2), ambiguous, random_source(), same, tasks, error) == ConfigResult::ok);
    assert(!tasks.empty()); for (const auto& activity : same.activities) assert(!activity.enabled);
    for (const auto& c : same.components) if (c.record.label == "Source") assert(c.record.qualified_names.empty());
}
void storage_profile_compatibility() {
    auto original=direct_configuration();auto large=original;std::string error;
    for(unsigned n=0;serialize_configuration_v2(large).size()<18000;++n){ConfiguredActivity a;a.activity_id=id<model::ActivityId>(500+n);a.route_id=id<model::RouteId>(600+n);a.zone_id=large.primary_zone_id;a.enabled=false;a.label=a.generated_label=std::string(128,'x');large.zones[0].activity_order.push_back(a.activity_id);large.activities.push_back(a);}
    assert(validate_configuration_v2(large,error));Memory memory;ConfigurationStore store(memory);ConfigurationV2 active;commit(store,original,{},active);assert(store.stage(large,active.revisions(),error)==ConfigResult::capacity);
    Memory old;const auto payload=serialize_configuration_v2(large);const auto record="AMITY2\n"+large.consortium_id.str()+"\n1\n1\n"+std::to_string(payload.size())+"\n"+sha256_hex(payload)+"\n"+payload;old.values["c0"]=record;old.values["cm0"]="2:1:0:"+sha256_hex(record);ConfigurationStore compatible(old);assert(compatible.recover(active,error)==ConfigResult::ok&&serialize_configuration_v2(active)==payload);
}

void migration_confirmation_barrier(){
    const std::string original="{\"version\":1,\"activities\":[]}";LegacyConfiguration legacy;ConfigurationV2 candidate;std::vector<std::string> tasks;std::string error;
    for(unsigned cut=1;cut<=40;++cut){Memory memory;memory.values["v1_active"]=original;memory.cut_at=cut;ConfigurationStore store(memory);try{if(migrate_legacy(store,memory,original,legacy,id<model::ConsortiumId>(1),id<model::OctopusId>(2),{},random_source(),candidate,tasks,error)==ConfigResult::ok)ready_all(store);}catch(const PowerCut&){}memory.cut_at=0;assert(!memory.values.count("cm0")&&!memory.values.count("cm1")&&memory.values.at("v1_active")==original);ConfigurationStore boot(memory);assert(boot.recover(candidate,error)==ConfigResult::missing);}
    Memory staged;staged.values["v1_active"]=original;ConfigurationStore first(staged);assert(migrate_legacy(first,staged,original,legacy,id<model::ConsortiumId>(1),id<model::OctopusId>(2),{},random_source(),candidate,tasks,error)==ConfigResult::ok);ready_all(first);assert(!staged.values.count("cm0")&&!staged.values.count("cm1"));const auto before_confirmation=staged.values;
    // Local firmware confirmation is the caller's durable barrier. Only the
    // subsequent commit can introduce a schema2 active marker.
    for(unsigned cut=1;cut<=32;++cut){Memory memory;memory.values=before_confirmation;ConfigurationStore resumed(memory);assert(migrate_legacy(resumed,memory,original,legacy,id<model::ConsortiumId>(1),id<model::OctopusId>(2),{},random_source(),candidate,tasks,error)==ConfigResult::ok);ready_all(resumed);memory.operations=0;memory.cut_at=cut;try{resumed.commit(candidate,error);}catch(const PowerCut&){}memory.cut_at=0;ConfigurationStore boot(memory);const auto result=boot.recover(candidate,error);assert(result==ConfigResult::missing||result==ConfigResult::ok);assert(memory.values.at("v1_active")==original&&memory.values.at("legacy_backup")==original);}
}
void candidate_network_recovery(){
    CandidateNetwork value;value.join_id=id<model::JoinId>(170);value.proposal_digest=std::string(64,'c');value.old_ssid="Old offline network";value.old_password="retained-secret";value.old_driver_configuration=std::string("binary\0configuration",20);value.ssid="Candidate LAN";value.password="candidate-secret";std::string error;CandidateNetwork recovered;
    Memory memory;CandidateNetworkJournal journal(memory);assert(journal.stage(value,error)==ConfigResult::ok);assert(journal.recover(recovered,error)==ConfigResult::ok&&!recovered.decided&&recovered.old_driver_configuration==value.old_driver_configuration);const auto before=memory.values;
    assert(journal.finish(value.join_id,value.proposal_digest,error)==ConfigResult::conflict);assert(journal.decide(id<model::JoinId>(171),value.proposal_digest,true,error)==ConfigResult::conflict);
    assert(journal.abandon(id<model::JoinId>(171),value.proposal_digest,error)==ConfigResult::conflict);assert(journal.abandon(value.join_id,std::string(64,'d'),error)==ConfigResult::conflict);
    // The adapter restores the exact old driver bytes before abandon. A cut
    // before cleanup retains the complete rollback material for passive retry.
    for(unsigned cut=1;cut<=8;++cut){Memory interrupted;interrupted.values=before;interrupted.cut_at=cut;CandidateNetworkJournal cancellation(interrupted);try{cancellation.abandon(value.join_id,value.proposal_digest,error);}catch(const PowerCut&){}interrupted.cut_at=0;CandidateNetworkJournal boot(interrupted);const auto status=boot.recover(recovered,error);assert(status==ConfigResult::missing||(status==ConfigResult::ok&&!recovered.decided&&recovered.old_driver_configuration==value.old_driver_configuration&&recovered.old_password==value.old_password));}
    for(unsigned cut=1;cut<=12;++cut){Memory interrupted;interrupted.values=before;interrupted.cut_at=cut;CandidateNetworkJournal transaction(interrupted);try{transaction.decide(value.join_id,value.proposal_digest,true,error);}catch(const PowerCut&){}interrupted.cut_at=0;CandidateNetworkJournal reboot(interrupted);assert(reboot.recover(recovered,error)==ConfigResult::ok&&recovered.old_password==value.old_password&&recovered.password==value.password);}
    assert(journal.decide(value.join_id,value.proposal_digest,true,error)==ConfigResult::ok);assert(journal.decide(value.join_id,value.proposal_digest,false,error)==ConfigResult::conflict);assert(journal.abandon(value.join_id,value.proposal_digest,error)==ConfigResult::conflict);const auto decided=memory.values;
    for(unsigned cut=1;cut<=8;++cut){Memory interrupted;interrupted.values=decided;interrupted.cut_at=cut;CandidateNetworkJournal transaction(interrupted);try{transaction.finish(value.join_id,value.proposal_digest,error);}catch(const PowerCut&){}interrupted.cut_at=0;CandidateNetworkJournal reboot(interrupted);const auto status=reboot.recover(recovered,error);assert(status==ConfigResult::missing||(status==ConfigResult::ok&&recovered.decided));}
    assert(journal.finish(value.join_id,value.proposal_digest,error)==ConfigResult::ok&&memory.values.at("net_candidate")=="NETWORK_DONE\n");Memory damaged;damaged.values=before;damaged.values["net_candidate"][80]^=1;CandidateNetworkJournal corrupt(damaged);assert(corrupt.recover(recovered,error)==ConfigResult::recovery_required);
}
void coordinator_intents() {
    Memory memory;CoordinatorIntentJournal journal(memory);std::string error;CoordinatorIntent intent;const auto consortium=id<model::ConsortiumId>(1);const auto zone=id<model::ZoneId>(2);const auto first=id<model::ActivityId>(3),second=id<model::ActivityId>(4);
    assert(journal.recover(intent,error)==ConfigResult::missing);assert(journal.request(consortium,zone,first,error)==ConfigResult::ok);assert(journal.recover(intent,error)==ConfigResult::ok&&intent.revision==1&&intent.last_requested_sessions.size()==1&&intent.last_requested_sessions[0].activity_id==first);const auto before=memory.values;
    for(unsigned cut=1;cut<=18;++cut){Memory m;m.values=before;m.cut_at=cut;CoordinatorIntentJournal interrupted(m);try{interrupted.request(consortium,zone,second,error);}catch(const PowerCut&){}m.cut_at=0;CoordinatorIntentJournal after(m);assert(after.recover(intent,error)==ConfigResult::ok&&intent.last_requested_sessions.size()==1&&(intent.last_requested_sessions[0].activity_id==first||intent.last_requested_sessions[0].activity_id==second));}
    auto writes=memory.writes;assert(journal.request(consortium,zone,first,error)==ConfigResult::ok&&memory.writes==writes);assert(journal.request(consortium,zone,{},error)==ConfigResult::ok);assert(journal.recover(intent,error)==ConfigResult::ok&&intent.last_requested_sessions.empty());memory.values["intent1"]="corrupt";assert(journal.recover(intent,error)==ConfigResult::recovery_required);
}
void discovery_monitor_upgrade() {
    auto initial=empty_configuration();auto peers=evidence();
    for(const auto& peer:peers){ConfiguredDomain domain;domain.domain_id=id<model::DomainId>(100+initial.domains.size());domain.owner_octopus_id=initial.octopuses[0].octopus_id;domain.ports.push_back({peer.name.port,model::PortRole::monitor,{}});initial.domains.push_back(domain);}
    ConfigurationV2 candidate;std::vector<std::string> tasks;std::string error;
    assert(propose_local_activity(initial,{"Watch","TV","Source","TV","",0},peers,random_source(),candidate,tasks,error));assert(tasks.empty());for(const auto& d:candidate.domains)assert(d.confirmed&&d.domain_revision==2);
    peers[0].direct_cable_confirmed=false;assert(propose_local_activity(initial,{"Watch","TV","Source","TV","",0},peers,random_source(),candidate,tasks,error));assert(!candidate.activities[0].enabled&&!tasks.empty());
    peers=evidence();peers[1].confirmed_screen_input=2;assert(!propose_local_activity(initial,{"Watch","TV","Source","TV","",0},peers,random_source(),candidate,tasks,error));
    peers[0].physical_address_confirmed=false;peers[0].pa_user_confirmed_fallback=true;peers[0].physical_address=0x2000;assert(propose_local_activity(initial,{"Watch","TV","Source","TV","",0},peers,random_source(),candidate,tasks,error));bool fallback=false;for(const auto& d:candidate.domains)for(const auto& port:d.ports)if(port.physical_address)fallback=port.physical_address->confirmed&&port.physical_address->provenance==model::PaProvenance::user_confirmed_fallback;assert(fallback);
}

// Historical fields exist only in decoder fixtures; no active remote model.
std::string old_remote_record(const ConfigurationV2& c,unsigned remote,model::OctopusId owner){return "{\"remote_id\":\""+id<model::MessageId>(remote).str()+"\",\"owner_octopus_id\":\""+owner.str()+"\",\"zone_id\":\""+c.primary_zone_id.str()+"\",\"binding_revision\":\"7\",\"label\":\"Sofa\",\"generated_label\":\"\",\"label_override\":true,\"activity_buttons\":[\""+c.activities[0].activity_id.str()+"\"]}";}
void original_controller_upgrade() {
    const auto original=id<model::OctopusId>(2),member=id<model::OctopusId>(9);
    auto legacy=direct_configuration();legacy.controller_octopus_id.reset();legacy.octopuses.insert(legacy.octopuses.begin(),{member,"Added device","",false,1});
    legacy.legacy_remote_configuration="{\"primary_remote_id\":\""+id<model::MessageId>(151).str()+"\",\"remotes\":["+old_remote_record(legacy,150,original)+","+old_remote_record(legacy,151,member)+"]}";
    const auto old_payload=serialize_configuration_v2(legacy);ConfigurationV2 decoded;std::string error;bool changed=false;
    assert(parse_configuration_v2(old_payload,decoded,error)&&!decoded.controller_octopus_id&&!decoded.legacy_remote_configuration.empty());
    assert(bind_original_controller(decoded,original,changed,error)&&changed&&decoded.controller_octopus_id==original&&decoded.legacy_remote_configuration.empty());
    assert(decoded.activities[0].activity_id==legacy.activities[0].activity_id&&decoded.external_slots[0].activity_id==legacy.external_slots[0].activity_id);
    const auto bound=serialize_configuration_v2(decoded);assert(bind_original_controller(decoded,original,changed,error)&&!changed);assert(!bind_original_controller(decoded,member,changed,error)&&serialize_configuration_v2(decoded)==bound);
    auto malformed=legacy;malformed.legacy_remote_configuration="{\"remotes\":["+old_remote_record(legacy,150,original)+","+old_remote_record(legacy,150,original)+"]}";assert(!bind_original_controller(malformed,original,changed,error));
    auto missing=legacy;assert(!bind_original_controller(missing,id<model::OctopusId>(99),changed,error));
    auto invalid=decoded;invalid.legacy_remote_configuration="{\"remotes\":["+old_remote_record(legacy,151,member)+"]}";assert(!validate_configuration_v2(invalid,error));
    Memory fresh_memory;ConfigurationStore fresh(fresh_memory);assert(fresh.stage(legacy,{},error)==ConfigResult::invalid&&fresh_memory.values.empty());
    const auto record="AMITY2\n"+legacy.consortium_id.str()+"\n1\n1\n"+std::to_string(old_payload.size())+"\n"+sha256_hex(old_payload)+"\n"+old_payload;
    Memory baseline;baseline.values["c0"]=record;baseline.values["cm0"]="2:1:0:"+sha256_hex(record);baseline.values["bond"]="original bond stays opaque";
    for(unsigned cut=1;cut<=60;++cut){Memory memory;memory.values=baseline.values;ConfigurationStore store(memory);ConfigurationV2 active;assert(store.recover(active,error)==ConfigResult::ok);assert(bind_original_controller(active,original,changed,error)&&changed);memory.operations=0;memory.cut_at=cut;try{if(store.stage(active,{1,1},error)==ConfigResult::ok){ready_all(store);store.commit(active,error);}}catch(const PowerCut&){}memory.cut_at=0;ConfigurationStore reboot(memory);assert(reboot.recover(active,error)==ConfigResult::ok&&memory.values.at("bond")=="original bond stays opaque");if(active.controller_octopus_id)assert(active.controller_octopus_id==original&&active.legacy_remote_configuration.empty());else assert(serialize_configuration_v2(active)==old_payload);}
    Memory memory;ConfigurationStore store(memory);ConfigurationV2 active;commit(store,decoded,{},active);auto old_backup=active;old_backup.controller_octopus_id.reset();assert(store.stage(old_backup,active.revisions(),error)==ConfigResult::ok&&store.staged()->controller_octopus_id==original);store.cancel_stage();auto moved=active;moved.controller_octopus_id=member;assert(store.stage(moved,active.revisions(),error)==ConfigResult::invalid);
    auto changed_projection=active;changed_projection.primary_zone_id=id<model::ZoneId>(99);changed_projection.zones.push_back({changed_projection.primary_zone_id,"Other","",false,{}});assert(store.stage(changed_projection,active.revisions(),error)==ConfigResult::invalid);
}

void legacy_owner_controller_upgrade() {
    const auto authority=id<model::OctopusId>(2),owner=id<model::OctopusId>(3),foreign=id<model::OctopusId>(4);
    auto full=direct_configuration();full.controller_octopus_id.reset();full.octopuses.push_back({owner,"Actuator","",false,1});full.octopuses.push_back({foreign,"Old remote owner","",false,1});
    full.legacy_remote_configuration="{\"remotes\":["+old_remote_record(full,155,foreign)+"]}";
    for(auto& c:full.components)for(auto& n:c.record.qualified_names)n.port.octopus_id=owner;
    for(auto& d:full.domains){d.owner_octopus_id=owner;for(auto& p:d.ports){p.port.octopus_id=owner;if(p.physical_address)p.physical_address->attachment=p.port;}}
    for(auto& b:full.binding_policies){b.port.octopus_id=owner;if(b.device_name)b.device_name->port.octopus_id=owner;}
    for(auto& r:full.resources)r.owner_octopus_id=owner;
    auto legacy=configuration_owner_slice(full,owner);legacy.legacy_remote_configuration=full.legacy_remote_configuration;legacy.octopuses.push_back({foreign,"Old remote owner","",false,1});auto normalized=full;bool changed=false;std::string error;assert(bind_original_controller(normalized,authority,changed,error));const auto incoming=configuration_owner_slice(normalized,owner);
    const auto payload=serialize_configuration_v2(legacy);const auto record="OWNER2\n"+owner.str()+"\n"+authority.str()+"\n1\n"+sha256_hex(payload)+"\n"+payload;
    Memory baseline;baseline.values["slice0"]=record;baseline.values["smark0"]="2:1:0:"+sha256_hex(record);baseline.values["bond"]="no bond migration";
    for(unsigned cut=1;cut<=60;++cut){Memory memory;memory.values=baseline.values;OwnerConfigurationJournal journal(memory);memory.cut_at=cut;try{journal.commit(incoming,owner,authority,1,error);}catch(const PowerCut&){}memory.cut_at=0;OwnerConfigurationJournal reboot(memory);ConfigurationV2 recovered;model::OctopusId o,a;uint64_t epoch=0;assert(reboot.recover(recovered,o,a,epoch,error)==ConfigResult::ok&&o==owner&&a==authority&&epoch==1);assert(serialize_configuration_v2(recovered)==payload||serialize_configuration_v2(recovered)==serialize_configuration_v2(incoming));assert(memory.values.at("bond")=="no bond migration");}
    Memory memory;memory.values=baseline.values;OwnerConfigurationJournal journal(memory);
    auto altered=incoming;altered.binding_policies[0].discrete_power_off=!altered.binding_policies[0].discrete_power_off;assert(journal.commit(altered,owner,authority,1,error)==ConfigResult::conflict);
    altered=incoming;const auto old_name=altered.components[0].record.qualified_names[0];auto new_name=old_name;new_name.osd_name="Changed name";altered.components[0].record.qualified_names[0]=new_name;for(auto& binding:altered.binding_policies)if(binding.device_name==old_name)binding.device_name=new_name;assert(journal.commit(altered,owner,authority,1,error)==ConfigResult::conflict);
    altered=incoming;altered.legacy_remote_configuration=full.legacy_remote_configuration;assert(journal.commit(altered,owner,authority,1,error)==ConfigResult::invalid);
    altered=incoming;altered.controller_octopus_id=owner;assert(journal.commit(altered,owner,authority,1,error)==ConfigResult::invalid);
    assert(journal.commit(incoming,owner,authority,1,error)==ConfigResult::ok);const auto writes=memory.writes;assert(journal.commit(incoming,owner,authority,1,error)==ConfigResult::ok&&memory.writes==writes);
    // Previously bound actuator journals also held unrelated home projection.
    // The exact deterministic smaller native slice may replace that format at
    // the same revision; changing even its location label must still conflict.
    auto old_projection=direct_configuration();old_projection.octopuses.push_back({owner,"Actuator","",false,1});
    const auto native_only=configuration_owner_slice(old_projection,owner);assert(native_only.routes.empty()&&native_only.activities.empty());
    const auto old_payload=serialize_configuration_v2(old_projection);const auto old_record="OWNER2\n"+owner.str()+"\n"+authority.str()+"\n1\n"+sha256_hex(old_payload)+"\n"+old_payload;Memory old_memory;old_memory.values["slice0"]=old_record;old_memory.values["smark0"]="2:1:0:"+sha256_hex(old_record);
    OwnerConfigurationJournal old_journal(old_memory);auto changed_label=native_only;changed_label.octopuses.back().label="Other";assert(old_journal.commit(changed_label,owner,authority,1,error)==ConfigResult::conflict);assert(old_journal.commit(native_only,owner,authority,1,error)==ConfigResult::ok);
}
void owner_journal_and_sync() {
    const auto owner=id<model::OctopusId>(2), authority=owner; const auto boot=id<model::BootId>(4);
    auto config=direct_configuration();auto slice=amity::synchronization::owner_slice(config,owner);std::string error;
    assert(validate_configuration_v2(slice,error));
    auto passive=amity::synchronization::owner_slice(config,id<model::OctopusId>(55)); assert(passive.activities.empty()&&passive.routes.empty());for(const auto& binding:passive.binding_policies)assert(binding.port.octopus_id!=id<model::OctopusId>(55)); assert(validate_configuration_v2(passive,error));
    Memory memory; OwnerConfigurationJournal journal(memory); assert(journal.commit(slice,owner,authority,1,error)==ConfigResult::ok);
    const auto before=memory.values; auto changed=slice; ++changed.presentation_revision; changed.octopuses[0].label="Cabinet";
    for(unsigned cut=1;cut<=22;++cut){ Memory m; m.values=before; OwnerConfigurationJournal interrupted(m); m.cut_at=cut;try{interrupted.commit(changed,owner,authority,1,error);}catch(const PowerCut&){} m.cut_at=0;OwnerConfigurationJournal after(m);ConfigurationV2 recovered;model::OctopusId o,a;uint64_t epoch=0;assert(after.recover(recovered,o,a,epoch,error)==ConfigResult::ok&&o==owner&&a==authority&&epoch==1);assert(serialize_configuration_v2(recovered)==serialize_configuration_v2(slice)||serialize_configuration_v2(recovered)==serialize_configuration_v2(changed)); }
    auto forged=slice; forged.binding_policies[0].discrete_power_off=!forged.binding_policies[0].discrete_power_off;assert(journal.commit(forged,owner,authority,1,error)==ConfigResult::conflict);assert(journal.commit(slice,owner,id<model::OctopusId>(8),1,error)==ConfigResult::invalid);assert(journal.commit(slice,owner,authority,2,error)==ConfigResult::invalid);
    const auto actuator=id<model::OctopusId>(55);auto actuator_slice=amity::synchronization::owner_slice(config,actuator);assert(actuator_slice.controller_octopus_id==authority&&validate_configuration_v2(actuator_slice,error));assert(std::any_of(actuator_slice.octopuses.begin(),actuator_slice.octopuses.end(),[&](const auto& b){return b.octopus_id==authority;}));
    std::vector<model::PortPolicySnapshot> policies;
    unsigned evidence_id=100;
    for(const auto& domain:slice.domains)for(const auto& port:domain.ports){
        model::PortPolicySnapshot p; p.port=port.port;p.owner_boot_id=boot;p.domain_id=domain.domain_id;p.domain_revision=domain.domain_revision;p.coordinator_epoch=7;p.config_revision=slice.config_revision;p.policy_generation=9;p.role=port.role;p.owned_logical_address=p.role==model::PortRole::playback_proxy?4:0;
        for(const auto& c:slice.components)p.components.push_back(c.record);
        p.physical_address=model::PhysicalAddressEvidence{p.port,p.domain_id,p.domain_revision,0x1000,model::PaProvenance::immediate_peer_report,model::FullyQualifiedCecAddress{p.port,static_cast<uint8_t>(p.port.side==model::Side::front?0:4)},true};
        for(const auto& policy:slice.binding_policies)if(policy.port==p.port){model::ComponentBinding b;b.binding_id=policy.binding_id;b.component_id=policy.component_id;b.capability=policy.capability;b.kind=policy.kind;b.device_name=policy.device_name;b.proxy_identity_resource_id=policy.proxy_identity_resource_id;b.owner_octopus_id=owner;b.domain_id=p.domain_id;b.domain_revision=p.domain_revision;b.binding_revision=12;b.role=p.role;b.status=model::BindingStatus::verified;b.endpoint={p.port,static_cast<uint8_t>(b.kind==model::BindingKind::proxy_representation?*p.owned_logical_address:p.port.side==model::Side::front?0:4)};b.physical_address_evidence_ref=id<model::EvidenceId>(evidence_id++);p.bindings.push_back({b,policy.discrete_power_on,policy.discrete_power_off,policy.image_view_on,policy.exact_path_selection});}
        policies.push_back(p);
    }
    const auto bytes=amity::synchronization::owner_state_json(slice,owner,boot,7,policies); assert(!bytes.empty());std::vector<model::PortPolicySnapshot> parsed;
    assert(amity::synchronization::parse_owner_state(bytes,slice,owner,boot,7,parsed,error));assert(parsed.size()==policies.size());
    auto expanded=slice;for(auto& board:expanded.octopuses)if(board.octopus_id==owner)board.supported_tentacles=4;
    auto mixed=policies;auto spare=policies.front();spare.port.tentacle_id=3;spare.domain_id=id<model::DomainId>(500);spare.bindings.clear();spare.physical_address.reset();spare.owned_logical_address.reset();spare.role=model::PortRole::monitor;mixed.push_back(spare);
    auto expanded_bytes=amity::synchronization::owner_state_json(expanded,owner,boot,7,mixed);assert(amity::synchronization::parse_owner_state(expanded_bytes,expanded,owner,boot,7,parsed,error)&&parsed.size()==mixed.size());
    mixed.back().role=model::PortRole::disabled;expanded_bytes=amity::synchronization::owner_state_json(expanded,owner,boot,7,mixed);assert(amity::synchronization::parse_owner_state(expanded_bytes,expanded,owner,boot,7,parsed,error));
    for(auto& policy:mixed)if(!policy.bindings.empty()){policy.role=model::PortRole::disabled;policy.owned_logical_address.reset();for(auto& binding:policy.bindings){binding.binding.role=model::PortRole::disabled;binding.binding.status=model::BindingStatus::unresolved;}}
    expanded_bytes=amity::synchronization::owner_state_json(expanded,owner,boot,7,mixed);assert(amity::synchronization::parse_owner_state(expanded_bytes,expanded,owner,boot,7,parsed,error));
    for(auto& policy:mixed)if(!policy.bindings.empty()){policy.role=model::PortRole::monitor;for(auto& binding:policy.bindings){binding.binding.role=model::PortRole::monitor;binding.discrete_power_on=binding.discrete_power_off=binding.image_view_on=binding.exact_path_selection=false;}}
    expanded_bytes=amity::synchronization::owner_state_json(expanded,owner,boot,7,mixed);assert(amity::synchronization::parse_owner_state(expanded_bytes,expanded,owner,boot,7,parsed,error));
    mixed[0].bindings[0].binding.status=model::BindingStatus::verified;expanded_bytes=amity::synchronization::owner_state_json(expanded,owner,boot,7,mixed);assert(!amity::synchronization::parse_owner_state(expanded_bytes,expanded,owner,boot,7,parsed,error));
    auto forbidden_metadata=bytes;forbidden_metadata.pop_back();forbidden_metadata+=",\"remote_status\":{}}";assert(!amity::synchronization::parse_owner_state(forbidden_metadata,slice,owner,boot,7,parsed,error));
    auto paired=policies;for(auto& p:paired)if(p.role==model::PortRole::playback_proxy){auto reporter=p.port;reporter.side=model::Side::back;p.physical_address->reporter=model::FullyQualifiedCecAddress{reporter,4};}
    auto paired_bytes=amity::synchronization::owner_state_json(slice,owner,boot,7,paired);assert(amity::synchronization::parse_owner_state(paired_bytes,slice,owner,boot,7,parsed,error));
    for(auto& p:paired)if(p.physical_address&&p.physical_address->reporter)p.physical_address->reporter->port.tentacle_id=1;
    paired_bytes=amity::synchronization::owner_state_json(slice,owner,boot,7,paired);assert(!amity::synchronization::parse_owner_state(paired_bytes,slice,owner,boot,7,parsed,error));
    model::CecObservation native;native.endpoint={policies[0].port,4};native.osd_name="Observed";native.owner_boot_id=boot;native.local_sequence=42;native.scan_id=id<model::ScanId>(20);native.domain_id=policies[0].domain_id;native.domain_revision=policies[0].domain_revision;native.received_at_ms=850;
    auto observed=amity::synchronization::owner_state_json(slice,owner,boot,7,policies,{native},1000);std::vector<model::CecObservation> equipment;assert(amity::synchronization::parse_owner_state(observed,slice,owner,boot,7,parsed,error,&equipment,1100)&&equipment.size()==1&&equipment[0].endpoint==native.endpoint&&equipment[0].received_at_ms==950&&!equipment[0].stale);
    assert(!amity::synchronization::parse_owner_state(bytes,slice,owner,id<model::BootId>(99),7,parsed,error));assert(!amity::synchronization::parse_owner_state(bytes,slice,owner,boot,8,parsed,error));
    auto altered=bytes;auto at=altered.find("\"binding_revision\":\"12\"");assert(at!=std::string::npos);altered.replace(at,23,"\"binding_revision\":12");assert(!amity::synchronization::parse_owner_state(altered,slice,owner,boot,7,parsed,error));
    altered=bytes;at=altered.find("\"tentacle_id\":0");altered.replace(at,15,"\"tentacle_id\":4");assert(!amity::synchronization::parse_owner_state(altered,slice,owner,boot,7,parsed,error));
    namespace wire=amity::consortium; const std::string payload=serialize_configuration_v2(slice);
    wire::SnapshotManifest manifest;manifest.transfer_id=id<model::MessageId>(9);manifest.owner_octopus_id=owner;manifest.owner_boot_id=boot;manifest.config_revision=slice.config_revision;manifest.total_length=payload.size();manifest.payload_hash=sha256_hex(payload);manifest.document_kind="configuration";
    wire::SnapshotTransfer transfer(sha256_hex);assert(transfer.begin(manifest,owner,boot,slice.config_revision,100)==wire::Error::none);
    wire::Json chunk;wire::JsonError detail; assert(wire::parse_json(wire::snapshot_chunk_json(manifest,1,payload),chunk,detail)); assert(transfer.accept(chunk,101)==wire::Error::invalid_message);
    for(unsigned n=0;n<manifest.chunk_count();++n){assert(wire::parse_json(wire::snapshot_chunk_json(manifest,n,payload),chunk,detail));assert(transfer.accept(chunk,101+n)==wire::Error::none);assert(transfer.accept(chunk,102+n)==wire::Error::duplicate);}
    assert(transfer.complete()&&transfer.payload()==payload);
    wire::SnapshotTransfer expired(sha256_hex);assert(expired.begin(manifest,owner,boot,slice.config_revision,10)==wire::Error::none);assert(expired.accept(chunk,5010)==wire::Error::not_ready);
    wire::PlanBarrier fence;fence.context={id<model::PlanId>(15),id<model::SessionId>(16),1,10,std::string(64,'a'),std::string(64,'b')};fence.phase="fence";wire::Json json;assert(wire::parse_json(wire::plan_barrier_json(fence),json,detail));wire::PlanBarrier decoded;assert(wire::parse_plan_barrier(json,decoded)==wire::Error::none&&!decoded.old_session_id);
}
int main() { codecs_and_semantics(); journal_and_revisions(); optional_hardware_capacity(); external_slots(); migration(); storage_profile_compatibility(); migration_confirmation_barrier(); candidate_network_recovery(); coordinator_intents(); discovery_monitor_upgrade();  original_controller_upgrade(); legacy_owner_controller_upgrade(); owner_journal_and_sync(); std::cout << "schema2: strict canonical codec, revisions, stable selectors/slots, migration, and storage power cuts passed\n"; return 0; }
