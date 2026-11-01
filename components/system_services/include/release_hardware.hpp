// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "amity/hardware_limits.hpp"
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#if !CONFIG_AMITY_BOARD_LEGACY_1T && !CONFIG_AMITY_BOARD_DEVELOPMENT_4T
#error "Select an explicit Amity board profile before building a release"
#endif
#endif
#if CONFIG_AMITY_BOARD_LEGACY_1T && CONFIG_AMITY_BOARD_DEVELOPMENT_4T
#error "Only one Amity board profile may be selected"
#endif
#define AMITY_RELEASE_TEXT_INNER(x) #x
#define AMITY_RELEASE_TEXT(x) AMITY_RELEASE_TEXT_INNER(x)
namespace system_services::updates {
inline const char release_hardware_descriptor[] =
    "AMITY_HARDWARE_BEGIN_V1\n"
#if CONFIG_AMITY_BOARD_DEVELOPMENT_4T
    "profile=esp32s3-development-" AMITY_RELEASE_TEXT(AMITY_MAX_TENTACLES) "t-v1\nrevision=1\ncapacity=" AMITY_RELEASE_TEXT(AMITY_MAX_TENTACLES) "\n"
    "reset=" AMITY_RELEASE_TEXT(CONFIG_AMITY_BOARD_RESET_GPIO) "\nstatus=-1\n"
    "slot0=" AMITY_RELEASE_TEXT(CONFIG_AMITY_TENTACLE0_FRONT_GPIO) "," AMITY_RELEASE_TEXT(CONFIG_AMITY_TENTACLE0_BACK_GPIO) "\n"
    "slot1=" AMITY_RELEASE_TEXT(CONFIG_AMITY_TENTACLE1_FRONT_GPIO) "," AMITY_RELEASE_TEXT(CONFIG_AMITY_TENTACLE1_BACK_GPIO) "\n"
    "slot2=" AMITY_RELEASE_TEXT(CONFIG_AMITY_TENTACLE2_FRONT_GPIO) "," AMITY_RELEASE_TEXT(CONFIG_AMITY_TENTACLE2_BACK_GPIO) "\n"
    "slot3=" AMITY_RELEASE_TEXT(CONFIG_AMITY_TENTACLE3_FRONT_GPIO) "," AMITY_RELEASE_TEXT(CONFIG_AMITY_TENTACLE3_BACK_GPIO) "\n"
#else
    "profile=esp32s3-legacy-1t-v1\nrevision=1\ncapacity=1\nreset=4\nstatus=-1\nslot0=1,2\n"
#endif
    "AMITY_HARDWARE_END_V1\n";
} // namespace system_services::updates
