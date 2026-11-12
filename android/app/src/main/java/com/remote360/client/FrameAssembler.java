package com.remote360.client;

import java.util.Arrays;

/** Allows one frame of packet reordering without displaying a frame twice or out of order. */
final class FrameAssembler {
    private InFlight latest;
    private InFlight previous;
    private long delivered = -1;

    void reset() {
        latest = null;
        previous = null;
        delivered = -1;
    }

    byte[] accept(byte[] packet, Protocol.VideoChunk chunk) {
        if (delivered >= 0 && (int) (chunk.frameId - delivered) <= 0) return null;
        if (latest == null) latest = new InFlight(chunk);
        int distance = (int) (chunk.frameId - latest.header.frameId);
        if (distance > 0) {
            previous = distance == 1 ? latest : null;
            latest = new InFlight(chunk);
        }
        InFlight current = distance >= 0 ? latest : distance == -1 ? previous : null;
        if (current == null || !current.accept(packet, chunk)) return null;
        delivered = chunk.frameId;
        previous = null;
        if (current == latest) latest = null;
        return current.pixels;
    }

    private static final class InFlight {
        final Protocol.VideoChunk header;
        final byte[] pixels;
        final boolean[] seen;
        final byte[][] parity;
        int received;

        InFlight(Protocol.VideoChunk chunk) {
            header = chunk;
            pixels = new byte[chunk.frameBytes];
            seen = new boolean[chunk.chunks];
            parity = new byte[(chunk.chunks + Protocol.VIDEO_PARITY_GROUP - 1) / Protocol.VIDEO_PARITY_GROUP][];
        }

        boolean accept(byte[] packet, Protocol.VideoChunk chunk) {
            if (chunk.width != header.width || chunk.height != header.height || chunk.chunks != header.chunks
                    || chunk.bc1 != header.bc1 || chunk.jpeg != header.jpeg || chunk.frameBytes != pixels.length) {
                return false;
            }
            int group = chunk.parity ? chunk.chunk : chunk.chunk / Protocol.VIDEO_PARITY_GROUP;
            if (chunk.parity) {
                if (parity[group] != null) return false;
                parity[group] = Arrays.copyOfRange(packet, chunk.headerSize,
                        chunk.headerSize + Protocol.VIDEO_PAYLOAD_MAX);
            } else {
                if (seen[chunk.chunk]) return false;
                System.arraycopy(packet, chunk.headerSize, pixels,
                        chunk.chunk * Protocol.VIDEO_PAYLOAD_MAX, chunk.payload);
                seen[chunk.chunk] = true;
                received++;
            }
            if (parity[group] != null && received < header.chunks) recover(group);
            return received == header.chunks;
        }

        private void recover(int group) {
            int first = group * Protocol.VIDEO_PARITY_GROUP;
            int end = Math.min(header.chunks, first + Protocol.VIDEO_PARITY_GROUP);
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
}
