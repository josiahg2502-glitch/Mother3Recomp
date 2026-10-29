package com.ylports.mother3recomp.cloud;

import java.io.IOException;

/** A server that keeps the cloud copy of the save file. All calls block. */
public interface CloudProvider {
    /** The cloud copy of the save. */
    final class RemoteSave {
        /** RomM save id or Hoard save id. */
        public final String id;
        /** Hoard version number; 0 for RomM, which keeps one copy per file name. */
        public final long version;
        /** Content hash in {@link #hashAlgorithm()}, lowercase hex; null when unknown. */
        public final String hash;
        /** Server timestamp, for display. */
        public final String updatedAt;

        public RemoteSave(String id, long version, String hash, String updatedAt) {
            this.id = id;
            this.version = version;
            this.hash = hash;
            this.updatedAt = updatedAt;
        }
    }

    /** The server moved on since the version an upload was based on. */
    final class ConflictException extends IOException {
        private static final long serialVersionUID = 1L;

        public ConflictException(String message) {
            super(message);
        }
    }

    /** "MD5" or "SHA-256": what the server reports for a file's content. */
    String hashAlgorithm();

    /** Identifies the account and game, so sync state never leaks across them. */
    String stateKey();

    /** The newest usable cloud save, or null when there is none. */
    RemoteSave latest() throws IOException;

    byte[] download(RemoteSave save) throws IOException;

    /**
     * Uploads a new cloud copy. {@code base} is the cloud save the device last
     * agreed with (null if none); providers that can detect a newer cloud copy
     * throw {@link ConflictException}.
     */
    RemoteSave upload(byte[] data, RemoteSave base) throws IOException;
}
