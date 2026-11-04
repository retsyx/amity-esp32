// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "release_manifest.hpp"
#include "esp_err.h"
#include <functional>
#include <optional>
namespace system_services::updates {
void maintenance_hooks(std::function<esp_err_t()> begin,std::function<void()> end);
esp_err_t stage_release(std::string_view canonical_manifest,std::string_view signature_hex,std::string& error);
// Atomic NVS report. An interrupted stream is never resumed from browser state.
std::string status_json();
std::optional<ReleaseManifest> staged_release();
bool installation_active();
esp_err_t begin_install(std::string& error);
void install_progress(uint64_t bytes);
esp_err_t install_selected();
esp_err_t install_staged();
void install_failed(const std::string& error);
void local_boot_healthy();
// Called only by the authenticated current-coordinator transport adapter.
// Chunks are exact ordered 2KiB blocks (last may be shorter); duplicate last
// blocks require the same bytes. Incomplete streams restart at zero; a fully
// staged partition survives a lost connection or reboot without boot selection.
esp_err_t peer_begin(std::string_view manifest,std::string_view signature_hex,std::string& error);
esp_err_t peer_chunk(std::string_view release_id,uint64_t offset,std::string_view bytes,std::string& error);
esp_err_t peer_stage(std::string_view release_id,std::string& error);
esp_err_t peer_install(std::string_view release_id,std::string& error,uint32_t reboot_delay_ms=1000);
esp_err_t partition_release(bool running,ReleaseManifest& release,std::string& signature_hex,std::string& error);
esp_err_t read_running_image(uint64_t offset,size_t count,std::string& bytes,std::string& error);
esp_err_t read_staged_image(uint64_t offset,size_t count,std::string& bytes,std::string& error);
void peer_abort(const std::string& error);
void poll_peer_timeout();
} // namespace system_services::updates
