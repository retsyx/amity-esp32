#include "apple_fabrics.h"
#include "apple_fabric_cleanup.hpp"

#include <app/server/Server.h>
#include <credentials/FabricTable.h>
#include <platform/CHIPDeviceLayer.h>
#include "esp_log.h"
#include "nvs.h"

namespace amity_matter {
namespace {
constexpr const char *tag = "apple_fabrics";

class AppleFabrics final : public chip::FabricTable::Delegate {
public:
    esp_err_t start()
    {
        auto err = nvs_open("amity_fabrics", NVS_READWRITE, &storage_);
        if (err != ESP_OK) return err;
        uint8_t pending = 0;
        err = nvs_get_u8(storage_, "apple_cleanup", &pending);
        if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return err;
        policy_.restore_pending(pending != 0);
        auto &table = chip::Server::GetInstance().GetFabricTable();
        for (const auto &fabric : table) {
            if (fabric.GetFabricIndex() != table.GetPendingNewFabricIndex())
                policy_.committed(fabric.GetFabricIndex(), static_cast<uint16_t>(fabric.GetVendorId()));
        }
        const auto added = table.AddFabricDelegate(this);
        if (added != CHIP_NO_ERROR) return ESP_FAIL;
        if (policy_.pending()) schedule();
        return ESP_OK;
    }

    void FabricWillBeRemoved(const chip::FabricTable &, chip::FabricIndex index) override
    {
        if (policy_.will_remove(index)) save_pending(true);
    }

    void OnFabricRemoved(const chip::FabricTable &, chip::FabricIndex index) override
    {
        if (policy_.removed(index)) schedule();
    }

    void OnFabricCommitted(const chip::FabricTable &table, chip::FabricIndex index) override
    {
        if (const auto *fabric = table.FindFabricWithIndex(index))
            policy_.committed(index, static_cast<uint16_t>(fabric->GetVendorId()));
        if (policy_.pending()) schedule();
    }

private:
    void save_pending(bool pending)
    {
        auto err = nvs_set_u8(storage_, "apple_cleanup", pending ? 1 : 0);
        if (err == ESP_OK) err = nvs_commit(storage_);
        if (err != ESP_OK) ESP_LOGW(tag, "Cannot persist Apple cleanup intent: %s", esp_err_to_name(err));
    }

    void schedule()
    {
        if (scheduled_) return;
        scheduled_ = true;
        const auto err = chip::DeviceLayer::PlatformMgr().ScheduleWork([](intptr_t context) {
            auto *self = reinterpret_cast<AppleFabrics *>(context);
            self->scheduled_ = false;
            self->cleanup();
        }, reinterpret_cast<intptr_t>(this));
        if (err != CHIP_NO_ERROR) {
            scheduled_ = false;
            ESP_LOGW(tag, "Cannot schedule Apple cleanup: %" CHIP_ERROR_FORMAT, err.Format());
            retry();
        }
    }

    void retry()
    {
        const CHIP_ERROR err = chip::DeviceLayer::SystemLayer().StartTimer(chip::System::Clock::Seconds32(1),
            [](chip::System::Layer *, void *context) { static_cast<AppleFabrics *>(context)->schedule(); }, this);
        if (err != CHIP_NO_ERROR)
            ESP_LOGW(tag, "Cannot retry Apple cleanup: %" CHIP_ERROR_FORMAT, err.Format());
    }

    void cleanup()
    {
        if (!policy_.pending()) return;
        auto &server = chip::Server::GetInstance();
        // Wait for in-flight commissioning to commit or roll back. A new Home
        // may have appeared before this work item executes.
        if (!server.GetFailSafeContext().IsFailSafeFullyDisarmed()) { retry(); return; }
        if (!policy_.has_home()) {
            auto &table = server.GetFabricTable();
            chip::FabricIndex keychains[CHIP_CONFIG_MAX_FABRICS];
            size_t count = 0;
            for (const auto &fabric : table)
                if (static_cast<uint16_t>(fabric.GetVendorId()) == apple_keychain_vendor)
                    keychains[count++] = fabric.GetFabricIndex();
            // Deletion invokes SDK delegates. Never mutate the table while
            // iterating it, or nest another Delete inside a removal delegate.
            for (size_t i = 0; i < count; ++i) {
                const auto err = table.Delete(keychains[i]);
                if (err != CHIP_NO_ERROR) {
                    ESP_LOGW(tag, "Apple Keychain cleanup failed: %" CHIP_ERROR_FORMAT, err.Format());
                    retry();
                    return;
                }
                ESP_LOGI(tag, "Removed Apple Keychain fabric after last Apple Home removal");
            }
        }
        save_pending(false);
        policy_.completed();
    }

    nvs_handle_t storage_ = 0;
    AppleFabricCleanup policy_;
    bool scheduled_ = false;
} apple_fabrics;
}

esp_err_t start_apple_fabric_cleanup() { return apple_fabrics.start(); }
}
