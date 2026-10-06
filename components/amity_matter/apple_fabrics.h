#pragma once

#include "esp_err.h"

namespace amity_matter {
// Call after ESP Matter starts, with the CHIP stack lock held.
esp_err_t start_apple_fabric_cleanup();
}
