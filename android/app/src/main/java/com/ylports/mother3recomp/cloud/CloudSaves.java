package com.ylports.mother3recomp.cloud;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.SharedPreferences;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.text.InputType;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

import org.libsdl.app.SdlSession;

import java.io.File;
import java.io.IOException;
import java.text.DateFormat;
import java.util.Date;
import java.util.UUID;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Cloud saves on the setup screen: provider setup (RomM pairing, Hoard
 * sign-in), and the syncs around a play session — before PLAY and after the
 * game exits. A failed sync never blocks playing; the local save stays.
 */
public final class CloudSaves {
    private static final String PREFS = "cloud_saves";
    private static final String CLIENT_VERSION = "0.1.3";
    /** Set while a game started from PLAY has not been synced back yet. */
    private static final String PLAYED = "played_since_sync";
    /** How long syncAfterPlay waits for the engine thread: 20 x 500 ms. */
    private static final int SYNC_WAIT_ATTEMPTS = 20;
    private static final long SYNC_WAIT_MS = 500;
    private static final Handler MAIN = new Handler(Looper.getMainLooper());

    /** A sync in flight; {@link #then} runs once it (and any conflict choice) ends. */
    private static final class Job {
        Runnable then;
    }

    /**
     * The sync in flight, process-wide so a recreated setup screen sees it
     * too (UI thread only). Two syncs at once could write the save file under
     * a starting game, or turn one upload into a false Hoard conflict.
     */
    private static Job running;

    private final Activity activity;
    private final SharedPreferences prefs;
    private final File saveFile;
    /** gbarecomp's "resume where you left off" marker for the suspend state. */
    private final File suspendMarker;
    private final File backupDir;
    private final String romSha1;
    private final Runnable onChanged;

    public CloudSaves(Activity activity, String romFile, String romSha1, Runnable onChanged) {
        this.activity = activity;
        this.prefs = activity.getSharedPreferences(PREFS, Activity.MODE_PRIVATE);
        String stem = romFile.contains(".") ? romFile.substring(0, romFile.lastIndexOf('.')) : romFile;
        // gbarecomp writes the save next to the ROM, as <rom>.sav.
        this.saveFile = new File(activity.getFilesDir(), "roms/" + stem + ".sav");
        this.suspendMarker = new File(activity.getFilesDir(), "roms/" + stem + ".suspend.pending");
        this.backupDir = new File(activity.getFilesDir(), "save-backups");
        this.romSha1 = romSha1;
        this.onChanged = onChanged;
    }

    public boolean configured() {
        return provider() != null;
    }

    /** Label for the setup-screen button. */
    public String buttonLabel() {
        String kind = prefs.getString("provider", null);
        if (kind == null) return "Cloud saves: off";
        return "Cloud saves: " + ("romm".equals(kind) ? "RomM" : "Hoard") + " ("
            + host(prefs.getString("url", "")) + ")";
    }

    // ── Sync around a play session ──────────────────────────────────────────

    /**
     * Syncs, then runs {@code then} whatever happened (including failures).
     * While another sync is running, {@code then} waits for that one instead:
     * it brings the save up to date just the same.
     */
    public void syncBeforePlay(Runnable then) {
        if (running == null && !configured()) {
            then.run();
            return;
        }
        runSync(SaveSync.Choice.AUTO, true, then);
    }

    /**
     * Remembers that PLAY started the game, so its progress is uploaded when
     * the player comes back, even if this screen (or the process) was
     * recreated meanwhile. Committed at once: the game may not let us return.
     */
    public void gameStarting() {
        prefs.edit().putBoolean(PLAYED, true).commit();
    }

    /** Uploads the session's progress after the game exits. */
    public void syncAfterPlay() {
        syncAfterPlay(0);
    }

    private void syncAfterPlay(int attempt) {
        // Kept for a later return while a sync is busy.
        if (!prefs.getBoolean(PLAYED, false) || running != null) return;
        if (SdlSession.nativeRunning()) {
            // The game screen can close before the engine thread ends (the
            // system finishing it, or another task on top), so this screen
            // resumes while the session is still writing the save. Poll until
            // the thread is gone; on a longer wait the flag stays set, and the
            // next return here or PLAY syncs instead.
            if (attempt < SYNC_WAIT_ATTEMPTS) {
                MAIN.postDelayed(() -> {
                    if (alive()) syncAfterPlay(attempt + 1);
                }, SYNC_WAIT_MS);
            }
            return;
        }
        prefs.edit().remove(PLAYED).apply();
        if (configured()) runSync(SaveSync.Choice.AUTO, false, null);
    }

    private void runSync(SaveSync.Choice choice, boolean showProgress, Runnable then) {
        if (running != null) {
            if (then != null && running.then == null) {
                running.then = then;
                toast("Waiting for the cloud sync to finish…");
            } else if (then == null && showProgress) {
                toast("A cloud sync is already running.");
            }
            // A second PLAY while the first one waits is dropped.
            return;
        }
        if (SdlSession.nativeRunning()) {
            toast("Cloud sync skipped: the game is still running.");
            if (then != null) then.run();
            return;
        }
        CloudProvider provider = provider();
        if (provider == null) {
            if (then != null) then.run();
            return;
        }
        Job job = new Job();
        job.then = then;
        running = job;
        AlertDialog progress = showProgress
            ? new AlertDialog.Builder(activity)
                .setTitle("Cloud saves")
                .setMessage("Syncing the save with " + providerName() + "…")
                .setCancelable(false)
                .show()
            : null;
        new Thread(() -> {
            SaveSync.Result result = null;
            String error = null;
            try {
                result = new SaveSync(provider, saveFile, backupDir, store()).sync(choice);
                if (result.outcome == SaveSync.Outcome.DOWNLOADED) {
                    // A suspend state embeds the old cartridge save: resuming it
                    // would put the replaced save back. Boot from the new one.
                    suspendMarker.delete();
                }
                cacheIds(provider);
            } catch (IOException | RuntimeException failure) {
                error = describe(failure);
            }
            final SaveSync.Result done = result;
            final String failure = error;
            activity.runOnUiThread(() -> {
                running = null;
                // Read now: PLAY may have attached itself while this ran.
                Runnable next = job.then;
                record(failure != null ? "Failed: " + failure : done.message);
                if (!alive()) {
                    // No window left to report on or ask in; never block playing.
                    if (next != null) next.run();
                    return;
                }
                if (progress != null && progress.isShowing()) progress.dismiss();
                if (failure != null) {
                    toast("Cloud sync failed: " + failure);
                    if (next != null) next.run();
                } else if (done.outcome == SaveSync.Outcome.CONFLICT) {
                    askConflict(done.message, next);
                } else {
                    if (progress != null || done.outcome != SaveSync.Outcome.IN_SYNC) {
                        toast(done.message);
                    }
                    if (next != null) next.run();
                }
            });
        }, "cloud-sync").start();
    }

    private void askConflict(String message, Runnable then) {
        new AlertDialog.Builder(activity)
            .setTitle("Cloud saves: choose a save")
            .setMessage(message + "\n\nWhichever save is replaced, on this device or in the "
                + "cloud, is first copied to save-backups.")
            .setCancelable(false)
            .setPositiveButton("Keep this device's",
                (d, w) -> runSync(SaveSync.Choice.KEEP_DEVICE, true, then))
            .setNegativeButton("Use the cloud's",
                (d, w) -> runSync(SaveSync.Choice.KEEP_CLOUD, true, then))
            .setNeutralButton("Skip", (d, w) -> {
                record("Skipped a conflict");
                if (then != null) then.run();
            })
            .show();
    }

    // ── Settings dialog ──────────────────────────────────────────────────────

    public void showSettings() {
        if (running != null) {
            // Switching or disconnecting now would race the sync's bookkeeping.
            toast("A cloud sync is running; try again in a moment.");
            return;
        }
        if (!configured()) {
            new AlertDialog.Builder(activity)
                .setTitle("Cloud saves")
                .setMessage("Keep the MOTHER 3 save in sync with your own server. "
                    + "It syncs when you press PLAY and when you leave the game.\n\n"
                    + "RomM: pair this device with a code.\n"
                    + "Hoard (self-hosted): sign in with your account or a device token.")
                .setPositiveButton("RomM", (d, w) -> setupRomm())
                .setNegativeButton("Hoard", (d, w) -> setupHoard())
                .setNeutralButton("Cancel", null)
                .show();
            return;
        }
        String last = prefs.getString("last_result", "never");
        long time = prefs.getLong("last_time", 0);
        new AlertDialog.Builder(activity)
            .setTitle("Cloud saves: " + providerName())
            .setMessage("Server: " + prefs.getString("url", "") + "\n"
                + "Account: " + prefs.getString("account", "?") + "\n"
                + "Last sync: " + (time > 0
                    ? DateFormat.getDateTimeInstance().format(new Date(time)) + " - " : "")
                + last + "\n\nBackups of replaced saves: files/save-backups")
            .setPositiveButton("Sync now", (d, w) -> runSync(SaveSync.Choice.AUTO, true, null))
            .setNegativeButton("Disconnect", (d, w) -> disconnect())
            .setNeutralButton("Close", null)
            .show();
    }

    private void setupRomm() {
        LinearLayout form = form();
        EditText url = field(form, "Server address, e.g. http://192.168.1.10:8080",
            InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        url.setText(prefs.getString("last_romm_url", ""));
        TextView status = note(form, "You will approve this device in RomM's web page.");
        AlertDialog dialog = new AlertDialog.Builder(activity)
            .setTitle("RomM")
            .setView(form)
            .setPositiveButton("Pair", null)
            .setNegativeButton("Cancel", null)
            .create();
        dialog.setOnShowListener(shown -> dialog.getButton(AlertDialog.BUTTON_POSITIVE)
            .setOnClickListener(v -> {
                final String base;
                try {
                    base = CloudHttp.normalizeBaseUrl(url.getText().toString());
                } catch (IOException error) {
                    status.setText(error.getMessage());
                    return;
                }
                status.setText("Contacting RomM…");
                background(() -> RommProvider.startPairing(base, deviceIdentifier(), deviceName(),
                        CLIENT_VERSION),
                    pairing -> {
                        dialog.dismiss();
                        prefs.edit().putString("last_romm_url", base).apply();
                        waitForApproval(base, pairing);
                    },
                    status::setText);
            }));
        dialog.show();
    }

    private void waitForApproval(String base, RommProvider.Pairing pairing) {
        AtomicBoolean cancelled = new AtomicBoolean(false);
        AlertDialog waiting = new AlertDialog.Builder(activity)
            .setTitle("Pair with RomM")
            .setMessage("On your phone or computer, open:\n\n" + pairing.verificationUrl
                + "\n\nand approve the code:\n\n" + pairing.userCode
                + "\n\nWaiting for approval…" + cleartextWarning(base))
            .setCancelable(false)
            .setNegativeButton("Cancel", (d, w) -> cancelled.set(true))
            .show();
        new Thread(() -> {
            String error = null;
            RommProvider.PairingPoll approved = null;
            long deadline = System.currentTimeMillis() + pairing.expiresInSeconds * 1000L;
            int interval = Math.max(2, pairing.intervalSeconds);
            try {
                while (!cancelled.get() && System.currentTimeMillis() < deadline) {
                    Thread.sleep(interval * 1000L);
                    if (cancelled.get()) break;
                    RommProvider.PairingPoll poll = RommProvider.pollPairing(base, pairing.deviceCode);
                    if (poll.state == RommProvider.PairingPoll.State.APPROVED) {
                        approved = poll;
                        break;
                    }
                    if (poll.state == RommProvider.PairingPoll.State.SLOW_DOWN) interval += 5;
                    if (poll.state == RommProvider.PairingPoll.State.DENIED) {
                        error = "The pairing request was denied in RomM.";
                        break;
                    }
                    if (poll.state == RommProvider.PairingPoll.State.EXPIRED) break;
                }
                if (approved == null && error == null && !cancelled.get()) {
                    error = "The pairing code expired; try again.";
                }
                if (approved != null) {
                    // Fails early, with a useful message, if the game is not in the library.
                    RommProvider check = new RommProvider(base, approved.token, approved.deviceId,
                        romSha1, saveFile.getName(), null);
                    String romId = check.romId();
                    configure("romm", base, approved.token, "device " + approved.deviceId,
                        approved.deviceId, romId);
                }
            } catch (IOException | RuntimeException failure) {
                error = describe(failure);
            } catch (InterruptedException interrupted) {
                Thread.currentThread().interrupt();
                return;
            }
            final String failure = error;
            final boolean paired = approved != null && failure == null;
            activity.runOnUiThread(() -> {
                if (!alive()) return;
                if (waiting.isShowing()) waiting.dismiss();
                if (failure != null) showError("RomM", failure);
                if (paired) {
                    toast("Paired with RomM.");
                    runSync(SaveSync.Choice.AUTO, true, null);
                }
            });
        }, "romm-pairing").start();
    }

    private void setupHoard() {
        LinearLayout form = form();
        EditText url = field(form, "Server address, e.g. http://192.168.1.10:12421",
            InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        url.setText(prefs.getString("last_hoard_url", ""));
        EditText user = field(form, "Username", InputType.TYPE_CLASS_TEXT
            | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        EditText password = field(form, "Password", InputType.TYPE_CLASS_TEXT
            | InputType.TYPE_TEXT_VARIATION_PASSWORD);
        EditText token = field(form, "…or a device token (hoard_v1_…)", InputType.TYPE_CLASS_TEXT
            | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        TextView status = note(form, "Signing in with a password creates a device token "
            + "(admin accounts). The password is not stored.");
        // The http address the player already agreed to send a password to.
        String[] cleartextConfirmed = {null};
        AlertDialog dialog = new AlertDialog.Builder(activity)
            .setTitle("Hoard (self-hosted)")
            .setView(form)
            .setPositiveButton("Sign in", null)
            .setNegativeButton("Cancel", null)
            .create();
        dialog.setOnShowListener(shown -> dialog.getButton(AlertDialog.BUTTON_POSITIVE)
            .setOnClickListener(v -> {
                final String base;
                try {
                    base = CloudHttp.normalizeBaseUrl(url.getText().toString());
                } catch (IOException error) {
                    status.setText(error.getMessage());
                    return;
                }
                final String name = user.getText().toString().trim();
                final String pass = password.getText().toString();
                final String pasted = token.getText().toString().trim();
                if (pasted.isEmpty() && (name.isEmpty() || pass.isEmpty())) {
                    status.setText("Enter username and password, or a device token.");
                    return;
                }
                if (pasted.isEmpty() && base.startsWith("http://")
                        && !base.equals(cleartextConfirmed[0])) {
                    cleartextConfirmed[0] = base;
                    status.setText("This address is http: the password would be sent "
                        + "unencrypted. Use it only on your home network. Tap Sign in again "
                        + "to continue, or use https or a device token.");
                    return;
                }
                status.setText("Signing in…");
                background(() -> {
                        String deviceToken = !pasted.isEmpty() ? pasted
                            : HoardProvider.mintDeviceToken(base, name, pass, deviceName());
                        String account = HoardProvider.whoami(base, deviceToken);
                        new HoardProvider(base, deviceToken, deviceName(), saveFile.getName(), null)
                            .saveId();
                        return new String[] {deviceToken, account};
                    },
                    signedIn -> {
                        dialog.dismiss();
                        prefs.edit().putString("last_hoard_url", base).apply();
                        configure("hoard", base, signedIn[0], signedIn[1], null, null);
                        toast("Signed in to Hoard as " + signedIn[1] + "."
                            + (pasted.isEmpty() ? "" : cleartextWarning(base)));
                        runSync(SaveSync.Choice.AUTO, true, null);
                    },
                    status::setText);
            }));
        dialog.show();
    }

    /** Switches to a new server/account; sync state starts over (first sync compares). */
    private void configure(String kind, String base, String token, String account,
                           String rommDeviceId, String rommRomId) {
        CloudProvider old = provider();
        if (old != null) SaveSync.forget(store(), old.stateKey());
        SharedPreferences.Editor edit = prefs.edit()
            .putString("provider", kind)
            .putString("url", base)
            .putString("token", token)
            .putString("account", account)
            .remove("romm_device_id").remove("romm_rom_id").remove("hoard_save_id")
            .remove("last_result").remove("last_time");
        if (rommDeviceId != null) edit.putString("romm_device_id", rommDeviceId);
        if (rommRomId != null) edit.putString("romm_rom_id", rommRomId);
        edit.apply();
        CloudProvider fresh = provider();
        if (fresh != null) SaveSync.forget(store(), fresh.stateKey());
        activity.runOnUiThread(onChanged);
    }

    private void disconnect() {
        CloudProvider old = provider();
        if (old != null) SaveSync.forget(store(), old.stateKey());
        prefs.edit().remove("provider").remove("url").remove("token").remove("account")
            .remove("romm_device_id").remove("romm_rom_id").remove("hoard_save_id")
            .remove("last_result").remove("last_time").apply();
        toast("Cloud saves turned off. The save on this device is unchanged.");
        onChanged.run();
    }

    // ── Plumbing ─────────────────────────────────────────────────────────────

    private CloudProvider provider() {
        String kind = prefs.getString("provider", null);
        String base = prefs.getString("url", null);
        String token = prefs.getString("token", null);
        if (kind == null || base == null || token == null) return null;
        if ("romm".equals(kind)) {
            String deviceId = prefs.getString("romm_device_id", null);
            if (deviceId == null) return null;
            return new RommProvider(base, token, deviceId, romSha1, saveFile.getName(),
                prefs.getString("romm_rom_id", null));
        }
        return new HoardProvider(base, token, deviceName(), saveFile.getName(),
            prefs.getString("hoard_save_id", null));
    }

    private void cacheIds(CloudProvider provider) throws IOException {
        if (provider instanceof RommProvider) {
            prefs.edit().putString("romm_rom_id", ((RommProvider) provider).romId()).apply();
        } else if (provider instanceof HoardProvider) {
            prefs.edit().putString("hoard_save_id", ((HoardProvider) provider).saveId()).apply();
        }
    }

    private String providerName() {
        return "romm".equals(prefs.getString("provider", null)) ? "RomM" : "Hoard";
    }

    private SaveSync.Store store() {
        return new SaveSync.Store() {
            @Override public String get(String key) {
                return prefs.getString(key, null);
            }

            @Override public void put(String key, String value) {
                if (value == null) prefs.edit().remove(key).apply();
                else prefs.edit().putString(key, value).apply();
            }
        };
    }

    private void record(String message) {
        prefs.edit().putString("last_result", message)
            .putLong("last_time", System.currentTimeMillis()).apply();
    }

    private String deviceIdentifier() {
        String id = prefs.getString("device_identifier", null);
        if (id == null) {
            id = UUID.randomUUID().toString();
            prefs.edit().putString("device_identifier", id).apply();
        }
        return id;
    }

    private static String deviceName() {
        String model = Build.MODEL == null ? "Android" : Build.MODEL;
        String maker = Build.MANUFACTURER == null ? "" : Build.MANUFACTURER;
        String name = model.toLowerCase().startsWith(maker.toLowerCase()) || maker.isEmpty()
            ? model : maker + " " + model;
        return name + " (MOTHER 3)";
    }

    private static String host(String url) {
        return url.replaceFirst("^https?://", "");
    }

    private static String cleartextWarning(String base) {
        return base.startsWith("http://")
            ? "\n\nThis connection is not encrypted (http): use it only on your home network."
            : "";
    }

    private interface Work<T> {
        T run() throws IOException;
    }

    private interface Sink<T> {
        void accept(T value);
    }

    /** Runs {@code work} off the UI thread and reports back on it. */
    private <T> void background(Work<T> work, Sink<T> done, Sink<String> failed) {
        new Thread(() -> {
            try {
                T value = work.run();
                activity.runOnUiThread(() -> {
                    if (alive()) done.accept(value);
                });
            } catch (IOException | RuntimeException error) {
                String message = describe(error);
                activity.runOnUiThread(() -> {
                    if (alive()) failed.accept(message);
                });
            }
        }, "cloud-setup").start();
    }

    /** False once the screen is gone: its dialogs can no longer be shown. */
    private boolean alive() {
        return !activity.isFinishing() && !activity.isDestroyed();
    }

    /** A message for the player; unexpected exceptions get a generic one. */
    public static String describe(Exception error) {
        String message = error.getMessage();
        if (error instanceof IOException && message != null && !message.isEmpty()) return message;
        return "Unexpected error (" + error.getClass().getSimpleName()
            + (message != null ? ": " + message : "") + ")";
    }

    private LinearLayout form() {
        int pad = Math.round(16 * activity.getResources().getDisplayMetrics().density);
        LinearLayout form = new LinearLayout(activity);
        form.setOrientation(LinearLayout.VERTICAL);
        form.setPadding(pad, pad / 2, pad, 0);
        return form;
    }

    private EditText field(LinearLayout form, String hint, int inputType) {
        EditText field = new EditText(activity);
        field.setHint(hint);
        field.setSingleLine(true);
        field.setInputType(inputType);
        form.addView(field);
        return field;
    }

    private TextView note(LinearLayout form, String text) {
        TextView view = new TextView(activity);
        view.setText(text);
        form.addView(view);
        return view;
    }

    private void showError(String title, String message) {
        new AlertDialog.Builder(activity).setTitle(title).setMessage(message)
            .setPositiveButton("OK", null).show();
    }

    private void toast(String message) {
        Toast.makeText(activity, message, Toast.LENGTH_LONG).show();
    }
}
