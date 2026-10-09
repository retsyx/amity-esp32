/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#include "amity/hub_service.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <unistd.h>

static void commands(void *context) {
    auto &hub = *static_cast<amity::HubService *>(context);
    for (;;) {
        char key;
        if (read(STDIN_FILENO, &key, 1) == 1) {
            switch (key) {
            case 's': hub.discover(); break;
            case '1': hub.select_activity(0); break;
            case '0': hub.standby(); break;
            case 'k': hub.press_key(0); break;
            case '+': hub.press_key(0x41); break;
            default: break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

extern "C" void app_main() {
    static amity::HubService hub;
    ESP_ERROR_CHECK(hub.start(1, 2));
    hub.configure({{"Fixture", "HarnessTV", "HarnessSource", "HarnessSource", "", 0}});
    hub.on_change([](const amity::Snapshot &s) {
        ESP_LOGI("cec_probe", "ready=%d front=%u back=%u devices=%u errors=%lu dropped=%lu",
                 s.ready, s.front_address, s.back_address, static_cast<unsigned>(s.devices.size()),
                 static_cast<unsigned long>(s.tx_errors), static_cast<unsigned long>(s.rx_dropped));
    });
    xTaskCreate(commands, "probe_input", 3072, &hub, 4, nullptr);
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        const auto s = hub.snapshot();
        ESP_LOGI("cec_probe", "alive ready=%d front=%u back=%u devices=%u errors=%lu",
                 s.ready, s.front_address, s.back_address, static_cast<unsigned>(s.devices.size()),
                 static_cast<unsigned long>(s.tx_errors));
    }
}
