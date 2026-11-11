/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "../core/compat.h"

struct r360_video_profile {
	u16 width;
	u16 height;
};

static const struct r360_video_profile r360_profiles[] = {
	{320, 180}, {512, 288}, {640, 360}, {960, 540}, {1280, 720}, {1920, 1080},
};
static const u8 r360_rates[] = {15, 24, 30, 60};
#define R360_DEFAULT_RATE_INDEX 2

static inline u8 r360_profile_for_source(u8 requested, u32 width, u32 height)
{
	if (requested >= ARRAY_SIZE(r360_profiles))
		requested = 0;
	while (requested && (r360_profiles[requested].width > width ||
			     r360_profiles[requested].height > height))
		requested--;
	return requested;
}
