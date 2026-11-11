#pragma once
#include "amity/system/discovery.hpp"
#include "identity_journal.hpp"
#include "esp_err.h"
#include <memory>

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
struct ConfiguredOctopus {
    model::OctopusId octopus_id;
    std::string label, generated_label;
    bool label_override = false;
    uint8_t supported_tentacles = 1;
};
struct Configuration {
    model::ConsortiumId consortium_id;
    uint64_t config_revision = 1, presentation_revision = 1;
    model::ZoneId primary_zone_id;
    std::vector<ConfiguredOctopus> octopuses;
    std::vector<ConfiguredComponent> components;
    std::vector<ConfiguredActivity> activities;
    std::vector<ConfiguredZone> zones;
    std::vector<ExternalSlot> external_slots;
    // Immutable first-setup controller; required in every saved configuration.
    std::optional<model::OctopusId> controller_octopus_id;
    // Global presentation order; external integration slots remain independent.
    std::vector<model::ActivityId> activity_order;
    RevisionPair revisions() const { return {config_revision, presentation_revision}; }
};
// One prepared immutable value is shared by persistence, control, and readers.
// Old readers may retain an earlier revision while a candidate is committed.
class PreparedConfiguration;
using ConfigurationSnapshot = std::shared_ptr<const PreparedConfiguration>;
class PreparedConfiguration {
public:
    const Configuration configuration;
    const std::string canonical, policy_hash, presentation_hash;
private:
    PreparedConfiguration(Configuration value, std::string bytes, std::string policy, std::string presentation)
        : configuration(std::move(value)), canonical(std::move(bytes)), policy_hash(std::move(policy)),
          presentation_hash(std::move(presentation)) {}
    friend class ConfigurationStore;
};
// The current saved format is the only readable and writable format.
constexpr unsigned current_configuration_version = 3;
// This board/112KiB NVS profile admits16KiB complete configuration payloads.
constexpr size_t configuration_payload_limit = 16384;
bool validate_configuration(const Configuration&, std::string& error);
bool parse_configuration(const std::string&, Configuration&, std::string& error);
std::string serialize_configuration(const Configuration&, bool executable_policy_only = false);
std::string sha256_hex(std::string_view);
std::string executable_policy_hash(const Configuration&);
std::string presentation_hash(const Configuration&);
const ConfiguredActivity* activity_for_slot(const Configuration&, unsigned slot);
std::vector<model::ActivityId> ordered_activities(const Configuration&);
// A running activity keeps its saved recipe and selected equipment targets.
// Presentation order and changes to other activities do not change that intent.
bool retains_activity(const Configuration& previous,const Configuration& candidate,model::ActivityId);
// Preserve slots and tombstones independently of presentation ordering.
bool reconcile_external_slots(const Configuration& previous, Configuration& candidate, std::string& error);
// Explicit user removal deletes every recipe referring to this owner's saved
// equipment. Ordinary missing/offline equipment keeps its saved selections.
bool propose_octopus_removal(const Configuration&, model::OctopusId,
    Configuration& candidate, std::vector<model::ActivityId>& affected, std::string& error);
enum class ConfigResult { ok, missing, conflict, invalid, storage_error, recovery_required, not_ready, capacity };
struct ConfigBackend : IdentityBackend { virtual bool erase(const char* key) = 0; };
class ConfigurationStore {
public:
    explicit ConfigurationStore(ConfigBackend& backend) : backend_(backend) {}
    ConfigResult recover(ConfigurationSnapshot&, std::string& error);
    ConfigResult stage(Configuration candidate, RevisionPair expected, std::string& error);
    ConfigResult commit(ConfigurationSnapshot&, std::string& error);
    void cancel_stage();
    ConfigurationSnapshot staged() const { return candidate_; }
private:
    ConfigResult write_verified(const char*, const std::string&);
    ConfigBackend& backend_;
    ConfigurationSnapshot active_, candidate_;
    std::string candidate_record_hash_;
    uint64_t generation_ = 0;
    unsigned active_slot_ = 1;
    bool initialized_ = false;
};

// Create a fresh empty configuration using current records and hardware capacity.
// This allocates IDs only; the caller owns storage and activation.
bool initialize_configuration(model::ConsortiumId, model::OctopusId, uint8_t supported_tentacles,
    const std::function<bool(uint8_t*, size_t)>& secure_random,
    Configuration&, std::string& error);
// A compiled carrier can add stable connector capacity without approving or
// persisting equipment availability.
bool reconcile_hardware_profile(Configuration&, model::OctopusId, uint8_t capacity,
    bool& changed, std::string& error);


// Real NVS-backed entrypoints. They share amity_config's two-generation journal;
// callers reserve the live owner-queue transaction before invoking activation.
esp_err_t load_system_configuration(ConfigurationSnapshot&, std::string& error);
esp_err_t stage_system_configuration(Configuration, RevisionPair expected, std::string& error);
esp_err_t activate_system_configuration(ConfigurationSnapshot&, std::string& error);
esp_err_t staged_system_configuration(ConfigurationSnapshot&);
esp_err_t cancel_staged_configuration();


} // namespace config_store
