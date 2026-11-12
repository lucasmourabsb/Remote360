package com.remote360.client;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;

import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.PortUnreachableException;
import java.net.SocketTimeoutException;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;

final class UdpClient implements Runnable {
    private static final long KEEPALIVE_NANOS = 8_000_000L;
    private static final long FEEDBACK_NANOS = 250_000_000L;
    private static final long STATUS_GAP_NANOS = 1_500_000_000L;

    interface Listener {
        void onStatus(UdpClient source, Protocol.Status status, long ackMs);
        void onRumble(UdpClient source, Protocol.Rumble rumble);
        void onVideoFrame(UdpClient source, VideoFrame frame);
        void onNetworkError(UdpClient source, String message);
        default void onVideoStatus(UdpClient source, Protocol.VideoStatus status) {}
    }

    private final String host;
    private final int port;
    private final GamepadState gamepad;
    private final Listener listener;
    private final AtomicBoolean running = new AtomicBoolean(true);
    private final AtomicInteger mediaGeneration = new AtomicInteger();
    private final ArrayBlockingQueue<EncodedFrame> decodeQueue = new ArrayBlockingQueue<>(1);
    private volatile boolean videoEnabled;
    private volatile int videoSettings = -1;
    private volatile long rendered;
    private boolean feedbackSupported;
    private Protocol.VideoStatus lastVideoStatus;
    private long lastAckAt;
    private int assemblyGeneration = -1;
    private final FrameAssembler frames = new FrameAssembler();
    private final AckTracker acknowledgements = new AckTracker();

    UdpClient(String host, GamepadState gamepad, boolean videoEnabled, Listener listener) {
        this(host, Protocol.PORT, gamepad, videoEnabled, listener);
    }

    UdpClient(String host, int port, GamepadState gamepad, boolean videoEnabled, Listener listener) {
        this.host = host;
        this.port = port;
        this.gamepad = gamepad;
        this.listener = listener;
        this.videoEnabled = videoEnabled;
    }

    void stop() {
        running.set(false);
    }

    void setVideoEnabled(boolean enabled) {
        if (videoEnabled == enabled) return;
        videoEnabled = enabled;
        discardQueuedVideo();
    }

    void setVideoSettings(int profile, int fpsIndex, boolean fast) {
        int settings = profile | fpsIndex << 4 | (fast ? 0x100 : 0);
        if (videoSettings == settings) return;
        videoSettings = settings;
        discardQueuedVideo();
    }

    boolean isCurrentVideo(VideoFrame frame) {
        return running.get() && videoEnabled && frame.generation == mediaGeneration.get();
    }

    void frameRendered(long frameId) {
        long count = (rendered >>> 32) + 1;
        rendered = (count << 32) | (frameId & 0xffffffffL);
    }

    @Override
    public void run() {
        try (DatagramSocket socket = new DatagramSocket()) {
            InetAddress address = InetAddress.getByName(host);
            socket.connect(new InetSocketAddress(address, port));
            socket.setReceiveBufferSize(4 * 1024 * 1024);
            socket.setSoTimeout(2);
            // Extra byte makes an oversized/truncated UDP datagram fail length validation.
            byte[] receive = new byte[Protocol.JPEG_HEADER_SIZE + Protocol.VIDEO_PAYLOAD_MAX + 1];
            DatagramPacket reply = new DatagramPacket(receive, receive.length);
            int sequence = 0;
            long nextSend = System.nanoTime();
            long nextFeedback = nextSend;
            Thread decoder = new Thread(this::decodeVideo, "Remote360-Video");
            decoder.start();
            try {
                while (running.get()) {
                    long now = System.nanoTime();
                    try {
                        if (now - nextSend >= 0) {
                            nextSend = now + KEEPALIVE_NANOS;
                            int settings = videoSettings;
                            byte[] packet = Protocol.inputPacket(sequence, gamepad.snapshot(), videoEnabled, settings,
                                    settings >= 0 && (settings & 0x100) != 0);
                            acknowledgements.sent(sequence++, now);
                            socket.send(new DatagramPacket(packet, packet.length));
                        }
                        long current = rendered;
                        if (videoEnabled && feedbackSupported && current != 0 && now - nextFeedback >= 0) {
                            nextFeedback = now + FEEDBACK_NANOS;
                            byte[] packet = Protocol.videoFeedback(current);
                            socket.send(new DatagramPacket(packet, packet.length));
                        }
                        reply.setLength(receive.length);
                        socket.receive(reply);
                        receivePacket(receive, reply.getLength());
                    } catch (SocketTimeoutException | PortUnreachableException ignored) {
                        // The console may be rebooting. Keep the same socket and retry on schedule.
                    }
                }
            } finally {
                decoder.interrupt();
                // Network work stays on this worker, never in Activity.onPause().
                // A lost release is also covered by the console's 350 ms watchdog.
                GamepadState.Snapshot neutral = new GamepadState.Snapshot(0, false, 0, 0, 0, 0, 0, 0);
                for (int i = 0; i < 3; i++) {
                    try {
                        byte[] packet = Protocol.inputPacket(sequence++, neutral, false);
                        socket.send(new DatagramPacket(packet, packet.length));
                    } catch (Exception ignored) {
                        break;
                    }
                }
            }
        } catch (Exception e) {
            if (running.get()) listener.onNetworkError(this, e.getClass().getSimpleName() + ": " + e.getMessage());
        }
    }

    private void receivePacket(byte[] packet, int length) {
        Protocol.Status status = Protocol.parseStatus(packet, length);
        if (status != null) {
            long now = System.nanoTime();
            if (lastAckAt != 0 && now - lastAckAt >= STATUS_GAP_NANOS) {
                frames.reset();
                discardQueuedVideo();
                lastVideoStatus = null;
                feedbackSupported = false;
            }
            lastAckAt = now;
            listener.onStatus(this, status, acknowledgements.acknowledge(status.ack, now));
            return;
        }
        Protocol.Rumble rumble = Protocol.parseRumble(packet, length);
        if (rumble != null) {
            listener.onRumble(this, rumble);
            return;
        }
        if (!videoEnabled) return;
        Protocol.VideoStatus videoStatus = Protocol.parseVideoStatus(packet, length);
        if (videoStatus != null) {
            if (lastVideoStatus != null) {
                if ((int) (videoStatus.ack - lastVideoStatus.ack) < 0) return;
                if ((int) (videoStatus.framesSent - lastVideoStatus.framesSent) < 0) {
                    frames.reset();
                    discardQueuedVideo();
                }
            }
            lastVideoStatus = videoStatus;
            feedbackSupported = videoStatus.feedbackSupported;
            listener.onVideoStatus(this, videoStatus);
            return;
        }
        Protocol.VideoChunk video = Protocol.parseVideo(packet, length);
        if (video == null) return;
        int generation = mediaGeneration.get();
        if (assemblyGeneration != generation) {
            frames.reset();
            assemblyGeneration = generation;
        }
        byte[] complete = frames.accept(packet, video);
        if (complete != null) {
            EncodedFrame frame = new EncodedFrame(video, complete, generation);
            while (!decodeQueue.offer(frame)) decodeQueue.poll();
        }
    }

    private void discardQueuedVideo() {
        mediaGeneration.incrementAndGet();
        decodeQueue.clear();
    }

    private void decodeVideo() {
        try {
            while (running.get() && !Thread.currentThread().isInterrupted()) {
                EncodedFrame frame = decodeQueue.take();
                Protocol.VideoChunk chunk = frame.chunk;
                byte[] pixels = frame.data;
                Bitmap bitmap = null;
                if (chunk.jpeg) {
                    BitmapFactory.Options options = new BitmapFactory.Options();
                    options.inJustDecodeBounds = true;
                    BitmapFactory.decodeByteArray(frame.data, 0, frame.data.length, options);
                    if (options.outWidth != chunk.width || options.outHeight != chunk.height) continue;
                    options.inJustDecodeBounds = false;
                    options.inPreferredConfig = Bitmap.Config.RGB_565;
                    bitmap = BitmapFactory.decodeByteArray(frame.data, 0, frame.data.length, options);
                    if (bitmap == null) continue;
                    pixels = null;
                } else if (chunk.bc1) {
                    pixels = Bc1.decode(frame.data, chunk.width, chunk.height);
                }
                if (videoEnabled && running.get() && frame.generation == mediaGeneration.get()) {
                    listener.onVideoFrame(this,
                            new VideoFrame(chunk.width, chunk.height, chunk.frameId, frame.generation, pixels, bitmap));
                }
            }
        } catch (InterruptedException stop) {
            Thread.currentThread().interrupt();
        }
    }

    private static final class EncodedFrame {
        final Protocol.VideoChunk chunk;
        final byte[] data;
        final int generation;

        EncodedFrame(Protocol.VideoChunk chunk, byte[] data, int generation) {
            this.chunk = chunk;
            this.data = data;
            this.generation = generation;
        }
    }

    static final class VideoFrame {
        final int width, height;
        final long id;
        final int generation;
        final byte[] rgb565;
        final Bitmap bitmap;

        VideoFrame(int width, int height, long id, int generation, byte[] rgb565, Bitmap bitmap) {
            this.width = width;
            this.height = height;
            this.id = id;
            this.generation = generation;
            this.rgb565 = rgb565;
            this.bitmap = bitmap;
        }
    }
}

