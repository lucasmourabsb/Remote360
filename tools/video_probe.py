#!/usr/bin/env python3
"""Measure Remote360 video from a console, with an optional GPU-independent test."""

import argparse
import hashlib
import json
import select
import socket
import struct
import time
import zlib
from pathlib import Path

from send_input import MAGIC, PORT, VERSION, input_packet

HEADER = struct.Struct("<4sBBHHHHIH")
STATUS = struct.Struct("<4sBBBBIIIIIHHIHH")
PAYLOAD = 1180
GROUP = 8
RESULTS = ("ok", "no_display", "unsupported_format", "unmapped", "gpu_busy", "changed", "cancelled",
           "encoder_limit")
PROFILES = ((320, 180), (512, 288), (640, 360), (960, 540), (1280, 720), (1920, 1080))
RATES = (15, 24, 30, 60)


class FrameReceiver:
    def __init__(self):
        self.frame_id = None
        self.geometry = None
        self.parts = {}
        self.parity = {}
        self.finished = False
        self.discarded = 0
        self.recovered = 0

    def accept(self, packet):
        if len(packet) < HEADER.size:
            return None
        magic, version, kind, width, height, index, count, frame_id, length = HEADER.unpack_from(packet)
        header_size = HEADER.size
        if kind in (10, 11):
            header_size += 4
            if len(packet) < header_size or (width, height) not in PROFILES[2:]:
                return None
            frame_size, = struct.unpack_from("<I", packet, HEADER.size)
            if not 4 <= frame_size <= 384 * 1024:
                return None
            codec = "jpeg"
        elif kind in (8, 9):
            if (width, height) not in PROFILES[:3]:
                return None
            frame_size = width * height // 2
            codec = "bc1"
        else:
            frame_size = width * height * 2
            codec = "rgb565"
        is_parity = kind in (6, 9, 11)
        if ((magic, version) != (MAGIC, VERSION) or kind not in (4, 6, 8, 9, 10, 11)
                or not (1 <= width <= 1920 and 1 <= height <= 1080)
                or not 1 <= count <= 512 or count != (frame_size + PAYLOAD - 1) // PAYLOAD
                or index >= ((count + GROUP - 1) // GROUP if is_parity else count)):
            return None
        expected = PAYLOAD if is_parity else min(PAYLOAD, frame_size - index * PAYLOAD)
        if length != expected or len(packet) != header_size + length:
            return None
        geometry = (width, height, count, codec, frame_size)
        if frame_id != self.frame_id:
            if self.frame_id is not None:
                if not 0 < (frame_id - self.frame_id) % (1 << 32) < (1 << 31):
                    return None
                if not self.finished:
                    self.discarded += 1
            self.frame_id = frame_id
            self.geometry = geometry
            self.parts.clear()
            self.parity.clear()
            self.finished = False
        if self.finished or self.geometry != geometry:
            return None
        destination = self.parity if is_parity else self.parts
        if index in destination:
            return None
        destination[index] = packet[header_size:]
        group = index if is_parity else index // GROUP
        if group in self.parity:
            members = range(group * GROUP, min((group + 1) * GROUP, count))
            missing = [i for i in members if i not in self.parts]
            if len(missing) == 1:
                value = int.from_bytes(self.parity[group], "little")
                for member in members:
                    if member != missing[0]:
                        value ^= int.from_bytes(self.parts[member], "little")
                length = min(PAYLOAD, frame_size - missing[0] * PAYLOAD)
                self.parts[missing[0]] = value.to_bytes(PAYLOAD, "little")[:length]
                self.recovered += 1
        if len(self.parts) == count:
            self.finished = True
            return width, height, frame_id, b"".join(self.parts[i] for i in range(count))
        return None


def parse_video_status(packet):
    if len(packet) != STATUS.size or packet[:6] != b"R360\x01\x05":
        return None
    (_, _, _, result, flags, ack, frames, capture_errors, send_errors,
     wsa_error, width, height, control, pitch, swap) = STATUS.unpack(packet)
    return {
        "capture_result": RESULTS[result] if result < len(RESULTS) else result,
        "video_requested": bool(flags & 1),
        "test_pattern": bool(flags & 2),
        "configurable": bool(flags & 8), "source_limited": bool(flags & 16),
        "encoder_limited": bool(flags & 32), "capture_limited": bool(flags & 64),
        "ack": ack, "frames_sent": frames, "capture_errors": capture_errors,
        "send_errors": send_errors, "last_socket_error": wsa_error,
        "source_width": width, "source_height": height,
        "display_control": f"0x{control:08x}", "pitch_pixels": pitch, "display_swap": f"0x{swap:04x}",
    }


def write_png(path, frame):
    width, height, _, pixels = frame
    scan = bytearray()
    for i, (pixel,) in enumerate(struct.iter_unpack("<H", pixels)):
        if i % width == 0:
            scan.append(0)
        scan.extend((((pixel >> 11) & 31) * 255 // 31,
                     ((pixel >> 5) & 63) * 255 // 63, (pixel & 31) * 255 // 31))

    def chunk(name, data):
        return struct.pack(">I", len(data)) + name + data + struct.pack(">I", zlib.crc32(name + data))

    path.write_bytes(b"\x89PNG\r\n\x1a\n"
                     + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
                     + chunk(b"IDAT", zlib.compress(scan)) + chunk(b"IEND", b""))


def matches_pattern(frame):
    width, height, frame_id, pixels = frame
    if (width, height) != (320, 180):
        return False
    colors = (0xffff, 0xffe0, 0x07ff, 0x07e0, 0xf81f, 0xf800, 0x001f, 0x0000)
    row = b"".join(color.to_bytes(2, "little") * 40 for color in colors)
    marker = (frame_id % 80) * 4
    bottom = b"\x04\x21" * marker + b"\xff\xff" * 4 + b"\x04\x21" * (316 - marker)
    return pixels == row * 156 + bottom * 24


def save_frame(path, frame, codec):
    if codec == "rgb565":
        write_png(path, frame)
    elif codec == "jpeg" and path.suffix.lower() in (".jpeg", ".jpg"):
        path.write_bytes(frame[3])
    else:
        # Optional host-only dependency for PNG conversion of compressed samples.
        from io import BytesIO
        from PIL import Image
        if codec == "bc1":
            width, height = frame[:2]
            header = [124, 0x81007, height, width, len(frame[3]), 0, 0] + [0] * 11
            header += [32, 4, 0x31545844, 0, 0, 0, 0, 0] + [0x1000, 0, 0, 0, 0]
            data = b"DDS " + struct.pack("<31I", *header) + frame[3]
        else:
            data = frame[3]
        with Image.open(BytesIO(data)) as decoded:
            decoded.convert("RGB").save(path)


def probe(host, seconds, pattern=False, parity=True, sample=None, profile=None, fps=30, fast=False):
    receiver = FrameReceiver()
    counts = {"status": 0, "data": 0, "parity": 0, "frames": 0}
    latest = None
    latest_codec = None
    first_status = last_status = None
    unique = set()
    pattern_matches = 0
    start = next_send = time.monotonic()
    sequence = 0
    flags = 0x0A | (4 if pattern else 0) | (0x10 if parity else 0)
    config = 0
    if profile is not None:
        flags |= 0x40 | (0x20 if profile == 1 else 0) | (0x80 if fast else 0)
        config = profile | RATES.index(fps) << 4
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as client:
        client.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 512 * 1024)
        client.connect((host, PORT))
        client.setblocking(False)
        while (now := time.monotonic()) < start + seconds:
            if now >= next_send:
                try:
                    client.send(input_packet(sequence, video_flags=flags, video_config=config))
                except ConnectionRefusedError:
                    pass
                sequence += 1
                next_send = now + 0.025
            if not select.select([client], [], [], max(0, next_send - time.monotonic()))[0]:
                continue
            try:
                packet = client.recv(1500)
            except (BlockingIOError, ConnectionRefusedError):
                continue
            if packet[:5] != b"R360\x01" or len(packet) < 6:
                continue
            kind = packet[5]
            if kind == 2 and len(packet) == 24:
                counts["status"] += 1
            elif kind == 5 and (status := parse_video_status(packet)) is not None:
                if first_status is None:
                    first_status = status
                last_status = status
            elif kind in (4, 6, 8, 9, 10, 11):
                counts["parity" if kind in (6, 9, 11) else "data"] += 1
                if (frame := receiver.accept(packet)) is not None:
                    counts["frames"] += 1
                    unique.add(hashlib.blake2s(frame[3], digest_size=8).digest())
                    if pattern and matches_pattern(frame):
                        pattern_matches += 1
                    latest = frame
                    latest_codec = receiver.geometry[3]
        try:
            client.send(input_packet(sequence))
        except (BlockingIOError, ConnectionRefusedError):
            pass
    if sample and latest:
        save_frame(sample, latest, latest_codec)
    summary = {
        **counts, "discarded_frames": receiver.discarded, "recovered_chunks": receiver.recovered,
        "distinct_frames": len(unique), "fps": round(counts["frames"] / seconds, 2),
        "resolution": list(latest[:2]) if latest else None, "codec": latest_codec,
        "sample": str(sample) if sample and latest else None,
        "first_console_status": first_status, "last_console_status": last_status,
    }
    if pattern:
        summary["matching_test_pattern_frames"] = pattern_matches
        summary["unexpected_test_pattern_frames"] = counts["frames"] - pattern_matches
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("xbox_ip")
    parser.add_argument("--seconds", type=float, default=8)
    parser.add_argument("--pattern", action="store_true", help="request the console's animated test bars")
    parser.add_argument("--no-parity", action="store_true")
    parser.add_argument("--sample", type=Path, help="save the last complete frame")
    resolution_labels = [f"{width}x{height}" for width, height in PROFILES]
    parser.add_argument("--resolution", choices=resolution_labels)
    parser.add_argument("--fps", type=int, choices=RATES, default=30)
    parser.add_argument("--fast", action="store_true", help="request the low latency BC1 profiles")
    args = parser.parse_args()
    if not 0 < args.seconds <= 300:
        parser.error("--seconds must be in (0, 300]")
    profile = resolution_labels.index(args.resolution) if args.resolution else (0 if args.fast else None)
    print(json.dumps(probe(args.xbox_ip, args.seconds, args.pattern, not args.no_parity,
                           args.sample, profile, args.fps, args.fast), indent=2))


if __name__ == "__main__":
    main()
