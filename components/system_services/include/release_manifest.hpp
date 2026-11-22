// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace system_services::updates {
struct ReleaseManifest {
    std::string release_id,firmware_version,image_sha256,image_elf_sha256,signing_key_id,canonical;
    std::string hardware_profile,hardware_mapping_sha256;
    uint64_t image_bytes=0;
};
// Manifest strings and keys use a closed ASCII vocabulary, and all counters
// except the small format tag are decimal strings. Sorted minimal serialization
// is therefore also RFC8785 JCS, without floating point or UTF16 ordering cases.
bool parse_release_manifest(std::string_view,ReleaseManifest&,std::string& error);
std::string compiled_hardware_profile();
std::string compiled_hardware_mapping_sha256();
} // namespace system_services::updates
