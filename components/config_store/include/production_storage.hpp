// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
namespace config_store {
// Fixed v1 domain separation for provisioned XTS root material. The default
// SDK NVS retains its original key; amity_cfg gets an independent key.
bool derive_production_storage_key(const uint8_t root[64],uint8_t output[64]);
}
