// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "release_manifest.hpp"
#include "esp_err.h"
#include <functional>
namespace system_services::updates {
void maintenance_hooks(std::function<esp_err_t()> begin,std::function<void()> end);
// Checks an arriving release once: its manifest, the image capacity and the
// publisher signature. The slot trailer written at staging then vouches for it.
esp_err_t validate_release(std::string_view manifest,std::string_view signature_hex,ReleaseManifest& release,std::string& error);
// Control pauses while an image is received and from boot selection to restart.
esp_err_t enter_maintenance(std::string& error);
void leave_maintenance();
// Called only by the authenticated current-coordinator transport adapter.
// Chunks are exact ordered 2KiB blocks (last may be shorter); repeated or
// out-of-order blocks are rejected. Incomplete streams restart at zero; a fully
// staged partition survives a lost connection or reboot without boot selection.
esp_err_t peer_begin(const ReleaseManifest& validated,std::string_view signature_hex,std::string& error);
esp_err_t peer_chunk(std::string_view release_id,uint64_t offset,std::string_view bytes,std::string& error);
esp_err_t peer_stage(std::string_view release_id,std::string& error);
esp_err_t peer_install(std::string_view release_id,std::string& error,uint32_t reboot_delay_ms=1000);
esp_err_t partition_release(bool running,ReleaseManifest& release,std::string& signature_hex,std::string& error);
esp_err_t read_running_image(uint64_t offset,size_t count,std::string& bytes,std::string& error);
esp_err_t read_staged_image(uint64_t offset,size_t count,std::string& bytes,std::string& error);
void peer_abort(const std::string& error);
void poll_peer_timeout();
} // namespace system_services::updates
