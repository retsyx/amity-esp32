#pragma once
#include <cstdint>
#include "sdkconfig.h"
constexpr uint8_t ESP_IMAGE_HEADER_MAGIC=0xE9;
struct esp_image_header_t {uint8_t magic;uint8_t segment_count;uint8_t spi_mode;uint8_t spi_speed_size;uint32_t entry_addr;uint8_t wp_pin;uint8_t spi_pin_drv[3];uint16_t chip_id;uint8_t min_chip_rev;uint16_t min_chip_rev_full;uint16_t max_chip_rev_full;uint8_t reserved[4];uint8_t hash_appended;};
struct esp_image_segment_header_t {uint32_t load_addr;uint32_t data_len;};
