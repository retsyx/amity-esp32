// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <string>
#include <string_view>
namespace amity::consortium::platform {
std::string hex(std::string_view);
bool unhex(std::string_view,std::string &,size_t limit);
}
