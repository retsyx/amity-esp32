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
std::string record(const Configuration& config) {
    const auto payload = serialize_configuration(config);
    return "AMITY3\n" + config.consortium_id.str() + "\n" + model::format_uint64(config.config_revision) + "\n" +
        model::format_uint64(config.presentation_revision) + "\n" + model::format_uint64(payload.size()) + "\n" + sha256_hex(payload) + "\n" + payload;
}
bool decode_record(const std::string& value, Configuration& out, std::string& error) {
    std::string_view input(value); std::vector<std::string_view> fields;
    for (unsigned n = 0; n < 6; ++n) {
        const auto newline = input.find('\n'); if (newline == std::string_view::npos) { error = "Configuration record header truncated"; return false; }
        fields.push_back(input.substr(0, newline)); input.remove_prefix(newline + 1);
    }
    auto consortium = model::ConsortiumId::parse(fields[1]); auto semantic = model::parse_uint64(fields[2]);
    auto presentation = model::parse_uint64(fields[3]); auto length = model::parse_uint64(fields[4]);
    if (fields[0] != "AMITY3" || !consortium || !semantic || !presentation || !length || *length != input.size() || *length > configuration_payload_limit ||
        sha256_hex(input) != fields[5] || !parse_configuration(std::string(input), out, error) || out.consortium_id != *consortium ||
        out.config_revision != *semantic || out.presentation_revision != *presentation || serialize_configuration(out) != input) {
        if (error.empty()) error = "Configuration record metadata, canonical payload, or digest mismatch";
        return false;
    }
    return true;
}
std::string presentation_hash(const Configuration& c) {
    auto encoded = serialize_configuration(c); auto* root = cJSON_Parse(encoded.c_str()); if (!root) return {};
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
ConfigResult ConfigurationStore::recover(Configuration& out, std::string& error) {
    initialized_ = true;
    Marker selected; std::string payload; auto status = selection(backend_, "cm0", "cm1", "c0", "c1", selected, payload, error);
    if (status != ConfigResult::ok) return status;
    Configuration config; if (!decode_record(payload, config, error)) return ConfigResult::recovery_required;
    active_ = config; generation_ = selected.generation; active_slot_ = selected.slot; candidate_.reset();
    out = std::move(config);
    backend_.erase(selected.slot ? "c0" : "c1");
    return ConfigResult::ok;
}
ConfigResult ConfigurationStore::stage(Configuration candidate, RevisionPair expected, std::string& error) {
    if (!initialized_) { Configuration current; auto status = recover(current, error); if (status != ConfigResult::ok && status != ConfigResult::missing) return status; }
    if (candidate_) { error = "A configuration transaction is already staged"; return ConfigResult::conflict; }
    if ((active_ && expected != active_->revisions()) || (!active_ && expected != RevisionPair{})) { error = "Configuration or presentation revision changed"; return ConfigResult::conflict; }
    if (generation_ == std::numeric_limits<uint64_t>::max()) { error = "Storage generation exhausted"; return ConfigResult::recovery_required; }
    if(!validate_configuration(candidate,error))return ConfigResult::invalid;

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
    if (!validate_configuration(candidate, error)) return ConfigResult::invalid;
    const auto encoded = serialize_configuration(candidate);
    Configuration canonical;
    if (!parse_configuration(encoded, canonical, error)) return ConfigResult::invalid;
    candidate = std::move(canonical); // Installed memory and journal bytes agree exactly.
    if (encoded.size() > configuration_payload_limit) { error = "Candidate exceeds the16KiB payload capacity for this board"; return ConfigResult::capacity; }
    const auto complete_record = record(candidate); const auto inactive = active_slot_ ? 0u : 1u;
    if (write_verified(inactive ? "c1" : "c0", complete_record) != ConfigResult::ok) { error = "Candidate write/readback failed; old active record retained"; return ConfigResult::storage_error; }
    candidate_ = std::move(candidate); return ConfigResult::ok;
}
ConfigResult ConfigurationStore::commit(Configuration& out, std::string& error) {
    if (!candidate_) { error = "No configuration transaction is staged"; return ConfigResult::not_ready; }
    const unsigned slot = active_slot_ ? 0u : 1u; const auto complete = record(*candidate_); std::string reread;
    if (backend_.read(slot ? "c1" : "c0", reread) != ReadResult::found || reread != complete) { error = "Staged candidate changed before commit"; return ConfigResult::recovery_required; }
    const Marker next{generation_ + 1, slot, sha256_hex(complete)};
    if (write_verified(slot ? "cm1" : "cm0", marker_text(next)) != ConfigResult::ok) { error = "Activation marker write/readback failed"; return ConfigResult::storage_error; }
    // This is the durable decision, not an assertion of simultaneous effects.
    active_ = candidate_; candidate_.reset(); generation_ = next.generation; active_slot_ = slot; out = *active_;
    // Prove the selected complete payload again after the durable marker. Only
    // obsolete records may be reclaimed; the only committed record is retained.
    std::string committed;
    if (backend_.read(slot ? "c1" : "c0", committed) != ReadResult::found || committed != complete) { error = "Committed payload readback failed; recovery required"; return ConfigResult::recovery_required; }
    backend_.erase(slot ? "c0" : "c1");
    return ConfigResult::ok;
}
void ConfigurationStore::cancel_stage() { candidate_.reset(); }

} // namespace config_store
