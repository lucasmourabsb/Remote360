package com.remote360.client;

import org.junit.Test;

import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.TimeUnit;

import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.fail;

public final class UdpClientTest {
    @Test
    public void videoRequestChangesOnAnOpenConnection() throws Exception {
        try (DatagramSocket xbox = new DatagramSocket(0, InetAddress.getByName("127.0.0.1"))) {
            xbox.setSoTimeout(1000);
            UdpClient client = new UdpClient("127.0.0.1", xbox.getLocalPort(), new GamepadState(), false,
                    new UdpClient.Listener() {
                @Override public void onStatus(UdpClient source, Protocol.Status status, long ackMs) {}
                @Override public void onRumble(UdpClient source, Protocol.Rumble rumble) {}
                @Override public void onVideoFrame(UdpClient source, UdpClient.VideoFrame frame) {}
                @Override public void onNetworkError(UdpClient source, String message) {}
            });
            Thread worker = new Thread(client);
            worker.setDaemon(true);
            worker.start();
            try {
                int sourcePort = awaitVideoFlag(xbox, false, 0);
                client.setVideoEnabled(true);
                awaitVideoFlag(xbox, true, sourcePort);
                client.setVideoEnabled(false);
                awaitVideoFlag(xbox, false, sourcePort);
            } finally {
                client.stop();
                worker.join(1500);
            }
            assertFalse(worker.isAlive());
        }
    }

    @Test
    public void reconnectingConsoleMayRestartFrameIdsWithoutRestartingTheApp() throws Exception {
        try (DatagramSocket xbox = new DatagramSocket(0, InetAddress.getByName("127.0.0.1"))) {
            xbox.setSoTimeout(1000);
            ArrayBlockingQueue<Long> frames = new ArrayBlockingQueue<>(4);
            UdpClient client = new UdpClient("127.0.0.1", xbox.getLocalPort(), new GamepadState(), true,
                    new UdpClient.Listener() {
                @Override public void onStatus(UdpClient source, Protocol.Status status, long ackMs) {}
                @Override public void onRumble(UdpClient source, Protocol.Rumble rumble) {}
                @Override public void onVideoFrame(UdpClient source, UdpClient.VideoFrame frame) {
                    frames.add(frame.id);
                }
                @Override public void onNetworkError(UdpClient source, String message) {}
            });
            Thread worker = new Thread(client);
            worker.setDaemon(true);
            worker.start();
            try {
                int port = awaitVideoFlag(xbox, true, 0);
                InetSocketAddress receiver = new InetSocketAddress("127.0.0.1", port);
                send(xbox, receiver, status(100, 500));
                send(xbox, receiver, ProtocolTest.video(500, 32, 32, 0));
                send(xbox, receiver, ProtocolTest.video(500, 32, 32, 1));
                assertEquals(Long.valueOf(500), frames.poll(2, TimeUnit.SECONDS));
                send(xbox, receiver, status(101, 1));
                send(xbox, receiver, ProtocolTest.video(1, 32, 32, 0));
                send(xbox, receiver, ProtocolTest.video(1, 32, 32, 1));
                assertEquals(Long.valueOf(1), frames.poll(2, TimeUnit.SECONDS));
            } finally {
                client.stop();
                worker.join(1500);
            }
            assertFalse(worker.isAlive());
        }
    }

    private static byte[] status(int sequence, int frames) {
        return ByteBuffer.allocate(Protocol.VIDEO_STATUS_SIZE).order(ByteOrder.LITTLE_ENDIAN)
                .put(new byte[]{'R', '3', '6', '0', 1, 5, 0, 1})
                .putInt(sequence).putInt(frames).array();
    }

    private static void send(DatagramSocket socket, InetSocketAddress target, byte[] data) throws Exception {
        socket.send(new DatagramPacket(data, data.length, target));
    }

    private static int awaitVideoFlag(DatagramSocket xbox, boolean expected, int port) throws Exception {
        byte[] data = new byte[Protocol.INPUT_SIZE];
        DatagramPacket packet = new DatagramPacket(data, data.length);
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(2);
        while (System.nanoTime() < deadline) {
            packet.setLength(data.length);
            xbox.receive(packet);
            if (Protocol.validHeader(data, packet.getLength(), 1, Protocol.INPUT_SIZE)
                    && (port == 0 || packet.getPort() == port)
                    && ((data[6] & 2) != 0) == expected) return packet.getPort();
        }
        fail("Expected video request flag " + expected + " on the same connection");
        return -1;
    }
}
