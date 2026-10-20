#include "system_identity.hpp"
#include "identity_journal.hpp"
#include "nvs.h"
#include "esp_random.h"
#include "bootloader_random.h"
#include <vector>

namespace config_store {
namespace {
class NvsIdentity final : public IdentityBackend {
public:
    explicit NvsIdentity(const char *space) : space_(space) {}
    ReadResult read(const char *key, std::string &value) override {
        nvs_handle_t h;
        auto e = nvs_open_from_partition("amity_cfg", space_, NVS_READONLY, &h);
        if (e == ESP_ERR_NVS_NOT_FOUND) return ReadResult::missing;
        if (e != ESP_OK) return ReadResult::error;
        size_t size = 0;
        e = nvs_get_blob(h, key, nullptr, &size);
        if (e == ESP_ERR_NVS_NOT_FOUND) { nvs_close(h); return ReadResult::missing; }
        constexpr size_t record_size = sizeof("1:unprovisioned:") - 1 + 32;
        if (e != ESP_OK || size != record_size) { nvs_close(h); return ReadResult::error; }
        std::vector<char> bytes(size);
        e = nvs_get_blob(h, key, bytes.data(), &size);
        nvs_close(h);
        if (e != ESP_OK) return ReadResult::error;
        value.assign(bytes.data(), bytes.size()); return ReadResult::found;
    }
    bool write(const char *key, const std::string &value) override {
        nvs_handle_t h;
        if (nvs_open_from_partition("amity_cfg", space_, NVS_READWRITE, &h) != ESP_OK) return false;
        auto e = nvs_set_blob(h, key, value.data(), value.size());
        if (e == ESP_OK) e = nvs_commit(h);
        nvs_close(h); return e == ESP_OK;
    }
private:
    const char *space_;
};
}
esp_err_t load_system_identity(amity::system::OctopusId &octopus, amity::system::ConsortiumId &consortium) {
    // The early startup path has not enabled Wi-Fi entropy yet. Enable the
    // supported S3 entropy source only while producing these nonsecret IDs.
    bootloader_random_enable();
    auto random = [](uint8_t *data, size_t size) { esp_fill_random(data, size); return true; };
    NvsIdentity origin("amity_origin"), singleton("amity_system");
    const bool valid = load_identity(origin, octopus, random) && load_identity(singleton, consortium, random);
    bootloader_random_disable();
    return valid ? ESP_OK : ESP_ERR_INVALID_STATE;
}
}
