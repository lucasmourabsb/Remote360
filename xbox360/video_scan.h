/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "video_bc1.h"
#include "video_pixels.h"

#define R360_FAST_MAX_WIDTH 640

/* The caller validates the framebuffer and keeps it and the map alive until
 * both slices finish. Slices share input and write disjoint BC1 block rows. */
struct r360_scan_slice {
	const volatile u8 *source;
	const u32 *columns, *rows;
	const u16 *column_end;
	u8 *output;
	u32 width, height, first, end;
	u32 format, swap_mode;
	bool reconstruct_rows;
};

typedef bool (*r360_scan_progress)(void *context);

static inline u16 r360_scan_pixel(const volatile u8 *source, u32 offset, u32 format, u32 swap_mode)
{
	u32 word = *(const volatile u32 *)(source + offset);
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
	if (format == 0 && swap_mode == 0)
		return r360_bgrx_word_rgb565(word);
	word = __builtin_bswap32(word);
#endif
	return r360_surface_rgb565(word, format, swap_mode);
}

/* Even x in a native-width, unswapped BGRX surface maps to an aligned pair.
 * Read both pixels with one bus access on the Xenon instead of two lwz. */
static inline void r360_scan_bgrx_pair(u16 *output, const volatile u8 *source)
{
#if defined(__powerpc__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
	u32 second, first;
	__asm__ __volatile__("ld %0,0(%2)\n\tsrdi %1,%0,32"
			     : "=&r"(second), "=&r"(first) : "b"(source) : "memory");
	output[0] = r360_bgrx_word_rgb565(first);
	output[1] = r360_bgrx_word_rgb565(second);
#else
	output[0] = r360_scan_pixel(source, 0, 0, 0);
	output[1] = r360_scan_pixel(source, 4, 0, 0);
#endif
}

struct r360_jpeg_capture {
	const volatile u8 *source;
	const u32 *columns, *rows;
	u16 *pixels;
	u32 width, first, end, format, swap_mode;
	bool native_bgrx;
};

static inline bool r360_copy_jpeg_rows(const struct r360_jpeg_capture *slice,
				       r360_scan_progress progress, void *context)
{
	for (u32 y = slice->first; y < slice->end; y++) {
		if (!(y & 3) && progress && !progress(context))
			return false;
		u16 *out = slice->pixels + y * slice->width;
		u32 row = slice->rows[y];
		if (slice->native_bgrx) {
			for (u32 x = 0; x < slice->width; x += 2) {
				u32 offset = r360_tiled_sample(slice->columns[x], row);
				r360_scan_bgrx_pair(out + x, slice->source + offset);
			}
		} else {
			for (u32 x = 0; x < slice->width; x++) {
				u32 offset = r360_tiled_sample(slice->columns[x], row);
				out[x] = r360_scan_pixel(slice->source, offset, slice->format, slice->swap_mode);
			}
		}
	}
	return true;
}

/* Process four output rows at a time. At the highest requested rate the
 * fifth row is a lookahead and is carried into the next band. Keeping that
 * row across the split makes reconstruction independent of worker count. */
static inline bool r360_scan_bc1(const struct r360_scan_slice *slice,
				 r360_scan_progress progress, void *context)
{
	u16 pixels[5 * R360_FAST_MAX_WIDTH];
	u32 width = slice->width;
	for (u32 y = slice->first; y < slice->end; y += 4) {
		if (progress && !progress(context))
			return false;
		bool reuse_row = slice->reconstruct_rows && y != slice->first;
		if (reuse_row)
			memcpy(pixels, pixels + 4 * width, width * sizeof(*pixels));
		u32 step = slice->reconstruct_rows ? 2 : 1;
		u32 count = slice->reconstruct_rows && y + 4 < slice->height ? 5 : 4;
		for (u32 x = 0; x < width; x = slice->column_end[x]) {
			u32 right = slice->column_end[x];
			for (u32 row = reuse_row ? 2 : 0; row < count; row += step) {
				u32 mapped_row = slice->rows[y + row];
				u16 *dst = pixels + row * width;
				for (u32 col = x; col < right; col++) {
					u32 offset = r360_tiled_sample(slice->columns[col], mapped_row);
					dst[col] = r360_scan_pixel(slice->source, offset, slice->format, slice->swap_mode);
				}
			}
		}
		if (slice->reconstruct_rows) {
			const u16 *below = pixels + (count == 5 ? 4 : 2) * width;
			for (u32 x = 0; x < width; x++) {
				pixels[width + x] = r360_rgb565_average(pixels[x], pixels[2 * width + x]);
				pixels[3 * width + x] = r360_rgb565_average(pixels[2 * width + x], below[x]);
			}
		}
		u8 *out = slice->output + y * width / 2;
		for (u32 x = 0; x < width; x += 4, out += 8)
			r360_bc1_fast_block(out, pixels + x, width);
	}
	return true;
}
