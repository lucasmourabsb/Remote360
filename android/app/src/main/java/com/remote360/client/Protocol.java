package com.remote360.client;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;

final class Protocol {
    static final int PORT = 36010;
    static final int INPUT_SIZE = 28;
    static final int STATUS_SIZE = 24;
    static final int RUMBLE_SIZE = 12;
    static final int VIDEO_HEADER_SIZE = 20;
    static final int JPEG_HEADER_SIZE = 24;
    static final int JPEG_MAX_BYTES = 384 * 1024;
    static final int VIDEO_PAYLOAD_MAX = 1180;
    static final int VIDEO_STATUS_SIZE = 40;
    static final int VIDEO_FEEDBACK_SIZE = 16;
    static final int VIDEO_PARITY_GROUP = 8;
    private static final int VIDEO_REQUEST_FLAGS = 0x1a; // video, status, parity
    static final int BC1_WIDTH = 512, BC1_HEIGHT = 288;
    static final int[] WIDTHS = {320, 512, 640, 960, 1280, 1920};
    static final int[] HEIGHTS = {180, 288, 360, 540, 720, 1080};
    static final int[] FRAME_RATES = {15, 24, 30, 60};

    static final int BTN_DPAD_UP = 0x0001;
    static final int BTN_DPAD_DOWN = 0x0002;
    static final int BTN_DPAD_LEFT = 0x0004;
    static final int BTN_DPAD_RIGHT = 0x0008;
    static final int BTN_START = 0x0010;
    static final int BTN_BACK = 0x0020;
    static final int BTN_THUMB_L = 0x0040;
    static final int BTN_THUMB_R = 0x0080;
    static final int BTN_SHOULDER_L = 0x0100;
    static final int BTN_SHOULDER_R = 0x0200;
    static final int BTN_A = 0x1000;
    static final int BTN_B = 0x2000;
    static final int BTN_X = 0x4000;
    static final int BTN_Y = 0x8000;

    private Protocol() {}

    static byte[] inputPacket(int sequence, GamepadState.Snapshot state) {
        return inputPacket(sequence, state, false);
    }

    static byte[] inputPacket(int sequence, GamepadState.Snapshot state, boolean video) {
        return inputPacket(sequence, state, video, false);
    }

    static byte[] inputPacket(int sequence, GamepadState.Snapshot state, boolean video, boolean sharper) {
        return inputPacket(sequence, state, video, sharper ? 0x20 : 0, 0);
    }

    static byte[] inputPacket(int sequence, GamepadState.Snapshot state, boolean video, int settings) {
        return inputPacket(sequence, state, video, settings, false);
    }

    static byte[] inputPacket(int sequence, GamepadState.Snapshot state, boolean video, int settings, boolean fast) {
        if (settings < 0) return inputPacket(sequence, state, video);
        int flags = 0x40 | ((settings & 15) == 1 ? 0x20 : 0) | (fast ? 0x80 : 0);
        return inputPacket(sequence, state, video, flags, settings);
    }

    private static byte[] inputPacket(int sequence, GamepadState.Snapshot state, boolean video,
                                      int videoFlags, int settings) {
        ByteBuffer b = ByteBuffer.allocate(INPUT_SIZE).order(ByteOrder.LITTLE_ENDIAN);
        b.put((byte) 'R').put((byte) '3').put((byte) '6').put((byte) '0');
        b.put((byte) 1).put((byte) 1)
                .put((byte) ((state.guide ? 1 : 0) | (video ? VIDEO_REQUEST_FLAGS | videoFlags : 0)))
                .put((byte) (video ? settings : 0));
        b.putInt(sequence);
        b.putShort((short) state.buttons);
        b.put((byte) state.leftTrigger).put((byte) state.rightTrigger);
        b.putShort((short) state.leftX).putShort((short) state.leftY);
        b.putShort((short) state.rightX).putShort((short) state.rightY);
        b.putInt((int) (android.os.SystemClock.elapsedRealtime() & 0xFFFFFFFFL));
        return b.array();
    }

    static boolean validHeader(byte[] data, int length, int type, int expectedLength) {
        return data != null && length >= 6 && length <= data.length && length == expectedLength
                && data[0] == 'R' && data[1] == '3' && data[2] == '6' && data[3] == '0'
                && data[4] == 1 && data[5] == type;
    }

    static Status parseStatus(byte[] data, int length) {
        if (!validHeader(data, length, 2, STATUS_SIZE)) return null;
        ByteBuffer b = ByteBuffer.wrap(data, 6, STATUS_SIZE - 6).order(ByteOrder.LITTLE_ENDIAN);
        int state = b.get() & 0xFF;
        int flags = b.get() & 0xFF;
        long ack = b.getInt() & 0xFFFFFFFFL;
        long valid = b.getInt() & 0xFFFFFFFFL;
        long invalid = b.getInt() & 0xFFFFFFFFL;
        long xboxMs = b.getInt() & 0xFFFFFFFFL;
        return new Status(state, (flags & 2) != 0, (flags & 1) != 0, (flags & 4) != 0,
                (flags & 8) != 0, (flags & 16) != 0, ack, valid, invalid, xboxMs);
    }

    static Rumble parseRumble(byte[] data, int length) {
        if (!validHeader(data, length, 3, RUMBLE_SIZE)) return null;
        ByteBuffer b = ByteBuffer.wrap(data, 6, RUMBLE_SIZE - 6).order(ByteOrder.LITTLE_ENDIAN);
        int left = b.get() & 0xFF;
        int right = b.get() & 0xFF;
        long ack = b.getInt() & 0xFFFFFFFFL;
        return new Rumble(left, right, ack);
    }

    static VideoStatus parseVideoStatus(byte[] data, int length) {
        if (!validHeader(data, length, 5, VIDEO_STATUS_SIZE)) return null;
        ByteBuffer b = ByteBuffer.wrap(data, 6, VIDEO_STATUS_SIZE - 6).order(ByteOrder.LITTLE_ENDIAN);
        int captureResult = b.get() & 0xff;
        int flags = b.get() & 0xff;
        long ack = b.getInt() & 0xffffffffL;
        long framesSent = b.getInt() & 0xffffffffL;
        long captureErrors = b.getInt() & 0xffffffffL;
        long sendErrors = b.getInt() & 0xffffffffL;
        b.getInt(); // last socket error
        int sourceWidth = b.getShort() & 0xffff;
        int sourceHeight = b.getShort() & 0xffff;
        return new VideoStatus(captureResult, (flags & 1) != 0, (flags & 4) != 0, ack, framesSent,
                captureErrors, sendErrors, flags, sourceWidth, sourceHeight);
    }

    static byte[] videoFeedback(long rendered) {
        return ByteBuffer.allocate(VIDEO_FEEDBACK_SIZE).order(ByteOrder.LITTLE_ENDIAN)
                .put(new byte[]{'R', '3', '6', '0', 1, 7, 0, 0})
                .putInt((int) rendered).putInt((int) (rendered >>> 32)).array();
    }

    static VideoChunk parseVideo(byte[] data, int length) {
        if (data == null || data.length < VIDEO_HEADER_SIZE || length < VIDEO_HEADER_SIZE) return null;
        int type = data[5];
        boolean jpeg = type == 10 || type == 11;
        boolean bc1 = type == 8 || type == 9;
        boolean parity = type == 6 || type == 9 || type == 11;
        int headerSize = jpeg ? JPEG_HEADER_SIZE : VIDEO_HEADER_SIZE;
        if (length < headerSize || !(type == 4 || parity || bc1 || jpeg)
                || !validHeader(data, length, type, length)) return null;
        ByteBuffer b = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN);
        int width = b.getShort(6) & 0xFFFF;
        int height = b.getShort(8) & 0xFFFF;
        int chunk = b.getShort(10) & 0xFFFF;
        int chunks = b.getShort(12) & 0xFFFF;
        long frameId = b.getInt(14) & 0xFFFFFFFFL;
        int payload = b.getShort(18) & 0xFFFF;
        if (width <= 0 || height <= 0 || width > 1920 || height > 1080
                || chunks <= 0 || chunks > 512
                || payload > VIDEO_PAYLOAD_MAX || headerSize + payload != length) {
            return null;
        }
        if (bc1 && !((width == 320 && height == 180) || (width == BC1_WIDTH && height == BC1_HEIGHT)
                || (width == 640 && height == 360))) return null;
        int frameBytes;
        if (jpeg) {
            boolean validProfile = false;
            for (int i = 2; i < WIDTHS.length; i++) {
                if (width == WIDTHS[i] && height == HEIGHTS[i]) validProfile = true;
            }
            frameBytes = b.getInt(20);
            if (!validProfile || frameBytes < 4 || frameBytes > JPEG_MAX_BYTES) return null;
        } else {
            frameBytes = bc1 ? width * height / 2 : width * height * 2;
        }
        int expectedChunks = (frameBytes + VIDEO_PAYLOAD_MAX - 1) / VIDEO_PAYLOAD_MAX;
        int indices = parity ? (chunks + VIDEO_PARITY_GROUP - 1) / VIDEO_PARITY_GROUP : chunks;
        if (chunk >= indices) return null;
        int expectedPayload = parity ? VIDEO_PAYLOAD_MAX
                : Math.min(VIDEO_PAYLOAD_MAX, frameBytes - chunk * VIDEO_PAYLOAD_MAX);
        if (chunks != expectedChunks || payload != expectedPayload || payload <= 0) return null;
        return new VideoChunk(width, height, chunk, chunks, frameId, payload, parity, bc1, jpeg,
                frameBytes, headerSize);
    }

    static final class Status {
        final int state;
        final boolean bound;
        final boolean watchdog;
        final boolean videoOnly;
        final boolean controllerConflict;
        final boolean controllerFailed;
        final long ack;
        final long valid;
        final long invalid;
        final long xboxMs;

        Status(int state, boolean bound, boolean watchdog, boolean videoOnly,
               boolean controllerConflict, boolean controllerFailed, long ack, long valid,
               long invalid, long xboxMs) {
            this.state = state;
            this.bound = bound;
            this.watchdog = watchdog;
            this.videoOnly = videoOnly;
            this.controllerConflict = controllerConflict;
            this.controllerFailed = controllerFailed;
            this.ack = ack;
            this.valid = valid;
            this.invalid = invalid;
            this.xboxMs = xboxMs;
        }
    }

    static final class Rumble {
        final int left;
        final int right;
        final long ack;

        Rumble(int left, int right, long ack) {
            this.left = left;
            this.right = right;
            this.ack = ack;
        }
    }

    static final class VideoStatus {
        final int captureResult;
        final boolean requested;
        final boolean feedbackSupported;
        final long ack;
        final long framesSent;
        final long captureErrors;
        final long sendErrors;
        final boolean configurable, sourceLimited, encoderLimited, captureLimited, fastSupported;
        final int sourceWidth, sourceHeight;

        VideoStatus(int captureResult, boolean requested, boolean feedbackSupported, long ack, long framesSent,
                    long captureErrors, long sendErrors, int flags, int sourceWidth, int sourceHeight) {
            this.captureResult = captureResult;
            this.requested = requested;
            this.feedbackSupported = feedbackSupported;
            this.ack = ack;
            this.framesSent = framesSent;
            this.captureErrors = captureErrors;
            this.sendErrors = sendErrors;
            configurable = (flags & 8) != 0;
            sourceLimited = (flags & 16) != 0;
            encoderLimited = (flags & 32) != 0;
            captureLimited = (flags & 64) != 0;
            fastSupported = (flags & 128) != 0;
            this.sourceWidth = sourceWidth;
            this.sourceHeight = sourceHeight;
        }
    }

    static final class VideoChunk {
        final int width;
        final int height;
        final int chunk;
        final int chunks;
        final long frameId;
        final int payload;
        final boolean parity;
        final boolean bc1;
        final boolean jpeg;
        final int frameBytes;
        final int headerSize;

        VideoChunk(int width, int height, int chunk, int chunks, long frameId, int payload,
                   boolean parity, boolean bc1, boolean jpeg, int frameBytes, int headerSize) {
            this.width = width;
            this.height = height;
            this.chunk = chunk;
            this.chunks = chunks;
            this.frameId = frameId;
            this.payload = payload;
            this.parity = parity;
            this.bc1 = bc1;
            this.jpeg = jpeg;
            this.frameBytes = frameBytes;
            this.headerSize = headerSize;
        }
    }
}

