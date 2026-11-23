/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// Seams for components/network_diagnostics/network_diagnostics.cpp.
#include "network_diagnostics.hpp"
#include "seams.hpp"

void amity::network_diagnostics::start() { AMITY_SEAM(amity::network_diagnostics::start); }
auto amity::network_diagnostics::snapshot() -> Snapshot { return AMITY_SEAM(amity::network_diagnostics::snapshot); }
void amity::network_diagnostics::record_console_call(uint32_t began_ms, uint32_t duration_ms) {
    AMITY_SEAM(amity::network_diagnostics::record_console_call, began_ms, duration_ms);
}
void amity::network_diagnostics::poll() { AMITY_SEAM(amity::network_diagnostics::poll); }
