#pragma once
#include "amity/hub.hpp"
#include "esp_err.h"
#include <functional>
#include <memory>

namespace amity {
// Thread-safe facade. Commands are queued; snapshot returns the latest state.
// Callback runs on the hub task, outside the snapshot lock. It should be brief.
class HubService {
public:
    HubService();
    ~HubService();
    esp_err_t start(int front_gpio, int back_gpio, uint16_t front_physical = 0x1000);
    bool configure(std::vector<Activity> activities, std::vector<Quirk> quirks = {});
    // Reserve command capacity before committing external state. The callback
    // runs synchronously on the caller, must return an error on failure, and
    // must not call configure recursively. A failed reservation never calls it.
    esp_err_t configure_with_commit(std::vector<Activity> activities, std::vector<Quirk> quirks,
                                    std::function<esp_err_t()> commit);
    // ESP_ERR_TIMEOUT means ignored during settling; same-target repair bypasses it.
    esp_err_t request_activity(int index);
    int activity_target() const;
    bool select_activity(int index);
    // repeat=true holds the key: each completed wire attempt permits the next
    // press, until release or an activity/configuration change cancels it.
    bool press_key(uint8_t key, bool repeat = false);
    // The worker discards this press if the activity/configuration changed
    // after the originating operation captured its generation. repeat=true
    // starts a held stream with the same cancellation policy as press_key().
    bool press_key_for_activity(uint8_t key, uint32_t activity_generation, bool repeat = false);
    uint32_t activity_generation() const;
    bool release_key();
    bool standby();
    bool discover();
    Snapshot snapshot() const;
    void on_change(std::function<void(const Snapshot &)> callback);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
