/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "../core/compat.h"

/* Opaque BC1. Choose endpoints from the lightest/darkest samples, then assign
 * the two interpolated colors by projection onto the endpoint color line. */
static inline void r360_bc1_encode_block(u8 *out, const u16 *pixels, u32 stride)
{
	u16 colors[16];
	u16 lo = 0, hi = 0;
	int min_luma = 32768, max_luma = -1;
	for (u32 y = 0; y < 4; y++) {
		for (u32 x = 0; x < 4; x++) {
			u16 v = pixels[y * stride + x];
			int luma = 3 * (v >> 11) + 3 * ((v >> 5) & 63) + (v & 31);
			colors[y * 4 + x] = v;
			if (luma < min_luma) { min_luma = luma; lo = v; }
			if (luma > max_luma) { max_luma = luma; hi = v; }
		}
	}
	u16 c0 = hi > lo ? hi : lo;
	u16 c1 = hi > lo ? lo : hi;
	put_le16(out, c0);
	put_le16(out + 2, c1);
	if (c0 == c1) {
		put_le32(out + 4, 0);
		return;
	}
	int r1 = c1 >> 11, g1 = (c1 >> 5) & 63, b1 = c1 & 31;
	int dr = (c0 >> 11) - r1, dg = ((c0 >> 5) & 63) - g1, db = (c0 & 31) - b1;
	int extent = 4 * dr * dr + dg * dg + 4 * db * db;
	int middle = extent * 3, high = extent * 5;
	u32 packed = 0;
	for (u32 i = 0; i < 16; i++) {
		int v = colors[i];
		int scaled = 6 * (4 * ((v >> 11) - r1) * dr +
				  (((v >> 5) & 63) - g1) * dg + 4 * ((v & 31) - b1) * db);
		u32 index = scaled < extent ? 1u : scaled < middle ? 3u : scaled < high ? 2u : 0u;
		packed |= index << (i * 2);
	}
	put_le32(out + 4, packed);
}

/* Use luminance for endpoint and index selection. This keeps a standard BC1
 * payload and avoids three per-pixel multiplications in the index pass. */
static inline void r360_bc1_fast_block(u8 *out, const u16 *pixels, u32 stride)
{
	u16 luminance[16];
	u32 low_key = ~0u, high_key = 0;
	for (u32 y = 0; y < 4; y++) {
		for (u32 x = 0; x < 4; x++) {
			u16 color = pixels[y * stride + x];
			u16 light = 3 * (color >> 11) + 3 * ((color >> 5) & 63) + (color & 31);
			luminance[y * 4 + x] = light;
			/* A single unsigned comparison selects luminance first and
			 * keeps the same color tie-break used by the scalar encoder. */
			u32 key = ((u32)light << 16) | color;
			low_key = low_key < key ? low_key : key;
			high_key = high_key > key ? high_key : key;
		}
	}
	u16 low = (u16)low_key, high = (u16)high_key;
	u32 minimum = low_key >> 16, maximum = high_key >> 16;
	bool reverse = high < low;
	put_le16(out, reverse ? low : high);
	put_le16(out + 2, reverse ? high : low);
	u32 indices = 0;
	if (low != high) {
		/* Preserve both endpoint colors even in a block of equal luminance. */
		if (minimum == maximum) {
			for (u32 y = 0; y < 4; y++)
				for (u32 x = 0; x < 4; x++)
					luminance[4 * y + x] = pixels[y * stride + x];
			minimum = low;
			maximum = high;
		}
		u32 first = 5 * minimum + maximum;
		u32 middle = 3 * (minimum + maximum);
		u32 last = minimum + 5 * maximum;
		for (u32 i = 0; i < 16; i++) {
			u32 score = 6 * luminance[i];
			u32 index = (score < middle) | (((score < last) ^ (score < first)) << 1);
			indices |= (index ^ reverse) << (2 * i);
		}
	}
	put_le32(out + 4, indices);
}
