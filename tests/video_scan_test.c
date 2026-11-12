/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <pthread.h>
#include <stdlib.h>

#include "../protocol/remote360_protocol.h"
#include "../xbox360/video_jpeg.h"
#include "../xbox360/video_scan.h"
#include "../xbox360/video_snapshot.h"

static u32 sample_bgrx(u32 x, u32 y)
{
	return ((x + 3 * y) & 255) | ((x ^ y) & 255) << 8 | ((2 * x - y) & 255) << 16;
}

static void *encode_scan(void *context)
{
	assert(r360_scan_bc1(context, 0, 0));
	return NULL;
}

static void check_scan(u32 width, u32 height, u32 x_offset, u32 y_offset,
		       u32 format, u32 swap_mode, bool reconstruct)
{
	struct r360_surface surface = {1280, 720, 1312, x_offset, y_offset, 4, swap_mode};
	u8 *source = calloc(1, r360_surface_bytes(&surface));
	u16 *pixels = malloc(width * height * sizeof(*pixels));
	u8 *reference = malloc(width * height / 2);
	u8 *actual = malloc(width * height / 2);
	assert(source && pixels && reference && actual);
	for (u32 y = 0; y < surface.height; y++)
		for (u32 x = 0; x < surface.width; x++)
			put_le32(source + r360_surface_offset(&surface, x, y), sample_bgrx(x, y));

	u32 columns[1280], rows[720];
	u16 column_end[1280];
	for (u32 x = 0; x < width; x++)
		columns[x] = r360_tiled_offset(x * surface.width / width + surface.x_offset, 0, surface.pitch) ^
			     (swap_mode == 3 ? 4 : 0);
	for (u32 y = 0; y < height; y++)
		rows[y] = r360_tiled_offset(0, y * surface.height / height + surface.y_offset, surface.pitch);
	u32 end = width;
	for (u32 x = width; x-- > 0;) {
		if (x + 1 < width && (columns[x] >> 12) != (columns[x + 1] >> 12)) end = x + 1;
		column_end[x] = (u16)end;
	}
	for (u32 y = 0; y < height; y++)
		for (u32 x = 0; x < width; x++) {
			u32 offset = r360_surface_offset(&surface, x * surface.width / width, y * surface.height / height);
			assert(r360_tiled_sample(columns[x], rows[y]) == offset);
			pixels[y * width + x] = r360_surface_rgb565(get_le32(source + offset), format, swap_mode);
		}
	if (width == surface.width) {
		u16 *paired = malloc(width * height * sizeof(*paired));
		u32 source_bytes = r360_surface_bytes(&surface);
		u8 *snapshot = malloc(source_bytes);
		assert(paired && snapshot && format == 0 && swap_mode == 0 && !(surface.x_offset & 1));
		for (u32 y = 0; y < height; y++)
			for (u32 x = 0; x < width; x += 2) {
				u32 address = r360_tiled_sample(columns[x], rows[y]);
				assert(address % 8 == 0 && r360_tiled_sample(columns[x + 1], rows[y]) == address + 4);
			}
		u32 offsets[] = {r360_surface_offset(&surface, 15, 28), r360_surface_offset(&surface, 1247, 691)};
		u32 anchors[ARRAY_SIZE(offsets)];
		for (u32 i = 0; i < ARRAY_SIZE(offsets); i++)
			memcpy(anchors + i, source + offsets[i], sizeof(anchors[i]));
		for (u32 offset = 0; offset < source_bytes; offset += 32768)
			r360_copy_scanout_block(snapshot + offset, source + offset, MIN(32768u, source_bytes - offset));
		assert(r360_scanout_matches(source, snapshot, offsets, anchors, ARRAY_SIZE(offsets)));
		/* The console can recycle its front buffer while we convert the private copy. */
		source[offsets[0]] ^= 0x80;
		assert(!r360_scanout_matches(source, snapshot, offsets, anchors, ARRAY_SIZE(offsets)));
		struct r360_jpeg_capture copy = {
			.source = snapshot, .columns = columns, .rows = rows, .pixels = paired,
			.width = width, .end = height, .native_bgrx = true,
		};
		assert(r360_copy_jpeg_rows(&copy, 0, 0));
		assert(!memcmp(pixels, paired, width * height * sizeof(*paired)));
		free(snapshot);
		free(paired);
	} else {
		if (reconstruct)
			for (u32 y = 1; y < height; y += 2)
				for (u32 x = 0; x < width; x++)
					pixels[y * width + x] = r360_rgb565_average(pixels[(y - 1) * width + x],
						pixels[MIN(y + 1, height - 2) * width + x]);
		for (u32 y = 0; y < height; y += 4)
			for (u32 x = 0; x < width; x += 4)
				r360_bc1_fast_block(reference + (y * width + x * 4) / 2, pixels + y * width + x, width);

		u32 cut = (height / 2) & ~3u;
		struct r360_scan_slice top = {
			.source = source, .columns = columns, .rows = rows, .column_end = column_end,
			.output = actual, .width = width, .height = height, .end = cut,
			.format = format, .swap_mode = swap_mode, .reconstruct_rows = reconstruct,
		};
		struct r360_scan_slice bottom = top;
		bottom.first = cut;
		bottom.end = height;
		pthread_t worker;
		assert(!pthread_create(&worker, NULL, encode_scan, &bottom));
		assert(r360_scan_bc1(&top, 0, 0));
		assert(!pthread_join(worker, NULL));
		assert(!memcmp(reference, actual, width * height / 2));
	}
	free(actual);
	free(reference);
	free(pixels);
	free(source);
}

struct jpeg_work {
	const u16 *pixels;
	u8 *output;
	u32 width, height, cut;
	bool low;
	int bytes;
};

static void *encode_bottom(void *context)
{
	struct jpeg_work *job = context;
	job->bytes = r360_encode_jpeg_slice(job->pixels, job->width, job->height, job->cut, job->height,
					   job->output, R360_JPEG_MAX_BYTES, job->low, 0, 0);
	return NULL;
}

static void check_jpeg(u32 width, u32 height, bool low)
{
	u16 *pixels = malloc(width * height * sizeof(*pixels));
	u8 *reference = malloc(R360_JPEG_MAX_BYTES);
	u8 *top = malloc(R360_JPEG_MAX_BYTES);
	u8 *bottom = malloc(R360_JPEG_MAX_BYTES);
	assert(pixels && reference && top && bottom);
	for (u32 y = 0; y < height; y++)
		for (u32 x = 0; x < width; x++)
			pixels[y * width + x] = r360_surface_rgb565(sample_bgrx(x, y), 0, 0);
	int expected = r360_encode_jpeg(pixels, width, height, reference, R360_JPEG_MAX_BYTES, low, 0, 0);
	assert(expected > 0);
	struct jpeg_work job = {pixels, bottom, width, height, (height / 2) & ~15u, low, 0};
	pthread_t worker;
	assert(!pthread_create(&worker, NULL, encode_bottom, &job));
	int bytes = r360_encode_jpeg_slice(pixels, width, height, 0, job.cut,
					  top, R360_JPEG_MAX_BYTES, low, 0, 0);
	assert(!pthread_join(worker, NULL));
	assert(bytes > 0 && job.bytes > 0 && bytes + job.bytes == expected);
	assert(!memcmp(top, reference, (size_t)bytes));
	assert(!memcmp(bottom, reference + bytes, (size_t)job.bytes));
	assert(r360_encode_jpeg_slice(pixels, width, height, 1, height, top, R360_JPEG_MAX_BYTES, low, 0, 0) == 0);
	free(bottom);
	free(top);
	free(reference);
	free(pixels);
}

int main(void)
{
	check_scan(320, 180, 0, 0, 0, 0, false);
	check_scan(320, 180, 8, 7, 0, 0, true);
	check_scan(512, 288, 7, 4, 1, 1, true);
	check_scan(640, 360, 4, 11, 0, 3, false);
	check_scan(1280, 720, 6, 7, 0, 0, false);
	check_jpeg(640, 360, false);
	check_jpeg(960, 540, true);
	check_jpeg(1280, 720, false);
	check_jpeg(1920, 1080, true);
	return 0;
}
