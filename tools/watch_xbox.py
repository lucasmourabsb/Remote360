#!/usr/bin/env python3
"""Read Remote360 diagnostics through XBDM without pausing the running title."""

import argparse
import ipaddress
import json
import re
import socket
import struct
import time


FIELDS = """magic version state error heartbeat socket rx invalid stale foreign
watchdogs seq peer port wsa bound sent errors width height format capture_errors
send_errors capture_result display_control pitch swap displayed last_frame
input_packets buttons axes phase player capture_ms encode_ms send_ms bc1_frames rumble_packets
stream_width stream_height target_fps jpeg_frames jpeg_bytes encoder_errors jpeg_burst capture_limit
fast_frames fast_mode fast_drops fast_partial
profile_frames profile_surface_address profile_display_us profile_mapping_us profile_pixels_us
profile_input_us profile_capture_us profile_encode_us profile_send_us
profile_map_reused profile_map_full profile_reconstructed_frames
parallel_ready parallel_frames parallel_overruns parallel_worker_us parallel_main_us""".split()
PHASES = ("idle", "queuing", "binding", "ready", "conflict", "failed")


def read(f, s, command):
    s.sendall((command + "\r\n").encode("ascii"))
    header = f.readline().strip()
    if not header.startswith(b"202-"):
        raise ValueError(f"{command}: {header!r}")
    lines = []
    while True:
        line = f.readline().strip()
        if line == b".":
            return lines
        if not line:
            raise ConnectionError("truncated XBDM reply")
        lines.append(line)


def stats(f, s, address, count):
    lines = read(f, s, f"getmem addr=0x{address:x} length=0x{count * 4:x}")
    values = struct.unpack(f">{count}I", bytes.fromhex(b"".join(lines).decode("ascii")))
    if values[:2] != (0x52333630, 1):
        raise ValueError("not a Remote360 v1 status block")
    return dict(zip(FIELDS, values))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("xbox")
    parser.add_argument("kind", choices=("video", "monitor", "controls", "next", "settings", "latency",
                                         "latency-probe", "latency-scan", "latency-dual", "latency-dual2"))
    parser.add_argument("--seconds", type=int, default=20)
    args = parser.parse_args()
    variants = {
        "video": (b"remote360_video.xex", 0x91C05000, 29),
        "monitor": (b"remote360_monitor.xex", 0x91C05000, 34),
        "controls": (b"remote360_controls.xex", 0x91C07000, 34),
        "next": (b"remote360_next.xex", 0x91C08000, 39),
        "settings": (b"remote360_settings.xex", 0x91C10000, 47),
        "latency": (b"remote360_latency.xex", 0x91C11000, 51),
        "latency-probe": (b"remote360_latency_probe.xex", 0x91C11000, 62),
        "latency-scan": (b"remote360_latency_scan.xex", 0x91C11000, 63),
        "latency-dual": (b"remote360_latency_dual.xex", 0x91C12000, len(FIELDS)),
        "latency-dual2": (b"remote360_latency_dual2.xex", 0x91C12000, len(FIELDS)),
    }
    name, address, count = variants[args.kind]
    with socket.create_connection((args.xbox, 730), 3) as s, s.makefile("rb") as f:
        f.readline()
        modules = read(f, s, "modules")
        if not any(re.match(rb'name="' + re.escape(name) + rb'" base=0x91c00000\b',
                            line, re.I) for line in modules):
            raise SystemExit(f"{name.decode()} is not loaded")
        start = time.monotonic()
        old = stats(f, s, address, count)
        then = time.monotonic()
        while time.monotonic() - start < args.seconds:
            time.sleep(1)
            new = stats(f, s, address, count)
            now = time.monotonic()
            dt = now - then
            report = {key: new[key] for key in
                      ("state", "bound", "width", "height", "capture_errors", "capture_result",
                       "send_errors", "rx")}
            report["peer"] = str(ipaddress.IPv4Address(new["peer"]))
            if "input_packets" in new:
                report.update(input_packets=new["input_packets"], buttons=hex(new["buttons"]),
                              axes=hex(new["axes"]), phase=PHASES[new["phase"]],
                              player=new["player"] + 1 if new["player"] < 4 else None)
            if "bc1_frames" in new:
                report.update({key: new[key] for key in
                               ("capture_ms", "encode_ms", "send_ms", "bc1_frames", "rumble_packets")})
            if "jpeg_frames" in new:
                report.update({key: new[key] for key in
                               ("stream_width", "stream_height", "target_fps", "encoder_errors",
                                "jpeg_burst", "capture_limit")})
                report["jpeg_kbps"] = round((new["jpeg_bytes"] - old["jpeg_bytes"]) * 8 / dt / 1000)
            if "fast_frames" in new:
                report.update({key: new[key] for key in ("fast_frames", "fast_mode", "fast_drops", "fast_partial")})
            if "profile_frames" in new and new["profile_frames"] != old["profile_frames"]:
                report.update({key: new[key] for key in FIELDS[51:63] if key in new})
                report["profile_surface_address"] = hex(new["profile_surface_address"])
            if "parallel_ready" in new:
                report.update({key: new[key] for key in FIELDS[63:]})
            report.update(fps_sent=round((new["sent"] - old["sent"]) / dt, 2),
                          fps_shown=(round((new["displayed"] - old["displayed"]) / dt, 2)
                                     if new["peer"] == old["peer"] and
                                     new["displayed"] >= old["displayed"] else None),
                          frames_sent=new["sent"], frames_shown=new["displayed"])
            print(json.dumps(report), flush=True)
            old, then = new, now


if __name__ == "__main__":
    main()
