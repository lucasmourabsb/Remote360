package com.remote360.client;

import java.util.Arrays;

/** Reassembles one bounded frame; late, duplicate and incomplete frames are never displayed. */
final class FrameAssembler {
    private long frameId = -1;
    private int width, height, chunks, received;
    private boolean bc1;
    private boolean jpeg;
    private byte[] pixels;
    private boolean[] seen;
    private byte[][] parity;

    void reset() {
        frameId = -1;
        pixels = null;
        seen = null;
        parity = null;
    }

    byte[] accept(byte[] packet, Protocol.VideoChunk chunk) {
        if (frameId >= 0 && chunk.frameId != frameId && (int) (chunk.frameId - frameId) <= 0) {
            return null;
        }
        if (chunk.frameId != frameId) {
            frameId = chunk.frameId;
            width = chunk.width;
            height = chunk.height;
            chunks = chunk.chunks;
            bc1 = chunk.bc1;
            jpeg = chunk.jpeg;
            received = 0;
            pixels = new byte[chunk.frameBytes];
            seen = new boolean[chunks];
            parity = new byte[(chunks + Protocol.VIDEO_PARITY_GROUP - 1) / Protocol.VIDEO_PARITY_GROUP][];
        }
        if (chunk.width != width || chunk.height != height || chunk.chunks != chunks || chunk.bc1 != bc1
                || chunk.jpeg != jpeg || chunk.frameBytes != pixels.length || received == chunks) return null;
        int group = chunk.parity ? chunk.chunk : chunk.chunk / Protocol.VIDEO_PARITY_GROUP;
        if (chunk.parity) {
            if (parity[group] != null) return null;
            parity[group] = Arrays.copyOfRange(packet, chunk.headerSize,
                    chunk.headerSize + Protocol.VIDEO_PAYLOAD_MAX);
        } else {
            if (seen[chunk.chunk]) return null;
            System.arraycopy(packet, chunk.headerSize, pixels,
                    chunk.chunk * Protocol.VIDEO_PAYLOAD_MAX, chunk.payload);
            seen[chunk.chunk] = true;
            received++;
        }
        if (parity[group] != null && received < chunks) recover(group);
        // Subsequent duplicate chunks cannot modify this buffer; the next frame gets a new one.
        return received == chunks ? pixels : null;
    }

    private void recover(int group) {
        int first = group * Protocol.VIDEO_PARITY_GROUP;
        int end = Math.min(chunks, first + Protocol.VIDEO_PARITY_GROUP);
        int missing = -1;
        for (int index = first; index < end; index++) {
            if (seen[index]) continue;
            if (missing >= 0) return;
            missing = index;
        }
        if (missing < 0) return;

        byte[] recovered = parity[group];
        for (int index = first; index < end; index++) {
            if (index == missing) continue;
            int offset = index * Protocol.VIDEO_PAYLOAD_MAX;
            int length = Math.min(Protocol.VIDEO_PAYLOAD_MAX, pixels.length - offset);
            for (int i = 0; i < length; i++) recovered[i] ^= pixels[offset + i];
        }
        int offset = missing * Protocol.VIDEO_PAYLOAD_MAX;
        int length = Math.min(Protocol.VIDEO_PAYLOAD_MAX, pixels.length - offset);
        System.arraycopy(recovered, 0, pixels, offset, length);
        seen[missing] = true;
        received++;
    }
}
