package com.remote360.client;

/** Opaque RGB blocks, decoded off the Android UI thread. */
final class Bc1 {
    private Bc1() {}

    private static int mix(int a, int b, int weight, int divisor) {
        int r = ((a >>> 11) * weight + (b >>> 11)) / divisor;
        int g = (((a >>> 5) & 63) * weight + ((b >>> 5) & 63)) / divisor;
        int bl = ((a & 31) * weight + (b & 31)) / divisor;
        return r << 11 | g << 5 | bl;
    }

    static byte[] decode(byte[] blocks, int width, int height) {
        byte[] out = new byte[width * height * 2];
        int[] palette = new int[4];
        int block = 0;
        for (int y = 0; y < height; y += 4) {
            for (int x = 0; x < width; x += 4, block += 8) {
                int a = (blocks[block] & 255) | (blocks[block + 1] & 255) << 8;
                int b = (blocks[block + 2] & 255) | (blocks[block + 3] & 255) << 8;
                palette[0] = a;
                palette[1] = b;
                palette[2] = a > b ? mix(a, b, 2, 3) : mix(a, b, 1, 2);
                palette[3] = a > b ? mix(b, a, 2, 3) : 0;
                for (int row = 0; row < 4; row++) {
                    int indices = blocks[block + 4 + row] & 255;
                    int dest = ((y + row) * width + x) * 2;
                    for (int col = 0; col < 4; col++, dest += 2) {
                        int color = palette[indices & 3];
                        out[dest] = (byte) color;
                        out[dest + 1] = (byte) (color >>> 8);
                        indices >>>= 2;
                    }
                }
            }
        }
        return out;
    }
}
