package com.remote360.client;

/** Elapsed time for the exact acknowledged input, including the server's ACK scheduling delay. */
final class AckTracker {
    private static final int SIZE = 1024;
    private final long[] sequences = new long[SIZE];
    private final long[] times = new long[SIZE];
    private final boolean[] valid = new boolean[SIZE];

    void sent(int sequence, long nowNanos) {
        int index = sequence & (SIZE - 1);
        sequences[index] = sequence & 0xFFFFFFFFL;
        times[index] = nowNanos;
        valid[index] = true;
    }

    long acknowledge(long sequence, long nowNanos) {
        int index = (int) sequence & (SIZE - 1);
        if (!valid[index] || sequences[index] != sequence) return -1;
        valid[index] = false;
        long elapsed = nowNanos - times[index];
        return elapsed >= 0 ? elapsed / 1_000_000L : -1;
    }
}
