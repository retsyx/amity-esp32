// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
// A firmware receive or activation that cannot finish entering maintenance
// leaves control running, as if it had never tried.
#include "firmware_update.hpp"
#include "partition_image.hpp"
#include "config_store.hpp"
#include "system_configuration.hpp"
#include "system_services.hpp"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include <cassert>
#include <cstdio>
#include <optional>
namespace u=system_services::updates;

static esp_partition_t slot{0x200000};
static std::optional<u::ReleaseManifest> staged;
static unsigned reboots=0;
const esp_partition_t* esp_ota_get_next_update_partition(const esp_partition_t*){return &slot;}
const esp_partition_t* esp_ota_get_running_partition(){return &slot;}
esp_err_t esp_ota_begin(const esp_partition_t*,size_t,esp_ota_handle_t* handle){*handle=1;return ESP_OK;}
esp_err_t esp_ota_write(esp_ota_handle_t,const void*,size_t){return ESP_OK;}
esp_err_t esp_ota_end(esp_ota_handle_t){return ESP_OK;}
esp_err_t esp_ota_abort(esp_ota_handle_t){return ESP_OK;}
esp_err_t esp_ota_set_boot_partition_skip_validate(const esp_partition_t*){return ESP_OK;}
int64_t esp_timer_get_time(){return 1000000;}
const esp_app_desc_t* esp_app_get_description(){static esp_app_desc_t app{};return &app;}
namespace config_store {
bool with_internal_flash_stack(std::function<void()> call){call();return true;}
std::string sha256_hex(std::string_view){return {};}
}
namespace system_services {void reboot_later(uint32_t){++reboots;}}
namespace system_services::updates {
bool parse_release_manifest(std::string_view,ReleaseManifest&,std::string&){return false;}
esp_err_t erase_partition_release(const esp_partition_t*){return ESP_OK;}
esp_err_t write_partition_release(const esp_partition_t*,const ReleaseManifest&,std::string_view,std::string&){return ESP_OK;}
esp_err_t read_partition_release(const esp_partition_t*,ReleaseManifest& release,std::string& signature,std::string& error){
    signature.clear();if(!staged){error="No staged image";return ESP_ERR_INVALID_STATE;}release=*staged;return ESP_OK;
}
esp_err_t read_partition_image(const esp_partition_t*,uint64_t,size_t,std::string&,std::string&){return ESP_FAIL;}
}

// The maintenance hooks main.cpp installs pause control first, then wait a
// bounded time for the CEC wires to drain.
static bool paused=false,wires_drain=false;
static unsigned ended=0;

int main(){
    u::maintenance_hooks([]{paused=true;return wires_drain?ESP_OK:ESP_ERR_TIMEOUT;},[]{paused=false;++ended;});
    u::ReleaseManifest release;release.release_id="r1";release.canonical="c1";release.image_bytes=4096;
    std::string error;
    // A browser upload or update_begin whose entry times out after pausing.
    // The upload handler then calls peer_abort, as upload_firmware's fail() does.
    assert(u::peer_begin(release,"",error)==ESP_ERR_TIMEOUT&&!error.empty());
    u::peer_abort("upload failed");assert(!paused);
    // Activation of a staged image with the same timeout.
    staged=release;paused=false;error.clear();
    assert(u::peer_install("r1",error,1000)==ESP_ERR_TIMEOUT&&!error.empty()&&reboots==0&&!paused);
    // A successful entry still holds maintenance until the receive ends.
    staged.reset();wires_drain=true;ended=0;error.clear();
    assert(u::peer_begin(release,"",error)==ESP_OK&&paused&&ended==0);
    u::peer_abort("cancel");assert(!paused&&ended==1);
    std::puts("PASS firmware maintenance: a failed entry leaves control running; a successful one holds it until the receive ends");
}
