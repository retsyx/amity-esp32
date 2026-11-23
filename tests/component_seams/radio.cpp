/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// Seams for components/radio_control/radio_control.cpp.
#include "radio_control.hpp"
#include "seams.hpp"

esp_err_t amity::radio::start() { return AMITY_SEAM(amity::radio::start); }
bool amity::radio::matter_will_advertise() { return AMITY_SEAM(amity::radio::matter_will_advertise); }
esp_err_t amity::radio::reserve_consortium(bool inviter, uint64_t &generation) {
    return AMITY_SEAM(amity::radio::reserve_consortium, inviter, generation);
}
void amity::radio::release(uint64_t generation) { AMITY_SEAM(amity::radio::release, generation); }
