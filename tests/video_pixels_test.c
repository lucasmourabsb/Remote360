/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../protocol/remote360_protocol.h"
#include "../xbox360/video_pixels.h"

static void test_layout(void)
{
	struct r360_surface s = { .width = 864, .height = 480, .pitch = 896, .x_offset = 8, .y_offset = 7 };
	assert(r360_surface_bytes(&s) == 896u * 487 * 4);
	assert(r360_surface_offset(&s, 0, 0) == (896u * 7 + 8) * 4);
	assert(r360_surface_offset(&s, 863, 479) == (896u * 486 + 871) * 4);
	s.swap_mode = 3;
	assert(r360_surface_offset(&s, 0, 0) == (((896u * 7 + 8) * 4) ^ 4));
	s.layout = 4;
	assert(r360_surface_bytes(&s) == 896u * 512 * 4);
	u32 size = r360_surface_bytes(&s);
	u8 *visited = calloc(size / 4, 1);
	assert(visited);
	for (u32 y = 0; y < s.height; y++) {
		for (u32 x = 0; x < s.width; x++) {
			u32 byte = r360_surface_offset(&s, x, y);
			assert(byte % 4 == 0 && byte + 4 <= size);
			assert(!visited[byte / 4]);
			visited[byte / 4] = 1;
		}
	}
	free(visited);
	s.pitch = 864;
	assert(r360_surface_bytes(&s) == 0);
	s.pitch = 896;
	s.x_offset = UINT32_MAX;
	assert(r360_surface_bytes(&s) == 0);
}

static void test_colors(void)
{
	assert(r360_surface_rgb565(0x00ff0000, 0, 0) == 0xf800);
	assert(r360_surface_rgb565(0x0000ff00, 0, 0) == 0x07e0);
	assert(r360_surface_rgb565(0x000000ff, 0, 0) == 0x001f);
	assert(r360_surface_rgb565(0xff000000, 0, 1) == 0xf800);
	assert(r360_surface_rgb565(0x0000ff00, 0, 2) == 0xf800);
	assert(r360_surface_rgb565(0x0000ff00, 0, 3) == 0xf800);
	assert(r360_surface_rgb565(0x3ff00000, 1, 0) == 0xf800);
	assert(r360_surface_rgb565(0x000ffc00, 1, 0) == 0x07e0);
	assert(r360_surface_rgb565(0x000003ff, 1, 0) == 0x001f);
}

static void write_fixture(const char *path)
{
	struct r360_surface s = { .width = 1280, .height = 720, .pitch = 1280, .layout = 4 };
	u32 size = r360_surface_bytes(&s);
	u8 *source = malloc(size);
	FILE *input = fopen(path, "rb");
	assert(input && source && fread(source, 1, size, input) == size);
	fclose(input);
	for (u32 y = 0; y < R360_VIDEO_HEIGHT; y++) {
		for (u32 x = 0; x < R360_VIDEO_WIDTH; x++) {
			u32 byte = r360_surface_offset(&s, x * s.width / R360_VIDEO_WIDTH, y * s.height / R360_VIDEO_HEIGHT);
			assert(byte + 4 <= size);
			u16 pixel = r360_surface_rgb565(get_le32(source + byte), 0, 0);
			putchar(pixel & 255);
			putchar(pixel >> 8);
		}
	}
	free(source);
}

int main(int argc, char **argv)
{
	test_layout();
	test_colors();
	if (argc == 2)
		write_fixture(argv[1]);
	return 0;
}
