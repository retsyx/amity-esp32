#pragma once
#include "esp_err.h"
#include "nvs.h"
#include <string>
namespace config_store {
// Dedicated partition, independent of Matter's network/fabric storage. Never erase on error.
esp_err_t init();
esp_err_t read(const char *key, std::string &value);
esp_err_t write(const char *key, const std::string &value);
}
