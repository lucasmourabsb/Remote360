package com.remote360.client;

import org.junit.Test;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import static org.junit.Assert.*;

public class FrameAssemblerTest {
    private byte[] accept(FrameAssembler assembler, long id, int chunk) {
        byte[] packet = ProtocolTest.video(id, 32, 32, chunk);
        return assembler.accept(packet, Protocol.parseVideo(packet, packet.length));
    }

    @Test public void outOfOrderChunksAreReconstructedExactly() {
        FrameAssembler assembler = new FrameAssembler();
        assertNull(accept(assembler, 2, 1));
        byte[] frame = accept(assembler, 2, 0);
        assertNotNull(frame);
        assertEquals(2048, frame.length);
        for (int i = 0; i < frame.length; i++) assertEquals((byte) i, frame[i]);
    }

    @Test public void duplicatesDoNotFinishOrRedisplayAFrame() {
        FrameAssembler assembler = new FrameAssembler();
        assertNull(accept(assembler, 2, 0));
        assertNull(accept(assembler, 2, 0));
        assertNotNull(accept(assembler, 2, 1));
        assertNull(accept(assembler, 2, 1));
    }

    @Test public void lateFrameCannotReplaceANewerIncompleteFrame() {
        FrameAssembler assembler = new FrameAssembler();
        assertNull(accept(assembler, 3, 0));
        assertNull(accept(assembler, 2, 1));
        assertNotNull(accept(assembler, 3, 1));
    }

    @Test public void frameIdsWrapFromUnsignedMaximumToZero() {
        FrameAssembler assembler = new FrameAssembler();
        assertNull(accept(assembler, 0xFFFFFFFFL, 0));
        assertNotNull(accept(assembler, 0xFFFFFFFFL, 1));
        assertNull(accept(assembler, 0, 1));
        assertNotNull(accept(assembler, 0, 0));
        assertNull(accept(assembler, 0xFFFFFFFFL, 0));
    }

    @Test public void parityRecoversTheShortFinalChunkEvenIfParityArrivesFirst() {
        FrameAssembler assembler = new FrameAssembler();
        byte[] parity = parity(3, 32, 32, 0);
        assertNull(assembler.accept(parity, Protocol.parseVideo(parity, parity.length)));
        byte[] frame = accept(assembler, 3, 0);
        assertNotNull(frame);
        for (int i = 0; i < frame.length; i++) assertEquals((byte) i, frame[i]);
        assertNull(accept(assembler, 3, 1));
    }

    @Test public void twoMissingChunksDoNotProduceAFakeFrame() {
        FrameAssembler assembler = new FrameAssembler();
        byte[] parity = parity(3, 64, 64, 0);
        assertNull(assembler.accept(parity, Protocol.parseVideo(parity, parity.length)));
        for (int i = 0; i < 5; i++) {
            byte[] packet = ProtocolTest.video(3, 64, 64, i);
            assertNull(assembler.accept(packet, Protocol.parseVideo(packet, packet.length)));
        }
        assertNull(accept(assembler, 4, 0));
        assertNotNull(accept(assembler, 4, 1));
    }

    @Test public void rejectsParityForAGroupOutsideTheFrame() {
        byte[] parity = parity(3, 32, 32, 0);
        parity[10] = 1;
        assertNull(Protocol.parseVideo(parity, parity.length));
    }

    private static byte[] parity(long id, int width, int height, int group) {
        int max = Protocol.VIDEO_PAYLOAD_MAX;
        int header = Protocol.VIDEO_HEADER_SIZE;
        int chunks = (width * height * 2 + max - 1) / max;
        byte[] parity = ByteBuffer.allocate(header + max).order(ByteOrder.LITTLE_ENDIAN)
                .put(new byte[]{'R', '3', '6', '0', 1, 6})
                .putShort((short) width).putShort((short) height)
                .putShort((short) group).putShort((short) chunks)
                .putInt((int) id).putShort((short) max).array();
        int first = group * Protocol.VIDEO_PARITY_GROUP;
        for (int i = first; i < Math.min(first + Protocol.VIDEO_PARITY_GROUP, chunks); i++) {
            byte[] packet = ProtocolTest.video(id, width, height, i);
            for (int p = header; p < packet.length; p++) parity[p] ^= packet[p];
        }
        return parity;
    }
}
