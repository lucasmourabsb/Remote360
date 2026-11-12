/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "video_jpeg.h"

#define R360_JPEG 1
#include "vendor/jpge/JPEGENC.h"
#include "vendor/jpge/jpegenc.inl"

int r360_encode_jpeg_slice(const u16 *pixels, u32 width, u32 height, u32 first, u32 end,
			  u8 *output, u32 capacity, bool low_quality,
			  r360_jpeg_progress progress, void *context)
{
	if (!pixels || !output || width < 16 || height < 16 || (width & 1) ||
	    (height & 1) || width > 1920 || height > 1080 || capacity < 8192 ||
	    first >= end || end > height || (first & 15) || (end != height && (end & 15)))
		return 0;

	/* The library checks bounds after each MCU. A 4:2:0 MCU has six 8×8
	 * blocks. Keep headroom for all six blocks, stuffing and markers. */
	JPEGE_IMAGE image = {0};
	image.pOutput = output;
	image.iBufferSize = (int)capacity;
	image.pHighWater = output + capacity - 4096;
	JPEGENCODE encode;
	int quality = low_quality ? JPEGE_Q_LOW : JPEGE_Q_MED;
	if (JPEGEncodeBegin(&image, &encode, (int)width, (int)height, JPEGE_PIXEL_RGB565,
			    JPEGE_SUBSAMPLE_420, quality) != JPEGE_SUCCESS)
		return 0;
	/* JPGE flushes the bitstream and resets all DC predictors on each MCU
	 * row. Starting with the absolute restart number preserves the stream. */
	if (first) {
		image.pc.pOut = output;
		image.iRestart = (int)first / 16;
		encode.y = (int)first;
	}
	u16 edge[16 * 16];
	while (encode.y < (int)end) {
		u32 x = (u32)encode.x;
		u32 y = (u32)encode.y;
		const u16 *mcu = pixels + y * width + x;
		int pitch = (int)width * 2;
		/* The encoder reads a full 16×16 block even at the last row or
		 * column. Replicate edge pixels while preserving JPEG dimensions. */
		if (width - x < 16 || height - y < 16) {
			for (u32 row = 0; row < 16; row++) {
				const u16 *source = pixels + MIN(y + row, height - 1) * width;
				for (u32 col = 0; col < 16; col++)
					edge[row * 16 + col] = source[MIN(x + col, width - 1)];
			}
			mcu = edge;
			pitch = 16 * 2;
		}
		if (JPEGAddMCU(&image, &encode, (u8 *)mcu, pitch) != JPEGE_SUCCESS)
			return 0;
		if (encode.x == 0 && progress && !progress(context))
			return -1;
	}
	return end == height ? JPEGEncodeEnd(&image) : (int)(image.pc.pOut - output);
}

int r360_encode_jpeg(const u16 *pixels, u32 width, u32 height,
		    u8 *output, u32 capacity, bool low_quality,
		    r360_jpeg_progress progress, void *context)
{
	return r360_encode_jpeg_slice(pixels, width, height, 0, height, output, capacity,
				     low_quality, progress, context);
}
