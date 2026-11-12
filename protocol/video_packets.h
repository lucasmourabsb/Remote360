/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "../core/compat.h"
#include "remote360_protocol.h"

#define R360_VIDEO_CHUNKS ((R360_VIDEO_BYTES + R360_VIDEO_PAYLOAD_MAX - 1) / R360_VIDEO_PAYLOAD_MAX)
#define R360_VIDEO_GROUPS ((R360_VIDEO_CHUNKS + R360_VIDEO_PARITY_GROUP - 1) / R360_VIDEO_PARITY_GROUP)

struct r360_video_layout {
	u16 width, height, chunks;
	u32 bytes;
	u8 packet_type, parity_type;
};

static const struct r360_video_layout r360_rgb565 = {
	R360_VIDEO_WIDTH, R360_VIDEO_HEIGHT, R360_VIDEO_CHUNKS, R360_VIDEO_BYTES,
	R360_PACKET_VIDEO, R360_PACKET_VIDEO_PARITY,
};
#define R360_BC1_LAYOUT(w, h) { (w), (h), \
	((w) * (h) / 2 + R360_VIDEO_PAYLOAD_MAX - 1) / R360_VIDEO_PAYLOAD_MAX, \
	(w) * (h) / 2, R360_PACKET_VIDEO_BC1, R360_PACKET_VIDEO_BC1_PARITY }
static const struct r360_video_layout r360_bc1 = R360_BC1_LAYOUT(R360_BC1_WIDTH, R360_BC1_HEIGHT);

static inline u32 r360_layout_header_size(const struct r360_video_layout *layout)
{
	return layout->packet_type == R360_PACKET_VIDEO_JPEG ?
	       R360_JPEG_HEADER_SIZE : R360_VIDEO_HEADER_SIZE;
}

static inline void r360_packet_header(u8 *packet, u8 type)
{
	memcpy(packet, "R360", 4);
	packet[4] = R360_PROTOCOL_VERSION;
	packet[5] = type;
}

static inline void r360_video_header(u8 *packet, const struct r360_video_layout *layout,
				     u8 type, u32 frame_id, u32 index, u32 payload)
{
	r360_packet_header(packet, type);
	put_le16(packet + 6, layout->width);
	put_le16(packet + 8, layout->height);
	put_le16(packet + 10, (u16)index);
	put_le16(packet + 12, layout->chunks);
	put_le32(packet + 14, frame_id);
	put_le16(packet + 18, (u16)payload);
	if (layout->packet_type == R360_PACKET_VIDEO_JPEG)
		put_le32(packet + 20, layout->bytes);
}

/* The sender calls this with 0 <= chunk < R360_VIDEO_CHUNKS. */
static inline int r360_layout_packet(u8 *packet, const struct r360_video_layout *layout,
				     const u8 *frame, u32 frame_id, u32 chunk)
{
	u32 offset = chunk * R360_VIDEO_PAYLOAD_MAX;
	u32 payload = MIN((u32)R360_VIDEO_PAYLOAD_MAX, layout->bytes - offset);
	u32 header = r360_layout_header_size(layout);
	r360_video_header(packet, layout, layout->packet_type, frame_id, chunk, payload);
	memcpy(packet + header, frame + offset, payload);
	return (int)(header + payload);
}

/* An XOR of up to eight chunks recovers one missing packet in the group. */
static inline int r360_layout_parity_packet(u8 *packet, const struct r360_video_layout *layout,
					    const u8 *frame, u32 frame_id, u32 group)
{
	u32 first = group * R360_VIDEO_PARITY_GROUP;
	u32 end = MIN(first + R360_VIDEO_PARITY_GROUP, (u32)layout->chunks);
	u32 header = r360_layout_header_size(layout);
	u8 *parity = packet + header;
	r360_video_header(packet, layout, layout->parity_type, frame_id, group, R360_VIDEO_PAYLOAD_MAX);
	memset(parity, 0, R360_VIDEO_PAYLOAD_MAX);
	for (u32 chunk = first; chunk < end; chunk++) {
		u32 offset = chunk * R360_VIDEO_PAYLOAD_MAX;
		u32 bytes = MIN((u32)R360_VIDEO_PAYLOAD_MAX, layout->bytes - offset);
		for (u32 i = 0; i < bytes; i++)
			parity[i] ^= frame[offset + i];
	}
	return (int)(header + R360_VIDEO_PAYLOAD_MAX);
}

/* Interleave up to four XOR groups to spread bursts of packet loss.
 * The last window can have fewer groups or a short group. */
struct r360_packet_order {
	u16 chunks, groups, first_group;
	u8 row, column;
	bool parity;
};

static inline void r360_packet_order_init(struct r360_packet_order *order, u16 chunks, bool parity)
{
	*order = (struct r360_packet_order){
		.chunks = chunks, .groups = (chunks + R360_VIDEO_PARITY_GROUP - 1) / R360_VIDEO_PARITY_GROUP,
		.parity = parity,
	};
}

static inline bool r360_packet_order_next(struct r360_packet_order *order, u32 *index, bool *parity)
{
	if (!order->parity) {
		if (order->first_group >= order->chunks)
			return false;
		*index = order->first_group++;
		*parity = false;
		return true;
	}
	while (order->first_group < order->groups) {
		u32 group = order->first_group + order->column;
		u32 chunk = group * R360_VIDEO_PARITY_GROUP + order->row;
		bool parity_row = order->row == R360_VIDEO_PARITY_GROUP;
		if (++order->column == 4 || group + 1 == order->groups) {
			order->column = 0;
			if (++order->row > R360_VIDEO_PARITY_GROUP) {
				order->row = 0;
				order->first_group += 4;
			}
		}
		if (parity_row || chunk < order->chunks) {
			*index = parity_row ? group : chunk;
			*parity = parity_row;
			return true;
		}
	}
	return false;
}

static inline int r360_video_packet(u8 *packet, const u8 *frame, u32 frame_id, u32 chunk)
{
	return r360_layout_packet(packet, &r360_rgb565, frame, frame_id, chunk);
}

static inline int r360_video_parity_packet(u8 *packet, const u8 *frame, u32 frame_id, u32 group)
{
	return r360_layout_parity_packet(packet, &r360_rgb565, frame, frame_id, group);
}

/* Deterministic, animated and independent of the GPU. For transport diagnosis only. */
static inline void r360_video_test_pattern(u8 *frame, u32 frame_id)
{
	static const u16 colors[] = {0xffff, 0xffe0, 0x07ff, 0x07e0, 0xf81f, 0xf800, 0x001f, 0x0000};
	u32 marker = (frame_id % 80) * 4;
	for (u32 y = 0; y < R360_VIDEO_HEIGHT; y++) {
		for (u32 x = 0; x < R360_VIDEO_WIDTH; x++) {
			u16 color = colors[x / (R360_VIDEO_WIDTH / ARRAY_SIZE(colors))];
			if (y >= R360_VIDEO_HEIGHT - 24)
				color = x >= marker && x < marker + 4 ? 0xffff : 0x2104;
			put_le16(frame + (y * R360_VIDEO_WIDTH + x) * 2, color);
		}
	}
}
