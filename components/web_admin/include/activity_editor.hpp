#pragma once
#include "amity/system_control.hpp"
#include "esp_http_server.h"
#include <string>
namespace web_admin {
// Call after ordinary controller administration authentication, before v2_request.
bool activity_editor_request(amity::SystemControl&,httpd_req_t*,const std::string& path,esp_err_t& result);
}
