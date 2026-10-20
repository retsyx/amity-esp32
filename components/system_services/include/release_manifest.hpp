// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace system_services::updates {
struct ReleaseManifest {
    std::string release_id,firmware_version,image_sha256,image_elf_sha256,signing_key_id;
    std::string source_commit,source_tree_sha256,security_profile,canonical;
    std::string hardware_profile,hardware_mapping_sha256;
    uint64_t image_bytes=0,slot_bytes=0,storage_write=0;
    uint64_t config_read_min=0,config_read_max=0,config_write=0;
    uint64_t storage_read_min=0,storage_read_max=0,rollback_readable_max=0;
    uint32_t required_capabilities=0;
};
// Manifest strings and keys use a closed ASCII vocabulary, and all counters
// except the small format tag are decimal strings. Sorted minimal serialization
// is therefore also RFC8785 JCS, without floating point or UTF16 ordering cases.
bool parse_release_manifest(std::string_view,ReleaseManifest&,std::string& error);
std::string compiled_hardware_profile();
std::string compiled_hardware_mapping_sha256();
bool compatible_release(const ReleaseManifest&,uint64_t actual_slot_bytes,
    uint64_t installed_storage,uint64_t rollback_readable_max,uint32_t capabilities,
    bool production_target,std::string& error);
} // namespace system_services::updates
