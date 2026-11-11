#!/usr/bin/env python3
"""Remote360 host-side input sender and status monitor.

Useful for bring-up before the Android APK is installed. It only uses Python's stdlib.
"""
from __future__ import annotations

import argparse
import socket
import struct
import time

MAGIC = b"R360"
VERSION = 1
PORT = 36010
TYPE_INPUT = 1
TYPE_STATUS = 2
TYPE_RUMBLE = 3

BUTTONS = {
    "up": 0x0001, "down": 0x0002, "left": 0x0004, "right": 0x0008,
    "start": 0x0010, "back": 0x0020, "ls": 0x0040, "rs": 0x0080,
    "lb": 0x0100, "rb": 0x0200, "a": 0x1000, "b": 0x2000,
    "x": 0x4000, "y": 0x8000,
}


def input_packet(seq: int, buttons: int = 0, guide: bool = False, lt: int = 0,
                 rt: int = 0, lx: int = 0, ly: int = 0, rx: int = 0, ry: int = 0,
                 *, video_flags: int = 0, video_config: int = 0) -> bytes:
    return struct.pack(
        "<4sBBBBIHBBhhhhI",
        MAGIC, VERSION, TYPE_INPUT, int(guide) | (video_flags & 0xFE), video_config & 0xFF,
        seq & 0xFFFFFFFF, buttons & 0xFFFF,
        lt & 0xFF, rt & 0xFF, lx, ly, rx, ry, int(time.monotonic() * 1000) & 0xFFFFFFFF,
    )


def decode_reply(data: bytes) -> str:
    if len(data) >= 6 and data[:4] == MAGIC and data[4] == VERSION:
        if data[5] == TYPE_STATUS and len(data) == 24:
            state, flags, ack, valid, invalid, server_ms = struct.unpack_from("<BBIIII", data, 6)
            return (f"status state={state} bound={bool(flags & 2)} watchdog={bool(flags & 1)} "
                    f"ack={ack} valid={valid} invalid={invalid} xbox_ms={server_ms}")
        if data[5] == TYPE_RUMBLE and len(data) == 12:
            left, right, ack = struct.unpack_from("<BBI", data, 6)
            return f"rumble left={left}% right={right}% ack={ack}"
    return f"unknown {data.hex()}"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("xbox_ip")
    parser.add_argument("--button", choices=sorted(BUTTONS))
    parser.add_argument("--hold", type=float, default=0.25, help="seconds to hold the selected button")
    parser.add_argument("--seconds", type=float, default=3.0, help="total stream duration")
    args = parser.parse_args()

    target = (args.xbox_ip, PORT)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(0.01)
    started = time.monotonic()
    seq = 0
    last_print = ""
    print(f"Remote360 -> {args.xbox_ip}:{PORT}")
    while time.monotonic() - started < args.seconds:
        elapsed = time.monotonic() - started
        buttons = BUTTONS.get(args.button, 0) if elapsed < args.hold else 0
        sock.sendto(input_packet(seq, buttons), target)
        seq += 1
        deadline = time.monotonic() + 1 / 120
        while time.monotonic() < deadline:
            try:
                reply, _ = sock.recvfrom(128)
            except socket.timeout:
                break
            text = decode_reply(reply)
            if text != last_print:
                print(text)
                last_print = text
        sleep = deadline - time.monotonic()
        if sleep > 0:
            time.sleep(sleep)


if __name__ == "__main__":
    main()

