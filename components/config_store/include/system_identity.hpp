#pragma once
#include "esp_err.h"
#include "amity/system/model.hpp"
namespace config_store {
// Origin survives reset. Singleton identity is reset-owned and confers no
// membership/enrollment authority. Must run before any control service starts.
esp_err_t load_system_identity(amity::system::OctopusId &, amity::system::ConsortiumId &);
}
