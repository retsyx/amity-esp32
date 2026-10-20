// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string_view>
namespace config_store {
// Fixed v1 domain separation for provisioned XTS root material. The default
// SDK NVS retains its original key; other NVS partitions get independent keys.
bool derive_production_storage_key(std::string_view partition,const uint8_t root[64],uint8_t output[64]);
bool production_storage_self_test();
}
