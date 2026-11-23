/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// The part of the CHIP device layer the consortium GATT transport uses: the
// error type its BLE manager returns.
#include <cstdint>

namespace chip {

class ChipError {
public:
    constexpr explicit ChipError(uint32_t value) : value_(value) {}
    constexpr uint32_t AsInteger() const { return value_; }
    constexpr bool operator==(const ChipError &other) const { return value_ == other.value_; }
    constexpr bool operator!=(const ChipError &other) const { return value_ != other.value_; }

private:
    uint32_t value_;
};

}  // namespace chip

using CHIP_ERROR = ::chip::ChipError;
#define CHIP_NO_ERROR CHIP_ERROR(0)
#define CHIP_ERROR_INCORRECT_STATE CHIP_ERROR(0x03)
