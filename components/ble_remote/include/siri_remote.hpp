/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
#include "remote_input.hpp"
#include <array>
#include <cstddef>
#include <cstdint>

namespace ble {
enum class SiriGeneration { None, Unsupported, Gen2, Gen3 };
// Manufacturer data includes the little-endian Apple company identifier.
SiriGeneration siri_classify_manufacturer(const uint8_t *data, size_t size);
// A valid Apple PNP ID may still identify an unsupported remote generation.
bool siri_parse_pnp(const uint8_t *data, size_t size, uint16_t &product);
bool siri_parse_firmware(const uint8_t *data, size_t size, uint16_t &revision);
constexpr bool siri_supported_product(uint16_t product) {
    return product == 0x0314 || product == 0x0315;
}
struct SiriProfile {
    uint16_t input_handle, touch_handle, audio_handle, init_handle;
    std::array<uint16_t, 7> notification_handles{};
    size_t notification_count = 0;
    std::array<std::array<uint8_t, 2>, 2> init_values{};
    size_t init_count = 0;
    uint8_t charging, discharging, plugged_in;
};
// Gen 2 and Gen 3 share the profile; firmware 0x83 changed handles and commands.
SiriProfile siri_profile(uint16_t firmware);
// Returns -1 for an unknown status so malformed reports cannot overwrite state.
int siri_charging_state(const SiriProfile &profile, const uint8_t *data, size_t size);

// Physical Apple reports and touch recognition only. Buttons become a batch
// for the same RemoteInputController used by HID remotes.
class SiriInputAdapter {
public:
    explicit SiriInputAdapter(RemoteInputController &controller) : controller_(controller) {}
    void buttons(const uint8_t *data, size_t size, InputStamp stamp);
    void touches(const uint8_t *data, size_t size, InputStamp stamp);
    void activity_changed();
    void cancel();
private:
    struct Touch {
        uint16_t timestamp = 0;
        int x = 0, y = 0;
        uint8_t pressure = 0, id = 0;
    };
    RemoteInputController &controller_;
    uint16_t buttons_ = 0;
    bool touch_active_ = false, touch_blocked_ = false;
    uint8_t physical_touches_ = 0;
    int primary_axis_ = -1;
    Touch start_touch_{}, last_touch_{};
    void reset_touch();
    static Touch decode_touch(const uint8_t *data, uint16_t timestamp);
};
} // namespace ble
