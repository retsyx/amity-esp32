/* Copyright 2026. GPL-3.0-or-later. */
#pragma once
#include "cec/board_profile.hpp"
#include "sdkconfig.h"

// Existing Amity board2 values remain the default firmware profile.
#define AMITY_CEC_FRONT_GPIO 1
#define AMITY_CEC_BACK_GPIO 2
#define AMITY_FW_RESET_GPIO 4

namespace amity {
#if CONFIG_AMITY_BOARD_DEVELOPMENT_5T
inline constexpr cec::BoardProfile board_profile{
    cec::BoardProfileId::Development5Tentacle, 1, 5,
    {
        {0, cec::Side::Front, CONFIG_AMITY_TENTACLE0_FRONT_GPIO},
        {0, cec::Side::Back, CONFIG_AMITY_TENTACLE0_BACK_GPIO},
        {1, cec::Side::Front, CONFIG_AMITY_TENTACLE1_FRONT_GPIO},
        {1, cec::Side::Back, CONFIG_AMITY_TENTACLE1_BACK_GPIO},
        {2, cec::Side::Front, CONFIG_AMITY_TENTACLE2_FRONT_GPIO},
        {2, cec::Side::Back, CONFIG_AMITY_TENTACLE2_BACK_GPIO},
        {3, cec::Side::Front, CONFIG_AMITY_TENTACLE3_FRONT_GPIO},
        {3, cec::Side::Back, CONFIG_AMITY_TENTACLE3_BACK_GPIO},
        {4, cec::Side::Front, CONFIG_AMITY_TENTACLE4_FRONT_GPIO},
        {4, cec::Side::Back, CONFIG_AMITY_TENTACLE4_BACK_GPIO},
    }, CONFIG_AMITY_BOARD_RESET_GPIO, -1,
    {
        {CONFIG_AMITY_TENTACLE0_PRESENCE_GPIO,
#ifdef CONFIG_AMITY_TENTACLE0_PRESENT_HIGH
            true,
#else
            false,
#endif
#ifdef CONFIG_AMITY_TENTACLE0_PRESENCE_PULLUP
            true,
#else
            false,
#endif
#ifdef CONFIG_AMITY_TENTACLE0_PRESENCE_PULLDOWN
            true},
#else
            false},
#endif
        {CONFIG_AMITY_TENTACLE1_PRESENCE_GPIO,
#ifdef CONFIG_AMITY_TENTACLE1_PRESENT_HIGH
            true,
#else
            false,
#endif
#ifdef CONFIG_AMITY_TENTACLE1_PRESENCE_PULLUP
            true,
#else
            false,
#endif
#ifdef CONFIG_AMITY_TENTACLE1_PRESENCE_PULLDOWN
            true},
#else
            false},
#endif
        {CONFIG_AMITY_TENTACLE2_PRESENCE_GPIO,
#ifdef CONFIG_AMITY_TENTACLE2_PRESENT_HIGH
            true,
#else
            false,
#endif
#ifdef CONFIG_AMITY_TENTACLE2_PRESENCE_PULLUP
            true,
#else
            false,
#endif
#ifdef CONFIG_AMITY_TENTACLE2_PRESENCE_PULLDOWN
            true},
#else
            false},
#endif
        {CONFIG_AMITY_TENTACLE3_PRESENCE_GPIO,
#ifdef CONFIG_AMITY_TENTACLE3_PRESENT_HIGH
            true,
#else
            false,
#endif
#ifdef CONFIG_AMITY_TENTACLE3_PRESENCE_PULLUP
            true,
#else
            false,
#endif
#ifdef CONFIG_AMITY_TENTACLE3_PRESENCE_PULLDOWN
            true},
#else
            false},
#endif
        {CONFIG_AMITY_TENTACLE4_PRESENCE_GPIO,
#ifdef CONFIG_AMITY_TENTACLE4_PRESENT_HIGH
            true,
#else
            false,
#endif
#ifdef CONFIG_AMITY_TENTACLE4_PRESENCE_PULLUP
            true,
#else
            false,
#endif
#ifdef CONFIG_AMITY_TENTACLE4_PRESENCE_PULLDOWN
            true},
#else
            false},
#endif
    }
};
#else
inline constexpr auto board_profile = cec::legacy_board2_profile;
static_assert(board_profile.tentacle_count == 1);
static_assert(board_profile.ports[0].gpio == AMITY_CEC_FRONT_GPIO);
static_assert(board_profile.ports[1].gpio == AMITY_CEC_BACK_GPIO);
static_assert(board_profile.reset_button == AMITY_FW_RESET_GPIO);
#endif
}
