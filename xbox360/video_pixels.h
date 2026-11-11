/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "../core/compat.h"

struct r360_surface {
	u32 width;
	u32 height;
	u32 pitch;
	u32 x_offset;
	u32 y_offset;
	u32 layout;
	u32 swap_mode;
};

static inline u32 r360_surface_bytes(const struct r360_surface *s)
{
	if (s->width < 64 || s->width > 2048 || s->height < 64 || s->height > 2048 ||
	    s->x_offset > 4096 || s->y_offset > 4096 ||
	    s->pitch < s->x_offset + s->width || s->pitch > 4096 ||
	    s->y_offset + s->height > 4096 || s->swap_mode > 3)
		return 0;
	u32 rows = s->y_offset + s->height;
	if (s->layout == 4) {
		if (s->pitch & 31)
			return 0;
		rows = ROUND_UP(rows, 32);
	} else if (s->layout > 1) {
		return 0;
	}
	return rows * s->pitch * 4;
}

/* Xenos 32-bit tiled address formula as documented by Xenia. */
static inline u32 r360_tiled_offset(u32 x, u32 y, u32 pitch)
{
	u32 outer = ((y >> 5) * (pitch >> 5) + (x >> 5)) << 6;
	u32 inner = (((y >> 1) & 7) << 3) | (x & 7);
	u32 bytes = (outer | inner) << 2;
	u32 bank = (y >> 4) & 1;
	u32 pipe = ((x >> 3) & 3) ^ (((y >> 3) & 1) << 1);
	return ((y & 1) << 4) | (pipe << 6) | (bank << 11) | (bytes & 15) |
	       (((bytes >> 4) & 1) << 5) | (((bytes >> 5) & 7) << 8) | ((bytes >> 8) << 12);
}

/* Split the tiled address across columns and rows. Row bit 7 is the pipe
 * permutation, so XOR that bit after adding the other row fields. */
static inline u32 r360_tiled_sample(u32 column, u32 row)
{
	return (column + (row & ~128u)) ^ (row & 128u);
}

static inline u32 r360_surface_offset(const struct r360_surface *s, u32 x, u32 y)
{
	x += s->x_offset;
	y += s->y_offset;
	u32 byte = s->layout == 4 ? r360_tiled_offset(x, y, s->pitch) : (y * s->pitch + x) * 4;
	return s->swap_mode == 3 ? byte ^ 4 : byte;
}

/* Memory byte zero occupies the low bits, independent of host byte order. */
static inline u16 r360_surface_rgb565(u32 raw, u32 format, u32 swap_mode)
{
	if (swap_mode == 1)
		raw = ((raw & 0xFF00FF00u) >> 8) | ((raw & 0x00FF00FFu) << 8);
	else if (swap_mode >= 2)
		raw = __builtin_bswap32(raw);
	if (format == 0)
		return (u16)(((raw >> 8) & 0xF800) | ((raw >> 5) & 0x07E0) | ((raw >> 3) & 0x001F));
	return (u16)(((raw >> 14) & 0xF800) | ((raw >> 9) & 0x07E0) | ((raw >> 5) & 0x001F));
}

/* The big-endian load of the common BGRX scanout can be packed directly. */
static inline u16 r360_bgrx_word_rgb565(u32 word)
{
	return (u16)((word & 0xF800u) | ((word >> 13) & 0x07E0u) | (word >> 27));
}

/* Average each RGB565 component independently, without carry between colors. */
static inline u16 r360_rgb565_average(u16 a, u16 b)
{
	return (u16)((a & b) + (((a ^ b) & 0xF7DEu) >> 1));
}
