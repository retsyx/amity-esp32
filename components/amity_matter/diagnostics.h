#pragma once
#include "esp_err.h"
#include <esp_matter.h>

namespace amity_diagnostics {
esp_err_t configure(esp_matter::node_t *node);
}
