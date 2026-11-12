/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "../core/compat.h"

/* Match the existing tiled reader's 32-bit accesses to physical memory.
 * Copy the native byte order; the conversion stage handles the swap mode. */
static inline void r360_copy_scanout_block(u8 *dst, const volatile u8 *src, u32 bytes)
{
	if (!((uintptr_t)src & 3u)) {
		for (; bytes >= 4; dst += 4, src += 4, bytes -= 4) {
			u32 word = *(const volatile u32 *)src;
			memcpy(dst, &word, sizeof(word));
		}
	}
	for (u32 i = 0; i < bytes; i++)
		dst[i] = src[i];
}

static inline bool r360_scanout_matches(const volatile u8 *live, const u8 *copy,
					const u32 *offsets, const u32 *anchors, u32 count)
{
	for (u32 i = 0; i < count; i++) {
		u32 pixel;
		memcpy(&pixel, copy + offsets[i], sizeof(pixel));
		if (pixel != anchors[i] || *(const volatile u32 *)(live + offsets[i]) != anchors[i])
			return false;
	}
	return true;
}
