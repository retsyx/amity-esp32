// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "release_manifest.hpp"
#include "esp_partition.h"
#include <cstddef>
#include <string>
#include <string_view>

namespace system_services::updates {
// A release travels with its OTA partition, including after that partition
// becomes the running image. The native application bytes remain unchanged.
inline constexpr size_t partition_release_bytes=0x3000;
inline constexpr size_t partition_release_header_bytes=64;
inline constexpr char partition_release_magic[16]="AMITY-IMAGE-V1\n";
esp_err_t validate_release(std::string_view manifest,std::string_view signature_hex,
                           ReleaseManifest& release,std::string& error);
esp_err_t erase_partition_release(const esp_partition_t* partition);
esp_err_t write_partition_release(const esp_partition_t* partition,const ReleaseManifest& release,
                                 std::string_view signature_hex,std::string& error);
esp_err_t read_partition_release(const esp_partition_t* partition,ReleaseManifest& release,
                                std::string& signature_hex,std::string& error);
esp_err_t read_partition_image(const esp_partition_t* partition,uint64_t offset,size_t count,
                              std::string& bytes,std::string& error);
}
