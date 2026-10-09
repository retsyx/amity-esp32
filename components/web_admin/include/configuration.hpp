/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
#include "amity/hub.hpp"
#include <string>
namespace web_admin {
struct Configuration { std::vector<amity::Activity> activities; std::vector<amity::Quirk> quirks; };
bool parse_configuration(const std::string &json,Configuration &config,std::string &error);
}
