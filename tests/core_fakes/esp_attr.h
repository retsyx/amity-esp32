/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#pragma once
// Memory placement attributes. The host has one memory, so placement is not
// observable here; post-build checks of the firmware ELF cover it.
#define IRAM_ATTR
#define DRAM_ATTR
#define EXT_RAM_BSS_ATTR
#define EXT_RAM_NOINIT_ATTR
#define RTC_NOINIT_ATTR
#define NOINLINE_ATTR __attribute__((noinline))
#define FORCE_INLINE_ATTR static inline __attribute__((always_inline))
#define WORD_ALIGNED_ATTR __attribute__((aligned(4)))
#define DMA_ATTR WORD_ALIGNED_ATTR DRAM_ATTR
