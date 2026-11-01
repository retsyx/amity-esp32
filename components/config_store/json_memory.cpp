// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#include "config_store.hpp"
#if defined(ESP_PLATFORM)
#include "sdkconfig.h"
#if CONFIG_SPIRAM
#include "cJSON.h"
#include "esp_heap_caps.h"
namespace config_store {
namespace {
void* allocate_json(size_t bytes){
    // JSON trees and their short duplicated strings are ordinary task data.
    // Once PSRAM owns them, exhaustion must not consume the internal/DMA pool
    // needed for CEC timing and authenticated transport progress.
    return heap_caps_malloc(bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
}
}
void initialize_json_memory(){
    // Called before configuration parsing and application worker creation.
    // cJSON hooks are global and must remain immutable during normal work.
    static bool initialized=false;
    if(initialized)return;
    initialized=true;
    if(!heap_caps_get_total_size(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT))return;
    cJSON_Hooks hooks{allocate_json,heap_caps_free};
    // ESP-IDF's default free already delegates to heap_caps_free, so values
    // allocated before this startup hook remain safe to release afterward.
    cJSON_InitHooks(&hooks);
}
}
#else
namespace config_store {void initialize_json_memory(){}}
#endif
#else
namespace config_store {void initialize_json_memory(){}}
#endif
