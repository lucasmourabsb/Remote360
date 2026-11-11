// SPDX-License-Identifier: GPL-3.0-or-later
#include <string.h>

#include <xecore/xboxkrnl.h>

#include "../core/gip.h"
#include "../core/platform.h"
#include "../protocol/video_packets.h"
#include "kernel_17559.h"
#include "net_360.h"
#include "video_pixels.h"
#ifdef R360_VIDEO_SETTINGS
#include "../protocol/video_profiles.h"
#include "video_jpeg.h"
#endif
#ifdef R360_EXPERIMENTAL_MEDIA
#include "video_bc1.h"
#define R360_SAMPLE_CAPACITY (R360_BC1_WIDTH * R360_BC1_HEIGHT)
#else
#define R360_SAMPLE_CAPACITY (R360_VIDEO_WIDTH * R360_VIDEO_HEIGHT)
#endif
#ifndef R360_VIDEO_ONLY
#include "xinput_360.h"
#endif

#define BARRIER() __asm__ __volatile__("lwsync" ::: "memory")
#define THREAD_FLAGS (0x2 | (1u << 3) << 24)
#define THREAD_STACK 0x8000
#define WATCHDOG_MS 350
#define PEER_TAKEOVER_MS 2000
#define STATUS_INTERVAL_MS 250
#ifndef R360_VIDEO_INTERVAL_MS
#define R360_VIDEO_INTERVAL_MS 100
#endif
#ifndef R360_VIDEO_PACKETS_PER_BURST
#define R360_VIDEO_PACKETS_PER_BURST 24
#endif
#define VIDEO_RETRY_MS 20

/* Xenos display registers used by libxenon and the XBDM screenshot path. */
#define XENOS_DISPLAY ((const volatile u32 *)0x7FC86100u)
#define GRPH_CONTROL 1
#define GRPH_SWAP_CONTROL 3
#define GRPH_SURFACE_ADDRESS 4
#define GRPH_PITCH 8
#define GRPH_OFFSET_X 9
#define GRPH_OFFSET_Y 10

#define R360_MAGIC 0x52333630 /* bytes: R360 */

enum {
	R360_ERR_NO_XAM = 1,
	R360_ERR_XINPUT_INIT,
	R360_ERR_XINPUT_BIND,
	R360_ERR_NET_EXPORT,
	R360_ERR_SOCKET,
	R360_ERR_INSECURE,
	R360_ERR_BIND,
	R360_ERR_NONBLOCK,
	R360_ERR_UNSUPPORTED_KERNEL,
	R360_ERR_INPUT_CONFLICT,
};

struct r360_status_block {
	u32 magic;
	u32 version;
	volatile u32 state;
	volatile s32 last_error;
	volatile u32 heartbeat;
	volatile u32 socket;
	volatile u32 packets_rx;
	volatile u32 invalid_packets;
	volatile u32 stale_packets;
	volatile u32 foreign_packets;
	volatile u32 watchdog_trips;
	volatile u32 last_seq;
	volatile u32 peer_ip;
	volatile u32 peer_port;
	volatile u32 last_wsa_error;
	volatile u32 xinput_bound;
	volatile u32 video_frames;
	volatile u32 video_errors;
	volatile u32 source_width;
	volatile u32 source_height;
	volatile u32 source_format;
	volatile u32 capture_errors;
	volatile u32 send_errors;
	volatile u32 last_capture_result;
	volatile u32 display_control;
	volatile u32 display_pitch;
	volatile u32 display_swap;
	volatile u32 client_frames_displayed;
	volatile u32 client_last_frame;
	volatile u32 input_non_neutral;
	volatile u32 input_buttons_seen;
	volatile u32 input_axes_seen;
	volatile u32 controller_phase;
	volatile u32 controller_player;
#ifdef R360_EXPERIMENTAL_MEDIA
	volatile u32 last_capture_ms, last_encode_ms, last_send_ms, bc1_frames, rumble_packets;
#endif
#ifdef R360_VIDEO_SETTINGS
	volatile u32 stream_width, stream_height, target_fps, jpeg_frames, jpeg_bytes, encoder_errors;
	volatile u32 jpeg_burst, capture_limit;
#endif
#ifdef R360_LOW_LATENCY
	volatile u32 fast_frames, fast_mode, fast_drops, fast_partial;
#endif
#ifdef R360_CAPTURE_PROFILE
	volatile u32 profile_frames, profile_surface_address;
	volatile u32 profile_display_us, profile_mapping_us, profile_pixels_us, profile_input_us;
	volatile u32 profile_capture_us, profile_encode_us, profile_send_us;
	volatile u32 profile_map_reused, profile_map_full;
#endif
#ifdef R360_FAST_SCAN
	volatile u32 profile_reconstructed_frames;
#endif
#ifdef R360_PARALLEL_BC1
	volatile u32 parallel_ready, parallel_frames, parallel_overruns;
	volatile u32 parallel_worker_us, parallel_main_us;
#endif
};

__attribute__((used)) struct r360_status_block g_r360 = {
	.magic = R360_MAGIC,
	.version = R360_PROTOCOL_VERSION,
	.state = R360_STATE_STARTING,
	.socket = R360_INVALID_SOCKET,
	.controller_player = ~0u,
};

static struct r360_net_api net;
static struct r360_sockaddr_in peer;
static bool peer_valid;
static bool have_seq;
static bool watchdog_neutral = true;
static bool video_requested;
static bool video_status_requested;
static bool video_test_pattern;
static bool video_parity_requested;
static bool video_bc1_requested;
static u64 last_input_ms;
static u64 last_status_ms;
static u64 last_video_ms;
static u32 video_frame_id;
#ifndef R360_VIDEO_ONLY
enum {
	INPUT_IDLE, INPUT_QUEUE_BIND, INPUT_WAIT_BIND, INPUT_READY,
	INPUT_CONFLICT, INPUT_FAILED,
};
static u64 controller_deadline;
static bool controller_requested;
#endif

/* Kept in .data by -fno-zero-initialized-in-bss. */
static u8 video_frame[R360_VIDEO_BYTES];
static u32 video_offsets[R360_SAMPLE_CAPACITY];
static struct r360_surface video_map_surface;
static const struct r360_video_layout *video_map_layout;
#ifdef R360_EXPERIMENTAL_MEDIA
static u16 video_samples[R360_SAMPLE_CAPACITY];
#endif
#ifdef R360_VIDEO_SETTINGS
static bool video_configurable;
static u8 video_profile;
static u8 video_rate = R360_DEFAULT_RATE_INDEX;
static u8 encoder_max_profile = ARRAY_SIZE(r360_profiles) - 1;
static u8 capture_max_profile = ARRAY_SIZE(r360_profiles) - 1;
static u8 rejected_snapshots;
static bool encoder_low_quality;
static bool encoder_limited;
static bool source_limited;
static u32 video_config_generation;
static u64 next_video_due;
static u16 *jpeg_pixels;
static u8 *jpeg_output;
static u32 jpeg_pixel_capacity;
static u32 jpeg_columns[1920];
static u32 jpeg_rows[1080];
static struct r360_surface jpeg_map_surface;
static u32 jpeg_map_width, jpeg_map_height;
static u32 burst_limit = R360_VIDEO_PACKETS_PER_BURST;
static u32 burst_last_sent, burst_last_displayed;
static u64 burst_last_check;
static u64 last_feedback_ms;
#endif
#ifdef R360_LOW_LATENCY
/* 640 × 360 BC1 occupies exactly the same space as 320 × 180 RGB565. */
static const struct r360_video_layout fast_layouts[] = {
	R360_BC1_LAYOUT(320, 180), R360_BC1_LAYOUT(512, 288), R360_BC1_LAYOUT(640, 360),
};
static bool video_fast;
static u16 column_run[640], row_run[360];
static u32 slow_fast_frames;
static u64 job_service_ms;
#endif

struct r360_video_job {
	u32 socket;
	struct r360_sockaddr_in destination;
	bool pattern;
	bool parity;
	bool bc1;
#ifdef R360_VIDEO_SETTINGS
	u32 config_generation;
#endif
#ifdef R360_CAPTURE_PROFILE
	u32 display_ticks, mapping_ticks, pixel_ticks, input_ticks;
	u32 surface_address;
	bool reused_mapping;
#endif
#ifdef R360_FAST_SCAN
	bool reconstruct_rows;
#endif
};

#ifdef R360_CAPTURE_PROFILE
static inline u32 capture_ticks(void)
{
	u32 ticks;
	/* 32-bit wrap is intentional; each difference spans less than one frame. */
	__asm__ __volatile__("mftb %0" : "=r"(ticks) :: "memory");
	return ticks;
}

static inline u32 capture_us(u32 ticks)
{
	/* Xenon time base: 3.192 GHz / 64, or 399 ticks per 8 microseconds. */
	return (ticks / 399) * 8 + ((ticks % 399) * 8) / 399;
}
#endif

struct r360_display_info {
	u16 front_buffer_width;
	u16 front_buffer_height;
	u8 front_buffer_color_format;
	u8 front_buffer_pixel_format;
	u8 pad0[2];
	u32 scaler_source_rect[4];
	u32 scaled_output_width;
	u32 scaled_output_height;
	u32 vertical_filter_type;
	u32 vertical_filter[3];
	u32 horizontal_filter_type;
	u32 horizontal_filter[3];
	u16 overscan_left;
	u16 overscan_top;
	u16 overscan_right;
	u16 overscan_bottom;
	u16 display_width;
	u16 display_height;
	u32 refresh_rate;
	u32 interlaced;
	u8 display_color_format;
	u8 pad1;
	u16 actual_display_width;
};
_Static_assert(sizeof(struct r360_display_info) == 0x58, "display info ABI");

typedef void (*r360_get_display_info_t)(struct r360_display_info *info);
static r360_get_display_info_t p_get_display_info;

extern u32 R360NetThread(void *context);
static void service_input(u32 socket);

static bool resolve_export(HANDLE module, u32 ordinal, void **out)
{
	void *p = 0;
	XexGetProcedureAddress(module, ordinal, &p);
	*out = p;
	return p != 0;
}

static bool resolve_network(void)
{
	HANDLE xam = 0;

	if (XexGetModuleHandle("xam.xex", &xam) < 0 || !xam)
		return false;
	return resolve_export(xam, ORD_NetDll_socket, (void **)&net.socket) &&
	       resolve_export(xam, ORD_NetDll_closesocket, (void **)&net.closesocket) &&
	       resolve_export(xam, ORD_NetDll_ioctlsocket, (void **)&net.ioctlsocket) &&
	       resolve_export(xam, ORD_NetDll_setsockopt, (void **)&net.setsockopt) &&
	       resolve_export(xam, ORD_NetDll_bind, (void **)&net.bind) &&
	       resolve_export(xam, ORD_NetDll_recvfrom, (void **)&net.recvfrom) &&
	       resolve_export(xam, ORD_NetDll_sendto, (void **)&net.sendto) &&
	       resolve_export(xam, ORD_NetDll_WSAGetLastError, (void **)&net.last_error);
}

static bool resolve_video(void)
{
	HANDLE krnl = 0;
	void *p = 0;

	if (XexGetModuleHandle("xboxkrnl.exe", &krnl) < 0 || !krnl)
		return false;
	XexGetProcedureAddress(krnl, 442 /* VdGetCurrentDisplayInformation */, &p);
	p_get_display_info = (r360_get_display_info_t)p;
	return p_get_display_info != 0;
}

static bool same_peer(const struct r360_sockaddr_in *a, const struct r360_sockaddr_in *b)
{
	return a->sin_family == b->sin_family && a->sin_port == b->sin_port &&
	       a->sin_addr.s_addr == b->sin_addr.s_addr;
}

static bool packet_header(const u8 *p, int n, u8 type, int expected)
{
	return n == expected && p[0] == 'R' && p[1] == '3' && p[2] == '6' && p[3] == '0' &&
	       p[4] == R360_PROTOCOL_VERSION && p[5] == type;
}

static bool seq_is_newer(u32 seq, u32 previous)
{
	return (s32)(seq - previous) > 0;
}

static void neutral_pad(void)
{
#ifndef R360_VIDEO_ONLY
	if (g_r360.xinput_bound)
		xa_xinput_neutralize(0);
	controller_requested = false;
#endif
}

static bool record_input(const u8 *p)
{
	u16 buttons = get_le16(p + 12);
	u32 axes = 0;
	for (u32 axis = 0; axis < 4; axis++) {
		s16 value = (s16)get_le16(p + 16 + 2 * axis);
		if (value < -3500 || value > 3500)
			axes |= 1u << axis;
	}
	if (p[14] > 20) axes |= 1u << 4;
	if (p[15] > 20) axes |= 1u << 5;
	if (p[6] & R360_INPUT_FLAG_GUIDE) axes |= 1u << 6;
	if (!buttons && !axes)
		return false;
	g_r360.input_non_neutral++;
	g_r360.input_buttons_seen |= buttons;
	g_r360.input_axes_seen |= axes;
	return true;
}

#ifndef R360_VIDEO_ONLY
static bool conflicting_controller_driver(void)
{
	static const char *const drivers[] = {
		"hiddriver.xex", "hiddriver360.xex", "WirelessInput360.xex",
		"controlleremu.xex",
	};
	for (u32 i = 0; i < sizeof(drivers) / sizeof(drivers[0]); i++) {
		HANDLE driver = 0;
		if (XexGetModuleHandle(drivers[i], &driver) >= 0 && driver)
			return true;
	}
	return false;
}

/* A connection and neutral packets alone must not register a controller.
 * Keep network service running while XAM queues and binds the device. */
static void service_controller(u64 now)
{
	if (g_r360.controller_phase == INPUT_IDLE && controller_requested) {
		controller_requested = false;
		if (conflicting_controller_driver()) {
			g_r360.last_error = -R360_ERR_INPUT_CONFLICT;
			g_r360.controller_phase = INPUT_CONFLICT;
			return;
		}
		if (xa_xinput_init()) {
			g_r360.last_error = -R360_ERR_XINPUT_INIT;
			g_r360.controller_phase = INPUT_FAILED;
			return;
		}
		g_r360.controller_phase = INPUT_QUEUE_BIND;
		controller_deadline = now + 2000;
	}
	if (g_r360.controller_phase == INPUT_QUEUE_BIND && xa_xinput_bind(0) == 0) {
		g_r360.controller_phase = INPUT_WAIT_BIND;
		controller_deadline = now + 4000;
	}
	if (g_r360.controller_phase == INPUT_WAIT_BIND) {
		int player = xa_xinput_player(0);
		if (player >= 0) {
			g_r360.controller_player = (u32)player;
			g_r360.xinput_bound = 1;
			g_r360.controller_phase = INPUT_READY;
			return;
		}
	}
	if ((g_r360.controller_phase == INPUT_QUEUE_BIND ||
	     g_r360.controller_phase == INPUT_WAIT_BIND) && now >= controller_deadline) {
		g_r360.last_error = -R360_ERR_XINPUT_BIND;
		g_r360.controller_phase = INPUT_FAILED;
		xa_xinput_release();
	}
}

static void apply_input(const u8 *p)
{
	struct gip_gamepad_state s;
	u16 b = get_le16(p + 12);

	memset(&s, 0, sizeof(s));
	if (b & R360_BTN_A) s.buttons |= GIP_GP_BTN_A;
	if (b & R360_BTN_B) s.buttons |= GIP_GP_BTN_B;
	if (b & R360_BTN_X) s.buttons |= GIP_GP_BTN_X;
	if (b & R360_BTN_Y) s.buttons |= GIP_GP_BTN_Y;
	if (b & R360_BTN_START) s.buttons |= GIP_GP_BTN_MENU;
	if (b & R360_BTN_BACK) s.buttons |= GIP_GP_BTN_VIEW;
	if (b & R360_BTN_DPAD_UP) s.buttons |= GIP_GP_BTN_DPAD_U;
	if (b & R360_BTN_DPAD_DOWN) s.buttons |= GIP_GP_BTN_DPAD_D;
	if (b & R360_BTN_DPAD_LEFT) s.buttons |= GIP_GP_BTN_DPAD_L;
	if (b & R360_BTN_DPAD_RIGHT) s.buttons |= GIP_GP_BTN_DPAD_R;
	if (b & R360_BTN_SHOULDER_L) s.buttons |= GIP_GP_BTN_BUMPER_L;
	if (b & R360_BTN_SHOULDER_R) s.buttons |= GIP_GP_BTN_BUMPER_R;
	if (b & R360_BTN_THUMB_L) s.buttons |= GIP_GP_BTN_STICK_L;
	if (b & R360_BTN_THUMB_R) s.buttons |= GIP_GP_BTN_STICK_R;
	s.guide = (p[6] & R360_INPUT_FLAG_GUIDE) != 0;
	s.trigger_left = (u16)p[14] * 1023 / 255;
	s.trigger_right = (u16)p[15] * 1023 / 255;
	s.stick_left_x = (s16)get_le16(p + 16);
	s.stick_left_y = (s16)get_le16(p + 18);
	s.stick_right_x = (s16)get_le16(p + 20);
	s.stick_right_y = (s16)get_le16(p + 22);
	xa_xinput_update(0, &s);
}
#endif

static void send_status(u32 socket)
{
	u8 p[R360_STATUS_SIZE];
	u64 now;

	if (!peer_valid)
		return;
	memset(p, 0, sizeof(p));
	r360_packet_header(p, R360_PACKET_STATUS);
	p[6] = (u8)g_r360.state;
	p[7] = (watchdog_neutral ? 1 : 0) | (g_r360.xinput_bound ? 2 : 0);
#ifdef R360_VIDEO_ONLY
	p[7] |= R360_STATUS_VIDEO_ONLY;
#else
	if (g_r360.controller_phase == INPUT_CONFLICT)
		p[7] |= R360_STATUS_VIDEO_ONLY | R360_STATUS_INPUT_CONFLICT;
	if (g_r360.controller_phase == INPUT_FAILED)
		p[7] |= R360_STATUS_VIDEO_ONLY | R360_STATUS_INPUT_FAILED;
#endif
	put_le32(p + 8, g_r360.last_seq);
	put_le32(p + 12, g_r360.packets_rx);
	put_le32(p + 16, g_r360.invalid_packets);
	now = xa_now_ms();
	put_le32(p + 20, (u32)now);
	net.sendto(R360_XNCALLER_SYSAPP, socket, (const char *)p, sizeof(p), 0,
		   (const struct r360_sockaddr *)&peer, sizeof(peer));
	last_status_ms = now;
}

static void send_video_status(u32 socket)
{
	u8 packet[R360_VIDEO_STATUS_SIZE] = {0};
	r360_packet_header(packet, R360_PACKET_VIDEO_STATUS);
	packet[6] = (u8)g_r360.last_capture_result;
	packet[7] = (video_requested ? R360_VIDEO_STATUS_REQUESTED : 0) |
		    (video_test_pattern ? R360_VIDEO_STATUS_PATTERN : 0) |
		    R360_VIDEO_STATUS_FEEDBACK;
#ifdef R360_VIDEO_SETTINGS
	packet[7] |= R360_VIDEO_STATUS_CONFIG;
	if (source_limited)
		packet[7] |= R360_VIDEO_STATUS_SOURCE_LIMIT;
	if (encoder_limited)
		packet[7] |= R360_VIDEO_STATUS_ENCODER_LIMIT;
	if (g_r360.capture_limit)
		packet[7] |= R360_VIDEO_STATUS_CAPTURE_LIMIT;
#endif
#ifdef R360_LOW_LATENCY
	packet[7] |= R360_VIDEO_STATUS_FAST;
#endif
	put_le32(packet + 8, g_r360.last_seq);
	put_le32(packet + 12, g_r360.video_frames);
	put_le32(packet + 16, g_r360.capture_errors);
	put_le32(packet + 20, g_r360.send_errors);
	put_le32(packet + 24, g_r360.last_wsa_error);
	put_le16(packet + 28, (u16)g_r360.source_width);
	put_le16(packet + 30, (u16)g_r360.source_height);
	put_le32(packet + 32, g_r360.display_control);
	put_le16(packet + 36, (u16)g_r360.display_pitch);
	put_le16(packet + 38, (u16)g_r360.display_swap);
	net.sendto(R360_XNCALLER_SYSAPP, socket, (const char *)packet, sizeof(packet), 0,
		   (const struct r360_sockaddr *)&peer, sizeof(peer));
}

#ifndef R360_VIDEO_ONLY
static void send_rumble(u32 socket, u8 left, u8 right)
{
	u8 p[R360_RUMBLE_SIZE];

	if (!peer_valid)
		return;
	memset(p, 0, sizeof(p));
	r360_packet_header(p, R360_PACKET_RUMBLE);
	p[6] = left;
	p[7] = right;
	put_le32(p + 8, g_r360.last_seq);
	int result = net.sendto(R360_XNCALLER_SYSAPP, socket, (const char *)p, sizeof(p), 0,
				(const struct r360_sockaddr *)&peer, sizeof(peer));
#ifdef R360_EXPERIMENTAL_MEDIA
	if (result == sizeof(p))
		g_r360.rumble_packets++;
#else
	(void)result;
#endif
}
#endif

static bool rebuild_video_map(const struct r360_surface *surface, u32 surface_size,
			      const struct r360_video_layout *layout)
{
	/* A rejected mode leaves the cache invalid even if the previous mode returns. */
	video_map_surface.width = 0;
	u16 sample_x[R360_BC1_WIDTH];
	for (u32 x = 0; x < layout->width; x++)
		sample_x[x] = (u16)(x * surface->width / layout->width);
	for (u32 y = 0; y < layout->height; y++) {
		u32 sample_y = y * surface->height / layout->height;
		for (u32 x = 0; x < layout->width; x++) {
			u32 src = r360_surface_offset(surface, sample_x[x], sample_y);
			if (src + 4 > surface_size)
				return false;
			video_offsets[y * layout->width + x] = src;
		}
	}
	video_map_surface = *surface;
	video_map_layout = layout;
	return true;
}

#ifdef R360_MAP_REUSE
struct r360_mapping_check {
	u32 address, size;
	u64 checked_ms;
};
static struct r360_mapping_check scanout_checks[4];
static u32 next_scanout_check;

/* The C alias uses 16 MiB pages on retail 17559. Require a full scan before
 * reusing a particular range, then check the start, each large-page boundary,
 * and the end on every capture. Other aliases keep their original checks. */
static bool reuse_scanout_check(u32 start, u32 size, u64 now)
{
	for (u32 i = 0; i < ARRAY_SIZE(scanout_checks); i++) {
		struct r360_mapping_check *check = &scanout_checks[i];
		if (check->address != start || check->size != size || now - check->checked_ms > 2000)
			continue;
		u32 end = start + size - 1;
		for (u32 page = (start & ~0xFFFFFFu) + 0x1000000u; page <= end; page += 0x1000000u) {
			if (!MmIsAddressValid((void *)page)) {
				check->address = 0;
				return false;
			}
		}
		if (!MmIsAddressValid((void *)end)) {
			check->address = 0;
			return false;
		}
		return true;
	}
	return false;
}

static void remember_scanout_check(u32 start, u32 size, u64 now)
{
	for (u32 i = 0; i < ARRAY_SIZE(scanout_checks); i++) {
		if (scanout_checks[i].address == start && scanout_checks[i].size == size) {
			scanout_checks[i].checked_ms = now;
			return;
		}
	}
	scanout_checks[next_scanout_check++ % ARRAY_SIZE(scanout_checks)] =
		(struct r360_mapping_check){start, size, now};
}
#endif

static const volatile u8 *front_buffer(u32 offset, u32 surface_size
#ifdef R360_MAP_REUSE
				       , struct r360_video_job *job, bool fast
#endif
				       )
{
	const u8 *fb;

	/* The display surface is inside the Xbox 360's 512 MiB of physical RAM. */
	if (!offset || offset > 0x20000000u - surface_size)
		return 0;
	fb = (const u8 *)(0xC0000000u + offset);
	if (!MmIsAddressValid((void *)fb)) {
		fb = (const u8 *)(0xA0000000u + offset);
		if (!MmIsAddressValid((void *)fb)) {
			fb = (const u8 *)(0xDFFFF000u + offset);
			if (!MmIsAddressValid((void *)fb))
				return 0;
		}
	}
	if ((u32)fb > 0xFFFFFFFFu - surface_size)
		return 0;
#ifdef R360_MAP_REUSE
	bool c_alias = fast && (u32)fb == 0xC0000000u + offset;
	u64 now = c_alias ? xa_now_ms() : 0;
	if (c_alias && reuse_scanout_check((u32)fb, surface_size, now)) {
		job->reused_mapping = true;
		return fb;
	}
#endif
	for (u32 page = 0; page < surface_size; page += 4096)
		if (!MmIsAddressValid((void *)(fb + page)))
			return 0;
	if (!MmIsAddressValid((void *)(fb + surface_size - 1)))
		return 0;
#ifdef R360_MAP_REUSE
	if (c_alias)
		remember_scanout_check((u32)fb, surface_size, now);
#endif
	return fb;
}

static bool service_video_job(void *context)
{
	const struct r360_video_job *job = context;
	service_input(job->socket);
	if (peer_valid && xa_now_ms() - last_status_ms >= STATUS_INTERVAL_MS) {
		send_status(job->socket);
		if (video_status_requested)
			send_video_status(job->socket);
	}
#ifdef R360_VIDEO_SETTINGS
	if (video_config_generation != job->config_generation)
		return false;
#endif
	return video_requested && !watchdog_neutral && same_peer(&peer, &job->destination) &&
	       video_test_pattern == job->pattern && video_parity_requested == job->parity &&
	       video_bc1_requested == job->bc1;
}

#ifdef R360_LOW_LATENCY
static bool service_fast_job(struct r360_video_job *job)
{
	u64 now = xa_now_ms();
	if (now - job_service_ms < 5)
		return true;
	job_service_ms = now;
#ifdef R360_CAPTURE_PROFILE
	u32 start = capture_ticks();
	bool ok = service_video_job(job);
	job->input_ticks += capture_ticks() - start;
	return ok;
#else
	return service_video_job(job);
#endif
}

static bool encode_fast_rows(u8 *out, const u16 *pixels, u32 width, u32 first, u32 end,
			     struct r360_video_job *job)
{
	out += first * width / 2;
	for (u32 y = first; y < end; y += 4) {
		if (job && !(y & 15) && !service_fast_job(job))
			return false;
		for (u32 x = 0; x < width; x += 4, out += 8)
			r360_bc1_fast_block(out, pixels + y * width + x, width);
	}
	return true;
}
#endif

#ifdef R360_PARALLEL_BC1
/* The controller/network thread already runs on CPU 3 (core 1). CPU 5 is
 * on core 2. Both write disjoint blocks from the same finished capture. */
#define ENCODE_THREAD_FLAGS (0x2 | (1u << 5) << 24)
#define ENCODE_HANDOFF() __asm__ __volatile__("sync" ::: "memory")
extern void R360EncodeThread(void);
static HANDLE encode_start, encode_done;
static struct {
	u8 *output;
	const u16 *pixels;
	u32 width, first, end;
} encode_work;
/* Only the network thread updates these two local flags. */
static bool encoder_thread_created, encoder_inflight;
static u32 encoder_consecutive_overruns;

__attribute__((used, noinline)) u32 R360EncodeThreadC(void *context)
{
	(void)context;
	while (NtWaitForSingleObjectEx((u32)encode_start, 0, 0, 0) == 0) {
		ENCODE_HANDOFF();
		u32 started = capture_ticks();
		encode_fast_rows(encode_work.output, encode_work.pixels, encode_work.width,
				 encode_work.first, encode_work.end, 0);
		g_r360.parallel_worker_us = capture_us(capture_ticks() - started);
		ENCODE_HANDOFF();
		NtSetEvent(encode_done, 0);
	}
	ExTerminateThread(0);
	return 0;
}

static void start_encoder_thread(void)
{
	HANDLE thread;
	u32 tid;
	/* Synchronization events automatically reset after a successful wait. */
	if (NtCreateEvent(&encode_start, 0, 1, 0) < 0)
		return;
	if (NtCreateEvent(&encode_done, 0, 1, 0) < 0) {
		NtClose(encode_start);
		return;
	}
	if (ExCreateThread(&thread, THREAD_STACK, &tid, 0, (void *)R360EncodeThread, 0,
			   ENCODE_THREAD_FLAGS) >= 0) {
		encoder_thread_created = true;
		g_r360.parallel_ready = 1;
		NtClose(thread);
	} else {
		NtClose(encode_start);
		NtClose(encode_done);
	}
}

static bool finish_parallel_encode(int64_t timeout)
{
	if (!encoder_inflight)
		return true;
	if (NtWaitForSingleObjectEx((u32)encode_done, 0, 0, &timeout) != 0)
		return false;
	ENCODE_HANDOFF();
	encoder_inflight = false;
	return true;
}

static bool encode_parallel_frame(const struct r360_video_layout *layout, struct r360_video_job *job)
{
	if (!encoder_thread_created)
		return encode_fast_rows(video_frame, jpeg_pixels, layout->width, 0, layout->height, job);
	u32 cut = (layout->height / 2) & ~3u;
	encode_work.output = video_frame;
	encode_work.pixels = jpeg_pixels;
	encode_work.width = layout->width;
	encode_work.first = cut;
	encode_work.end = layout->height;
	encoder_inflight = true;
	ENCODE_HANDOFF();
	u32 started = capture_ticks();
	if (NtSetEvent(encode_start, 0) < 0) {
		encoder_inflight = false;
		encoder_thread_created = false;
		g_r360.parallel_ready = 0;
		return encode_fast_rows(video_frame, jpeg_pixels, layout->width, 0, layout->height, job);
	}
	u32 main_started = capture_ticks();
	bool valid = encode_fast_rows(video_frame, jpeg_pixels, layout->width, 0, cut, job);
	g_r360.parallel_main_us = capture_us(capture_ticks() - main_started);
	/* Give the worker up to two display ticks, including the work already
	 * done on this thread. Keep processing input while waiting. */
	while (!finish_parallel_encode(0)) {
		if (!valid || !service_video_job(job))
			return false;
		if (capture_us(capture_ticks() - started) < 34000) {
			if (finish_parallel_encode(-20000))
				break;
			continue;
		}
		g_r360.parallel_overruns++;
		if (++encoder_consecutive_overruns >= 3) {
			encoder_thread_created = false;
			g_r360.parallel_ready = 0;
		}
		return false;
	}
	encoder_consecutive_overruns = 0;
	if (valid) {
		g_r360.parallel_frames++;
		return service_video_job(job);
	}
	return false;
}
#endif

#ifdef R360_VIDEO_SETTINGS
static void reset_video_limits(void)
{
	encoder_max_profile = ARRAY_SIZE(r360_profiles) - 1;
	capture_max_profile = ARRAY_SIZE(r360_profiles) - 1;
	encoder_low_quality = false;
	encoder_limited = false;
	source_limited = false;
	rejected_snapshots = 0;
	g_r360.capture_limit = 0;
	burst_limit = R360_VIDEO_PACKETS_PER_BURST;
	burst_last_check = 0;
	last_feedback_ms = 0;
	g_r360.jpeg_burst = burst_limit;
#ifdef R360_LOW_LATENCY
	slow_fast_frames = 0;
#endif
}

static void adapt_jpeg_burst(u64 now)
{
	if (!burst_last_check || now - last_feedback_ms > 750) {
		burst_last_check = now;
		burst_last_sent = g_r360.jpeg_frames;
		burst_last_displayed = g_r360.client_frames_displayed;
		return;
	}
	if (now - burst_last_check < 2000)
		return;
	u32 sent = g_r360.jpeg_frames - burst_last_sent;
	u32 displayed = g_r360.client_frames_displayed - burst_last_displayed;
	if (sent >= 12 && r360_rates[video_rate] >= 30) {
		if (displayed + 2 >= (sent * 9) / 10)
			burst_limit = MIN(burst_limit + 16, 112u);
		else if (displayed + 2 < (sent * 7) / 10 && burst_limit > 40)
			burst_limit = burst_limit >= 56 ? burst_limit - 16 : 40;
	}
	burst_last_check = now;
	burst_last_sent = g_r360.jpeg_frames;
	burst_last_displayed = g_r360.client_frames_displayed;
	g_r360.jpeg_burst = burst_limit;
}

static void clear_jpeg_buffers(void)
{
	if (jpeg_pixels)
		ExFreePool(jpeg_pixels);
	if (jpeg_output)
		ExFreePool(jpeg_output);
	jpeg_pixels = 0;
	jpeg_output = 0;
	jpeg_pixel_capacity = 0;
}

static bool prepare_capture_pixels(u32 pixels)
{
	if (pixels <= jpeg_pixel_capacity)
		return true;
	/* The old image is never read after a resolution change. */
	if (jpeg_pixels)
		ExFreePool(jpeg_pixels);
	jpeg_pixel_capacity = 0;
	jpeg_pixels = ExAllocatePoolWithTag(pixels * sizeof(*jpeg_pixels), 0x52334642);
	if (!jpeg_pixels)
		return false;
	jpeg_pixel_capacity = pixels;
	return true;
}

static bool prepare_jpeg_buffers(u32 pixels)
{
	if (!jpeg_output)
		jpeg_output = ExAllocatePoolWithTag(R360_JPEG_MAX_BYTES, 0x52334a50);
	return jpeg_output && prepare_capture_pixels(pixels);
}

static const struct r360_video_layout *select_layout(const struct r360_surface *surface,
						     struct r360_video_layout *jpeg)
{
	u8 profile = r360_profile_for_source(video_profile, surface->width, surface->height);
	source_limited = profile < video_profile;
	g_r360.capture_limit = profile > capture_max_profile;
	profile = MIN(profile, capture_max_profile);
	profile = MIN(profile, encoder_max_profile);
#ifdef R360_LOW_LATENCY
	if (video_fast) {
		if (profile >= ARRAY_SIZE(fast_layouts)) {
			g_r360.capture_limit = 1;
			profile = ARRAY_SIZE(fast_layouts) - 1;
		}
		for (;;) {
			const struct r360_video_layout *layout = &fast_layouts[profile];
			if (prepare_capture_pixels((u32)layout->width * layout->height))
				return layout;
			encoder_limited = true;
			g_r360.encoder_errors++;
			if (profile == 0)
				return &r360_rgb565;
			encoder_max_profile = --profile;
		}
	}
#endif
	while (profile >= 2) {
		const struct r360_video_profile *dimensions = &r360_profiles[profile];
		if (prepare_jpeg_buffers((u32)dimensions->width * dimensions->height)) {
			*jpeg = (struct r360_video_layout){
				.width = dimensions->width, .height = dimensions->height,
				.packet_type = R360_PACKET_VIDEO_JPEG,
				.parity_type = R360_PACKET_VIDEO_JPEG_PARITY,
			};
			return jpeg;
		}
		encoder_limited = true;
		encoder_max_profile = --profile;
		g_r360.encoder_errors++;
	}
	return profile == 1 ? &r360_bc1 : &r360_rgb565;
}

static u32 jpeg_sample_offset(u32 index, u32 width)
{
	return r360_tiled_sample(jpeg_columns[index % width], jpeg_rows[index / width]);
}

static void prepare_jpeg_map(const struct r360_surface *surface, u32 width, u32 height)
{
	if (jpeg_map_width == width && jpeg_map_height == height &&
	    memcmp(&jpeg_map_surface, surface, sizeof(*surface)) == 0)
		return;
	for (u32 x = 0; x < width; x++) {
		u32 source = x * surface->width / width + surface->x_offset;
		jpeg_columns[x] = r360_tiled_offset(source, 0, surface->pitch) ^
				  (surface->swap_mode == 3 ? 4 : 0);
	}
	for (u32 y = 0; y < height; y++) {
		u32 source = y * surface->height / height + surface->y_offset;
		jpeg_rows[y] = r360_tiled_offset(0, source, surface->pitch);
	}
	jpeg_map_surface = *surface;
	jpeg_map_width = width;
	jpeg_map_height = height;
#ifdef R360_LOW_LATENCY
	if (width <= ARRAY_SIZE(column_run) && height <= ARRAY_SIZE(row_run)) {
		u32 end = width;
		for (u32 x = width; x-- > 0;) {
			if (x + 1 < width && (jpeg_columns[x] >> 12) != (jpeg_columns[x + 1] >> 12))
				end = x + 1;
			column_run[x] = (u16)end;
		}
		end = height;
		for (u32 y = height; y-- > 0;) {
			if (y + 1 < height && (jpeg_rows[y] >> 12) != (jpeg_rows[y + 1] >> 12))
				end = y + 1;
			row_run[y] = (u16)end;
		}
	}
#endif
}

#ifdef R360_LOW_LATENCY
/* Walk one 32×32 Xenos tile at a time so nearby samples reuse the same
 * physical memory, then write pixels to their linear destination. */
static bool copy_fast_tiles(const struct r360_surface *surface, const volatile u8 *fb,
			    u32 width, u32 height, u32 format, struct r360_video_job *job)
{
	bool bgrx = format == 0 && surface->swap_mode == 0;
	u32 step = 1;
#ifdef R360_FAST_SCAN
	step = job->reconstruct_rows ? 2 : 1;
#endif
	for (u32 top = 0; top < height; top = row_run[top]) {
		u32 bottom = row_run[top];
		if (!service_fast_job(job))
			return false;
		for (u32 left = 0; left < width; left = column_run[left]) {
			u32 right = column_run[left];
			for (u32 y = ROUND_UP(top, step); y < bottom; y += step) {
				u16 *dst = jpeg_pixels + y * width;
				u32 row = jpeg_rows[y];
				if (bgrx) {
					for (u32 x = left; x < right; x++) {
						u32 offset = r360_tiled_sample(jpeg_columns[x], row);
						dst[x] = r360_bgrx_word_rgb565(*(const volatile u32 *)(fb + offset));
					}
				} else {
					for (u32 x = left; x < right; x++) {
						u32 offset = r360_tiled_sample(jpeg_columns[x], row);
						u32 word = *(const volatile u32 *)(fb + offset);
						dst[x] = r360_surface_rgb565(__builtin_bswap32(word), format, surface->swap_mode);
					}
				}
			}
		}
	}
#ifdef R360_FAST_SCAN
	if (job->reconstruct_rows) {
		for (u32 y = 1; y < height; y += 2) {
			if ((y & 15) == 1 && !service_fast_job(job))
				return false;
			u16 *dst = jpeg_pixels + y * width;
			const u16 *above = dst - width;
			const u16 *below = y + 1 < height ? dst + width : above;
			for (u32 x = 0; x < width; x++)
				dst[x] = r360_rgb565_average(above[x], below[x]);
		}
	}
#endif
	return true;
}
#endif

static enum r360_video_capture_result copy_jpeg_pixels(const struct r360_surface *surface,
						       const struct r360_video_layout *layout,
						       const volatile u8 *fb, u32 format,
						       struct r360_video_job *job, bool fast)
{
	const u32 width = layout->width;
	const u32 height = layout->height;
	prepare_jpeg_map(surface, width, height);
	u32 anchor[64];
	u32 offset[ARRAY_SIZE(anchor)];
	u32 last_pixel = width * height - 1;
	for (u32 i = 0; i < ARRAY_SIZE(anchor); i++) {
		offset[i] = jpeg_sample_offset(i * last_pixel / (ARRAY_SIZE(anchor) - 1), width);
		anchor[i] = *(const volatile u32 *)(fb + offset[i]);
	}
#ifdef R360_LOW_LATENCY
	if (fast) {
#ifdef R360_CAPTURE_PROFILE
		u32 pixels_started = capture_ticks();
#endif
		if (!copy_fast_tiles(surface, fb, width, height, format, job))
			return R360_CAPTURE_CANCELLED;
#ifdef R360_CAPTURE_PROFILE
		job->pixel_ticks = capture_ticks() - pixels_started;
#endif
	} else
#else
	(void)fast;
#endif
	for (u32 y = 0; y < height; y++) {
		if (!(y & 31) && !service_video_job(job))
			return R360_CAPTURE_CANCELLED;
		u16 *dst = jpeg_pixels + y * width;
		u32 row = jpeg_rows[y];
		for (u32 x = 0; x < width; x++) {
			u32 location = r360_tiled_sample(jpeg_columns[x], row);
			u32 word = *(const volatile u32 *)(fb + location);
			dst[x] = r360_surface_rgb565(__builtin_bswap32(word), format, surface->swap_mode);
		}
	}
	for (u32 i = 0; i < ARRAY_SIZE(anchor); i++)
		if (*(const volatile u32 *)(fb + offset[i]) != anchor[i]) {
#ifdef R360_LOW_LATENCY
			/* Scanout can change within one buffer. For interactive
			 * playback, prefer a fresh frame with a possible seam to
			 * repeatedly dropping every frame during movement. The
			 * caller still checks the display layout before sending. */
			if (fast) {
				g_r360.fast_partial++;
				break;
			}
#endif
			return R360_CAPTURE_CHANGED;
		}
	return R360_CAPTURE_OK;
}
#endif

static enum r360_video_capture_result capture_video_frame(const struct r360_video_layout **chosen,
							  struct r360_video_layout *jpeg,
							  struct r360_video_job *job)
{
#ifdef R360_CAPTURE_PROFILE
	u32 display_started = capture_ticks();
#endif
	struct r360_display_info display = {0};
	if (!p_get_display_info || !MmIsAddressValid((void *)XENOS_DISPLAY))
		return R360_CAPTURE_NO_DISPLAY;
	p_get_display_info(&display);

	u32 control = XENOS_DISPLAY[GRPH_CONTROL];
	u32 swap = XENOS_DISPLAY[GRPH_SWAP_CONTROL];
	struct r360_surface surface = {
		.width = display.front_buffer_width,
		.height = display.front_buffer_height,
		.pitch = XENOS_DISPLAY[GRPH_PITCH] & 0x3FFFu,
		.x_offset = XENOS_DISPLAY[GRPH_OFFSET_X] & 0x3FFFu,
		.y_offset = XENOS_DISPLAY[GRPH_OFFSET_Y] & 0x3FFFu,
		/* The retail Xbox Aurora scanout is tiled even when control reads 0x2. */
		.layout = 4,
		.swap_mode = swap & 3,
	};
	u32 format = (control >> 8) & 7;
	g_r360.display_control = control;
	g_r360.display_pitch = surface.pitch;
	g_r360.display_swap = swap;
	g_r360.source_format = format;
	g_r360.source_width = surface.width;
	g_r360.source_height = surface.height;
	u32 surface_size = r360_surface_bytes(&surface);
#ifdef R360_CAPTURE_PROFILE
	job->display_ticks = capture_ticks() - display_started;
#endif
	if ((control & 3) != 2 || format > 1 || !surface_size)
		return R360_CAPTURE_FORMAT;

	const struct r360_video_layout *layout = *chosen;
	bool is_jpeg = false;
	bool is_fast = false;
#ifdef R360_VIDEO_SETTINGS
	if (video_configurable) {
		layout = select_layout(&surface, jpeg);
		*chosen = layout;
		is_jpeg = layout == jpeg;
#ifdef R360_LOW_LATENCY
		is_fast = video_fast && layout->packet_type == R360_PACKET_VIDEO_BC1;
#endif
	}
#else
	(void)jpeg;
	(void)job;
#endif
	if (!is_jpeg && !is_fast &&
	    (video_map_layout != layout || memcmp(&video_map_surface, &surface, sizeof(surface))) &&
	    !rebuild_video_map(&surface, surface_size, layout))
		return R360_CAPTURE_FORMAT;
	u32 offset = XENOS_DISPLAY[GRPH_SURFACE_ADDRESS] & ~0xFFu;
#ifdef R360_CAPTURE_PROFILE
	u32 mapping_started = capture_ticks();
#endif
	const volatile u8 *fb = front_buffer(offset, surface_size
#ifdef R360_MAP_REUSE
					     , job, is_fast
#endif
					     );
#ifdef R360_CAPTURE_PROFILE
	job->mapping_ticks = capture_ticks() - mapping_started;
	job->surface_address = (u32)fb;
#endif
	if (!fb)
		return R360_CAPTURE_UNMAPPED;
	if ((XENOS_DISPLAY[GRPH_SURFACE_ADDRESS] & ~0xFFu) != offset)
		return R360_CAPTURE_CHANGED;

#ifdef R360_VIDEO_SETTINGS
	if (is_jpeg || is_fast) {
		enum r360_video_capture_result result = copy_jpeg_pixels(&surface, layout, fb, format, job, is_fast);
		if (result != R360_CAPTURE_OK)
			return result;
		if (XENOS_DISPLAY[GRPH_CONTROL] != control || XENOS_DISPLAY[GRPH_SWAP_CONTROL] != swap ||
		    (XENOS_DISPLAY[GRPH_PITCH] & 0x3FFFu) != surface.pitch ||
		    (XENOS_DISPLAY[GRPH_OFFSET_X] & 0x3FFFu) != surface.x_offset ||
		    (XENOS_DISPLAY[GRPH_OFFSET_Y] & 0x3FFFu) != surface.y_offset)
			return R360_CAPTURE_CHANGED;
		return R360_CAPTURE_OK;
	}
#endif
	u32 count = (u32)layout->width * layout->height;
#ifdef R360_EXPERIMENTAL_MEDIA
	/* Sample the same latched buffer across the copy. A flip alone is fine
	 * when the previous buffer is still intact; recycling it is not. */
	u32 anchors[64];
	for (u32 i = 0; i < ARRAY_SIZE(anchors); i++)
		anchors[i] = *(const volatile u32 *)(fb + video_offsets[i * (count - 1) / 63]);
#endif
	for (u32 pixel = 0; pixel < count; pixel++) {
		u32 word = *(const volatile u32 *)(fb + video_offsets[pixel]);
		u16 color = r360_surface_rgb565(__builtin_bswap32(word), format, surface.swap_mode);
#ifdef R360_EXPERIMENTAL_MEDIA
		if (layout == &r360_bc1)
			video_samples[pixel] = color;
		else
#endif
			put_le16(video_frame + pixel * 2, color);
	}
	if (XENOS_DISPLAY[GRPH_CONTROL] != control || XENOS_DISPLAY[GRPH_SWAP_CONTROL] != swap ||
	    (XENOS_DISPLAY[GRPH_PITCH] & 0x3FFFu) != surface.pitch ||
	    (XENOS_DISPLAY[GRPH_OFFSET_X] & 0x3FFFu) != surface.x_offset ||
	    (XENOS_DISPLAY[GRPH_OFFSET_Y] & 0x3FFFu) != surface.y_offset)
		return R360_CAPTURE_CHANGED;
#ifdef R360_EXPERIMENTAL_MEDIA
	for (u32 i = 0; i < ARRAY_SIZE(anchors); i++)
		if (*(const volatile u32 *)(fb + video_offsets[i * (count - 1) / 63]) != anchors[i])
			return R360_CAPTURE_CHANGED;
#else
	if ((XENOS_DISPLAY[GRPH_SURFACE_ADDRESS] & ~0xFFu) != offset)
		return R360_CAPTURE_CHANGED;
#endif
	return R360_CAPTURE_OK;
}

static void send_video_frame(u32 socket)
{
	u8 packet[R360_JPEG_PACKET_MAX];
	u32 frame_id;
	u32 retries_left = 2;

	if (!peer_valid)
		return;
	struct r360_video_job job = {
		.socket = socket, .destination = peer, .pattern = video_test_pattern,
		.parity = video_parity_requested, .bc1 = video_bc1_requested,
#ifdef R360_VIDEO_SETTINGS
		.config_generation = video_config_generation,
#endif
	};
	const struct r360_video_layout *layout = job.bc1 ? &r360_bc1 : &r360_rgb565;
	struct r360_video_layout jpeg_layout;
	const u8 *frame_data = video_frame;
	enum r360_video_capture_result result = R360_CAPTURE_OK;
#ifdef R360_LOW_LATENCY
	job_service_ms = 0;
	bool fast = video_configurable && video_fast && !job.pattern;
#endif
#ifdef R360_FAST_SCAN
	job.reconstruct_rows = fast && r360_rates[video_rate] == 60;
#endif
#ifdef R360_CAPTURE_PROFILE
	u32 profile_start = capture_ticks();
#endif
#ifdef R360_EXPERIMENTAL_MEDIA
	u64 stamp = xa_now_ms();
#endif
	if (job.pattern)
		r360_video_test_pattern(video_frame, video_frame_id + 1);
	else
		result = capture_video_frame(&layout, &jpeg_layout, &job);
#ifdef R360_EXPERIMENTAL_MEDIA
	g_r360.last_capture_ms = (u32)(xa_now_ms() - stamp);
#endif
	g_r360.last_capture_result = result;
	if (result != R360_CAPTURE_OK) {
		if (result == R360_CAPTURE_CANCELLED)
			return;
		g_r360.capture_errors++;
		g_r360.video_errors++;
#ifdef R360_VIDEO_SETTINGS
		bool watch_capture = layout == &jpeg_layout;
#ifdef R360_LOW_LATENCY
		watch_capture |= fast && layout->packet_type == R360_PACKET_VIDEO_BC1 && layout->width > 320;
#endif
		if (watch_capture && result == R360_CAPTURE_CHANGED && ++rejected_snapshots >= 8) {
			u8 profile = r360_profile_for_source(video_profile, layout->width, layout->height);
			capture_max_profile = profile - 1;
			rejected_snapshots = 0;
		} else if (result != R360_CAPTURE_CHANGED) {
			rejected_snapshots = 0;
		}
#endif
		return;
	}
#ifdef R360_VIDEO_SETTINGS
	rejected_snapshots = 0;
#endif
#ifdef R360_CAPTURE_PROFILE
	u32 profile_after_capture = capture_ticks();
	u32 profile_input_ticks = job.input_ticks;
#endif
#ifdef R360_EXPERIMENTAL_MEDIA
	stamp = xa_now_ms();
#ifdef R360_LOW_LATENCY
	if (fast && layout->packet_type == R360_PACKET_VIDEO_BC1) {
#ifdef R360_PARALLEL_BC1
		if (!encode_parallel_frame(layout, &job))
			return;
#else
		if (!encode_fast_rows(video_frame, jpeg_pixels, layout->width, 0, layout->height, &job))
			return;
#endif
	} else
#endif
	if (layout == &r360_bc1) {
		u8 *out = video_frame;
		for (u32 y = 0; y < R360_BC1_HEIGHT; y += 4) {
			if (!(y & 15)) {
				if (!service_video_job(&job))
					return;
			}
			for (u32 x = 0; x < R360_BC1_WIDTH; x += 4, out += 8)
				r360_bc1_encode_block(out, video_samples + y * R360_BC1_WIDTH + x,
						     R360_BC1_WIDTH);
		}
	}
#ifdef R360_VIDEO_SETTINGS
	if (layout == &jpeg_layout) {
		int bytes = r360_encode_jpeg(jpeg_pixels, layout->width, layout->height,
					     jpeg_output, R360_JPEG_MAX_BYTES, encoder_low_quality,
					     service_video_job, &job);
		if (bytes < 0)
			return;
		if (!bytes) {
			g_r360.encoder_errors++;
			g_r360.last_capture_result = R360_CAPTURE_ENCODER;
			encoder_limited = true;
			if (encoder_low_quality) {
				u8 profile = r360_profile_for_source(video_profile, layout->width, layout->height);
				encoder_max_profile = profile - 1;
			}
			encoder_low_quality = true;
			return;
		}
		jpeg_layout.bytes = (u32)bytes;
		jpeg_layout.chunks = ((u32)bytes + R360_VIDEO_PAYLOAD_MAX - 1) / R360_VIDEO_PAYLOAD_MAX;
		frame_data = jpeg_output;
	}
#endif
	g_r360.last_encode_ms = (u32)(xa_now_ms() - stamp);
	stamp = xa_now_ms();
#endif
#ifdef R360_CAPTURE_PROFILE
	u32 profile_after_encode = capture_ticks();
#endif
	frame_id = ++video_frame_id;
	u32 chunk = 0;
	u32 group = 0;
	u32 groups = (layout->chunks + R360_VIDEO_PARITY_GROUP - 1) / R360_VIDEO_PARITY_GROUP;
	u32 total_packets = layout->chunks + (job.parity ? groups : 0);
	u32 burst = R360_VIDEO_PACKETS_PER_BURST;
#ifdef R360_VIDEO_SETTINGS
	if (layout == &jpeg_layout)
		burst = burst_limit;
#endif
	for (u32 sent = 0; sent < total_packets; sent++) {
		if (sent && sent % burst == 0)
			xa_sleep_ms(1);
		if (!(sent & 7)) {
#ifdef R360_LOW_LATENCY
			if (fast ? !service_fast_job(&job) : !service_video_job(&job))
				return;
#else
			if (!service_video_job(&job))
				return;
#endif
		}
		int size;
		if (job.parity && chunk == MIN((group + 1) * R360_VIDEO_PARITY_GROUP, (u32)layout->chunks))
			size = r360_layout_parity_packet(packet, layout, frame_data, frame_id, group++);
		else
			size = r360_layout_packet(packet, layout, frame_data, frame_id, chunk++);
		while (net.sendto(R360_XNCALLER_SYSAPP, socket, (const char *)packet, size, 0,
				   (const struct r360_sockaddr *)&job.destination, sizeof(job.destination)) != size) {
			u32 error = (u32)net.last_error(R360_XNCALLER_SYSAPP);
			g_r360.last_wsa_error = error;
#ifdef R360_VIDEO_SETTINGS
			if (layout == &jpeg_layout && burst_limit > 40 &&
			    (error == R360_WSAEWOULDBLOCK || error == R360_WSAENOBUFS)) {
				burst_limit = burst_limit >= 56 ? burst_limit - 16 : 40;
				burst = burst_limit;
				g_r360.jpeg_burst = burst_limit;
			}
#endif
			if (!retries_left || (error != R360_WSAEWOULDBLOCK && error != R360_WSAENOBUFS)) {
				g_r360.send_errors++;
				g_r360.video_errors++;
				return;
			}
			retries_left--;
			xa_sleep_ms(1);
			if (!service_video_job(&job))
				return;
		}
	}
	g_r360.video_frames++;
#ifdef R360_EXPERIMENTAL_MEDIA
	g_r360.last_send_ms = (u32)(xa_now_ms() - stamp);
	if (layout->packet_type == R360_PACKET_VIDEO_BC1)
		g_r360.bc1_frames++;
#endif
#ifdef R360_LOW_LATENCY
	if (fast && layout->packet_type == R360_PACKET_VIDEO_BC1) {
		g_r360.fast_frames++;
#ifdef R360_CAPTURE_PROFILE
		u32 profile_after_send = capture_ticks();
		g_r360.profile_surface_address = job.surface_address;
		g_r360.profile_display_us = capture_us(job.display_ticks);
		g_r360.profile_mapping_us = capture_us(job.mapping_ticks);
		g_r360.profile_pixels_us = capture_us(job.pixel_ticks);
		g_r360.profile_input_us = capture_us(profile_input_ticks);
		g_r360.profile_capture_us = capture_us(profile_after_capture - profile_start);
		g_r360.profile_encode_us = capture_us(profile_after_encode - profile_after_capture);
		g_r360.profile_send_us = capture_us(profile_after_send - profile_after_encode);
		if (job.reused_mapping)
			g_r360.profile_map_reused++;
		else
			g_r360.profile_map_full++;
		g_r360.profile_frames++;
#endif
#ifdef R360_FAST_SCAN
		if (job.reconstruct_rows)
			g_r360.profile_reconstructed_frames++;
#endif
		u32 time = g_r360.last_capture_ms + g_r360.last_encode_ms + g_r360.last_send_ms;
		u32 budget = 2000 / r360_rates[video_rate];
		if (layout->width > 320 && time > budget) {
			if (++slow_fast_frames >= 2) {
				u8 profile = r360_profile_for_source(video_profile, layout->width, layout->height);
				capture_max_profile = profile - 1;
				slow_fast_frames = 0;
				g_r360.fast_drops++;
			}
		} else {
			slow_fast_frames = 0;
		}
	}
#endif
#ifdef R360_VIDEO_SETTINGS
	g_r360.stream_width = layout->width;
	g_r360.stream_height = layout->height;
	if (layout == &jpeg_layout) {
		g_r360.jpeg_frames++;
		g_r360.jpeg_bytes += layout->bytes;
		adapt_jpeg_burst(xa_now_ms());
	}
#endif
}

static int make_socket(void)
{
	struct r360_sockaddr_in local;
	u32 one = 1;
	u32 socket = net.socket(R360_XNCALLER_SYSAPP, R360_AF_INET, R360_SOCK_DGRAM,
				R360_IPPROTO_UDP);

	if (socket == R360_INVALID_SOCKET)
		return -R360_ERR_SOCKET;

	/* Retail DashLaunch/OpenXBDM needs an insecure SystemApp socket for LAN peers. */
	if (net.setsockopt(R360_XNCALLER_SYSAPP, socket, R360_SOL_SOCKET, R360_SO_INSECURE,
			   (const char *)&one, sizeof(one)) == R360_SOCKET_ERROR) {
		g_r360.last_wsa_error = (u32)net.last_error(R360_XNCALLER_SYSAPP);
		net.closesocket(R360_XNCALLER_SYSAPP, socket);
		return -R360_ERR_INSECURE;
	}
	net.setsockopt(R360_XNCALLER_SYSAPP, socket, R360_SOL_SOCKET, R360_SO_REUSEADDR,
		       (const char *)&one, sizeof(one));

	memset(&local, 0, sizeof(local));
	local.sin_family = R360_AF_INET;
	local.sin_port = R360_PORT;
	if (net.bind(R360_XNCALLER_SYSAPP, socket, (const struct r360_sockaddr *)&local,
		     sizeof(local)) == R360_SOCKET_ERROR) {
		g_r360.last_wsa_error = (u32)net.last_error(R360_XNCALLER_SYSAPP);
		net.closesocket(R360_XNCALLER_SYSAPP, socket);
		return -R360_ERR_BIND;
	}
	if (net.ioctlsocket(R360_XNCALLER_SYSAPP, socket, R360_FIONBIO, &one) == R360_SOCKET_ERROR) {
		g_r360.last_wsa_error = (u32)net.last_error(R360_XNCALLER_SYSAPP);
		net.closesocket(R360_XNCALLER_SYSAPP, socket);
		return -R360_ERR_NONBLOCK;
	}
	return (int)socket;
}

static void accept_packet(const u8 *p, const struct r360_sockaddr_in *from)
{
	u32 seq = get_le32(p + 8);
	u64 now = xa_now_ms();

	if (peer_valid && !same_peer(&peer, from) && now - last_input_ms < PEER_TAKEOVER_MS) {
		g_r360.foreign_packets++;
		return;
	}
	if (!peer_valid || !same_peer(&peer, from) || now - last_input_ms >= PEER_TAKEOVER_MS) {
		neutral_pad();
		peer = *from;
		peer_valid = true;
		have_seq = false;
		g_r360.client_frames_displayed = 0;
		g_r360.client_last_frame = 0;
		g_r360.input_non_neutral = 0;
		g_r360.input_buttons_seen = 0;
		g_r360.input_axes_seen = 0;
#ifdef R360_VIDEO_SETTINGS
		video_config_generation++;
		next_video_due = 0;
		reset_video_limits();
#endif
	}
	if (have_seq && !seq_is_newer(seq, g_r360.last_seq)) {
		g_r360.stale_packets++;
		return;
	}
	have_seq = true;
	g_r360.last_seq = seq;
	g_r360.peer_ip = peer.sin_addr.s_addr;
	g_r360.peer_port = peer.sin_port;
	g_r360.packets_rx++;
	video_requested = (p[6] & R360_INPUT_FLAG_VIDEO) != 0;
	video_status_requested = (p[6] & R360_INPUT_FLAG_VIDEO_STATUS) != 0;
	video_test_pattern = video_requested && (p[6] & R360_INPUT_FLAG_TEST_PATTERN) != 0;
	video_parity_requested = video_requested && (p[6] & R360_INPUT_FLAG_VIDEO_PARITY) != 0;
#ifdef R360_EXPERIMENTAL_MEDIA
	video_bc1_requested = video_requested && !video_test_pattern && (p[6] & R360_INPUT_FLAG_VIDEO_BC1) != 0;
#endif
#ifdef R360_VIDEO_SETTINGS
	bool configurable = video_requested && (p[6] & R360_INPUT_FLAG_VIDEO_CONFIG) != 0;
	u8 profile = configurable ? p[7] & 15 : 0;
	u8 rate = configurable ? p[7] >> 4 : R360_DEFAULT_RATE_INDEX;
#ifdef R360_LOW_LATENCY
	bool fast = configurable && (p[6] & R360_INPUT_FLAG_VIDEO_FAST) != 0;
	bool mode_changed = fast != video_fast;
	video_fast = fast;
	g_r360.fast_mode = fast;
#else
	bool mode_changed = false;
#endif
	if (profile >= ARRAY_SIZE(r360_profiles))
		profile = 0;
	if (rate >= ARRAY_SIZE(r360_rates))
		rate = R360_DEFAULT_RATE_INDEX;
	if (mode_changed || configurable != video_configurable || profile != video_profile) {
		reset_video_limits();
	}
	if (mode_changed || configurable != video_configurable || profile != video_profile || rate != video_rate) {
		video_config_generation++;
		next_video_due = 0;
	}
	video_configurable = configurable;
	video_profile = profile;
	video_rate = rate;
	g_r360.target_fps = r360_rates[video_rate];
#endif
#ifdef R360_VIDEO_ONLY
	record_input(p);
#else
	bool active = record_input(p);
	if (g_r360.xinput_bound)
		apply_input(p);
	else if (g_r360.controller_phase == INPUT_IDLE && active)
		controller_requested = true;
#endif
	last_input_ms = now;
	watchdog_neutral = false;
	g_r360.state = R360_STATE_CLIENT_ACTIVE;
}

/* Keep controller input and its watchdog serviced during video capture and sends. */
static void service_input(u32 socket)
{
	u8 buffer[64];
	for (int drained = 0; drained < 8; drained++) {
		struct r360_sockaddr_in from;
		int from_len = sizeof(from);
		int n = net.recvfrom(R360_XNCALLER_SYSAPP, socket, (char *)buffer,
				     sizeof(buffer), 0, (struct r360_sockaddr *)&from, &from_len);
		if (n == R360_SOCKET_ERROR)
			break;
		if (from_len != sizeof(from)) {
			g_r360.invalid_packets++;
			continue;
		}
		if (packet_header(buffer, n, R360_PACKET_INPUT, R360_INPUT_SIZE)) {
			accept_packet(buffer, &from);
		} else if (packet_header(buffer, n, R360_PACKET_VIDEO_FEEDBACK, R360_VIDEO_FEEDBACK_SIZE)) {
			if (peer_valid && video_requested && same_peer(&peer, &from)) {
				u32 displayed = get_le32(buffer + 12);
				if (seq_is_newer(displayed, g_r360.client_frames_displayed)) {
					g_r360.client_frames_displayed = displayed;
					g_r360.client_last_frame = get_le32(buffer + 8);
#ifdef R360_VIDEO_SETTINGS
					last_feedback_ms = xa_now_ms();
#endif
				}
			}
		} else {
			g_r360.invalid_packets++;
		}
	}
	if (!watchdog_neutral && xa_now_ms() - last_input_ms >= WATCHDOG_MS) {
		neutral_pad();
		watchdog_neutral = true;
		video_requested = false;
		video_test_pattern = false;
		video_parity_requested = false;
		video_bc1_requested = false;
#ifdef R360_VIDEO_SETTINGS
		video_configurable = false;
		next_video_due = 0;
#endif
#ifdef R360_LOW_LATENCY
		video_fast = false;
		g_r360.fast_mode = 0;
#endif
		video_status_requested = false;
		g_r360.watchdog_trips++;
		g_r360.state = R360_STATE_LISTENING;
	}
#ifndef R360_VIDEO_ONLY
	u8 left, right;
	if (g_r360.xinput_bound && xa_xinput_take_rumble(0, &left, &right) && !watchdog_neutral)
		send_rumble(socket, left, right);
#endif
}

__attribute__((used, noinline)) u32 R360NetThreadC(void *context)
{
	int sock;
	(void)context;

	if (!r360_kernel_supported()) {
		g_r360.last_error = -R360_ERR_UNSUPPORTED_KERNEL;
		g_r360.state = R360_STATE_FAILED;
		ExTerminateThread(0);
		return 0;
	}
#ifndef R360_VIDEO_ONLY
	if (conflicting_controller_driver()) {
		g_r360.last_error = -R360_ERR_INPUT_CONFLICT;
		g_r360.controller_phase = INPUT_CONFLICT;
	}
#endif

	if (!resolve_network()) {
		g_r360.last_error = -R360_ERR_NET_EXPORT;
		g_r360.state = R360_STATE_FAILED;
		ExTerminateThread(0);
		return 0;
	}
	if (!resolve_video())
		g_r360.video_errors++;
	sock = make_socket();
	if (sock < 0) {
		g_r360.last_error = sock;
		g_r360.state = R360_STATE_FAILED;
		ExTerminateThread(0);
		return 0;
	}
	g_r360.socket = (u32)sock;
	g_r360.state = R360_STATE_LISTENING;
	last_input_ms = xa_now_ms();
	last_status_ms = last_input_ms;
	last_video_ms = last_input_ms;
#ifdef R360_PARALLEL_BC1
	start_encoder_thread();
#endif

	for (;;) {
		service_input((u32)sock);
#ifdef R360_PARALLEL_BC1
		/* Do not free or reuse the worker's buffers after an overrun. */
		if (!finish_parallel_encode(0)) {
			g_r360.heartbeat++;
			xa_sleep_ms(1);
			continue;
		}
#endif

		{
			u64 now = xa_now_ms();
#ifndef R360_VIDEO_ONLY
			service_controller(now);
#endif
			if (peer_valid && now - last_status_ms >= STATUS_INTERVAL_MS) {
				send_status((u32)sock);
				if (video_status_requested)
					send_video_status((u32)sock);
			}
			u32 video_delay = R360_VIDEO_INTERVAL_MS;
			if (g_r360.last_capture_result == R360_CAPTURE_BUSY ||
			    g_r360.last_capture_result == R360_CAPTURE_CHANGED)
				video_delay = VIDEO_RETRY_MS;
			bool due = now - last_video_ms >= video_delay;
#ifdef R360_VIDEO_SETTINGS
			if (video_configurable) {
				u64 tick = now * r360_rates[video_rate];
				if (!next_video_due || next_video_due > tick + 1000)
					next_video_due = tick;
				due = tick >= next_video_due;
				if (due) {
					/* Maintain a fractional cadence while on time, and skip
					 * elapsed slots when capture or transmission falls behind. */
					if (tick - next_video_due >= 1000)
						next_video_due = tick;
					next_video_due += 1000;
				}
			}
			if (!video_requested && now - last_video_ms >= 5000)
				clear_jpeg_buffers();
#endif
			if (peer_valid && video_requested && !watchdog_neutral && due) {
				last_video_ms = now;
				send_video_frame((u32)sock);
#ifdef R360_LOW_LATENCY
				if (video_fast) {
					g_r360.heartbeat++;
					NtYieldExecution();
					continue;
				}
#endif
			}
		}
		g_r360.heartbeat++;
		xa_sleep_ms(2);
	}
}

static void start(void)
{
	HANDLE thread;
	u32 tid;
	g_r360.state = R360_STATE_STARTING;
	NTSTATUS status = ExCreateThread(&thread, THREAD_STACK, &tid, 0,
					(void *)R360NetThread, 0, THREAD_FLAGS);
	if (status < 0) {
		g_r360.last_error = status;
		g_r360.state = R360_STATE_FAILED;
	} else {
		NtClose(thread);
	}
}

__attribute__((used, noinline)) int DllMainC(unsigned int handle, unsigned int reason,
					     unsigned int reserved)
{
	(void)handle;
	(void)reserved;
	if (reason == 1)
		start();
	return 1;
}

