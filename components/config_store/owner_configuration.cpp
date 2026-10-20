#include "system_configuration.hpp"
#include <algorithm>

namespace config_store {
namespace {
struct Marker { uint64_t generation; unsigned slot; std::string hash; };
std::optional<Marker> decode_marker(std::string_view text) {
    if (text.substr(0, 2) != "2:") return {};
    text.remove_prefix(2); auto end = text.find(':'); if (end == std::string_view::npos) return {};
    auto generation = model::parse_uint64(text.substr(0, end)); text.remove_prefix(end + 1);
    if (!generation || !*generation || text.size() != 66 || (text[0] != '0' && text[0] != '1') || text[1] != ':') return {};
    for (char c : text.substr(2)) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return {};
    return Marker{*generation, static_cast<unsigned>(text[0] - '0'), std::string(text.substr(2))};
}
ConfigResult selected(ConfigBackend& backend, Marker& out, std::string& record, std::string& error) {
    std::optional<Marker> latest;
    for (const auto* key : {"smark0", "smark1"}) {
        std::string value; auto status = backend.read(key, value);
        if (status == ReadResult::error) return ConfigResult::storage_error;
        if (status == ReadResult::missing) continue;
        auto marker = decode_marker(value); if (!marker) { error = "Owner slice activation marker is corrupt"; return ConfigResult::recovery_required; }
        if (latest && marker->generation == latest->generation && (marker->hash != latest->hash || marker->slot != latest->slot)) return ConfigResult::recovery_required;
        if (!latest || marker->generation > latest->generation) latest = marker;
    }
    if (!latest) return ConfigResult::missing;
    if (backend.read(latest->slot ? "slice1" : "slice0", record) != ReadResult::found || sha256_hex(record) != latest->hash) { error = "Committed owner slice is missing or corrupt"; return ConfigResult::recovery_required; }
    out = *latest; return ConfigResult::ok;
}
bool decode_record(const std::string& record, ConfigurationV2& config, model::OctopusId& owner, model::OctopusId& authority, uint64_t& epoch, std::string& error) {
    std::string_view rest(record); std::vector<std::string_view> fields;
    for (unsigned n = 0; n < 5; ++n) { auto end = rest.find('\n'); if (end == std::string_view::npos) return false; fields.push_back(rest.substr(0, end)); rest.remove_prefix(end + 1); }
    auto owner_id = model::OctopusId::parse(fields[1]), authority_id = model::OctopusId::parse(fields[2]); auto generation = model::parse_uint64(fields[3]);
    if (fields[0] != "OWNER2" || !owner_id || !authority_id || !generation || !*generation || sha256_hex(rest) != fields[4] ||
        !parse_configuration_v2(std::string(rest), config, error) || serialize_configuration_v2(config) != rest) return false;
    if (config.controller_octopus_id && *config.controller_octopus_id != *authority_id) return false;
    if (std::none_of(config.octopuses.begin(), config.octopuses.end(), [&](const ConfiguredOctopus& b) { return b.octopus_id == *owner_id; })) return false;
    owner = *owner_id; authority = *authority_id; epoch = *generation; return true;
}
bool write(ConfigBackend& backend, const char* key, const std::string& value) {
    if (!backend.write(key, value)) return false;
    std::string actual; return backend.read(key, actual) == ReadResult::found && actual == value;
}
}
ConfigResult OwnerConfigurationJournal::recover(ConfigurationV2& config, model::OctopusId& owner, model::OctopusId& authority, uint64_t& epoch, std::string& error) {
    Marker marker{}; std::string record; auto status = selected(backend_, marker, record, error); if (status != ConfigResult::ok) return status;
    if (!decode_record(record, config, owner, authority, epoch, error)) { error = "Owner slice metadata or canonical payload is corrupt"; return ConfigResult::recovery_required; }
    backend_.erase(marker.slot ? "slice0" : "slice1");
    return ConfigResult::ok;
}
ConfigResult OwnerConfigurationJournal::commit(const ConfigurationV2& config, model::OctopusId owner, model::OctopusId authority, uint64_t epoch, std::string& error) {
    if (!owner || !authority || epoch!=1 || !config.legacy_remote_configuration.empty() || !validate_configuration_v2(config, error)) return ConfigResult::invalid;
    if (!config.controller_octopus_id || *config.controller_octopus_id != authority) { error = "The owner configuration must come from the original controller"; return ConfigResult::invalid; }
    if (std::none_of(config.octopuses.begin(), config.octopuses.end(), [&](const ConfiguredOctopus& b) { return b.octopus_id == owner; })) return ConfigResult::invalid;
    Marker marker{}; std::string previous; auto status = selected(backend_, marker, previous, error);
    if (status != ConfigResult::ok && status != ConfigResult::missing) return status;
    const auto payload = serialize_configuration_v2(config);
    if (status == ConfigResult::ok) {
        ConfigurationV2 old; model::OctopusId old_owner, old_authority; uint64_t old_epoch;
        if (!decode_record(previous, old, old_owner, old_authority, old_epoch, error)) return ConfigResult::recovery_required;
        bool legacy_projection_upgrade = false;
        if ((!old.controller_octopus_id || *old.controller_octopus_id==authority) && epoch == 1 && old_epoch == 1 && old_authority == authority && old_owner == owner && old.revisions() == config.revisions()) {
            auto normalized = old;
            if (std::none_of(normalized.octopuses.begin(), normalized.octopuses.end(), [&](const auto& b) { return b.octopus_id == authority; })) {
                const auto controller = std::find_if(config.octopuses.begin(), config.octopuses.end(), [&](const auto& b) { return b.octopus_id == authority; });
                if (controller != config.octopuses.end()) normalized.octopuses.push_back(*controller);
            }
            bool changed = false; std::string detail;
            if (bind_original_controller(normalized, authority, changed, detail)) {
                normalized = configuration_owner_slice(normalized, owner);
                legacy_projection_upgrade = serialize_configuration_v2(normalized) == payload;
            }
        }
        if ((old.controller_octopus_id && old.controller_octopus_id != config.controller_octopus_id) || old_owner != owner || config.consortium_id != old.consortium_id || epoch < old_epoch || authority != old_authority ||
            config.config_revision < old.config_revision || config.presentation_revision < old.presentation_revision ||
            (config.config_revision == old.config_revision && executable_policy_hash(config) != executable_policy_hash(old) && !legacy_projection_upgrade)) { error = "Owner slice authority or revision is stale/conflicting"; return ConfigResult::conflict; }
        if (old.revisions() == config.revisions() && old_authority == authority && old_epoch == epoch) {
            if (serialize_configuration_v2(old) == payload) return ConfigResult::ok;
            if (!legacy_projection_upgrade) return ConfigResult::conflict;
        }
    }
    if (marker.generation == UINT64_MAX) return ConfigResult::recovery_required;
    const unsigned slot = status == ConfigResult::missing ? 0 : !marker.slot;
    const std::string record = "OWNER2\n" + owner.str() + "\n" + authority.str() + "\n" + model::format_uint64(epoch) + "\n" + sha256_hex(payload) + "\n" + payload;
    if (!write(backend_, slot ? "slice1" : "slice0", record)) return ConfigResult::storage_error;
    const std::string commit = "2:" + model::format_uint64(marker.generation + 1) + ":" + std::to_string(slot) + ":" + sha256_hex(record);
    if (!write(backend_, slot ? "smark1" : "smark0", commit)) return ConfigResult::storage_error;
    std::string proof; if (backend_.read(slot ? "slice1" : "slice0", proof) != ReadResult::found || proof != record) return ConfigResult::recovery_required;
    backend_.erase(slot ? "slice0" : "slice1"); return ConfigResult::ok;
}
} // namespace config_store
