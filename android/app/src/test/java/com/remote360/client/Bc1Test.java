package com.remote360.client;

import org.junit.Test;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import static org.junit.Assert.*;

public class Bc1Test {
    @Test public void adjacentBlocksDecodeInRasterOrder() {
        byte[] blocks = {(byte) 0xff, (byte) 0xff, 0, 0, (byte) 0xe4, (byte) 0xe4, (byte) 0xe4, (byte) 0xe4,
                0, (byte) 0xf8, 0, (byte) 0xf8, 0, 0, 0, 0};
        ByteBuffer out = ByteBuffer.wrap(Bc1.decode(blocks, 8, 4)).order(ByteOrder.LITTLE_ENDIAN);
        for (int row = 0; row < 4; row++) {
            assertEquals(0xffff, out.getShort() & 65535);
            assertEquals(0, out.getShort() & 65535);
            assertEquals(0xa554, out.getShort() & 65535);
            assertEquals(0x52aa, out.getShort() & 65535);
            for (int x = 0; x < 4; x++) assertEquals(0xf800, out.getShort() & 65535);
        }
    }

    @Test public void compressedPacketsWithOneMissingChunkRecoverTheCompressedFrame() {
        final int frameBytes = 512 * 288 / 2;
        final int total = 63;
        FrameAssembler assembler = new FrameAssembler();
        byte[] frame = null;
        byte[] expected = new byte[frameBytes];
        for (int i = 0; i < frameBytes; i++) expected[i] = (byte) (i * 7 + i / 1180);
        for (int chunk = 0; chunk < total; chunk++) {
            if (chunk == 3) continue;
            int length = Math.min(1180, frameBytes - chunk * 1180);
            byte[] packet = header(8, chunk, total, length);
            System.arraycopy(expected, chunk * 1180, packet, 20, length);
            Protocol.VideoChunk part = Protocol.parseVideo(packet, packet.length);
            assertNotNull(part);
            assertNull(assembler.accept(packet, part));
        }
        byte[] parity = header(9, 0, total, 1180);
        for (int i = 0; i < 8 * 1180; i++) parity[20 + i % 1180] ^= expected[i];
        Protocol.VideoChunk part = Protocol.parseVideo(parity, parity.length);
        assertNotNull(part);
        frame = assembler.accept(parity, part);
        assertArrayEquals(expected, frame);
        parity[6] = 0x10; // unsupported dimensions must not allocate a new format
        assertNull(Protocol.parseVideo(parity, parity.length));
    }

    private static byte[] header(int type, int index, int count, int size) {
        return ByteBuffer.allocate(20 + size).order(ByteOrder.LITTLE_ENDIAN)
                .put(new byte[]{'R', '3', '6', '0', 1, (byte) type})
                .putShort((short) 512).putShort((short) 288).putShort((short) index).putShort((short) count)
                .putInt(101).putShort((short) size).array();
    }
}
