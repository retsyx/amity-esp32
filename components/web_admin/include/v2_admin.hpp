// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "amity/system_control.hpp"
#include "esp_http_server.h"
namespace web_admin {
esp_err_t load_v2(amity::SystemControl&,bool defer_configuration=false);
bool configuration_is_passive();
// Called only after the common administration authorization boundary.
bool v2_request(httpd_req_t*, const std::string& path, esp_err_t& result);
std::string active_v2_json();
}
