#pragma once
#include "amity/hub_service.hpp"
#include "amity_matter.h"
namespace amity_homekit {
struct Status {
    bool started=false, available=false, pairing=false, busy=false, paired=false;
    unsigned controllers=0;
    esp_err_t error=ESP_OK;
    char manual_code[11]{}, qr_payload[32]{};
};
// Matter owns provisioning and Wi-Fi. Start the worker and restore saved HomeKit state.
esp_err_t start(amity::HubService &hub, const amity_matter_callbacks_t &callbacks);
Status status();
esp_err_t open_pairing();
esp_err_t close_pairing();
esp_err_t remove_pairings();
}
