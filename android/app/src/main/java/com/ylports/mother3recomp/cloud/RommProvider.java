package com.ylports.mother3recomp.cloud;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.IOException;
import java.net.URLEncoder;
import java.nio.charset.StandardCharsets;
import java.time.OffsetDateTime;
import java.time.format.DateTimeParseException;
import java.util.ArrayList;
import java.util.List;

/**
 * RomM (https://github.com/rommapp/romm) save sync.
 *
 * The device pairs through RomM's device authorization flow: it shows a short
 * code, the player approves it in RomM's web UI, and the device receives a
 * client token bound to a RomM device record. Saves are attached to the ROM
 * in the player's RomM library, found by the ROM's SHA-1.
 */
public final class RommProvider implements CloudProvider {
    /** Folder name RomM files this client's uploads under. */
    static final String EMULATOR = "mother3recomp";
    private static final String[] SCOPES = {
        "roms.read", "assets.read", "assets.write", "devices.read", "devices.write",
    };

    private final String baseUrl;
    private final String token;
    private final String deviceId;
    private final String romSha1;
    private final String fileName;
    private String romId;

    public RommProvider(String baseUrl, String token, String deviceId, String romSha1,
                        String fileName, String cachedRomId) {
        this.baseUrl = baseUrl;
        this.token = token;
        this.deviceId = deviceId;
        this.romSha1 = romSha1;
        this.fileName = fileName;
        this.romId = cachedRomId;
    }

    // ── Pairing ──────────────────────────────────────────────────────────────

    public static final class Pairing {
        public final String deviceCode;
        public final String userCode;
        /** Page the player opens (on a phone or PC) to approve the device. */
        public final String verificationUrl;
        public final int intervalSeconds;
        public final int expiresInSeconds;

        Pairing(String deviceCode, String userCode, String verificationUrl, int interval,
                int expiresIn) {
            this.deviceCode = deviceCode;
            this.userCode = userCode;
            this.verificationUrl = verificationUrl;
            this.intervalSeconds = interval;
            this.expiresInSeconds = expiresIn;
        }
    }

    /** Result of one poll: token != null once approved; otherwise see {@link #state}. */
    public static final class PairingPoll {
        public enum State { PENDING, SLOW_DOWN, APPROVED, DENIED, EXPIRED }

        public final State state;
        public final String token;
        public final String deviceId;

        PairingPoll(State state, String token, String deviceId) {
            this.state = state;
            this.token = token;
            this.deviceId = deviceId;
        }
    }

    public static Pairing startPairing(String baseUrl, String deviceIdentifier,
                                       String deviceName, String clientVersion)
            throws IOException {
        JSONObject body = new JSONObject();
        try {
            body.put("client_device_identifier", deviceIdentifier);
            body.put("name", deviceName);
            body.put("client", "Mother3Recomp");
            body.put("platform", "android");
            body.put("client_version", clientVersion);
            JSONArray scopes = new JSONArray();
            for (String scope : SCOPES) scopes.put(scope);
            body.put("requested_scopes", scopes);
        } catch (JSONException impossible) {
            throw new IOException(impossible);
        }
        CloudHttp.Response response =
            CloudHttp.postJson(baseUrl + "/api/auth/device/init", null, body);
        if (response.status == 404) {
            throw new IOException("This RomM server does not support device pairing; "
                + "update RomM.");
        }
        JSONObject json = response.check("Pairing").json();
        try {
            return new Pairing(json.getString("device_code"), json.getString("user_code"),
                baseUrl + json.getString("verification_path_complete"),
                json.optInt("interval", 5), json.optInt("expires_in", 600));
        } catch (JSONException error) {
            throw new IOException("Unexpected pairing response from RomM");
        }
    }

    public static PairingPoll pollPairing(String baseUrl, String deviceCode) throws IOException {
        JSONObject body = new JSONObject();
        try {
            body.put("device_code", deviceCode);
        } catch (JSONException impossible) {
            throw new IOException(impossible);
        }
        CloudHttp.Response response =
            CloudHttp.postJson(baseUrl + "/api/auth/device/token", null, body);
        if (response.ok()) {
            JSONObject json = response.json();
            try {
                return new PairingPoll(PairingPoll.State.APPROVED,
                    json.getString("access_token"), json.getString("device_id"));
            } catch (JSONException error) {
                throw new IOException("Unexpected token response from RomM");
            }
        }
        String detail = response.errorMessage();
        switch (detail) {
            case "authorization_pending": return new PairingPoll(PairingPoll.State.PENDING, null, null);
            case "slow_down": return new PairingPoll(PairingPoll.State.SLOW_DOWN, null, null);
            case "access_denied": return new PairingPoll(PairingPoll.State.DENIED, null, null);
            case "expired_token": return new PairingPoll(PairingPoll.State.EXPIRED, null, null);
            default:
                if (response.status == 429) {
                    return new PairingPoll(PairingPoll.State.SLOW_DOWN, null, null);
                }
                throw new IOException("Pairing failed: " + detail);
        }
    }

    // ── Library ──────────────────────────────────────────────────────────────

    /** RomM's id for this game, looked up by the ROM's SHA-1 (cached by the caller). */
    public String romId() throws IOException {
        if (romId != null) return romId;
        CloudHttp.Response response = CloudHttp.get(
            baseUrl + "/api/roms/by-hash?sha1_hash=" + enc(romSha1), token);
        if (response.status == 404) {
            throw new IOException("MOTHER 3 is not in your RomM library (no ROM with SHA-1 "
                + romSha1 + "). Add the same ROM file to RomM and scan the library.");
        }
        if (response.status == 401 || response.status == 403) {
            throw new IOException("RomM rejected the device token; pair the device again.");
        }
        JSONObject rom = response.check("Looking up the game").json();
        romId = String.valueOf(rom.optLong("id"));
        return romId;
    }

    @Override public String hashAlgorithm() {
        return "MD5";
    }

    @Override public String stateKey() {
        return "romm|" + baseUrl + "|" + romSha1;
    }

    @Override public RemoteSave latest() throws IOException {
        CloudHttp.Response response = CloudHttp.get(baseUrl + "/api/saves?rom_id="
            + enc(romId()) + "&device_id=" + enc(deviceId), token);
        authCheck(response);
        JSONArray saves = response.check("Listing cloud saves").jsonArray();
        RemoteSave newest = null;
        OffsetDateTime newestTime = null;
        for (int i = 0; i < saves.length(); ++i) {
            JSONObject save = saves.optJSONObject(i);
            if (save == null || save.optBoolean("missing_from_fs", false)) continue;
            // Only files a GBA flash 512 Kbit save can come from (see SaveSync).
            if (!SaveSync.isPlausibleSaveSize(save.optLong("file_size_bytes", -1))) continue;
            String updated = save.optString("updated_at", "");
            OffsetDateTime time = parseTime(updated);
            if (newest == null || (time != null && (newestTime == null || time.isAfter(newestTime)))) {
                String hash = save.isNull("content_hash") ? null
                    : save.optString("content_hash", null);
                newest = new RemoteSave(String.valueOf(save.optLong("id")), 0,
                    hash == null || hash.isEmpty() ? null : hash.toLowerCase(), updated);
                newestTime = time;
            }
        }
        return newest;
    }

    @Override public byte[] download(RemoteSave save) throws IOException {
        CloudHttp.Response response = CloudHttp.get(baseUrl + "/api/saves/" + enc(save.id)
            + "/content?device_id=" + enc(deviceId), token);
        authCheck(response);
        byte[] data = response.check("Downloading the cloud save").body;
        // Tell RomM this device now holds that version (best effort).
        try {
            JSONObject body = new JSONObject().put("device_id", deviceId);
            CloudHttp.postJson(baseUrl + "/api/saves/" + enc(save.id) + "/downloaded", token, body);
        } catch (JSONException | IOException ignored) {
            // Bookkeeping only.
        }
        return data;
    }

    @Override public RemoteSave upload(byte[] data, RemoteSave base) throws IOException {
        String md5 = SaveSync.hex(SaveSync.digest("MD5", data));
        List<CloudHttp.Part> parts = new ArrayList<>();
        parts.add(CloudHttp.Part.file("saveFile", fileName, data));
        CloudHttp.Response response = CloudHttp.multipart(baseUrl + "/api/saves?rom_id="
            + enc(romId()) + "&emulator=" + EMULATOR + "&device_id=" + enc(deviceId)
            + "&overwrite=true&content_hash=" + md5, token, parts);
        authCheck(response);
        JSONObject save = response.check("Uploading the save").json();
        String hash = save.isNull("content_hash") ? md5 : save.optString("content_hash", md5);
        return new RemoteSave(String.valueOf(save.optLong("id")), 0, hash.toLowerCase(),
            save.optString("updated_at", ""));
    }

    private static void authCheck(CloudHttp.Response response) throws IOException {
        if (response.status == 401 || response.status == 403) {
            throw new IOException("RomM rejected the device token; pair the device again.");
        }
    }

    private static OffsetDateTime parseTime(String value) {
        try {
            return OffsetDateTime.parse(value);
        } catch (DateTimeParseException error) {
            return null;
        }
    }

    static String enc(String value) {
        try {
            return URLEncoder.encode(value, StandardCharsets.UTF_8.name());
        } catch (java.io.UnsupportedEncodingException impossible) {
            throw new IllegalStateException(impossible);
        }
    }
}
