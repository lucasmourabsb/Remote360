#!/usr/bin/env python3
"""Loopback Remote360 simulator for Android emulator and host tests."""

import argparse
import ctypes
import json
from pathlib import Path
import select
import socket
import struct
import subprocess
import tempfile
import time

from send_input import PORT

ROOT = Path(__file__).resolve().parents[1]
WRAPPER = """
#include "protocol/video_packets.h"
static u8 frame[R360_VIDEO_BYTES];
static struct r360_video_layout jpeg;
static const u8 *pixels = frame;
static const struct r360_video_layout *layout = &r360_rgb565;
static struct r360_packet_order order;
void pattern(u32 id) { if (layout == &r360_rgb565) r360_video_test_pattern(frame, id); }
void set_jpeg(const u8 *data, u32 size, u16 width, u16 height) {
    jpeg = (struct r360_video_layout){width, height,
        (size + R360_VIDEO_PAYLOAD_MAX - 1) / R360_VIDEO_PAYLOAD_MAX,
        size, R360_PACKET_VIDEO_JPEG, R360_PACKET_VIDEO_JPEG_PARITY};
    layout = size ? &jpeg : &r360_rgb565;
    pixels = size ? data : frame;
}
int data(u8 *packet, u32 id, u32 index) { return r360_layout_packet(packet, layout, pixels, id, index); }
int parity(u8 *packet, u32 id, u32 group) { return r360_layout_parity_packet(packet, layout, pixels, id, group); }
u32 chunk_count(void) { return layout->chunks; }
u32 group_count(void) { return (layout->chunks + R360_VIDEO_PARITY_GROUP - 1) / R360_VIDEO_PARITY_GROUP; }
u32 group_size(void) { return R360_VIDEO_PARITY_GROUP; }
void start_order(bool parity) { r360_packet_order_init(&order, layout->chunks, parity); }
int ordered_packet(u8 *packet, u32 id) {
    u32 index;
    bool parity;
    if (!r360_packet_order_next(&order, &index, &parity)) return 0;
    return parity ? r360_layout_parity_packet(packet, layout, pixels, id, index)
                  : r360_layout_packet(packet, layout, pixels, id, index);
}
"""
MODES = ("clean", "loss", "double_loss", "late_parity", "stall", "capture_error", "burst", "restart", "offline")
PROFILES = ((320, 180), (512, 288), (640, 360), (960, 540), (1280, 720), (1920, 1080))
RATES = (15, 24, 30, 60)


def build_codec(directory):
    source = Path(directory) / "encoder.c"
    binary = Path(directory) / "encoder.so"
    source.write_text(WRAPPER)
    subprocess.run(
        ["clang", "-shared", "-fPIC", "-O2", "-Wall", "-Werror", "-I", str(ROOT), str(source), "-o", str(binary)],
        check=True,
    )
    lib = ctypes.CDLL(str(binary))
    lib.pattern.argtypes = (ctypes.c_uint32,)
    lib.pattern.restype = None
    lib.set_jpeg.argtypes = (ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint16, ctypes.c_uint16)
    lib.set_jpeg.restype = None
    for function in (lib.data, lib.parity):
        function.argtypes = (ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32)
        function.restype = ctypes.c_int
    lib.start_order.argtypes = (ctypes.c_bool,)
    lib.start_order.restype = None
    lib.ordered_packet.argtypes = (ctypes.c_void_p, ctypes.c_uint32)
    lib.ordered_packet.restype = ctypes.c_int
    return lib


def ordered_packets(codec, frame, parity):
    codec.pattern(frame)
    codec.start_order(parity)
    buffer = ctypes.create_string_buffer(1204)
    while (length := codec.ordered_packet(buffer, frame)) > 0:
        yield buffer.raw[:length]


def packets(codec, frame, parity, mode):
    if mode in ("clean", "burst", "restart"):
        yield from ordered_packets(codec, frame, parity)
        return
    codec.pattern(frame)
    buffer = ctypes.create_string_buffer(1204)
    size = codec.group_size()
    count = codec.chunk_count()
    groups = codec.group_count()
    for group in range(groups):
        first = group * size
        last = min(first + size, count)
        lost = min(2 if mode == "double_loss" else 1, last - first) if mode in ("loss", "double_loss") else 0
        if mode == "late_parity" and parity and count > 1 and group + 1 == groups:
            lost = 1
        for index in range(first, last - lost):
            length = codec.data(buffer, frame, index)
            yield buffer.raw[:length]
        if parity:
            length = codec.parity(buffer, frame, group)
            yield buffer.raw[:length]


def run(codec, port, control, seconds, report_interval, fixtures):
    started = time.monotonic()
    peer = None
    flags = last_seq = received = frames = capture_errors = frame = 0
    console_frames = 0
    displayed = last_displayed = 0
    last_rx = 0
    settings = (-1, -1)
    width, height, fps = 320, 180, 10
    next_frame = next_status = next_control = started
    report_at = started + report_interval
    mode = "clean"
    sequence = ()
    held_parity = None
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as server:
        server.bind(("127.0.0.1", port))
        server.setblocking(False)
        print(json.dumps({"ready": True, "port": port, "mode": mode}), flush=True)
        while (now := time.monotonic()) - started < seconds:
            if now >= next_control:
                try:
                    candidate = control.read_text().strip() if control else "clean"
                    if candidate in MODES and candidate != mode:
                        mode = candidate
                        held_parity = None
                        if mode == "restart":
                            frame = console_frames = 0
                        print(json.dumps({"mode": mode, "elapsed": round(now - started, 2)}), flush=True)
                except FileNotFoundError:
                    pass
                next_control = now + 0.1
            if select.select([server], [], [], 0.002)[0]:
                for _ in range(16):
                    try:
                        packet, sender = server.recvfrom(256)
                    except BlockingIOError:
                        break
                    if len(packet) == 28 and packet[:6] == b"R360\x01\x01":
                        if peer != sender:
                            displayed = last_displayed = 0
                            held_parity = None
                        peer = sender
                        flags = packet[6]
                        if fixtures and flags & 0x40 and flags & 2:
                            requested = packet[7] & 15, packet[7] >> 4
                        else:
                            requested = (-1, -1)
                        if settings != requested:
                            settings = requested
                            profile, rate = requested
                            fps = RATES[rate] if 0 <= rate < len(RATES) else 10
                            if profile in fixtures:
                                width, height = PROFILES[profile]
                                sequence = fixtures[profile]
                            else:
                                width, height = PROFILES[0]
                                sequence = ()
                            codec.set_jpeg(None, 0, width, height)
                            held_parity = None
                            next_frame = now
                            print(json.dumps({"resolution": [width, height], "target_fps": fps,
                                              "input_profile": profile, "input_rate": rate}), flush=True)
                        last_seq = struct.unpack_from("<I", packet, 8)[0]
                        received += 1
                        last_rx = now
                    elif sender == peer and len(packet) == 16 and packet[:6] == b"R360\x01\x07":
                        frame_id, count = struct.unpack_from("<II", packet, 8)
                        if count > displayed:
                            last_displayed, displayed = frame_id, count
            if now >= report_at:
                print(json.dumps({"elapsed": round(now - started, 2), "mode": mode, "input": received,
                                  "resolution": [width, height], "target_fps": fps,
                                  "frames_sent": frames, "capture_errors": capture_errors,
                                  "frames_displayed": displayed, "last_displayed_id": last_displayed}), flush=True)
                report_at = now + report_interval
            if peer is None or now - last_rx > 0.4 or mode == "offline":
                continue
            if now >= next_status:
                clock = int(now * 1000) & 0xFFFFFFFF
                server.sendto(struct.pack("<4sBBBBIIII", b"R360", 1, 2, 4, 4, last_seq, received, 0, clock), peer)
                if flags & 8:
                    server.sendto(struct.pack("<4sBBBBIIIIIHHIHH", b"R360", 1, 5,
                                              2 if mode == "capture_error" else 0,
                                              (5 if flags & 2 else 4) | (8 if fixtures else 0),
                                              last_seq, console_frames, capture_errors,
                                              0, 0, width, height, 2, width, 0), peer)
                next_status = now + 0.25
            if now >= next_frame and flags & 2:
                next_frame = max(next_frame + 1 / fps, now)
                if mode == "capture_error":
                    capture_errors += 1
                if mode in ("stall", "capture_error"):
                    continue
                frame += 1
                if sequence:
                    fixture = sequence[frame % len(sequence)]
                    codec.set_jpeg(fixture, len(fixture), width, height)
                for index, packet in enumerate(packets(codec, frame, bool(flags & 0x10), mode)):
                    burst_size = 48 if fixtures else 24
                    if mode != "burst" and index and index % burst_size == 0:
                        time.sleep(0.0015 if fixtures else 0.004)
                    if (mode == "late_parity" and codec.chunk_count() > 1 and packet[5] in (6, 11)
                            and struct.unpack_from("<H", packet, 10)[0] + 1 == codec.group_count()):
                        held_parity = packet
                        continue
                    server.sendto(packet, peer)
                    if index == 0 and held_parity is not None:
                        server.sendto(held_parity, peer)
                        held_parity = None
                frames += 1
                console_frames += 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=PORT)
    parser.add_argument("--seconds", type=float, default=120)
    parser.add_argument("--report-interval", type=float, default=20)
    parser.add_argument("--control", type=Path, help="file containing one of: " + ", ".join(MODES))
    parser.add_argument("--jpeg-dir", type=Path, help="WIDTHxHEIGHT.jpg or WIDTHxHEIGHT-*.jpg test frames")
    args = parser.parse_args()
    if args.seconds <= 0 or args.report_interval <= 0:
        parser.error("seconds and report interval must be positive")
    fixtures = {}
    if args.jpeg_dir:
        for profile, (width, height) in enumerate(PROFILES[2:], start=2):
            path = args.jpeg_dir / f"{width}x{height}.jpg"
            paths = ([path] if path.is_file() else []) + sorted(args.jpeg_dir.glob(f"{width}x{height}-*.jpg"))
            if paths:
                fixtures[profile] = tuple(path.read_bytes() for path in paths)
        if not fixtures:
            parser.error("JPEG test frame directory has no known resolutions")
    with tempfile.TemporaryDirectory(prefix="remote360-codec-") as directory:
        run(build_codec(directory), args.port, args.control, args.seconds, args.report_interval, fixtures)


if __name__ == "__main__":
    main()
