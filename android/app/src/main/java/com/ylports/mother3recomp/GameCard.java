package com.ylports.mother3recomp;

import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * What the game's file select shows for a game (where, who, play time), read
 * from the suspended game when there is one, else from the in-game save.
 * Nothing is written. Room names come from the player's own ROM, so the app
 * carries none of the game's text.
 *
 * English v1.3 layout, found by comparing saves and suspend states:
 * - Battery save (datacrystal.tcrf.net, MOTHER 3 SRAM map): summary of slot
 *   1 at 0x00 (slot 2 at +0x64): "M3" at +2, play time in 1/60 s at +0x08,
 *   room at +0x12, party (character numbers) at +0x16, the leader's
 *   character data from +0x3C (name +0x3E, level +0x4E), in use when +0x68
 *   is set. Character data, 0x6C bytes per character number, at 0x4834 for
 *   slot 1: name (8 characters) at +2, level at +0x12.
 * - Suspend state (GBAS snapshot): EWRAM is the first 256 KiB of the BUS0
 *   section. Room at 0x0200C3AC, play time at 0x02004A70, party at
 *   0x02004860, character data at 0x02004110 in the save's layout.
 * - ROM: room names, 1000 u16 offsets at 0xD1EEC0 into the strings at
 *   0xD1F690; characters are ASCII - 0x20, 0x40 a space, 0xFF01 a line break,
 *   0xFFFF the end.
 */
final class GameCard {
    final String place;        // null when the ROM's table can't be trusted
    final int[] party;         // character numbers, leader first
    final String leader;
    final int level;
    final List<String> companions;
    final long playFrames;

    private GameCard(String place, int[] party, String leader, int level, List<String> companions,
                     long playFrames) {
        this.place = place;
        this.party = party;
        this.leader = leader;
        this.level = level;
        this.companions = companions;
        this.playFrames = playFrames;
    }

    /** "01:14", hours and minutes as the game shows them. */
    String playTime() {
        long minutes = playFrames / 3600;
        return String.format(Locale.US, "%02d:%02d", minutes / 60, minutes % 60);
    }

    // ── Suspended game ────────────────────────────────────────────────────

    private static final int EWRAM_SIZE = 0x40000;
    private static final int RAM_ROOM = 0xC3AC;
    private static final int RAM_PLAY_TIME = 0x4A70;
    private static final int RAM_PARTY = 0x4860;
    private static final int RAM_CHARACTERS = 0x4110;

    static GameCard fromSuspendState(File state, File rom) {
        ByteBuffer ewram = suspendedEwram(state);
        if (ewram == null) return null;
        return build(ewram, RAM_CHARACTERS, ewram.getShort(RAM_ROOM) & 0xFFFF,
            ewram.getInt(RAM_PLAY_TIME) & 0xFFFFFFFFL, RAM_PARTY, rom);
    }

    private static ByteBuffer suspendedEwram(File state) {
        try (RandomAccessFile in = new RandomAccessFile(state, "r")) {
            byte[] head = new byte[52];
            in.readFully(head);
            ByteBuffer header = ByteBuffer.wrap(head).order(ByteOrder.LITTLE_ENDIAN);
            if (head[0] != 'G' || head[1] != 'B' || head[2] != 'A' || head[3] != 'S') return null;
            int sections = header.getInt(48);
            long at = 52;
            for (int i = 0; i < sections && at + 8 <= in.length(); ++i) {
                in.seek(at);
                byte[] tag = new byte[4];
                in.readFully(tag);
                int size = Integer.reverseBytes(in.readInt());
                if (tag[0] == 'B' && tag[1] == 'U' && tag[2] == 'S' && tag[3] == '0') {
                    if (size < EWRAM_SIZE) return null;
                    byte[] ewram = new byte[EWRAM_SIZE];
                    in.readFully(ewram);
                    return ByteBuffer.wrap(ewram).order(ByteOrder.LITTLE_ENDIAN);
                }
                at += 8 + (size & 0xFFFFFFFFL);
            }
        } catch (IOException | RuntimeException ignored) {
            // no card rather than a wrong one
        }
        return null;
    }

    // ── In-game save ──────────────────────────────────────────────────────

    private static final int SAVE_SIZE = 0x10000;
    private static final int SLOT_STRIDE = 0x64;
    private static final int[] SLOT_CHARACTERS = {0x4834, 0xA834};

    /** Slot 0 or 1 of the battery save, or null when it is empty. */
    static GameCard fromSave(File sav, int slot, File rom) {
        ByteBuffer save;
        try (RandomAccessFile in = new RandomAccessFile(sav, "r")) {
            if (in.length() < SAVE_SIZE) return null;
            byte[] data = new byte[SAVE_SIZE];
            in.readFully(data);
            save = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN);
        } catch (IOException e) {
            return null;
        }
        int summary = slot * SLOT_STRIDE;
        if (save.get(2) != 'M' || save.get(3) != '3' || save.get(summary + 0x68) == 0) return null;
        // The summary carries the leader itself; the rest of the party comes
        // from the slot's character data.
        int leader = save.get(summary + 0x16) & 0xFF;
        if (leader != (save.get(summary + 0x3C) & 0xFF)) return null;
        return build(save, SLOT_CHARACTERS[slot], save.getShort(summary + 0x12) & 0xFFFF,
            save.getInt(summary + 0x08) & 0xFFFFFFFFL, summary + 0x16, rom);
    }

    // ── Shared ────────────────────────────────────────────────────────────

    private static final int CHARACTER_SIZE = 0x6C;
    private static final int CHARACTER_COUNT = 13;

    private static GameCard build(ByteBuffer data, int characters, int room, long frames,
                                  int party, File rom) {
        List<String> names = new ArrayList<>();
        int[] numbers = new int[4];
        int level = 0;
        for (int i = 0; i < 4; ++i) {
            int number = data.get(party + i) & 0xFF;
            if (number == 0) break;
            numbers[i] = number;
            int block = characters + CHARACTER_SIZE * number;
            if (number > CHARACTER_COUNT || (data.get(block) & 0xFF) != number) return null;
            String name = decode(data, block + 2, 8);
            if (name == null) return null;
            if (i == 0) level = data.get(block + 0x12) & 0xFF;
            names.add(name);
        }
        if (names.isEmpty() || level < 1 || level > 99) return null;
        return new GameCard(roomName(rom, room), java.util.Arrays.copyOf(numbers, names.size()),
            names.get(0), level, names.subList(1, names.size()), frames);
    }

    /** Up to `length` characters in the game's encoding, or null if one isn't plain text. */
    private static String decode(ByteBuffer data, int at, int length) {
        StringBuilder text = new StringBuilder();
        for (int i = 0; i < length; ++i) {
            int c = data.getShort(at + 2 * i) & 0xFFFF;
            if (c == 0xFFFF) break;
            if (c == 0xFF01) return text.toString();  // a room's second line
            if (c == 0x40) { text.append(' '); continue; }
            if (c > 0x5E) return null;
            text.append((char) (c + 0x20));
        }
        return text.toString().trim();
    }

    private static final long ROOM_OFFSETS = 0xD1EEC0L;
    private static final long ROOM_STRINGS = 0xD1F690L;
    private static final int ROOM_COUNT = 1000;
    private static final int CHECK_ROOM = 11;
    private static final String CHECK_NAME = "Prayer Sanctuary";

    /**
     * Whether `rom` is laid out as this class (and Portraits) expect: another
     * ROM (the Japanese one, a different patch) keeps its data elsewhere, so
     * check a room name known to be in the table.
     */
    static boolean knownLayout(RandomAccessFile in) throws IOException {
        return CHECK_NAME.equals(readRoom(in, CHECK_ROOM));
    }

    static String roomName(File rom, int room) {
        if (room <= 0 || room >= ROOM_COUNT) return null;
        try (RandomAccessFile in = new RandomAccessFile(rom, "r")) {
            if (!knownLayout(in)) return null;
            String name = readRoom(in, room);
            return name == null || name.isEmpty() ? null : name;
        } catch (IOException e) {
            return null;
        }
    }

    private static String readRoom(RandomAccessFile in, int room) throws IOException {
        in.seek(ROOM_OFFSETS + 2L * room);
        int offset = Short.reverseBytes(in.readShort()) & 0xFFFF;
        in.seek(ROOM_STRINGS + offset);
        byte[] raw = new byte[80];
        in.readFully(raw);
        return decode(ByteBuffer.wrap(raw).order(ByteOrder.LITTLE_ENDIAN), 0, raw.length / 2);
    }
}
