#pragma once
#include "nvs.h"
#include <cstddef>
#include <initializer_list>
namespace config_store::detail {
// Blob data uses one32-byte entry per32 bytes plus chunk headers/index. Leave
// journal decision metadata room when admitting ordinary writes. IDF already
// removes its GC reserve page from available_entries (not free_entries).
inline bool blob_space(size_t bytes, bool decision = false) {
    nvs_stats_t stats{};
    if (nvs_get_stats("amity_cfg", &stats) != ESP_OK) return false;
    const size_t required = (bytes + 31) / 32 + 2 * ((bytes + 3999) / 4000) + 4;
    return required <= stats.available_entries && (decision || stats.available_entries - required >= 32);
}
inline bool decision_key(const char* key) {
    if (!key) return false;
    for (const auto* prefix : {"cm", "smark", "imark"}) {
        const char* value=key;const char* expected=prefix;
        while(*expected&&*value==*expected){++value;++expected;}
        if(!*expected)return true;
    }
    return false;
}
}
