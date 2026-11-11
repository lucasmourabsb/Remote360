/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "../core/compat.h"

typedef bool (*r360_jpeg_progress)(void *context);

/* Native-endian RGB565, even dimensions, caller-provided bounded output.
 * Positive result: JPEG bytes. Zero: overflow or encoder error. -1: cancelled. */
int r360_encode_jpeg(const u16 *pixels, u32 width, u32 height,
		    u8 *output, u32 capacity, bool low_quality,
		    r360_jpeg_progress progress, void *context);
