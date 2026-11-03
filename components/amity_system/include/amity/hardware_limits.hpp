// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Shared by firmware and offline tooling. Fixed-format schema/menu boundaries
// are checked against this value by tests/hardware_capacity_test.py.
#define AMITY_MAX_TENTACLES 4

namespace amity {
inline constexpr unsigned max_tentacles = AMITY_MAX_TENTACLES;
inline constexpr unsigned last_tentacle_id = max_tentacles - 1;
inline constexpr unsigned max_cec_ports = 2 * max_tentacles;
}
