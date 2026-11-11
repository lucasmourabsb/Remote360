import importlib.util
import pathlib
import struct

MODULE = pathlib.Path(__file__).parents[1] / "tools" / "send_input.py"
spec = importlib.util.spec_from_file_location("send_input", MODULE)
send_input = importlib.util.module_from_spec(spec)
assert spec.loader
spec.loader.exec_module(send_input)


def test_input_packet_layout():
    packet = send_input.input_packet(0x11223344, send_input.BUTTONS["a"] | send_input.BUTTONS["up"],
                                     guide=True, lt=1, rt=255, lx=-2, ly=3, rx=-4, ry=5)
    assert len(packet) == 28
    assert packet[:8] == b"R360\x01\x01\x01\x00"
    assert struct.unpack_from("<I", packet, 8)[0] == 0x11223344
    assert struct.unpack_from("<H", packet, 12)[0] == 0x1001
    assert packet[14:16] == bytes((1, 255))
    assert struct.unpack_from("<hhhh", packet, 16) == (-2, 3, -4, 5)


def test_status_decode():
    packet = b"R360" + bytes((1, 2)) + struct.pack("<BBIIII", 4, 2, 7, 100, 3, 999)
    text = send_input.decode_reply(packet)
    assert "state=4" in text
    assert "bound=True" in text
    assert "ack=7" in text

