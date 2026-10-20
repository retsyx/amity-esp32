// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <string_view>
namespace amity::consortium {
// The configured DHCP hostname, without a domain suffix. Matches the local
// administration/provisioning policy; this metadata never grants authority.
inline bool valid_hostname(std::string_view name) {
    const auto alnum=[](char c){return (c>='a'&&c<='z')||(c>='0'&&c<='9');};
    return !name.empty()&&name.size()<=32&&alnum(name.front())&&alnum(name.back())&&
        std::all_of(name.begin(),name.end(),[&](char c){return alnum(c)||c=='-';});
}
}
