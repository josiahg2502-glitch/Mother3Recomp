package com.ylports.mother3recomp.cloud;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.text.SimpleDateFormat;
import java.util.Arrays;
import java.util.Date;
import java.util.Locale;

/**
 * Three-way save sync between the device's save file and one cloud provider.
 *
 * The store remembers the content hash both sides agreed on at the last
 * successful sync (the "base"). Only the side that moved away from the base
 * is copied over the other; when both moved (or there is no base and they
 * differ), the player chooses. Every overwritten local save, and a cloud save
 * the player chose to overwrite, is kept in the backup folder first.
 */
public final class SaveSync {
    /** MOTHER 3 uses 512 Kbit flash. */
    static final int FLASH_SIZE = 64 * 1024;
    /** mGBA appends a 16-byte RTC block to some .sav files; it is dropped. */
    static final int MGBA_RTC_FOOTER = 16;
    private static final int BACKUPS_KEPT = 10;

    public interface Store {
        String get(String key);
        void put(String key, String value);
    }

    public enum Choice { AUTO, KEEP_DEVICE, KEEP_CLOUD }

    public enum Outcome { NOTHING_TO_DO, IN_SYNC, UPLOADED, DOWNLOADED, CONFLICT }

    public static final class Result {
        public final Outcome outcome;
        public final String message;

        Result(Outcome outcome, String message) {
            this.outcome = outcome;
            this.message = message;
        }
    }

    private final CloudProvider provider;
    private final File saveFile;
    private final File backupDir;
    private final Store store;

    public SaveSync(CloudProvider provider, File saveFile, File backupDir, Store store) {
        this.provider = provider;
        this.saveFile = saveFile;
        this.backupDir = backupDir;
        this.store = store;
    }

    static boolean isPlausibleSaveSize(long size) {
        return size == FLASH_SIZE || size == FLASH_SIZE + MGBA_RTC_FOOTER;
    }

    /** Forget the agreed state, e.g. after switching server or account. */
    public static void forget(Store store, String stateKey) {
        store.put(baseKey(stateKey), null);
    }

    private static String baseKey(String stateKey) {
        return "base|" + stateKey;
    }

    public Result sync(Choice choice) throws IOException {
        final String algorithm = provider.hashAlgorithm();
        final String key = baseKey(provider.stateKey());
        final String base = store.get(key);

        byte[] local = saveFile.isFile() ? readFile(saveFile) : null;
        if (local != null && !isPlausibleSaveSize(local.length)) {
            throw new IOException("The save on this device has an unexpected size ("
                + local.length + " bytes); not syncing it.");
        }
        String localHash = local == null ? null : hex(digest(algorithm, local));

        CloudProvider.RemoteSave remote = provider.latest();
        byte[] remoteData = null;
        String remoteHash = remote == null ? null : remote.hash;
        if (remote != null && remoteHash == null) {
            // Server without a content hash for this file: compare the bytes.
            remoteData = provider.download(remote);
            remoteHash = hex(digest(algorithm, remoteData));
        }

        if (local == null && remote == null) {
            return new Result(Outcome.NOTHING_TO_DO, "No save yet, on this device or in the cloud.");
        }
        if (local != null && localHash.equals(remoteHash)) {
            store.put(key, localHash);
            return new Result(Outcome.IN_SYNC, "Save is up to date.");
        }

        final boolean push;
        switch (choice) {
            case KEEP_DEVICE:
                if (local == null) throw new IOException("There is no save on this device.");
                push = true;
                break;
            case KEEP_CLOUD:
                if (remote == null) throw new IOException("There is no save in the cloud.");
                push = false;
                break;
            default:
                if (remote == null) {
                    push = true;
                } else if (local == null) {
                    push = false;
                } else {
                    boolean localChanged = !localHash.equals(base);
                    boolean remoteChanged = !remoteHash.equals(base);
                    if (localChanged && !remoteChanged) push = true;
                    else if (!localChanged && remoteChanged) push = false;
                    else return new Result(Outcome.CONFLICT, base == null
                        ? "This device and the cloud have different saves."
                        : "The save changed both on this device and in the cloud.");
                }
        }

        if (push) {
            if (remote != null && choice == Choice.KEEP_DEVICE) {
                // The player chose to replace a cloud save this device never
                // had. RomM keeps no history of it, so keep a copy here, as
                // for a replaced local save.
                if (remoteData == null) remoteData = provider.download(remote);
                backup(remoteData, "cloud-before-upload");
            }
            try {
                CloudProvider.RemoteSave uploaded = provider.upload(local, remote);
                store.put(key, uploaded.hash != null ? uploaded.hash : localHash);
            } catch (CloudProvider.ConflictException conflict) {
                return new Result(Outcome.CONFLICT, conflict.getMessage());
            }
            return new Result(Outcome.UPLOADED, "Uploaded this device's save.");
        }

        if (remoteData == null) remoteData = provider.download(remote);
        if (remote.hash != null && !remote.hash.equals(hex(digest(algorithm, remoteData)))) {
            throw new IOException("The downloaded save does not match its checksum; "
                + "kept the save on this device.");
        }
        if (!isPlausibleSaveSize(remoteData.length)) {
            throw new IOException("The cloud save has an unexpected size ("
                + remoteData.length + " bytes); kept the save on this device.");
        }
        byte[] data = remoteData.length == FLASH_SIZE ? remoteData
            : Arrays.copyOf(remoteData, FLASH_SIZE);
        if (local != null) backup(local, "before-download");
        writeAtomically(saveFile, data);
        store.put(key, remoteHash);
        return new Result(Outcome.DOWNLOADED, "Downloaded the cloud save"
            + (local != null ? " (previous save backed up)." : "."));
    }

    private void backup(byte[] data, String reason) throws IOException {
        if (!backupDir.isDirectory() && !backupDir.mkdirs()) {
            throw new IOException("Cannot create " + backupDir);
        }
        String stamp = new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(new Date());
        writeAtomically(new File(backupDir, saveFile.getName() + "." + stamp + "." + reason + ".bak"),
            data);
        File[] all = backupDir.listFiles((dir, name) -> name.startsWith(saveFile.getName() + "."));
        if (all != null && all.length > BACKUPS_KEPT) {
            Arrays.sort(all, (a, b) -> a.getName().compareTo(b.getName()));
            for (int i = 0; i < all.length - BACKUPS_KEPT; ++i) all[i].delete();
        }
    }

    static void writeAtomically(File target, byte[] data) throws IOException {
        File parent = target.getParentFile();
        if (parent != null && !parent.isDirectory() && !parent.mkdirs()) {
            throw new IOException("Cannot create " + parent);
        }
        File temp = new File(target.getPath() + ".tmp");
        try (FileOutputStream out = new FileOutputStream(temp)) {
            out.write(data);
            out.getFD().sync();
        }
        if (!temp.renameTo(target)) {
            temp.delete();
            throw new IOException("Cannot write " + target);
        }
    }

    static byte[] readFile(File file) throws IOException {
        return CloudHttp.readAll(new FileInputStream(file));
    }

    static byte[] digest(String algorithm, byte[] data) {
        try {
            return MessageDigest.getInstance(algorithm).digest(data);
        } catch (NoSuchAlgorithmException impossible) {
            throw new IllegalStateException(impossible);
        }
    }

    static String hex(byte[] bytes) {
        StringBuilder out = new StringBuilder(bytes.length * 2);
        for (byte b : bytes) out.append(String.format(Locale.US, "%02x", b & 0xff));
        return out.toString();
    }
}
