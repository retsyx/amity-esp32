#pragma once
#include <cstdint>
struct esp_app_desc_t { char version[32];uint8_t app_elf_sha256[32]; };
const esp_app_desc_t* esp_app_get_description();
