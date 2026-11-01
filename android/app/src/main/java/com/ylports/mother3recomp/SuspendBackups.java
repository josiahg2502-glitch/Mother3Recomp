package com.ylports.mother3recomp;

import android.util.Log;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.text.SimpleDateFormat;
import java.util.Arrays;
import java.util.Date;
import java.util.Locale;

/**
 * Keeps copies of the last suspend states. Players often go days without an
 * in-game save, so the suspend state holds their progress, yet it is the most
 * fragile file: only the engine build and ROM bytes that wrote it can load it,
 * a newer battery save (an import) stops the game from resuming it, and the
 * next suspend overwrites it. Before a game starts, the current state (and
 * its RetroAchievements sidecar) is copied to
 * files/suspend-backups/<stem>.<state time>.state; the newest {@link #KEEP}
 * are kept. {@link #restore} puts one back for the next PLAY.
 */
final class SuspendBackups {
    private static final String TAG = "Mother3Suspend";
    private static final int KEEP = 8;

    private SuspendBackups() {}

    private static String stem(String romFile) {
        return romFile.contains(".") ? romFile.substring(0, romFile.lastIndexOf('.')) : romFile;
    }

    static void keep(File filesDir, String romFile) {
        String stem = stem(romFile);
        File state = new File(filesDir, "roms/" + stem + ".suspend.state");
        if (!state.isFile() || state.length() == 0) return;
        File root = new File(filesDir, "suspend-backups");
        String stamp = new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US)
            .format(new Date(state.lastModified()));
        File copy = new File(root, stem + "." + stamp + ".state");
        if (copy.exists()) return;  // this state is backed up already
        if (!root.isDirectory() && !root.mkdirs()) {
            Log.w(TAG, "cannot create " + root);
            return;
        }
        try {
            File sidecar = new File(state.getPath() + ".ra");
            if (sidecar.isFile()) copyAtomically(sidecar, new File(copy.getPath() + ".ra"));
            // The state last: its presence marks a complete backup.
            copyAtomically(state, copy);
        } catch (IOException e) {
            Log.w(TAG, "cannot back up " + state.getName(), e);
            return;
        }
        prune(root, stem);
    }

    /** The backups of this ROM's suspend state, newest first. */
    static File[] list(File filesDir, String romFile) {
        String stem = stem(romFile);
        File[] all = new File(filesDir, "suspend-backups").listFiles(
            (d, name) -> name.startsWith(stem + ".") && name.endsWith(".state"));
        if (all == null) return new File[0];
        Arrays.sort(all, (a, b) -> b.getName().compareTo(a.getName()));
        return all;
    }

    /**
     * Makes {@code backup} the state the next PLAY resumes. The current state
     * is backed up first, so a restore can itself be undone. Not while the
     * game runs: it would overwrite the files when it suspends.
     */
    static void restore(File filesDir, String romFile, File backup) throws IOException {
        keep(filesDir, romFile);
        String stem = stem(romFile);
        File state = new File(filesDir, "roms/" + stem + ".suspend.state");
        File sidecar = new File(state.getPath() + ".ra");
        File backupSidecar = new File(backup.getPath() + ".ra");
        copyAtomically(backup, state);
        // RetroAchievements progress must match the state it belongs to.
        if (backupSidecar.isFile()) copyAtomically(backupSidecar, sidecar);
        else sidecar.delete();
        // The engine resumes the state only while this marker exists and
        // names the battery save as it is now.
        SuspendMarker.write(filesDir, stem, "0");
    }

    private static void prune(File root, String stem) {
        String[] names = root.list((d, name) -> name.startsWith(stem + ".") && name.endsWith(".state"));
        if (names == null || names.length <= KEEP) return;
        Arrays.sort(names);  // yyyyMMdd-HHmmss sorts by time
        for (int i = 0; i < names.length - KEEP; ++i) {
            new File(root, names[i]).delete();
            new File(root, names[i] + ".ra").delete();
        }
    }

    private static void copyAtomically(File from, File to) throws IOException {
        File temp = new File(to.getPath() + ".tmp");
        try (InputStream in = new FileInputStream(from);
             FileOutputStream out = new FileOutputStream(temp)) {
            byte[] buffer = new byte[64 << 10];
            for (int n; (n = in.read(buffer)) > 0; ) out.write(buffer, 0, n);
            out.getFD().sync();
        }
        if (!temp.renameTo(to)) {
            temp.delete();
            throw new IOException("cannot write " + to);
        }
    }
}
