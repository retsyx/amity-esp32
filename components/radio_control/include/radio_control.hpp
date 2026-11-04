// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "esp_err.h"
#include "amity/consortium/radio.hpp"
namespace amity::radio {
esp_err_t start();
// Synchronous admission called by the wrapped SDK method before any Matter
// advertisement state is changed, including remotely opened windows.
bool matter_will_advertise();
consortium::RadioReservation status();
esp_err_t reserve_consortium(bool inviter, uint64_t& generation);
void release(uint64_t generation);
}
