/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
#include "amity/system_control.hpp"
#include "configuration.hpp"
#include <string>
namespace web_admin {
esp_err_t load(amity::SystemControl &hub,bool defer_configuration=false);
esp_err_t finish_migration_bridge();
bool member_recovery_only();
esp_err_t start(amity::SystemControl &hub,const char *hostname,const char *initial_auth);
// Permanently revoke user-facing HTTP for this boot, preserving credentials.
esp_err_t stop();
}
