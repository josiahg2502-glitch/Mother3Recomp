package com.ylports.mother3recomp;

import android.util.Log;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.Locale;

/**
 * Since mstan/gbarecomp d4bea3c the engine resumes a suspended game only when
 * its marker (roms/<stem>.suspend.pending: the frame, then the battery save's
 * SHA-1) still matches the battery save, so a newer .sav is never rolled
 * back. Earlier engines wrote the frame alone, and the new one refuses that
 * ("incomplete marker cannot resume"): a player's suspended game would be
 * dropped on the first launch after the update. Before then, such a marker
 * is completed with the battery save as it is now, which is what the earlier
 * engines trusted when they resumed it. Fingerprinted markers are left alone.
 */
final class SuspendMarker {
    private static final String TAG = "Mother3Suspend";

    private SuspendMarker() {}

    static void upgrade(File filesDir, String romFile) {
        String stem = romFile.contains(".") ? romFile.substring(0, romFile.lastIndexOf('.')) : romFile;
        File marker = new File(filesDir, "roms/" + stem + ".suspend.pending");
        if (!marker.isFile()) return;
        try {
            String[] fields = new String(readAll(marker), StandardCharsets.US_ASCII).trim().split("\\s+");
            if (fields.length != 1 || fields[0].isEmpty()) return;  // already fingerprinted
            File save = new File(filesDir, "roms/" + stem + ".sav");
            String battery = save.isFile() ? sha1(readAll(save)) : "missing";
            File temp = new File(marker.getPath() + ".tmp");
            try (FileOutputStream out = new FileOutputStream(temp)) {
                out.write((fields[0] + "\n" + battery + "\n").getBytes(StandardCharsets.US_ASCII));
                out.getFD().sync();
            }
            if (!temp.renameTo(marker)) {
                temp.delete();
                throw new IOException("cannot write " + marker);
            }
        } catch (IOException | NoSuchAlgorithmException e) {
            Log.w(TAG, "cannot upgrade " + marker.getName(), e);
        }
    }

    private static String sha1(byte[] data) throws NoSuchAlgorithmException {
        StringBuilder hex = new StringBuilder();
        for (byte b : MessageDigest.getInstance("SHA-1").digest(data))
            hex.append(String.format(Locale.US, "%02x", b & 0xff));
        return hex.toString();
    }

    private static byte[] readAll(File file) throws IOException {
        try (InputStream in = new FileInputStream(file)) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buffer = new byte[64 << 10];
            for (int n; (n = in.read(buffer)) > 0; ) out.write(buffer, 0, n);
            return out.toByteArray();
        }
    }
}
