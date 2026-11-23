/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// Scenario controls for the OpenSSL-backed PSA fake.
#include <cstddef>

namespace amity_fake {

// The next count psa_generate_key calls fail with PSA_ERROR_INSUFFICIENT_MEMORY.
void fail_key_generations(unsigned count);
// Keys that exist, and derivations set up and not yet aborted.
std::size_t psa_keys();
std::size_t psa_derivations();

}  // namespace amity_fake
