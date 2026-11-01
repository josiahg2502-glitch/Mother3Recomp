package com.ylports.mother3recomp.cloud;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Arrays;
import java.util.Base64;
import java.util.HashMap;
import java.util.Map;
import java.util.Random;
import java.util.UUID;

/** Integration test against real RomM and Hoard containers (see compose.yml). */
public final class CloudIT {
    static int failures = 0;

    static void check(boolean ok, String what) {
        System.out.println((ok ? "  ok   " : "  FAIL ") + what);
        if (!ok) ++failures;
    }

    static byte[] save(int seed, int size) {
        byte[] b = new byte[size];
        new Random(seed).nextBytes(b);
        return b;
    }

    static final class MemStore implements SaveSync.Store {
        final Map<String, String> map = new HashMap<>();
        public String get(String k) { return map.get(k); }
        public void put(String k, String v) { if (v == null) map.remove(k); else map.put(k, v); }
    }

    /** Runs the shared sync scenarios against one provider. `other` is a second device. */
    static void scenarios(String label, CloudProvider device, CloudProvider other) throws Exception {
        System.out.println("== " + label);
        File dir = Files.createTempDirectory("m3sync").toFile();
        File saveFile = new File(dir, "roms/mother3_en.sav");
        File backups = new File(dir, "save-backups");
        MemStore store = new MemStore();
        SaveSync sync = new SaveSync(device, saveFile, backups, store);

        SaveSync.Result r = sync.sync(SaveSync.Choice.AUTO);
        check(r.outcome == SaveSync.Outcome.NOTHING_TO_DO, "empty both sides -> " + r.outcome);

        byte[] l1 = save(1, SaveSync.FLASH_SIZE);
        SaveSync.writeAtomically(saveFile, l1);
        r = sync.sync(SaveSync.Choice.AUTO);
        check(r.outcome == SaveSync.Outcome.UPLOADED, "first local save uploads -> " + r.outcome);
        r = sync.sync(SaveSync.Choice.AUTO);
        check(r.outcome == SaveSync.Outcome.IN_SYNC, "second sync is in sync -> " + r.outcome);

        byte[] l2 = save(2, SaveSync.FLASH_SIZE);
        SaveSync.writeAtomically(saveFile, l2);
        r = sync.sync(SaveSync.Choice.AUTO);
        check(r.outcome == SaveSync.Outcome.UPLOADED, "played locally -> " + r.outcome);

        // Another device continues from l2 and uploads l3.
        byte[] l3 = save(3, SaveSync.FLASH_SIZE);
        other.upload(l3, other.latest());
        r = sync.sync(SaveSync.Choice.AUTO);
        check(r.outcome == SaveSync.Outcome.DOWNLOADED, "other device played -> " + r.outcome);
        check(Arrays.equals(SaveSync.readFile(saveFile), l3), "local file now equals the cloud save");
        File[] kept = backups.listFiles();
        check(kept != null && kept.length == 1
            && Arrays.equals(SaveSync.readFile(kept[0]), l2), "previous local save backed up");

        // Both sides change: conflict, then the player keeps the cloud.
        byte[] l4 = save(4, SaveSync.FLASH_SIZE);
        SaveSync.writeAtomically(saveFile, l4);
        byte[] l5 = save(5, SaveSync.FLASH_SIZE);
        other.upload(l5, other.latest());
        r = sync.sync(SaveSync.Choice.AUTO);
        check(r.outcome == SaveSync.Outcome.CONFLICT, "both changed -> " + r.outcome + " (" + r.message + ")");
        r = sync.sync(SaveSync.Choice.KEEP_CLOUD);
        check(r.outcome == SaveSync.Outcome.DOWNLOADED && Arrays.equals(SaveSync.readFile(saveFile), l5),
            "KEEP_CLOUD takes the cloud save -> " + r.outcome);

        // Conflict again, the player keeps this device.
        byte[] l6 = save(6, SaveSync.FLASH_SIZE);
        SaveSync.writeAtomically(saveFile, l6);
        other.upload(save(7, SaveSync.FLASH_SIZE), other.latest());
        r = sync.sync(SaveSync.Choice.AUTO);
        check(r.outcome == SaveSync.Outcome.CONFLICT, "both changed again -> " + r.outcome);
        r = sync.sync(SaveSync.Choice.KEEP_DEVICE);
        check(r.outcome == SaveSync.Outcome.UPLOADED, "KEEP_DEVICE uploads -> " + r.outcome);
        File[] cloudCopies = backups.listFiles((d, n) -> n.endsWith(".cloud-before-upload.bak"));
        check(cloudCopies != null && cloudCopies.length == 1
            && Arrays.equals(SaveSync.readFile(cloudCopies[0]), save(7, SaveSync.FLASH_SIZE)),
            "replaced cloud save backed up");
        check(Arrays.equals(other.download(other.latest()), l6), "cloud now holds this device's save");
        r = sync.sync(SaveSync.Choice.AUTO);
        check(r.outcome == SaveSync.Outcome.IN_SYNC, "then in sync -> " + r.outcome);

        // A save made by mGBA elsewhere (64 KiB + 16-byte RTC footer).
        byte[] mgba = Arrays.copyOf(save(8, SaveSync.FLASH_SIZE), SaveSync.FLASH_SIZE + 16);
        other.upload(mgba, other.latest());
        r = sync.sync(SaveSync.Choice.AUTO);
        byte[] now = SaveSync.readFile(saveFile);
        check(r.outcome == SaveSync.Outcome.DOWNLOADED && now.length == SaveSync.FLASH_SIZE
            && Arrays.equals(now, Arrays.copyOf(mgba, SaveSync.FLASH_SIZE)),
            "mGBA-style save downloads and the RTC footer is dropped -> " + r.outcome);
        r = sync.sync(SaveSync.Choice.AUTO);
        check(r.outcome == SaveSync.Outcome.UPLOADED, "normalised save is uploaded back -> " + r.outcome);
        r = sync.sync(SaveSync.Choice.AUTO);
        check(r.outcome == SaveSync.Outcome.IN_SYNC, "converges -> " + r.outcome);

        // A local file with a wrong size is refused, not uploaded.
        SaveSync.writeAtomically(saveFile, new byte[1234]);
        try {
            sync.sync(SaveSync.Choice.AUTO);
            check(false, "odd-sized local save refused");
        } catch (IOException expected) {
            check(expected.getMessage().contains("unexpected size"), "odd-sized local save refused: " + expected.getMessage());
        }
    }

    // ── RomM ────────────────────────────────────────────────────────────────

    static CloudHttp.Response basic(String method, String url, JSONObject body) throws IOException {
        java.net.HttpURLConnection c = (java.net.HttpURLConnection) new java.net.URL(url).openConnection();
        c.setRequestMethod(method);
        c.setRequestProperty("Authorization", "Basic " + Base64.getEncoder()
            .encodeToString("admin:romm-test-pw-123".getBytes(StandardCharsets.UTF_8)));
        if (body != null) {
            c.setDoOutput(true);
            c.setRequestProperty("Content-Type", "application/json");
            c.getOutputStream().write(body.toString().getBytes(StandardCharsets.UTF_8));
        }
        int status = c.getResponseCode();
        byte[] data = CloudHttp.readAll(status >= 400 ? c.getErrorStream() : c.getInputStream());
        return new CloudHttp.Response(status, data, c.getHeaderFields());
    }

    static void romm(String sha1) throws Exception {
        String base = CloudHttp.normalizeBaseUrl("localhost:18080/");
        check(base.equals("http://localhost:18080"), "normalizeBaseUrl -> " + base);

        RommProvider.Pairing pairing = RommProvider.startPairing(base, UUID.randomUUID().toString(),
            "Test device (MOTHER 3)", "0.1.3");
        check(pairing.verificationUrl.startsWith(base + "/pair/device?user_code="),
            "pairing URL " + pairing.verificationUrl + " code " + pairing.userCode);
        Thread.sleep(pairing.intervalSeconds * 1000L);
        RommProvider.PairingPoll poll = RommProvider.pollPairing(base, pairing.deviceCode);
        check(poll.state == RommProvider.PairingPoll.State.PENDING, "poll before approval -> " + poll.state);

        JSONArray scopes = new JSONArray(Arrays.asList("roms.read", "assets.read", "assets.write",
            "devices.read", "devices.write"));
        CloudHttp.Response approve = basic("POST", base + "/api/auth/device/approve",
            new JSONObject().put("user_code", pairing.userCode).put("approved_scopes", scopes));
        check(approve.ok(), "admin approves the code -> HTTP " + approve.status + " " + approve.text());

        Thread.sleep(pairing.intervalSeconds * 1000L + 500);
        poll = RommProvider.pollPairing(base, pairing.deviceCode);
        check(poll.state == RommProvider.PairingPoll.State.APPROVED && poll.token.startsWith("rmm_"),
            "poll after approval -> " + poll.state);

        // Wait for the library scan to index the test ROM.
        RommProvider device = null;
        for (int i = 0; i < 60; ++i) {
            try {
                device = new RommProvider(base, poll.token, poll.deviceId, sha1, "mother3_en.sav", null);
                device.romId();
                break;
            } catch (IOException notYet) {
                if (i == 59) throw notYet;
                Thread.sleep(2000);
            }
        }
        check(device != null, "game found in the library by SHA-1, rom id " + device.romId());

        try {
            new RommProvider(base, poll.token, poll.deviceId, "0000000000000000000000000000000000000000",
                "mother3_en.sav", null).romId();
            check(false, "missing game reported");
        } catch (IOException expected) {
            check(expected.getMessage().contains("not in your RomM library"), "missing game: " + expected.getMessage());
        }
        try {
            new RommProvider(base, "rmm_invalid", poll.deviceId, sha1, "mother3_en.sav", null).romId();
            check(false, "bad token reported");
        } catch (IOException expected) {
            check(expected.getMessage().contains("pair the device again"), "bad token: " + expected.getMessage());
        }

        // A second paired device plays elsewhere.
        RommProvider.Pairing p2 = RommProvider.startPairing(base, UUID.randomUUID().toString(), "Desk PC", "0.1.3");
        basic("POST", base + "/api/auth/device/approve",
            new JSONObject().put("user_code", p2.userCode).put("approved_scopes", scopes));
        Thread.sleep(p2.intervalSeconds * 1000L + 500);
        RommProvider.PairingPoll poll2 = RommProvider.pollPairing(base, p2.deviceCode);
        RommProvider other = new RommProvider(base, poll2.token, poll2.deviceId, sha1, "mother3_en.sav", null);
        scenarios("RomM", device, other);
    }

    // ── Hoard ───────────────────────────────────────────────────────────────

    static void hoard() throws Exception {
        String base = "http://localhost:12421";
        try {
            HoardProvider.mintDeviceToken(base, "tester", "wrong-password", "x");
            check(false, "wrong password reported");
        } catch (IOException expected) {
            check(expected.getMessage().contains("Wrong username or password"), "wrong password: " + expected.getMessage());
        }
        String token = HoardProvider.mintDeviceToken(base, "tester", "hoard-test-pw-123", "Test device (MOTHER 3)");
        check(token.startsWith("hoard_v1_"), "password sign-in mints a device token");
        check("tester".equals(HoardProvider.whoami(base, token)), "whoami with the new token");
        try {
            HoardProvider.whoami(base, "hoard_v1_deadbeef");
            check(false, "bad token reported");
        } catch (IOException expected) {
            check(true, "bad token: " + expected.getMessage());
        }
        String token2 = HoardProvider.mintDeviceToken(base, "tester", "hoard-test-pw-123", "Desk PC");
        HoardProvider device = new HoardProvider(base, token, "Test device (MOTHER 3)", "mother3_en.sav", null);
        HoardProvider other = new HoardProvider(base, token2, "Desk PC", "mother3_en.sav", null);
        check(device.saveId().equals(other.saveId()), "both devices share one save record " + device.saveId());

        scenarios("Hoard", device, other);
        // Optimistic concurrency: an upload based on a stale version is refused.
        other.upload(save(41, SaveSync.FLASH_SIZE), other.latest());
        CloudProvider.RemoteSave stale = device.latest();
        other.upload(save(42, SaveSync.FLASH_SIZE), other.latest());
        try {
            device.upload(save(43, SaveSync.FLASH_SIZE), stale);
            check(false, "stale upload refused with 409 (base v" + stale.version + ")");
        } catch (CloudProvider.ConflictException expected) {
            check(true, "stale upload (base v" + stale.version + ") refused: " + expected.getMessage());
        }
    }

    public static void main(String[] args) throws Exception {
        String sha1 = new String(Files.readAllBytes(new File(args[0]).toPath())).trim();
        romm(sha1);
        hoard();
        System.out.println(failures == 0 ? "ALL OK" : "FAILURES: " + failures);
        System.exit(failures == 0 ? 0 : 1);
    }
}
