#pragma once
#include "amity/hub.hpp"
#include <string>
namespace web_admin {
struct Configuration { std::vector<amity::Activity> activities; std::vector<amity::Quirk> quirks; };
bool parse_configuration(const std::string &json,Configuration &config,std::string &error);
}
