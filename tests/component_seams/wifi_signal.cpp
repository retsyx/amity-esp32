/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// Seams for components/network_diagnostics/wifi_signal.cpp. LowSignalLog is
// pure and has no seam.
#include "wifi_signal.hpp"
#include "seams.hpp"

auto amity::network_diagnostics::wifi_signal() -> WifiSignal { return AMITY_SEAM(amity::network_diagnostics::wifi_signal); }
void amity::network_diagnostics::poll_wifi_signal() { AMITY_SEAM(amity::network_diagnostics::poll_wifi_signal); }
