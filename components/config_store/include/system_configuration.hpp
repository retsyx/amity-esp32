#pragma once
#include "amity/system/discovery.hpp"
#include "identity_journal.hpp"
#include "esp_err.h"

namespace config_store {
namespace model = amity::system;
struct RevisionPair {
    uint64_t config_revision = 0, presentation_revision = 0;
    friend bool operator==(const RevisionPair& a, const RevisionPair& b) { return a.config_revision == b.config_revision && a.presentation_revision == b.presentation_revision; }
    friend bool operator!=(const RevisionPair& a, const RevisionPair& b) { return !(a == b); }
};
struct ConfiguredComponent {
    model::Component record;
    std::string kind; // source, display, receiver, switch, audio, placeholder
    std::string generated_label;
    bool label_override = false;
};
struct ConfiguredActivity {
    model::ActivityId activity_id;
    model::ZoneId zone_id;
    std::string label, generated_label;
    bool label_override = false;
    std::optional<model::ComponentId> source, display, audio, switch_device;
    uint8_t switch_input = 0;
};
struct ConfiguredZone {
    model::ZoneId zone_id;
    std::string label, generated_label;
    bool label_override = false;
    std::vector<model::ActivityId> activity_order;
};
struct ExternalSlot {
    uint16_t slot = 0;
    model::ActivityId activity_id;
    bool tombstone = false, hidden = false;
    std::string alias;
};
struct ScopedQuirk { model::ComponentId component_id; uint32_t vendor_id = 0; std::vector<uint8_t> power_on, power_off; };
struct ConfiguredOctopus {
    model::OctopusId octopus_id;
    std::string label, generated_label;
    bool label_override = false;
    uint8_t supported_tentacles = 1;
};
struct ConfigurationV2 {
    model::ConsortiumId consortium_id;
    uint64_t config_revision = 1, presentation_revision = 1;
    model::ZoneId primary_zone_id;
    std::vector<ConfiguredOctopus> octopuses;
    std::vector<ConfiguredComponent> components;
    std::vector<ConfiguredActivity> activities;
    std::vector<ConfiguredZone> zones;
    std::vector<ExternalSlot> external_slots;
    std::vector<ScopedQuirk> quirks;
    // Immutable first-setup controller. Absent only in legacy schema2 records
    // awaiting an upgrade using authenticated original-authority evidence.
    std::optional<model::OctopusId> controller_octopus_id;
    // Global presentation order; external integration slots remain independent.
    std::vector<model::ActivityId> activity_order;
    // Decode-only format hint; never serialized or used as availability state.
    bool legacy_encoding = false;
    RevisionPair revisions() const { return {config_revision, presentation_revision}; }
};
// This board/112KiB NVS profile admits16KiB new staged payloads. The decoder
// retains32KiB compatibility so existing larger committed backups remain readable.
constexpr size_t configuration_payload_limit = 16384;
constexpr size_t configuration_compatibility_limit = 32768;
bool validate_configuration_v2(const ConfigurationV2&, std::string& error);
bool parse_configuration_v2(const std::string&, ConfigurationV2&, std::string& error);
std::string serialize_configuration_v2(const ConfigurationV2&, bool executable_policy_only = false);
std::string sha256_hex(std::string_view);
std::string executable_policy_hash(const ConfigurationV2&);
const ConfiguredActivity* activity_for_slot(const ConfigurationV2&, unsigned slot);
std::vector<model::ActivityId> ordered_activities(const ConfigurationV2&);
// Preserve slots and tombstones independently of presentation ordering.
bool reconcile_external_slots(const ConfigurationV2& previous, ConfigurationV2& candidate, std::string& error);
// The caller proves the original controller from durable membership/startup
// authority, never record ordering. This helper neither transfers nor changes
// any BLE bond or security identity.
bool bind_original_controller(ConfigurationV2&, model::OctopusId authenticated_original,
    bool& changed, std::string& error);

enum class ConfigResult { ok, missing, conflict, invalid, storage_error, recovery_required, not_ready, capacity };
struct ConfigBackend : IdentityBackend { virtual bool erase(const char* key) = 0; };
class ConfigurationStore {
public:
    explicit ConfigurationStore(ConfigBackend& backend) : backend_(backend) {}
    ConfigResult recover(ConfigurationV2&, std::string& error);
    ConfigResult stage(ConfigurationV2 candidate, RevisionPair expected, std::string& error);
    ConfigResult commit(ConfigurationV2&, std::string& error);
    void cancel_stage();
    const std::optional<ConfigurationV2>& staged() const { return candidate_; }
private:
    ConfigResult write_verified(const char*, const std::string&);
    ConfigBackend& backend_;
    std::optional<ConfigurationV2> active_, candidate_;
    uint64_t generation_ = 0;
    unsigned active_slot_ = 1;
    bool initialized_ = false;
};

struct LegacyActivity { std::string name, display, source, audio, switch_device; uint8_t switch_input = 0; };
struct LegacyQuirk { uint32_t vendor_id = 0; std::vector<uint8_t> power_on, power_off; };
struct LegacyAlias { unsigned slot = 0; bool hidden = false; std::string alias; };
struct LegacyConfiguration { std::vector<LegacyActivity> activities; std::vector<LegacyQuirk> quirks; std::vector<LegacyAlias> aliases; };
struct LegacyEvidence { model::QualifiedDeviceName name; bool native = true, unique = true; };
// Stable migration seed and original blob are journaled before any generated IDs
// are exposed. Presence is resolved at execution; it never disables saved intent.
ConfigResult migrate_legacy(ConfigurationStore&, ConfigBackend&, const std::string& original_json,
    const LegacyConfiguration&, model::ConsortiumId, model::OctopusId,
    const std::vector<LegacyEvidence>&, const std::function<bool(uint8_t*, size_t)>& secure_random,
    ConfigurationV2&, std::vector<std::string>& assignment_tasks, std::string& error);
bool propose_local_activity(const ConfigurationV2& previous, const LegacyActivity& desired,
    const std::vector<LegacyEvidence>&, const std::function<bool(uint8_t*, size_t)>& secure_random,
    ConfigurationV2& candidate, std::vector<std::string>& assignment_tasks, std::string& error);
// A compiled carrier can add stable connector capacity without approving or
// persisting equipment availability.
bool reconcile_hardware_profile(ConfigurationV2&, model::OctopusId, uint8_t capacity,
    bool& changed, std::string& error);


// Real NVS-backed entrypoints. They share amity_config's two-generation journal;
// callers reserve the live owner-queue transaction before invoking activation.
esp_err_t load_system_configuration(ConfigurationV2&, std::string& error);
esp_err_t stage_system_configuration(ConfigurationV2, RevisionPair expected, std::string& error);
esp_err_t activate_system_configuration(ConfigurationV2&, std::string& error);
esp_err_t staged_system_configuration(ConfigurationV2&);
esp_err_t cancel_staged_configuration();
esp_err_t migrate_system_configuration(const std::string& original_json, const LegacyConfiguration&,
    model::ConsortiumId, model::OctopusId, const std::vector<LegacyEvidence>&,
    ConfigurationV2&, std::vector<std::string>& assignment_tasks, std::string& error);

} // namespace config_store
