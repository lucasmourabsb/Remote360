"""Cross-check the console's real C packets with the host's independent receiver."""

from pathlib import Path
import ctypes
import random
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from video_lab import build_codec, packets
from video_probe import FrameReceiver, matches_pattern


class VideoWireTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="remote360-wire-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.codec = build_codec(cls.directory.name)

    def decode(self, receiver, wire):
        return [frame for packet in wire if (frame := receiver.accept(packet)) is not None]

    def test_known_pattern_matches_after_recovery_and_reordering(self):
        for frame_id in (1, 79, 80, 255, 0xFFFFFFFF):
            with self.subTest(frame_id=frame_id):
                raw, = self.decode(FrameReceiver(), packets(self.codec, frame_id, False, "clean"))
                lost = list(packets(self.codec, frame_id, True, "loss"))
                random.Random(frame_id).shuffle(lost)
                receiver = FrameReceiver()
                recovered, = self.decode(receiver, lost + lost[:5])
                self.assertEqual(recovered, raw)
                self.assertTrue(matches_pattern(recovered))
                self.assertEqual(receiver.recovered, self.codec.group_count())
                w, h, _, pixel = recovered
                self.assertEqual((w, h, len(pixel)), (320, 180, 115200))
                self.assertEqual(struct.unpack_from("<H", pixel, 160 * 2)[0], 0xf81f)
                marker = (frame_id % 80) * 4
                self.assertEqual(struct.unpack_from("<H", pixel, (179 * w + marker) * 2)[0], 0xffff)

    def test_unrecoverable_frame_is_dropped_then_next_frame_displays(self):
        receiver = FrameReceiver()
        self.assertEqual(self.decode(receiver, packets(self.codec, 1, True, "double_loss")), [])
        self.assertEqual(len(self.decode(receiver, packets(self.codec, 2, True, "loss"))), 1)
        self.assertEqual(receiver.discarded, 1)

    def test_older_client_receives_plain_rgb565_only(self):
        wire = list(packets(self.codec, 1, False, "clean"))
        self.assertEqual(len(wire), self.codec.chunk_count())
        self.assertTrue(all(packet[5] == 4 for packet in wire))
        self.assertEqual(len(self.decode(FrameReceiver(), wire)), 1)

    def test_jpeg_payload_is_recovered_at_its_variable_length(self):
        data = random.Random(31).randbytes(19 * 1180 + 13)
        fixture = ctypes.create_string_buffer(data)
        self.codec.set_jpeg(fixture, len(data), 1280, 720)
        try:
            wire = list(packets(self.codec, 10, True, "loss"))
            random.Random(42).shuffle(wire)
            receiver = FrameReceiver()
            self.assertEqual(self.decode(receiver, wire), [(1280, 720, 10, data)])
            self.assertEqual(receiver.recovered, 3)
        finally:
            self.codec.set_jpeg(None, 0, 320, 180)


if __name__ == "__main__":
    unittest.main()
