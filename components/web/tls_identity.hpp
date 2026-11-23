/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
#include "esp_err.h"
#include <string>
namespace web {
// Reads the saved HTTPS certificate and private key, or creates and saves a
// self-signed pair for hostname when none is saved or the saved certificate
// is unusable.
esp_err_t tls_identity(const std::string &hostname,std::string &certificate,std::string &private_key);
}
