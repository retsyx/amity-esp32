// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>

namespace amity::consortium {
// The original main admits at most three headless secondary Amities.
inline constexpr std::size_t maximum_roster_peers=3;
// One bounded TLS handshake can identify a rebooted member while every roster
// slot still has its old connection. It grants no second operational owner.
inline constexpr std::size_t maximum_pending_connections=1;
inline constexpr std::size_t maximum_connection_slots=maximum_roster_peers+maximum_pending_connections;
}
