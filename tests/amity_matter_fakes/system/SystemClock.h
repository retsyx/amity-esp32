/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// CHIP's System Layer durations.
#include <chrono>
#include <cstdint>

namespace chip {
namespace System {
namespace Clock {

using Seconds16 = std::chrono::duration<uint16_t>;
using Seconds32 = std::chrono::duration<uint32_t>;
using Milliseconds32 = std::chrono::duration<uint32_t, std::milli>;
using Timeout = Milliseconds32;

}  // namespace Clock
}  // namespace System
}  // namespace chip
