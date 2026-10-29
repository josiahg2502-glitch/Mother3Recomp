package com.ylports.mother3recomp;

import android.graphics.Bitmap;
import android.graphics.Color;

import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;

/**
 * The file select's character portraits, decoded from the player's own ROM
 * (English v1.3) when the launcher shows them; the app carries none.
 *
 * They are 32x32 cells of 4bpp tiles (1D, 16 tiles) in one sheet; the game
 * shows the top 32x24, frame included. Cell and 16-colour palette per
 * character number (1 Flint ... 13 Claus) were read back from the game's file
 * select with each character leading the party.
 */
final class Portraits {
    static final int WIDTH = 32, HEIGHT = 24;

    private static final long SHEET = 0x1B99564L;
    private static final int CELL_BYTES = 0x200;
    private static final int[] CELL = {-1, 0, 1, 3, 5, 4, 6, 2, 8, 9, 10, 11, 7, 22};
    private static final long[] PALETTE = {-1, 0x1B9D998L, 0x1B9D998L, 0x1B9D978L, 0x1B9D998L,
        0x1B9D9B8L, 0x1B9D978L, 0x1B9D958L, 0x1B9D958L, 0x1B9D9B8L, 0x1B9D978L, 0x1B9D978L,
        0x1B9D958L, 0x1B9DA38L};

    private Portraits() {}

    /** The portrait of character `number`, or null for an unknown one or ROM. */
    static Bitmap load(File rom, int number) {
        if (number <= 0 || number >= CELL.length) return null;
        try (RandomAccessFile in = new RandomAccessFile(rom, "r")) {
            if (!GameCard.knownLayout(in)) return null;
            byte[] colours = new byte[32];
            in.seek(PALETTE[number]);
            in.readFully(colours);
            int[] palette = new int[16];
            for (int i = 1; i < 16; ++i) {  // colour 0 is transparent
                int c = (colours[2 * i] & 0xFF) | (colours[2 * i + 1] & 0xFF) << 8;
                palette[i] = Color.rgb((c & 31) << 3, (c >> 5 & 31) << 3, (c >> 10 & 31) << 3);
            }
            byte[] tiles = new byte[12 * 32];
            in.seek(SHEET + (long) CELL_BYTES * CELL[number]);
            in.readFully(tiles);
            int[] pixels = new int[WIDTH * HEIGHT];
            for (int t = 0; t < 12; ++t)
                for (int y = 0; y < 8; ++y)
                    for (int x = 0; x < 8; ++x) {
                        int pair = tiles[t * 32 + y * 4 + x / 2] & 0xFF;
                        int index = (x & 1) != 0 ? pair >> 4 : pair & 15;
                        pixels[(t / 4 * 8 + y) * WIDTH + t % 4 * 8 + x] = palette[index];
                    }
            return Bitmap.createBitmap(pixels, WIDTH, HEIGHT, Bitmap.Config.ARGB_8888);
        } catch (IOException | RuntimeException e) {
            return null;
        }
    }
}
