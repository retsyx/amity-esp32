/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ble {
// A report descriptor decoder, independent of Bluetooth and Amity. Report IDs
// are supplied separately, as they are by the ESP-IDF HID host event API.
class HidKeyboard {
public:
    using Callback = void (*)(uint16_t page, uint16_t usage, bool pressed, void *context);
    explicit HidKeyboard(Callback callback = nullptr, void *context = nullptr)
        : callback_(callback), context_(context) {}
    bool add_map(uint8_t map_index, const uint8_t *descriptor, size_t length);
    bool input(uint8_t map_index, uint8_t report_id, const uint8_t *data, size_t length);
    void release_all();
    void clear();
private:
    struct Field {
        uint16_t offset, size, count;
        uint32_t usage_min, usage_max;
        int32_t logical_min;
        bool variable;
        std::vector<uint32_t> usages;
    };
    struct Report {
        uint8_t map, id;
        uint16_t bits = 0;
        std::vector<Field> fields;
        std::vector<uint32_t> down;
    };
    std::vector<Report> reports_;
    Callback callback_;
    void *context_;
    std::vector<uint32_t> pressed() const;
    void dispatch(const std::vector<uint32_t> &before, const std::vector<uint32_t> &after);
};
} // namespace ble
