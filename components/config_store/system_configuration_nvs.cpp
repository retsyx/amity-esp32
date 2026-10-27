#include "system_configuration.hpp"
#include "candidate_network.hpp"
#include "nvs_budget.hpp"
#include "config_store.hpp"
#include "nvs.h"
#include "esp_random.h"
#include <mutex>
#include <atomic>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

namespace config_store {
namespace {
class DirectNvsConfiguration final : public ConfigBackend {
public:
    ReadResult read(const char* key, std::string& value) override {
        nvs_handle_t handle;
        auto error = nvs_open_from_partition("amity_cfg", "amity_config", NVS_READONLY, &handle);
        if (error == ESP_ERR_NVS_NOT_FOUND) return ReadResult::missing;
        if (error != ESP_OK) return ReadResult::error;
        size_t length = 0; error = nvs_get_blob(handle, key, nullptr, &length);
        if (error == ESP_ERR_NVS_NOT_FOUND) { nvs_close(handle); return ReadResult::missing; }
        if (error != ESP_OK || length > configuration_compatibility_limit + 256) { nvs_close(handle); return ReadResult::error; }
        std::string bytes(length, '\0'); error = nvs_get_blob(handle, key, bytes.data(), &length); nvs_close(handle);
        if (error != ESP_OK || length != bytes.size()) return ReadResult::error;
        value = std::move(bytes); return ReadResult::found;
    }
    bool write(const char* key, const std::string& value) override {
        if (value.size() > configuration_compatibility_limit + 256 || !detail::blob_space(value.size(), detail::decision_key(key))) return false;
        nvs_handle_t handle;
        if (nvs_open_from_partition("amity_cfg", "amity_config", NVS_READWRITE, &handle) != ESP_OK) return false;
        auto error = nvs_set_blob(handle, key, value.data(), value.size());
        if (error == ESP_OK) error = nvs_commit(handle);
        nvs_close(handle); return error == ESP_OK;
    }
    bool erase(const char* key) override {
        nvs_handle_t handle;
        if (nvs_open_from_partition("amity_cfg", "amity_config", NVS_READWRITE, &handle) != ESP_OK) return false;
        auto error = nvs_erase_key(handle, key);
        if (error == ESP_OK || error == ESP_ERR_NVS_NOT_FOUND) error = nvs_commit(handle);
        nvs_close(handle); return error == ESP_OK;
    }
};
// Flash operations must run on an internal stack even when the HTTP or
// synchronization caller uses PSRAM. The caller blocks with its external stack
// untouched while IDF disables the flash/PSRAM cache in the internal I/O task.
struct FlashCall { std::function<void()> function; SemaphoreHandle_t complete; };
QueueHandle_t flash_queue = nullptr;
std::atomic<TaskHandle_t> flash_task{nullptr};
std::mutex flash_dispatch_gate;
void flash_worker(void*) {
    for (;;) {
        FlashCall* call = nullptr;
        if (xQueueReceive(flash_queue, &call, portMAX_DELAY) == pdTRUE && call) {
            call->function();
            xSemaphoreGive(call->complete);
        }
    }
}
bool on_flash_stack(std::function<void()> function) {
    if (flash_task.load() && xTaskGetCurrentTaskHandle() == flash_task.load()) { function(); return true; }
    std::lock_guard<std::mutex> dispatch(flash_dispatch_gate);
    if (!flash_task.load()) {
        flash_queue = xQueueCreate(1, sizeof(FlashCall*));
        if (!flash_queue) return false;
        TaskHandle_t created = nullptr;
        if (xTaskCreate(flash_worker, "config_flash", 8192, nullptr, 2, &created) != pdPASS) {
            vQueueDelete(flash_queue); flash_queue = nullptr; return false;
        }
        flash_task.store(created);
    }
    auto complete = xSemaphoreCreateBinary(); if (!complete) return false;
    FlashCall call{std::move(function), complete}; auto* request = &call;
    const bool sent = xQueueSend(flash_queue, &request, portMAX_DELAY) == pdTRUE;
    const bool finished = sent && xSemaphoreTake(complete, portMAX_DELAY) == pdTRUE;
    vSemaphoreDelete(complete); return finished;
}
class NvsConfiguration final : public ConfigBackend {
public:
    ReadResult read(const char* key, std::string& value) override {
        ReadResult result = ReadResult::error;
        return on_flash_stack([&] { result = direct_.read(key, value); }) ? result : ReadResult::error;
    }
    bool write(const char* key, const std::string& value) override {
        bool result = false; return on_flash_stack([&] { result = direct_.write(key, value); }) && result;
    }
    bool erase(const char* key) override {
        bool result = false; return on_flash_stack([&] { result = direct_.erase(key); }) && result;
    }
private:
    DirectNvsConfiguration direct_;
};
NvsConfiguration backend;
ConfigurationStore store(backend);
CandidateNetworkJournal candidate_network_store(backend);
std::mutex gate;
bool recovered = false;
ConfigResult recover_once(std::string& error) {
    if (recovered) return ConfigResult::ok;
    ConfigurationV2 current; auto result = store.recover(current, error);
    if (result == ConfigResult::ok || result == ConfigResult::missing) recovered = true;
    return result;
}
esp_err_t result(ConfigResult value) {
    switch (value) {
    case ConfigResult::ok: return ESP_OK; case ConfigResult::missing: return ESP_ERR_NVS_NOT_FOUND;
    case ConfigResult::invalid: return ESP_ERR_INVALID_ARG; case ConfigResult::capacity: return ESP_ERR_INVALID_SIZE;
    case ConfigResult::conflict: case ConfigResult::not_ready: case ConfigResult::recovery_required: return ESP_ERR_INVALID_STATE;
    case ConfigResult::storage_error: return ESP_FAIL;
    }
    return ESP_FAIL;
}
}
bool with_internal_flash_stack(std::function<void()> function) { return on_flash_stack(std::move(function)); }
esp_err_t check_blob_storage(size_t bytes, bool decision) {
    bool fits = false;
    if (!on_flash_stack([&] { fits = detail::blob_space(bytes, decision); })) return ESP_FAIL;
    return fits ? ESP_OK : ESP_ERR_NVS_NOT_ENOUGH_SPACE;
}
esp_err_t storage_budget(StorageBudget& out) {
    nvs_stats_t stats{}; esp_err_t status = ESP_FAIL;
    if (!on_flash_stack([&] { status = nvs_get_stats("amity_cfg", &stats); })) return ESP_FAIL;
    if (status != ESP_OK) return status;
    out = {stats.used_entries, stats.available_entries, stats.total_entries, configuration_payload_limit}; return ESP_OK;
}
esp_err_t load_system_configuration(ConfigurationV2& out, std::string& error) {
    std::lock_guard<std::mutex> lock(gate); auto status = store.recover(out, error);
    if (status == ConfigResult::ok || status == ConfigResult::missing) recovered = true;
    return result(status);
}
esp_err_t stage_system_configuration(ConfigurationV2 candidate, RevisionPair expected, std::string& error) {
    std::lock_guard<std::mutex> lock(gate); const auto status = recover_once(error);
    if (status != ConfigResult::ok && status != ConfigResult::missing) return result(status);
    return result(store.stage(std::move(candidate), expected, error));
}
esp_err_t staged_system_configuration(ConfigurationV2& out) {
    std::lock_guard<std::mutex> lock(gate); if (!store.staged()) return ESP_ERR_INVALID_STATE;
    out = *store.staged(); return ESP_OK;
}
esp_err_t cancel_staged_configuration() { std::lock_guard<std::mutex> lock(gate); store.cancel_stage(); return ESP_OK; }
esp_err_t activate_system_configuration(ConfigurationV2& out, std::string& error) {
    std::lock_guard<std::mutex> lock(gate); return result(store.commit(out, error));
}
esp_err_t load_candidate_network(CandidateNetwork& out,std::string& error){std::lock_guard<std::mutex> lock(gate);return result(candidate_network_store.recover(out,error));}
esp_err_t stage_candidate_network(const CandidateNetwork& value,std::string& error){std::lock_guard<std::mutex> lock(gate);return result(candidate_network_store.stage(value,error));}
esp_err_t decide_candidate_network(model::JoinId join,const std::string& digest,bool use_candidate,std::string& error){std::lock_guard<std::mutex> lock(gate);return result(candidate_network_store.decide(join,digest,use_candidate,error));}
esp_err_t finish_candidate_network(model::JoinId join,const std::string& digest,std::string& error){std::lock_guard<std::mutex> lock(gate);return result(candidate_network_store.finish(join,digest,error));}
esp_err_t abandon_candidate_network(model::JoinId join,const std::string& digest,std::string& error){std::lock_guard<std::mutex> lock(gate);return result(candidate_network_store.abandon(join,digest,error));}
esp_err_t migrate_system_configuration(const std::string& original, const LegacyConfiguration& legacy, model::ConsortiumId consortium,
    model::OctopusId octopus, const std::vector<LegacyEvidence>& evidence, ConfigurationV2& out, std::vector<std::string>& tasks, std::string& error) {
    std::lock_guard<std::mutex> lock(gate); const auto status = recover_once(error);
    if (status != ConfigResult::ok && status != ConfigResult::missing) return result(status);
    const auto random = [](uint8_t* data, size_t size) { esp_fill_random(data, size); return true; };
    auto migrated = legacy;
    for (unsigned n = 0; n < legacy.activities.size() && n < 16; ++n) {
        std::string record;
        esp_err_t alias_result = ESP_FAIL;
        if (!on_flash_stack([&] { alias_result = config_store::read(("hap_input_" + std::to_string(n)).c_str(), record); }) || alias_result != ESP_OK) continue;
        const auto split = record.find('\0');
        if (split != std::string::npos && split + 1 < record.size() && record.substr(0, split) == legacy.activities[n].name &&
            (record[split + 1] == '0' || record[split + 1] == '1') && record.size() - split - 2 <= 128)
            migrated.aliases.push_back({n, record[split + 1] == '1', record.substr(split + 2)});
    }
    return result(migrate_legacy(store, backend, original, migrated, consortium, octopus, evidence, random, out, tasks, error));
}
} // namespace config_store
