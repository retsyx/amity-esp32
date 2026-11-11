#include "system_configuration.hpp"
#include <limits>

namespace config_store {
namespace {
struct Marker { uint64_t generation = 0; unsigned slot = 0; std::string digest; };
std::optional<Marker> marker(std::string_view input) {
    if (input.substr(0, 2) != "2:") return {};
    input.remove_prefix(2);
    const auto separator = input.find(':'); if (separator == std::string_view::npos) return {};
    auto generation = model::parse_uint64(input.substr(0, separator)); input.remove_prefix(separator + 1);
    if (!generation || !*generation || input.size() != 66 || (input[0] != '0' && input[0] != '1') || input[1] != ':') return {};
    for (char c : input.substr(2)) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return {};
    return Marker{*generation, static_cast<unsigned>(input[0] - '0'), std::string(input.substr(2))};
}
std::string marker_text(const Marker& m) { return "2:" + model::format_uint64(m.generation) + ":" + std::to_string(m.slot) + ":" + m.digest; }
ConfigResult selection(ConfigBackend& backend, const char* mark0, const char* mark1, const char* record0, const char* record1,
    Marker& selected, std::string& payload, std::string& error) {
    std::optional<Marker> best;
    for (const auto* key : {mark0, mark1}) {
        std::string value; const auto status = backend.read(key, value);
        if (status == ReadResult::error) { error = "Storage read failed"; return ConfigResult::storage_error; }
        if (status == ReadResult::missing) continue;
        auto parsed = marker(value); if (!parsed) { error = "Committed activation marker is corrupt; recovery required"; return ConfigResult::recovery_required; }
        if (best && best->generation == parsed->generation && (best->slot != parsed->slot || best->digest != parsed->digest)) { error = "Conflicting committed activation markers"; return ConfigResult::recovery_required; }
        if (!best || parsed->generation > best->generation) best = parsed;
    }
    if (!best) return ConfigResult::missing;
    if (backend.read(best->slot ? record1 : record0, payload) != ReadResult::found || sha256_hex(payload) != best->digest) {
        error = "Committed record is missing or corrupt; previous state was not silently replaced"; return ConfigResult::recovery_required;
    }
    selected = *best; return ConfigResult::ok;
}
bool verified(ConfigBackend& backend, const char* key, const std::string& payload) {
    if (!backend.write(key, payload)) return false;
    std::string reread; return backend.read(key, reread) == ReadResult::found && reread == payload;
}
std::string record(const Configuration& config,std::string payload) {
    if(payload.empty())return {};
    const auto digest=sha256_hex(payload);if(digest.empty())return {};
    const auto header="AMITY3\n" + config.consortium_id.str() + "\n" + model::format_uint64(config.config_revision) + "\n" +
        model::format_uint64(config.presentation_revision) + "\n" + model::format_uint64(payload.size()) + "\n" + digest + "\n";
    payload.reserve(payload.size()+header.size());payload.insert(0,header);return payload;
}
bool decode_record(const std::string& value, Configuration& out, std::string& canonical, std::string& error) {
    std::string_view input(value); std::vector<std::string_view> fields;
    for (unsigned n = 0; n < 6; ++n) {
        const auto newline = input.find('\n'); if (newline == std::string_view::npos) { error = "Configuration record header truncated"; return false; }
        fields.push_back(input.substr(0, newline)); input.remove_prefix(newline + 1);
    }
    auto consortium = model::ConsortiumId::parse(fields[1]); auto semantic = model::parse_uint64(fields[2]);
    auto presentation = model::parse_uint64(fields[3]); auto length = model::parse_uint64(fields[4]);
    if (fields[0] != "AMITY3" || !consortium || !semantic || !presentation || !length || *length != input.size() || *length > configuration_payload_limit ||
        sha256_hex(input) != fields[5] || !parse_configuration(std::string(input), out, error) || out.consortium_id != *consortium ||
        out.config_revision != *semantic || out.presentation_revision != *presentation || (canonical=serialize_configuration(out)) != input) {
        if (error.empty()) error = "Configuration record metadata, canonical payload, or digest mismatch";
        return false;
    }
    return true;
}
void canonicalize(Configuration& c) {
    const auto sort=[](auto& values,auto key){std::sort(values.begin(),values.end(),[&](const auto& a,const auto& b){return key(a)<key(b);});};
    sort(c.octopuses,[](const auto& value){return value.octopus_id;});
    sort(c.components,[](const auto& value){return value.record.component_id;});
    sort(c.activities,[](const auto& value){return value.activity_id;});
    sort(c.zones,[](const auto& value){return value.zone_id;});
    sort(c.external_slots,[](const auto& value){return value.slot;});
}

}

ConfigResult ConfigurationStore::write_verified(const char* key, const std::string& value) {
    return verified(backend_, key, value) ? ConfigResult::ok : ConfigResult::storage_error;
}
ConfigResult ConfigurationStore::recover(ConfigurationSnapshot& out, std::string& error) {
    initialized_ = true;
    Marker selected; std::string payload; auto status = selection(backend_, "cm0", "cm1", "c0", "c1", selected, payload, error);
    if (status != ConfigResult::ok) return status;
    Configuration config; std::string canonical;
    if (!decode_record(payload, config, canonical, error)) return ConfigResult::recovery_required;
    auto policy=executable_policy_hash(config),presentation=presentation_hash(config);
    if(policy.empty()||presentation.empty()){error="Configuration digest preparation failed";return ConfigResult::storage_error;}
    active_ = ConfigurationSnapshot(new PreparedConfiguration(std::move(config),std::move(canonical),std::move(policy),std::move(presentation)));
    generation_ = selected.generation; active_slot_ = selected.slot; cancel_stage();out = active_;
    backend_.erase(selected.slot ? "c0" : "c1");
    return ConfigResult::ok;
}
ConfigResult ConfigurationStore::stage(Configuration candidate, RevisionPair expected, std::string& error) {
    if (!initialized_) { ConfigurationSnapshot current; auto status = recover(current, error); if (status != ConfigResult::ok && status != ConfigResult::missing) return status; }
    if (candidate_) { error = "A configuration transaction is already staged"; return ConfigResult::conflict; }
    if ((active_ && expected != active_->configuration.revisions()) || (!active_ && expected != RevisionPair{})) { error = "Configuration or presentation revision changed"; return ConfigResult::conflict; }
    if (generation_ == std::numeric_limits<uint64_t>::max()) { error = "Storage generation exhausted"; return ConfigResult::recovery_required; }
    if(!validate_configuration(candidate,error))return ConfigResult::invalid;
    if (active_) {
        const auto& current=active_->configuration;
        if (candidate.consortium_id != current.consortium_id) { error = "Import cannot replace consortium authority"; return ConfigResult::invalid; }
        if(candidate.primary_zone_id!=current.primary_zone_id&&(!current.activities.empty()||!current.components.empty())){error="The primary home integration projection cannot change in this installation";return ConfigResult::invalid;}
        if (candidate.controller_octopus_id != current.controller_octopus_id) { error = "The original controller cannot be replaced"; return ConfigResult::invalid; }
        if (!reconcile_external_slots(current, candidate, error)) return ConfigResult::invalid;
    }
    // These projections exclude revision counters. Prepare each once after
    // slot reconciliation and retain them with the shared immutable value.
    auto policy=executable_policy_hash(candidate),presentation=presentation_hash(candidate);
    if(policy.empty()||presentation.empty()){error="Configuration comparison failed";return ConfigResult::storage_error;}
    if (active_) {
        const auto& current=active_->configuration;
        const bool semantic=policy!=active_->policy_hash,labels=presentation!=active_->presentation_hash;
        if ((semantic && current.config_revision == UINT64_MAX) || (labels && current.presentation_revision == UINT64_MAX)) { error = "Configuration revision exhausted"; return ConfigResult::recovery_required; }
        candidate.config_revision = current.config_revision + (semantic ? 1 : 0);
        candidate.presentation_revision = current.presentation_revision + (labels ? 1 : 0);
    } else { candidate.config_revision = 1; candidate.presentation_revision = 1; }
    canonicalize(candidate);
    auto payload=serialize_configuration(candidate);
    if(payload.empty()){error="Candidate encoding failed";return ConfigResult::storage_error;}
    // Reconciliation only adds validated existing slots; its possible growth
    // and the assigned revision widths are checked on the final saved bytes.
    if(payload.size()>configuration_payload_limit){error="Configuration exceeds saved payload capacity";return ConfigResult::invalid;}
    const auto complete_record = record(candidate,payload); const auto inactive = active_slot_ ? 0u : 1u;
    if(complete_record.empty()){error="Candidate encoding failed";return ConfigResult::storage_error;}
    auto digest=sha256_hex(complete_record);
    if(digest.empty()){error="Candidate encoding failed";return ConfigResult::storage_error;}
    auto prepared=ConfigurationSnapshot(new PreparedConfiguration(std::move(candidate),std::move(payload),std::move(policy),std::move(presentation)));
    if (write_verified(inactive ? "c1" : "c0", complete_record) != ConfigResult::ok) { error = "Candidate write/readback failed; old active record retained"; return ConfigResult::storage_error; }
    candidate_ = std::move(prepared); candidate_record_hash_=std::move(digest);return ConfigResult::ok;
}
ConfigResult ConfigurationStore::commit(ConfigurationSnapshot& out, std::string& error) {
    if (!candidate_) { error = "No configuration transaction is staged"; return ConfigResult::not_ready; }
    const unsigned slot = active_slot_ ? 0u : 1u;std::string complete;
    // Staging and activation may be separated by other work. Check the exact
    // staged record before selecting it; no graph encoding is repeated here.
    if(candidate_record_hash_.empty()||backend_.read(slot ? "c1" : "c0",complete)!=ReadResult::found||sha256_hex(complete)!=candidate_record_hash_){error="Staged candidate changed before commit";return ConfigResult::recovery_required;}
    const Marker next{generation_ + 1, slot, candidate_record_hash_};
    if (write_verified(slot ? "cm1" : "cm0", marker_text(next)) != ConfigResult::ok) { error = "Activation marker write/readback failed"; return ConfigResult::storage_error; }
    // The marker is the durable decision. Writing it cannot change the verified
    // payload; all consumers receive the same prepared configuration allocation.
    active_ = std::move(candidate_);cancel_stage();generation_ = next.generation; active_slot_ = slot; out = active_;
    backend_.erase(slot ? "c0" : "c1");
    return ConfigResult::ok;
}
void ConfigurationStore::cancel_stage() { candidate_.reset();candidate_record_hash_.clear(); }

} // namespace config_store
