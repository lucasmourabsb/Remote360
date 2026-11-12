#!/usr/bin/env python3
"""Read Remote360 diagnostics through XBDM without pausing the running title."""

import argparse
import ipaddress
import json
from pathlib import Path
import re
import socket
import statistics
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
parallel_ready parallel_frames parallel_overruns parallel_worker_us parallel_main_us
fused_frames fused_cancellations parallel_jpeg_frames
snapshot_attempts snapshot_accepted snapshot_recycled snapshot_copy_us snapshot_alloc_errors snapshot_cancelled
snapshot_load_stage""".split()
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


def status_address(linker_map):
    match = re.search(r"^\s*[\da-fA-F]+:[\da-fA-F]+\s+g_r360\s+([\da-fA-F]+)\s",
                      linker_map.read_text(), re.MULTILINE)
    if not match:
        raise ValueError(f"g_r360 not found in {linker_map}")
    return int(match[1], 16)


def summarize(rows):
    if not rows:
        return {}
    result = {"samples": len(rows), "received_profile": [rows[-1].get("stream_width"),
                                                        rows[-1].get("stream_height")]}
    for key in ("fps_sent", "fps_shown", "profile_capture_us", "profile_encode_us", "profile_send_us", "snapshot_copy_us"):
        samples = sorted(row[key] for row in rows if row.get(key) is not None)
        if samples:
            result[key] = {"min": samples[0], "median": statistics.median(samples), "max": samples[-1]}
    intervals = {}
    for row in rows:
        key = (row.get("stream_width", 0), row.get("stream_height", 0),
               row.get("target_fps"), row.get("fast_mode"))
        group = intervals.setdefault(key, [0., 0, 0., 0, 0])
        group[0] += row["seconds"]
        group[1] += row["sent_delta"]
        if row["shown_delta"] is not None:
            group[2] += row["seconds"]
            group[3] += row["shown_delta"]
        group[4] += row["sent_delta"] == 0
    result["by_last_sent_profile"] = [
        {"size": [width, height] if width and height else None,
         "target_fps": fps, "fast_mode": fast, "seconds": round(seconds, 2),
         "frames_sent": sent, "frames_shown": shown, "fps_sent": round(sent / seconds, 2),
         "fps_shown": round(shown / feedback, 2) if feedback else None,
         "intervals_without_sends": stalls}
        for (width, height, fps, fast), (seconds, sent, feedback, shown, stalls) in intervals.items()
    ]
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("xbox")
    parser.add_argument("kind", choices=("video", "monitor", "controls", "next", "settings", "latency",
                                         "latency-probe", "latency-scan", "latency-dual", "latency-dual2",
                                         "play", "preflight", "snapshot-check", "snapshot-32", "snapshot-32b",
                                         "snapshot-32c"))
    parser.add_argument("--seconds", type=int, default=20)
    parser.add_argument("--map", type=Path, help="linker map for the exact installed XEX")
    parser.add_argument("--summary", action="store_true", help="append a JSON summary after sampling")
    args = parser.parse_args()
    if args.seconds <= 0:
        parser.error("--seconds must be positive")
    variants = {
        "video": (b"remote360_video.xex", 0x91C05000, 29),
        "monitor": (b"remote360_monitor.xex", 0x91C05000, 34),
        "controls": (b"remote360_controls.xex", 0x91C07000, 34),
        "next": (b"remote360_next.xex", 0x91C08000, 39),
        "settings": (b"remote360_settings.xex", 0x91C10000, 47),
        "latency": (b"remote360_latency.xex", 0x91C11000, 51),
        "latency-probe": (b"remote360_latency_probe.xex", 0x91C11000, 62),
        "latency-scan": (b"remote360_latency_scan.xex", 0x91C11000, 63),
        "latency-dual": (b"remote360_latency_dual.xex", 0x91C12000, 68),
        "latency-dual2": (b"remote360_latency_dual2.xex", 0x91C12000, 68),
        "play": (b"remote360_play.xex", 0x91C12000, FIELDS.index("snapshot_attempts")),
        "snapshot-check": (b"remote360_snapshot_check.xex", 0x91C12000, len(FIELDS)),
        "snapshot-32": (b"remote360_snapshot32.xex", 0x91C12000, len(FIELDS)),
        "snapshot-32b": (b"remote360_snapshot32b.xex", 0x91C12000, len(FIELDS)),
        "snapshot-32c": (b"remote360_snapshot32c.xex", 0x91C12000, len(FIELDS)),
    }
    rows = []
    with socket.create_connection((args.xbox, 730), 3) as s, s.makefile("rb") as f:
        f.readline()
        modules = read(f, s, "modules")
        if args.kind == "preflight":
            active_title = read(f, s, "xbeinfo running")
            aurora = any(line.lower().endswith(b'\\aurora.xex"') for line in active_title)
            remote360 = [line.split(b'"', 2)[1].decode("ascii", "replace")
                         for line in modules if line.lower().startswith(b'name="remote360')]
            ready = aurora and not remote360
            print(json.dumps({"aurora": aurora, "remote360_loaded": remote360,
                              "ready": ready}))
            if not ready:
                raise SystemExit("Start from Aurora after a reboot; do not unload or swap the live plugin.")
            return
        name, address, count = variants[args.kind]
        if args.map:
            address = status_address(args.map)
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
                      ("state", "heartbeat", "bound", "width", "height", "capture_errors", "capture_result",
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
                report.update({key: new[key] for key in FIELDS[63:FIELDS.index("snapshot_attempts")] if key in new})
            if "snapshot_attempts" in new:
                report.update({key: new[key] for key in FIELDS[FIELDS.index("snapshot_attempts"):]
                               if key != "snapshot_copy_us" or new["snapshot_attempts"] != old["snapshot_attempts"]})
            sent = (new["sent"] - old["sent"]) & 0xFFFFFFFF
            shown = (new["displayed"] - old["displayed"]
                     if (new["peer"], new["port"]) == (old["peer"], old["port"]) and
                     new["displayed"] >= old["displayed"] and new["peer"] else None)
            report.update(seconds=dt, sent_delta=sent, shown_delta=shown,
                          fps_sent=round(sent / dt, 2),
                          fps_shown=round(shown / dt, 2) if shown is not None else None,
                          frames_sent=new["sent"], frames_shown=new["displayed"])
            print(json.dumps(report), flush=True)
            if args.summary:
                rows.append(report)
            old, then = new, now
    if args.summary:
        print(json.dumps({"summary": summarize(rows)}), flush=True)


if __name__ == "__main__":
    main()
