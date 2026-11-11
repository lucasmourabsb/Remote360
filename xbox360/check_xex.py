#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Static checks for the unencrypted, mapped XEX files produced by this build.

Passing these checks does not establish console compatibility or runtime safety.

- The image must cover the whole PE. SynthXEX sizes the image from the bytes the PE
  file holds, so a section whose virtual size runs past its raw data (zero-initialised
  .bss) ends up partly outside the loaded image: writes there land in memory the
  module doesn't own.
- Header bounds, complete image coverage and chained hashes must be valid.
- With --no-fp: no floating-point instructions. DLL code runs on the
  kernel's DPCs, which have no FP context, and an FP instruction there faults.

  check_xex.py [--no-fp OBJDUMP] app.exe app.xex
"""
import hashlib
from pathlib import Path
import re
import struct
import subprocess
import sys

FP = re.compile(r"\s(l|st)f[sd](u|x|ux)?\s|\sf[a-z]+\.?\s|\sm[ft]fs")


def pe_size_of_image(path):
    d = Path(path).read_bytes()
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    return struct.unpack_from("<I", d, pe + 24 + 56)[0]


def xex_image_size(path):
    d = Path(path).read_bytes()
    if len(d) < 24 or d[:4] != b"XEX2":
        raise ValueError("not an XEX2 file")
    _, _, payload, _, security, count = struct.unpack_from(">6I", d)
    if not 24 + count * 8 <= security or not security + 0x184 <= payload <= len(d):
        raise ValueError("invalid header bounds")
    size = struct.unpack_from(">I", d, security + 4)[0]
    if len(d) - payload != size:
        raise ValueError("truncated image or unexpected trailing bytes")
    headers = dict(struct.unpack_from(">II", d, 24 + i * 8) for i in range(count))
    fmt = headers.get(0x003FF, 0)
    if not security + 0x184 <= fmt <= payload - 16:
        raise ValueError("invalid basefile format header")
    if struct.unpack_from(">IHHII", d, fmt) != (16, 0, 1, size, 0):
        raise ValueError("expected one unencrypted, mapped image block")
    digest = hashlib.sha1(d[security + 0x17c:payload] + d[:security + 8]).digest()
    if digest != d[security + 0x164:security + 0x178]:
        raise ValueError("header integrity hash mismatch")
    flags = struct.unpack_from(">I", d, security + 0x10c)[0]
    page_size = 4096 if flags & 0x10000000 else 65536
    page_count = struct.unpack_from(">I", d, security + 0x180)[0]
    if not page_count or security + 0x184 + page_count * 24 > payload:
        raise ValueError("invalid page descriptor count")
    expected = d[security + 0x114:security + 0x128]
    cursor = payload
    for i in range(page_count):
        offset = security + 0x184 + i * 24
        descriptor = d[offset:offset + 24]
        page_bytes = (struct.unpack_from(">I", descriptor)[0] >> 4) * page_size
        if not page_bytes or cursor + page_bytes > len(d):
            raise ValueError("page descriptor points outside the image")
        actual = hashlib.sha1(d[cursor:cursor + page_bytes] + descriptor).digest()
        if actual != expected:
            raise ValueError(f"image integrity hash mismatch at descriptor {i}")
        expected = descriptor[4:]
        cursor += page_bytes
    if cursor != len(d) or expected != bytes(20):
        raise ValueError("page chain does not cover the complete image")
    return size


def fp_instructions(objdump, exe):
    out = subprocess.run([objdump, "-d", "--no-show-raw-insn", exe], capture_output=True,
                         text=True, check=True).stdout
    hits = []
    for line in out.splitlines():
        m = re.match(r"\s*([0-9a-f]+):\s+(.*)", line)  # instruction lines only
        if m and FP.search(" " + m.group(2) + " "):
            hits.append(f"{m.group(1)}: {m.group(2).strip()}")
    return hits


def main():
    args = sys.argv[1:]
    objdump = None
    if args[:1] == ["--no-fp"]:
        objdump, args = args[1], args[2:]
    exe, xex = args
    need, have = pe_size_of_image(exe), xex_image_size(xex)
    if have < need:
        sys.exit(f"{xex}: maps {have:#x} bytes but {exe} needs {need:#x} "
                 f"({need - have:#x} unmapped, likely .bss)")
    if objdump:
        fp = fp_instructions(objdump, exe)
        if fp:
            sys.exit(f"{exe}: {len(fp)} floating-point instructions, e.g. {fp[0]}")
    print(f"{xex}: image {have:#x} bytes, headers and hash chain valid"
          + (", no FP" if objdump else ""))


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, struct.error) as error:
        sys.exit(f"XEX validation failed: {error}")
