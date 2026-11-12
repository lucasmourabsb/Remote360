package com.remote360.client;

import org.junit.Test;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import static org.junit.Assert.*;

public class ProtocolTest {
    static byte[] video(long id, int width, int height, int chunk) {
        int bytes = width * height * 2;
        int chunks = (bytes + Protocol.VIDEO_PAYLOAD_MAX - 1) / Protocol.VIDEO_PAYLOAD_MAX;
        int payload = Math.min(Protocol.VIDEO_PAYLOAD_MAX, bytes - chunk * Protocol.VIDEO_PAYLOAD_MAX);
        ByteBuffer b = ByteBuffer.allocate(20 + payload).order(ByteOrder.LITTLE_ENDIAN);
        b.put(new byte[]{'R', '3', '6', '0', 1, 4});
        b.putShort((short) width).putShort((short) height).putShort((short) chunk).putShort((short) chunks);
        b.putInt((int) id).putShort((short) payload);
        for (int i = 0; i < payload; i++) b.put((byte) ((chunk * Protocol.VIDEO_PAYLOAD_MAX + i) & 255));
        return b.array();
    }

    @Test public void inputUsesLittleEndianAndIndependentFeatureFlags() {
        GamepadState.Snapshot state = new GamepadState.Snapshot(0x1001, true, 1, 255, -2, 3, -4, 5);
        byte[] packet = Protocol.inputPacket(0x11223344, state, true);
        assertEquals(28, packet.length);
        assertEquals(0x1b, packet[6]);
        ByteBuffer b = ByteBuffer.wrap(packet).order(ByteOrder.LITTLE_ENDIAN);
        assertEquals(0x11223344, b.getInt(8));
        assertEquals(0x1001, b.getShort(12));
        assertEquals(255, packet[15] & 255);
        assertEquals(-2, b.getShort(16));
        assertEquals(5, b.getShort(22));
        assertEquals(1, Protocol.inputPacket(0, state)[6]);
    }

    @Test public void malformedOrTruncatedHeadersAreRejected() {
        assertNull(Protocol.parseStatus(null, 24));
        assertNull(Protocol.parseStatus(new byte[2], 24));
        assertNull(Protocol.parseVideo(new byte[6], 20));
        assertNull(Protocol.parseRumble(new byte[12], 12));
    }

    @Test public void selecting720pKeepsTheProfileEvenWithAnOldFastModePreference() {
        GamepadState.Snapshot state = new GamepadState.Snapshot(0, false, 0, 0, 0, 0, 0, 0);
        byte[] fullHd = Protocol.inputPacket(1, state, true, 0x35, true);
        byte[] hd = Protocol.inputPacket(2, state, true, 0x34, true);
        byte[] fast = Protocol.inputPacket(3, state, true, 0x32, true);
        assertEquals(0x35, fullHd[7] & 0xff);
        assertEquals(0x34, hd[7] & 0xff);
        assertEquals(0x40, hd[6] & 0xc0);
        assertEquals(0x40, fullHd[6] & 0xc0);
        assertEquals(0x32, fast[7] & 0xff);
        assertEquals(0xc0, fast[6] & 0xc0);
    }

    @Test public void statusPreservesUnsignedAcknowledgement() {
        ByteBuffer b = ByteBuffer.allocate(24).order(ByteOrder.LITTLE_ENDIAN);
        b.put(new byte[]{'R', '3', '6', '0', 1, 2, 4, 3});
        b.putInt(-1).putInt(100).putInt(3).putInt(999);
        Protocol.Status status = Protocol.parseStatus(b.array(), 24);
        assertNotNull(status);
        assertTrue(status.bound);
        assertTrue(status.watchdog);
        assertEquals(0xFFFFFFFFL, status.ack);
        assertEquals(100, status.valid);
    }

    @Test public void finalVideoChunkMustHaveTheExactRemainingLength() {
        byte[] packet = video(9, 320, 180, 97);
        Protocol.VideoChunk chunk = Protocol.parseVideo(packet, packet.length);
        assertNotNull(chunk);
        assertEquals(98, chunk.chunks);
        assertEquals(740, chunk.payload);
        packet[18]--;
        assertNull(Protocol.parseVideo(packet, packet.length));
    }

    @Test public void incorrectChunkCountCannotProduceAnIncompleteFrame() {
        byte[] packet = video(3, 320, 180, 0);
        packet[12] = 1;
        packet[13] = 0;
        assertNull(Protocol.parseVideo(packet, packet.length));
    }

    @Test public void oversizedDatagramsAreRejected() {
        byte[] packet = video(3, 320, 180, 0);
        byte[] oversized = java.util.Arrays.copyOf(packet, packet.length + 1);
        assertNull(Protocol.parseVideo(oversized, oversized.length));
    }

    @Test public void jpegFinalChunkUsesTheDeclaredCompressedSize() {
        ByteBuffer b = ByteBuffer.allocate(24 + 13).order(ByteOrder.LITTLE_ENDIAN);
        b.put(new byte[]{'R', '3', '6', '0', 1, 10});
        b.putShort((short) 1920).putShort((short) 1080).putShort((short) 99).putShort((short) 100);
        b.putInt(17).putShort((short) 13).putInt(99 * 1180 + 13);
        Protocol.VideoChunk part = Protocol.parseVideo(b.array(), b.capacity());
        assertNotNull(part);
        assertTrue(part.jpeg);
        assertEquals(24, part.headerSize);
        assertEquals(99 * 1180 + 13, part.frameBytes);
        b.putInt(20, 99 * 1180 + 12);
        assertNull(Protocol.parseVideo(b.array(), b.capacity()));
        b.putInt(20, Protocol.JPEG_MAX_BYTES + 1);
        assertNull(Protocol.parseVideo(b.array(), b.capacity()));
    }

    @Test public void videoStatusReportsSeparateFailureCounters() {
        ByteBuffer b = ByteBuffer.allocate(Protocol.VIDEO_STATUS_SIZE).order(ByteOrder.LITTLE_ENDIAN);
        b.put(new byte[]{'R', '3', '6', '0', 1, 5, 4, 5});
        b.putInt(51).putInt(17).putInt(9).putInt(-1);
        Protocol.VideoStatus status = Protocol.parseVideoStatus(b.array(), b.capacity());
        assertNotNull(status);
        assertEquals(4, status.captureResult);
        assertTrue(status.requested);
        assertTrue(status.feedbackSupported);
        assertEquals(17, status.framesSent);
        assertEquals(9, status.captureErrors);
        assertEquals(0xFFFFFFFFL, status.sendErrors);
        assertNull(Protocol.parseVideoStatus(b.array(), b.capacity() - 1));
    }

    @Test public void feedbackReportsDisplayedCountAndFrameIdInWireOrder() {
        ByteBuffer b = ByteBuffer.wrap(Protocol.videoFeedback(0x00000102aabbccddL)).order(ByteOrder.LITTLE_ENDIAN);
        assertEquals(7, b.get(5));
        assertEquals(0xaabbccdd, b.getInt(8));
        assertEquals(258, b.getInt(12));
    }
}
