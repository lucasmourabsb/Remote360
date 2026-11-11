# Remote360 protocol v1

Remote360 uses UDP port `36010` on the Xbox 360. Every packet starts with the four ASCII bytes
`R360`, followed by protocol version `1` and a packet type. Multi-byte payload fields are little
endian so the Android client and the big-endian Xenon CPU never depend on native struct layout.

## Input packet

Exactly 28 bytes.

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 4 | `R360` |
| 4 | 1 | version = 1 |
| 5 | 1 | type = 1 |
| 6 | 1 | flags, bit 0 = Guide, bit 1 = video, bit 2 = test bars, bit 3 = video status, bit 4 = video parity, bit 5 = optional BC1 video, bit 6 = use video settings, bit 7 = prioritize fluency |
| 7 | 1 | when bit 6 is set, low nibble is the resolution index and high nibble is the frame rate index |
| 8 | 4 | sequence |
| 12 | 2 | XInput button mask |
| 14 | 1 | left trigger 0..255 |
| 15 | 1 | right trigger 0..255 |
| 16 | 2 | left X signed 16-bit |
| 18 | 2 | left Y signed 16-bit, up positive |
| 20 | 2 | right X signed 16-bit |
| 22 | 2 | right Y signed 16-bit, up positive |
| 24 | 4 | client monotonic milliseconds, low 32 bits |

Resolution indices 0–5 request 320 × 180, 512 × 288, 640 × 360, 960 × 540, 1280 × 720,
and 1920 × 1080. Frame rate indices 0–3 request a ceiling of 15, 24, 30, and 60 FPS.
The console selects a supported resolution within the source dimensions and may step down when
encoding or capture cannot keep the requested profile. The data packet dimensions report the
effective choice. A legacy console ignores bit 6 and byte 7.

## Status packet

Exactly 24 bytes. Type is 2. Byte 6 is server state. Byte 7 has bit 0 set when the watchdog
has neutralized input, bit 1 after the virtual controller is bound and bit 2 for the video-only
build or controller fallback. Bit 3 reports a conflicting controller driver; bit 4
reports controller initialization or registration failure. Offsets 8, 12 and 16
are the last accepted sequence, valid-packet count and invalid-packet count. Offset 20 is the
Xbox system time in milliseconds, low 32 bits. Do not subtract clocks from different devices to
estimate latency. The Android client matches the ACK sequence to its own send timestamp instead.

## Rumble packet

Exactly 12 bytes. Type is 3. Bytes 6 and 7 are left and right motor strengths from 0 to 100.
Offset 8 is the last accepted input sequence.

## Video packet

Type is 4. The first 20 bytes are a header and up to 1180 bytes of RGB565 frame data follow it.

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 4 | `R360` |
| 4 | 1 | version = 1 |
| 5 | 1 | type = 4 |
| 6 | 2 | width |
| 8 | 2 | height |
| 10 | 2 | chunk index |
| 12 | 2 | chunk count |
| 14 | 4 | frame id |
| 18 | 2 | payload byte count |
| 20 | N | RGB565 pixels, little endian |

The video-only Xbox build reads the live display surface at a target of 10 fps and outputs 320x180
RGB565. It does not pause the title. The client validates the chunk count and final length before
displaying a complete frame. The Xbox drops a capture if the display surface changes while copying.
The test bars flag substitutes an animated, deterministic frame without reading the GPU.
Older clients and consoles continue using raw type-4 packets and ignore unsupported flags.

## Video status

Type 5 is 40 bytes and is sent only when the client requests it with input flag 3. Byte 6 is the
last capture result: 0 ok, 1 display unavailable, 2 unsupported format or layout, 3 surface
unmapped, 4 display update pending, 5 display changed during capture, 6 request cancelled.
Byte 7 contains the active video flag (bit 0), test bars flag (bit 1), feedback capability
(bit 2), settings capability (bit 3), source limit (bit 4), encoder limit (bit 5),
capture limit (bit 6) and fluency mode capability (bit 7). Result 7 means the encoder reached
its buffer limit.
Offsets 8, 12, 16, 20 and 24 are 32-bit fields for the last input sequence, complete
frames sent, capture failures, send failures and last socket error. Offsets 28 and 30 are 16-bit
source dimensions. Offset 32 is the 32-bit display control register. Offsets 36 and 38 are
16-bit source pitch in pixels and the low 16 bits of the display swap register.

## Video parity

Type 6 uses the same 20-byte header as type 4. The index at offset 10 is a zero-based group index
rather than a chunk index. The count at offset 12 remains the number of type-4 chunks in the frame.
One group covers up to eight consecutive chunks. The 1180-byte payload is their bytewise XOR,
with the last short chunk zero-padded for the calculation. A receiver can recover exactly one
missing data chunk in a group with parity. Multiple missing chunks leave the frame incomplete.
The Xbox sends parity only when the client requests it with input flag 4.

## Optional BC1 video

With input flags 1 and 5 set, a supporting Xbox module sends type 8 data and type 9 parity
using the same 20-byte header. Legacy BC1 is fixed at 512 × 288. The payload is a
raster-order array of 4×4 BC1 blocks, 8 bytes per block, with little-endian color endpoints
and indices. At 512 × 288 there are 63 data chunks, with an optional 8 parity packets.
If the client requests settings and fluency mode (bits 6 and 7) on a supporting module,
the same packet types may carry 320 × 180 or 640 × 360. These have 25 and 98 data chunks,
respectively. Fluency mode can accept a frame whose contents change while being sampled,
while still rejecting changes to the display layout. Each frame can be
decoded independently after assembly. Test bars, legacy clients and legacy servers continue
using RGB565. A receiver rejects a change of dimensions or format within the same frame ID.

## JPEG video

Type 10 carries baseline JPEG data and type 11 carries parity. The first 20 bytes match the
other video headers. An additional 32-bit little-endian field at offset 20 contains the complete
JPEG frame size, bounded to 384 KiB. Payload begins at offset 24. The valid JPEG dimensions are
profiles 2 through 5. The final data chunk must match the remaining bytes exactly, and parity
retains the same eight-chunk groups and zero padding. Frames are independent, so the receiver
can discard an incomplete or stale frame while continuing to decode new ones.

## Video feedback

Type 7 is 16 bytes and is sent only after the console advertises support in the video status.
Bytes 6 and 7 are reserved. Offsets 8 and 12 contain the last frame ID copied to the Android
image view and the cumulative count of frames copied by the current connection. Feedback does
not update the controller or renew the input watchdog. It is a receiver diagnostic, not a physical
screen measurement or an end-to-end latency estimate.

The server keeps one active sender at a time. A different sender can take over after two seconds
without valid input. At 350 ms without input, the server sends a neutral controller state to avoid
stuck buttons or sticks. The same sender may restart its sequence after two seconds of inactivity.
The status bound bit is set only after XAM returns a valid user index. It does not prove gameplay
compatibility. Protocol v1 is unauthenticated and intended only for a trusted LAN.

