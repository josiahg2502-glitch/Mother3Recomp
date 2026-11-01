package com.ylports.mother3recomp.cloud;

import com.github.luben.zstd.ZstdInputStream;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.ByteArrayInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

/**
 * Self-hosted Hoard (https://github.com/rleeon/hoard) save sync.
 *
 * Hoard keeps every upload as a numbered version of one "save" (game slug +
 * label). An upload names the version it was based on, and the server
 * refuses it (409) when someone uploaded in between. Versions download as a
 * zstd-compressed tar.
 */
public final class HoardProvider implements CloudProvider {
    static final String GAME_SLUG = "mother-3";
    static final String GAME_NAME = "MOTHER 3";
    static final String LABEL = "default";

    private final String baseUrl;
    private final String token;
    private final String deviceName;
    private final String fileName;
    private String saveId;

    public HoardProvider(String baseUrl, String token, String deviceName, String fileName,
                         String cachedSaveId) {
        this.baseUrl = baseUrl;
        this.token = token;
        this.deviceName = deviceName;
        this.fileName = fileName;
        this.saveId = cachedSaveId;
    }

    // ── Sign-in ──────────────────────────────────────────────────────────────

    /** The account behind a device token; throws if the server rejects it. */
    public static String whoami(String baseUrl, String token) throws IOException {
        CloudHttp.Response response = CloudHttp.get(baseUrl + "/v1/auth/whoami", token);
        if (response.status == 401) throw new IOException("Hoard rejected the token.");
        return response.check("Signing in").json().optString("username", "?");
    }

    /**
     * Signs in with a password and mints a device token for this handheld.
     * Minting needs an admin account (the owner of a self-hosted server is
     * one); otherwise the player creates a token in the Hoard web panel.
     */
    public static String mintDeviceToken(String baseUrl, String username, String password,
                                         String deviceName) throws IOException {
        JSONObject login = new JSONObject();
        try {
            login.put("username", username);
            login.put("password", password);
        } catch (JSONException impossible) {
            throw new IOException(impossible);
        }
        CloudHttp.Response response = CloudHttp.postJson(baseUrl + "/v1/auth/login", null, login);
        if (response.status == 401) throw new IOException("Wrong username or password.");
        if (response.status == 404) {
            throw new IOException("This Hoard server has password sign-in turned off; "
                + "enter a device token instead.");
        }
        response.check("Signing in");
        String session = cookie(response.header("Set-Cookie"), "hoard_session");
        if (session == null) throw new IOException("Hoard did not return a session.");
        try {
            JSONObject me = CloudHttp.get(baseUrl + "/v1/auth/whoami", session)
                .check("Signing in").json();
            if (!me.optBoolean("is_admin", false)) {
                throw new IOException("Only an admin account can create device tokens here. "
                    + "Create one in the Hoard web panel (Users > New token) and enter it.");
            }
            JSONObject request = new JSONObject();
            request.put("user_id", me.getString("user_id"));
            request.put("device_name", deviceName);
            JSONObject minted = CloudHttp.postJson(baseUrl + "/v1/admin/tokens", session, request)
                .check("Creating a device token").json();
            return minted.getString("token");
        } catch (JSONException error) {
            throw new IOException("Unexpected response from Hoard");
        } finally {
            // The browser-style session was only needed to mint the token.
            try {
                CloudHttp.send("POST", baseUrl + "/v1/auth/logout", session, null, null);
            } catch (IOException ignored) {
                // Expires on its own.
            }
        }
    }

    private static String cookie(String setCookie, String name) {
        if (setCookie == null) return null;
        for (String line : setCookie.split("\n")) {
            String pair = line.split(";", 2)[0].trim();
            int eq = pair.indexOf('=');
            if (eq > 0 && pair.substring(0, eq).trim().equals(name)) {
                return pair.substring(eq + 1).trim();
            }
        }
        return null;
    }

    // ── Saves ────────────────────────────────────────────────────────────────

    /** The server-side save record for this game, created on first use. */
    public String saveId() throws IOException {
        if (saveId != null) return saveId;
        saveId = findSave();
        if (saveId != null) return saveId;
        JSONObject body = new JSONObject();
        try {
            body.put("game_slug", GAME_SLUG);
            body.put("label", LABEL);
            body.put("display_name", GAME_NAME);
            body.put("client_os", "android");
            body.put("local_path_hint", fileName);
        } catch (JSONException impossible) {
            throw new IOException(impossible);
        }
        CloudHttp.Response response = CloudHttp.postJson(baseUrl + "/v1/saves", token, body);
        authCheck(response);
        if (response.status == 409) {
            saveId = findSave();  // created concurrently by another device
            if (saveId != null) return saveId;
        }
        saveId = response.check("Creating the cloud save").json().optString("id", null);
        if (saveId == null) throw new IOException("Hoard did not return a save id.");
        return saveId;
    }

    private String findSave() throws IOException {
        CloudHttp.Response response =
            CloudHttp.get(baseUrl + "/v1/saves?game_slug=" + GAME_SLUG, token);
        authCheck(response);
        JSONArray saves = response.check("Listing cloud saves").jsonArray();
        for (int i = 0; i < saves.length(); ++i) {
            JSONObject save = saves.optJSONObject(i);
            if (save != null && LABEL.equals(save.optString("label"))) {
                return save.optString("id", null);
            }
        }
        return null;
    }

    @Override public String hashAlgorithm() {
        return "SHA-256";
    }

    @Override public String stateKey() {
        return "hoard|" + baseUrl + "|" + GAME_SLUG + "|" + LABEL;
    }

    @Override public RemoteSave latest() throws IOException {
        String id = saveId();
        CloudHttp.Response response = CloudHttp.get(baseUrl + "/v1/saves/" + id, token);
        authCheck(response);
        JSONObject save = response.check("Reading the cloud save").json();
        long version = save.optLong("latest_version_num", 0);
        if (version <= 0) return null;
        JSONObject detail = CloudHttp.get(baseUrl + "/v1/saves/" + id + "/snapshots/" + version,
            token).check("Reading the cloud save").json();
        String hash = null;
        JSONArray files = detail.optJSONArray("files");
        boolean found = false;
        for (int i = 0; files != null && i < files.length(); ++i) {
            JSONObject file = files.optJSONObject(i);
            if (file == null || !baseName(file.optString("relative_path")).equals(fileName)) continue;
            if (!SaveSync.isPlausibleSaveSize(file.optLong("size_bytes", -1))) continue;
            String sha = file.optString("sha256", "");
            hash = sha.isEmpty() ? null : sha.toLowerCase();
            found = true;
        }
        if (!found) {
            throw new IOException("The newest Hoard version (v" + version + ") has no "
                + fileName + ".");
        }
        return new RemoteSave(id, version, hash, detail.optString("created_at", ""));
    }

    @Override public byte[] download(RemoteSave save) throws IOException {
        CloudHttp.Response response = CloudHttp.get(baseUrl + "/v1/saves/" + save.id
            + "/snapshots/" + save.version + "/download", token);
        authCheck(response);
        byte[] archive = response.check("Downloading the cloud save").body;
        try (InputStream tar = new ZstdInputStream(new ByteArrayInputStream(archive))) {
            byte[] data = Tar.extract(tar, fileName);
            if (data == null) throw new IOException("The cloud version has no " + fileName + ".");
            return data;
        }
    }

    @Override public RemoteSave upload(byte[] data, RemoteSave base) throws IOException {
        String id = saveId();
        List<CloudHttp.Part> parts = new ArrayList<>();
        parts.add(CloudHttp.Part.field("device_name", deviceName));
        if (base != null && base.version > 0) {
            parts.add(CloudHttp.Part.field("base_version", String.valueOf(base.version)));
        }
        parts.add(CloudHttp.Part.file("files", fileName, data));
        CloudHttp.Response response =
            CloudHttp.multipart(baseUrl + "/v1/saves/" + id + "/snapshots", token, parts);
        authCheck(response);
        if (response.status == 409) {
            throw new ConflictException("Another device uploaded a newer save.");
        }
        JSONObject snapshot = response.check("Uploading the save").json();
        String sha = SaveSync.hex(SaveSync.digest("SHA-256", data));
        return new RemoteSave(id, snapshot.optLong("version_num", 0), sha,
            snapshot.optString("created_at", ""));
    }

    private static void authCheck(CloudHttp.Response response) throws IOException {
        if (response.status == 401) {
            throw new IOException("Hoard rejected the device token; sign in again.");
        }
    }

    private static String baseName(String path) {
        int slash = Math.max(path.lastIndexOf('/'), path.lastIndexOf('\\'));
        return slash >= 0 ? path.substring(slash + 1) : path;
    }

    /** Reads one regular file out of a ustar/GNU/PAX tar stream. */
    static final class Tar {
        private static final long MAX_ENTRY_BYTES = 64L * 1024 * 1024;

        private Tar() {}

        static byte[] extract(InputStream in, String wantedName) throws IOException {
            byte[] header = new byte[512];
            String longName = null;
            while (readFully(in, header)) {
                if (isZeroBlock(header)) return null;
                String name = longName != null ? longName : headerName(header);
                longName = null;
                long size = parseOctal(header, 124, 12);
                byte type = header[156];
                boolean wanted = type == 'L' || type == 'x' || type == '0' || type == 0;
                if (!wanted || size > MAX_ENTRY_BYTES) {
                    // Other entry types, and anything far larger than a save.
                    skip(in, padded(size));
                    continue;
                }
                byte[] data = readData(in, size);
                if (type == 'L') {
                    longName = cString(data, 0, data.length);
                } else if (type == 'x') {
                    String path = paxPath(data);
                    if (path != null) longName = path;
                } else if ((type == '0' || type == 0) && baseName(name).equals(wantedName)) {
                    return data;
                }
            }
            return null;
        }

        private static byte[] readData(InputStream in, long size) throws IOException {
            byte[] data = new byte[(int) size];
            if (!readFully(in, data)) throw new IOException("Truncated archive");
            skip(in, padded(size) - size);
            return data;
        }

        private static long padded(long size) {
            return (size + 511) / 512 * 512;
        }

        private static String headerName(byte[] h) {
            String name = cString(h, 0, 100);
            if (new String(h, 257, 5, StandardCharsets.US_ASCII).equals("ustar")) {
                String prefix = cString(h, 345, 155);
                if (!prefix.isEmpty()) name = prefix + "/" + name;
            }
            return name;
        }

        private static String paxPath(byte[] data) {
            // Records: "<len> <key>=<value>\n"
            String text = new String(data, StandardCharsets.UTF_8);
            for (String record : text.split("\n")) {
                int space = record.indexOf(' ');
                if (space < 0) continue;
                String kv = record.substring(space + 1);
                if (kv.startsWith("path=")) return kv.substring(5);
            }
            return null;
        }

        private static long parseOctal(byte[] h, int offset, int length) {
            long value = 0;
            for (int i = offset; i < offset + length; ++i) {
                byte b = h[i];
                if (b == 0 || b == ' ') {
                    if (value != 0) break;
                    continue;
                }
                if (b < '0' || b > '7') break;
                value = value * 8 + (b - '0');
            }
            return value;
        }

        private static String cString(byte[] b, int offset, int length) {
            int end = offset;
            while (end < offset + length && b[end] != 0) ++end;
            return new String(b, offset, end - offset, StandardCharsets.UTF_8);
        }

        private static boolean isZeroBlock(byte[] b) {
            for (byte x : b) if (x != 0) return false;
            return true;
        }

        private static boolean readFully(InputStream in, byte[] buffer) throws IOException {
            int off = 0;
            while (off < buffer.length) {
                int n = in.read(buffer, off, buffer.length - off);
                if (n < 0) {
                    if (off == 0) return false;
                    throw new IOException("Truncated archive");
                }
                off += n;
            }
            return true;
        }

        private static void skip(InputStream in, long count) throws IOException {
            byte[] scratch = new byte[4096];
            while (count > 0) {
                int n = in.read(scratch, 0, (int) Math.min(scratch.length, count));
                if (n < 0) throw new IOException("Truncated archive");
                count -= n;
            }
        }
    }
}
