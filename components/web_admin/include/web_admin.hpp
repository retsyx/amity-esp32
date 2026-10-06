#pragma once
#include "amity/hub_service.hpp"
#include "configuration.hpp"
#include <string>
namespace web_admin {
esp_err_t load(amity::HubService &hub);
esp_err_t start(amity::HubService &hub,const char *hostname,const char *initial_auth);
}
