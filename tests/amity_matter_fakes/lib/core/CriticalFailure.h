/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// The firmware's CHIP is built without CALLER_HANDLES_CRITICAL_FAILURE: a
// critical failure aborts, so the error a caller reads is always success. The
// fake fails the scenario where the target would abort.
#include "lib/core/CHIPError.h"

namespace chip {

class CriticalFailure {
public:
    CriticalFailure(CHIP_ERROR error);

    CHIP_ERROR GetError() const { return CHIP_NO_ERROR; }
    operator CHIP_ERROR() const { return GetError(); }
    bool operator==(const ChipError &other) const { return GetError() == other; }
    bool operator!=(const ChipError &other) const { return GetError() != other; }
};

}  // namespace chip

inline bool operator==(const chip::ChipError &lhs, const chip::CriticalFailure &rhs) { return lhs == rhs.GetError(); }
inline bool operator!=(const chip::ChipError &lhs, const chip::CriticalFailure &rhs) { return lhs != rhs.GetError(); }
