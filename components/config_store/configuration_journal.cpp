#include "system_configuration.hpp"
#include "cJSON.h"
#include <limits>
#include <set>

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
std::string record(const ConfigurationV2& config) {
    const auto payload = serialize_configuration_v2(config);
    return "AMITY2\n" + config.consortium_id.str() + "\n" + model::format_uint64(config.config_revision) + "\n" +
        model::format_uint64(config.presentation_revision) + "\n" + model::format_uint64(payload.size()) + "\n" + sha256_hex(payload) + "\n" + payload;
}
bool decode_record(const std::string& value, ConfigurationV2& out, std::string& error) {
    std::string_view input(value); std::vector<std::string_view> fields;
    for (unsigned n = 0; n < 6; ++n) {
        const auto newline = input.find('\n'); if (newline == std::string_view::npos) { error = "Configuration record header truncated"; return false; }
        fields.push_back(input.substr(0, newline)); input.remove_prefix(newline + 1);
    }
    auto consortium = model::ConsortiumId::parse(fields[1]); auto semantic = model::parse_uint64(fields[2]);
    auto presentation = model::parse_uint64(fields[3]); auto length = model::parse_uint64(fields[4]);
    if (fields[0] != "AMITY2" || !consortium || !semantic || !presentation || !length || *length != input.size() || *length > configuration_compatibility_limit ||
        sha256_hex(input) != fields[5] || !parse_configuration_v2(std::string(input), out, error) || out.consortium_id != *consortium ||
        out.config_revision != *semantic || out.presentation_revision != *presentation || serialize_configuration_v2(out) != input) {
        if (error.empty()) error = "Configuration record metadata, canonical payload, or digest mismatch";
        return false;
    }
    return true;
}
std::vector<model::OctopusId> owners(const ConfigurationV2& c) {
    std::vector<model::OctopusId> result;
    auto add = [&](model::OctopusId id) { if (std::find(result.begin(), result.end(), id) == result.end()) result.push_back(id); };
    for (const auto& b : c.binding_policies) add(b.port.octopus_id);
    for (const auto& d : c.domains) { if (d.confirmed) add(d.owner_octopus_id); for (const auto& p : d.ports) add(p.port.octopus_id); }
    for (const auto& r : c.resources) add(r.owner_octopus_id);
    return result;
}
std::string presentation_hash(const ConfigurationV2& c) {
    auto encoded = serialize_configuration_v2(c); auto* root = cJSON_Parse(encoded.c_str()); if (!root) return {};
    const auto retain = [](cJSON* object, const std::set<std::string>& allowed) {
        for (auto* child = object->child; child;) { auto* next = child->next; if (!allowed.count(child->string)) cJSON_DeleteItemFromObjectCaseSensitive(object, child->string); child = next; }
    };
    retain(root, {"octopuses", "components", "activities", "zones", "external_slots", "activity_order"});
    for (const auto* category : {"octopuses", "components", "activities", "zones", "external_slots"}) {
        auto* values = cJSON_GetObjectItemCaseSensitive(root, category);
        if (!values) continue;
        for (auto* item = values->child; item; item = item->next) retain(item, {"octopus_id", "component_id", "activity_id", "zone_id", "slot", "label", "generated_label", "label_override", "activity_order", "hidden", "alias"});
    }
    char* text = cJSON_PrintUnformatted(root); std::string result = text ? sha256_hex(text) : std::string{};
    cJSON_free(text); cJSON_Delete(root); return result;
}

}

ConfigResult ConfigurationStore::write_verified(const char* key, const std::string& value) {
    return verified(backend_, key, value) ? ConfigResult::ok : ConfigResult::storage_error;
}
ConfigResult ConfigurationStore::recover(ConfigurationV2& out, std::string& error) {
    initialized_ = true;
    Marker selected; std::string payload; auto status = selection(backend_, "cm0", "cm1", "c0", "c1", selected, payload, error);
    if (status != ConfigResult::ok) return status;
    ConfigurationV2 config; if (!decode_record(payload, config, error)) return ConfigResult::recovery_required;
    active_ = config; generation_ = selected.generation; active_slot_ = selected.slot; candidate_.reset();
    candidate_digest_.clear(); required_ = owners(config); ready_.clear(); applied_.clear(); out = std::move(config);
    backend_.erase(selected.slot ? "c0" : "c1");
    std::string migration; if (backend_.read("migration_map", migration) == ReadResult::found) backend_.erase("migration_map");
    // Only after the selected complete record is proven may superseded,
    // nonsecret wizard/promotion artifacts be reclaimed. Keep the original
    // legacy backup and identity seed, which are independent recovery data.
    for(const auto* key:{"d0","d1","dm0","dm1","hprepare","hauthority"})backend_.erase(key);
    return ConfigResult::ok;
}
ConfigResult ConfigurationStore::stage(ConfigurationV2 candidate, RevisionPair expected, std::string& error) {
    if (!initialized_) { ConfigurationV2 current; auto status = recover(current, error); if (status != ConfigResult::ok && status != ConfigResult::missing) return status; }
    if (candidate_) { error = "A configuration transaction is already staged"; return ConfigResult::conflict; }
    if ((active_ && expected != active_->revisions()) || (!active_ && expected != RevisionPair{})) { error = "Configuration or presentation revision changed"; return ConfigResult::conflict; }
    if (generation_ == std::numeric_limits<uint64_t>::max()) { error = "Storage generation exhausted"; return ConfigResult::recovery_required; }
    if(!validate_configuration_v2(candidate,error))return ConfigResult::invalid;
    candidate.legacy_remote_configuration.clear();
    bool semantic = true;
    if (active_) {
        if (candidate.consortium_id != active_->consortium_id) { error = "Import cannot replace consortium authority"; return ConfigResult::invalid; }
        if(candidate.primary_zone_id!=active_->primary_zone_id&&(!active_->activities.empty()||!active_->components.empty())){error="The primary home integration projection cannot change in this installation";return ConfigResult::invalid;}
        if (active_->controller_octopus_id) {
            if (candidate.controller_octopus_id && candidate.controller_octopus_id != active_->controller_octopus_id) { error = "The original controller cannot be replaced"; return ConfigResult::invalid; }
            candidate.controller_octopus_id = active_->controller_octopus_id;
        }
        if (!reconcile_external_slots(*active_, candidate, error)) return ConfigResult::invalid;
        semantic = executable_policy_hash(candidate) != executable_policy_hash(*active_);
        const bool presentation = presentation_hash(candidate) != presentation_hash(*active_);
        if ((semantic && active_->config_revision == UINT64_MAX) || (presentation && active_->presentation_revision == UINT64_MAX)) { error = "Configuration revision exhausted"; return ConfigResult::recovery_required; }
        candidate.config_revision = active_->config_revision + (semantic ? 1 : 0);
        candidate.presentation_revision = active_->presentation_revision + (presentation ? 1 : 0);
    } else { candidate.config_revision = 1; candidate.presentation_revision = 1; }
    if (!candidate.controller_octopus_id) { error = "Establish the original controller before activating this configuration"; return ConfigResult::invalid; }
    if (!validate_configuration_v2(candidate, error)) return ConfigResult::invalid;
    const auto encoded = serialize_configuration_v2(candidate);
    ConfigurationV2 canonical;
    if (!parse_configuration_v2(encoded, canonical, error)) return ConfigResult::invalid;
    candidate = std::move(canonical); // Installed memory and journal bytes agree exactly.
    if (encoded.size() > configuration_payload_limit) { error = "Candidate exceeds the16KiB payload capacity for this board"; return ConfigResult::capacity; }
    const auto complete_record = record(candidate); const auto inactive = active_slot_ ? 0u : 1u;
    if (write_verified(inactive ? "c1" : "c0", complete_record) != ConfigResult::ok) { error = "Candidate write/readback failed; old active record retained"; return ConfigResult::storage_error; }
    candidate_digest_ = sha256_hex(encoded); candidate_ = std::move(candidate);
    previous_required_ = required_; previous_applied_ = applied_;
    required_ = semantic ? owners(*candidate_) : std::vector<model::OctopusId>{}; ready_.clear(); return ConfigResult::ok;
}
ConfigResult ConfigurationStore::ready(model::OctopusId owner, std::string_view digest) {
    if (!candidate_ || digest != candidate_digest_ || std::find(required_.begin(), required_.end(), owner) == required_.end()) return ConfigResult::not_ready;
    if (std::find(ready_.begin(), ready_.end(), owner) == ready_.end()) ready_.push_back(owner);
    return ConfigResult::ok;
}
ConfigResult ConfigurationStore::commit(ConfigurationV2& out, std::string& error) {
    if (!candidate_ || ready_.size() != required_.size()) { error = "Required owners have not acknowledged the exact staged payload"; return ConfigResult::not_ready; }
    const unsigned slot = active_slot_ ? 0u : 1u; const auto complete = record(*candidate_); std::string reread;
    if (backend_.read(slot ? "c1" : "c0", reread) != ReadResult::found || reread != complete) { error = "Staged candidate changed before commit"; return ConfigResult::recovery_required; }
    const Marker next{generation_ + 1, slot, sha256_hex(complete)};
    if (write_verified(slot ? "cm1" : "cm0", marker_text(next)) != ConfigResult::ok) { error = "Activation marker write/readback failed"; return ConfigResult::storage_error; }
    // This is the durable decision, not an assertion of simultaneous effects.
    active_ = candidate_; candidate_.reset(); generation_ = next.generation; active_slot_ = slot; out = *active_;
    applied_.clear(); previous_required_.clear(); previous_applied_.clear();
    // Prove the selected complete payload again after the durable marker. Only
    // obsolete records may be reclaimed; the only committed record is retained.
    std::string committed;
    if (backend_.read(slot ? "c1" : "c0", committed) != ReadResult::found || committed != complete) { error = "Committed payload readback failed; recovery required"; return ConfigResult::recovery_required; }
    backend_.erase(slot ? "c0" : "c1");
    std::string migration;
    if (backend_.read("migration_map", migration) == ReadResult::found) backend_.erase("migration_map");
    return ConfigResult::ok;
}
void ConfigurationStore::cancel_stage() {
    if (!candidate_) return;
    candidate_.reset(); candidate_digest_.clear(); ready_.clear();
    required_ = previous_required_; applied_ = previous_applied_; previous_required_.clear(); previous_applied_.clear();
}
ConfigResult ConfigurationStore::applied(model::OctopusId owner, RevisionPair revisions, std::string_view hash) {
    if (!active_ || active_->revisions() != revisions || executable_policy_hash(*active_) != hash || std::find(required_.begin(), required_.end(), owner) == required_.end()) return ConfigResult::not_ready;
    if (std::find(applied_.begin(), applied_.end(), owner) == applied_.end()) applied_.push_back(owner);
    return ConfigResult::ok;
}
bool ConfigurationStore::activation_complete() const { return active_ && applied_.size() == required_.size(); }

} // namespace config_store
