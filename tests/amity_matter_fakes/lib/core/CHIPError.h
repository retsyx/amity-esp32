/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// CHIP_ERROR as the pinned CHIP SDK defines it for the Amity build, where
// CHIP_CONFIG_ERROR_FORMAT_AS_STRING is 0: Format() returns the integer code.
// The codes are checked against the SDK by sdk_parity/chip.def.
#include <cinttypes>
#include <cstdint>

// The Amity build's CHIPConfig.h value.
#define CHIP_CONFIG_ERROR_FORMAT_AS_STRING 0

namespace chip {

class ChipError {
public:
    using StorageType = uint32_t;
    using FormatType = StorageType;
#define CHIP_ERROR_INTEGER_FORMAT PRIx32
#define CHIP_ERROR_FORMAT CHIP_ERROR_INTEGER_FORMAT

    explicit constexpr ChipError(StorageType error) : mError(error) {}

    constexpr bool operator==(const ChipError &other) const { return mError == other.mError; }
    constexpr bool operator!=(const ChipError &other) const { return mError != other.mError; }
    constexpr StorageType AsInteger() const { return mError; }
    FormatType Format() const { return mError; }
    static constexpr bool IsSuccess(ChipError error) { return error.mError == 0; }

private:
    StorageType mError;
};

}  // namespace chip

using CHIP_ERROR = ::chip::ChipError;

// SDK core errors: their range and part are 0, so each value is its code.
#define CHIP_CORE_ERROR(e) CHIP_ERROR(e)
#define CHIP_NO_ERROR CHIP_ERROR(0)
#define CHIP_ERROR_INCORRECT_STATE CHIP_CORE_ERROR(0x03)
#define CHIP_ERROR_NO_MEMORY CHIP_CORE_ERROR(0x0b)
#define CHIP_ERROR_INVALID_ARGUMENT CHIP_CORE_ERROR(0x2f)
#define CHIP_ERROR_TIMEOUT CHIP_CORE_ERROR(0x32)
#define CHIP_ERROR_INTERNAL CHIP_CORE_ERROR(0xac)
#define CHIP_ERROR_NOT_FOUND CHIP_CORE_ERROR(0xd8)
#define CHIP_ERROR_BUSY CHIP_CORE_ERROR(0xdb)
