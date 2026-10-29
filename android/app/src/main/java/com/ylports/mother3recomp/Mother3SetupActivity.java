package com.ylports.mother3recomp;

import android.app.AlertDialog;

import org.gbarecomp.GbaSetupActivity;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Date;
import java.util.List;
import java.util.Locale;

/**
 * Thin MOTHER 3 launcher diagnostic wrapper.
 *
 * Native stdout/stderr are redirected by gbarecomp to
 * files/android-runtime.log. If SDL_main returns or the previous native
 * process died, surface the tail here instead of making PLAY appear to do
 * nothing.
 *
 * Also hosts the desktop launcher's MOTHER 3 settings (widescreen, in-game
 * button prompts, mods). They are stored in the same files the native side
 * reads on every platform: config.ini [Launcher] and mods/enabled.txt.
 */
public final class Mother3SetupActivity extends GbaSetupActivity {
    private static final int PICK_MOD = 2001;
    private static final int PICK_SAVE = 2002;
    private static final long MAX_MOD_BYTES = 16L * 1024 * 1024;
    private static final int SAVE_BYTES = 65536;  // 512 Kbit flash
    private static final String[] PROMPT_STYLES =
        {"Auto", "Xbox", "PlayStation", "Nintendo", "On-screen buttons"};
    private android.widget.LinearLayout modList;
    private android.widget.Button raButton;
    private android.widget.Button cloudButton;
    private com.ylports.mother3recomp.cloud.CloudSaves cloudSaves;

    @Override
    protected boolean supportsAutomaticLaunch() { return false; }

    @Override
    protected void onCreate(android.os.Bundle savedInstanceState) {
        // Before PLAY: the engine resumes only markers that name the battery save.
        org.gbarecomp.GbaGameConfig config = org.gbarecomp.GbaGameConfig.load(this);
        SuspendMarker.upgrade(getFilesDir(), config.romFile);
        cloudSaves = new com.ylports.mother3recomp.cloud.CloudSaves(this, config.romFile,
            config.romSha1.length > 0 ? config.romSha1[0] : "", this::refreshCloudButton);
        super.onCreate(savedInstanceState);
    }

    @Override
    protected void onResume() {
        super.onResume();
        refreshRetroAchievementsButton();
        refreshCloudButton();
        showNativeLogIfChanged();
        // Only after a game started from PLAY (remembered across recreation).
        cloudSaves.syncAfterPlay();
    }

    /**
     * PLAY goes through here: sync the save with the cloud first, then keep a
     * copy of the suspend state the game is about to resume.
     */
    @Override
    public void startActivity(android.content.Intent intent) {
        if (intent.getComponent() == null
                || !gameActivityClass().getName().equals(intent.getComponent().getClassName())) {
            super.startActivity(intent);
            return;
        }
        cloudSaves.syncBeforePlay(() -> {
            // A sync can outlive this screen; a newer one handles PLAY then.
            if (isDestroyed()) return;
            // After the sync: a download leaves the state on disk but no longer
            // resumes it, and the game's next suspend overwrites it.
            SuspendBackups.keep(getFilesDir(), org.gbarecomp.GbaGameConfig.load(this).romFile);
            cloudSaves.gameStarting();
            Mother3SetupActivity.super.startActivity(intent);
        });
    }

    private void showNativeLogIfChanged() {
        File status = new File(getFilesDir(), "android-exit-code");
        if (!status.isFile()) return;
        try {
            String code = readTail(status, 32).trim();
            status.delete();
            if (code.equals("0")) return;
        } catch (IOException error) {
            return;
        }
        File log = new File(getFilesDir(), "android-runtime.log");
        if (!log.isFile() || log.length() == 0) return;

        String text;
        try {
            text = readTail(log, 12000);
        } catch (IOException error) {
            return;
        }
        if (text.trim().isEmpty()) return;

        final String visible = lastLines(text, 45);
        new AlertDialog.Builder(this)
            .setTitle("MOTHER 3 - startup failed")
            .setMessage(visible)
            .setNeutralButton("Copy diagnostics", (dialog, which) -> copyDiagnostics())
            .setPositiveButton("OK", null)
            .show();
    }

    @Override
    protected void addGameActions(android.widget.LinearLayout preferences) {
        android.widget.Switch widescreen = new android.widget.Switch(this);
        widescreen.setText("Widescreen (16:9 field and battles)");
        styleOption(widescreen);
        widescreen.setChecked("1".equals(iniValue("Launcher", "m3_widescreen")));
        widescreen.setOnCheckedChangeListener((view, checked) ->
            setIniValue("Launcher", "m3_widescreen", checked ? "1" : "0"));
        preferences.addView(widescreen);

        android.widget.Switch hqAudio = new android.widget.Switch(this);
        hqAudio.setText("HQ audio (changes the game's RNG; off for speedruns)");
        styleOption(hqAudio);
        hqAudio.setChecked(!"0".equals(iniValue("Launcher", "m3_hq_audio")));
        hqAudio.setOnCheckedChangeListener((view, checked) ->
            setIniValue("Launcher", "m3_hq_audio", checked ? "1" : "0"));
        preferences.addView(hqAudio);

        android.widget.Button prompts = new android.widget.Button(this);
        int style = 0;
        try { style = Integer.parseInt(iniValue("Launcher", "m3_button_prompts")); }
        catch (NumberFormatException ignored) {}
        final int[] current = {Math.max(0, Math.min(4, style))};
        prompts.setText("In-game buttons: " + PROMPT_STYLES[current[0]]);
        prompts.setAllCaps(false);
        prompts.setOnClickListener(view -> {
            current[0] = (current[0] + 1) % PROMPT_STYLES.length;
            prompts.setText("In-game buttons: " + PROMPT_STYLES[current[0]]);
            setIniValue("Launcher", "m3_button_prompts", Integer.toString(current[0]));
        });
        preferences.addView(prompts);

        raButton = new android.widget.Button(this);
        raButton.setAllCaps(false);
        raButton.setOnClickListener(view -> showRetroAchievementsDialog());
        preferences.addView(raButton);
        refreshRetroAchievementsButton();

        android.widget.TextView modsTitle = new android.widget.TextView(this);
        modsTitle.setText("Mods (IPS / BPS / UPS, applied in name order; long-press to remove)");
        modsTitle.setTextColor(android.graphics.Color.rgb(164, 175, 207));
        modsTitle.setTextSize(13);
        preferences.addView(modsTitle);
        modList = new android.widget.LinearLayout(this);
        modList.setOrientation(android.widget.LinearLayout.VERTICAL);
        preferences.addView(modList);
        refreshMods();
        android.widget.Button addMod = new android.widget.Button(this);
        addMod.setText("Add mod");
        addMod.setAllCaps(false);
        addMod.setOnClickListener(view -> {
            android.content.Intent intent =
                new android.content.Intent(android.content.Intent.ACTION_OPEN_DOCUMENT);
            intent.addCategory(android.content.Intent.CATEGORY_OPENABLE);
            intent.setType("*/*");
            startActivityForResult(intent, PICK_MOD);
        });
        preferences.addView(addMod);

        android.widget.Button importSave = new android.widget.Button(this);
        importSave.setText("Import a save from another emulator (.sav / .srm)");
        importSave.setAllCaps(false);
        importSave.setOnClickListener(view -> {
            android.content.Intent intent =
                new android.content.Intent(android.content.Intent.ACTION_OPEN_DOCUMENT);
            intent.addCategory(android.content.Intent.CATEGORY_OPENABLE);
            intent.setType("*/*");
            startActivityForResult(intent, PICK_SAVE);
        });
        preferences.addView(importSave);

        cloudButton = new android.widget.Button(this);
        cloudButton.setAllCaps(false);
        cloudButton.setOnClickListener(view -> cloudSaves.showSettings());
        preferences.addView(cloudButton);
        refreshCloudButton();

        android.widget.Button earlier = new android.widget.Button(this);
        earlier.setText("Resume from an earlier point");
        earlier.setAllCaps(false);
        earlier.setOnClickListener(view -> showSuspendBackups());
        preferences.addView(earlier);

        android.widget.Button button = new android.widget.Button(this);
        button.setText("Copy diagnostics");
        button.setOnClickListener(view -> copyDiagnostics());
        preferences.addView(button);
    }

    /** Lists the suspend-state backups and puts one back for the next PLAY. */
    private void showSuspendBackups() {
        if (org.libsdl.app.SdlSession.nativeRunning()) {
            // It would write its own state over the restored one when it suspends.
            android.widget.Toast.makeText(this, "The game is still closing. Try again in a moment.",
                android.widget.Toast.LENGTH_SHORT).show();
            return;
        }
        final String romFile = org.gbarecomp.GbaGameConfig.load(this).romFile;
        File[] backups = SuspendBackups.list(getFilesDir(), romFile);
        if (backups.length == 0) {
            new AlertDialog.Builder(this)
                .setTitle("Earlier points")
                .setMessage("None yet. A copy of the suspended game is kept each time PLAY resumes it.")
                .setPositiveButton("OK", null)
                .show();
            return;
        }
        String[] labels = new String[backups.length];
        for (int i = 0; i < backups.length; ++i) labels[i] = backupLabel(backups[i]);
        new AlertDialog.Builder(this)
            .setTitle("Resume from an earlier point")
            .setItems(labels, (dialog, which) -> confirmRestore(romFile, backups[which], labels[which]))
            .setNegativeButton("Cancel", null)
            .show();
    }

    private void confirmRestore(String romFile, File backup, String label) {
        new AlertDialog.Builder(this)
            .setTitle("Resume from " + label + "?")
            .setMessage("PLAY will pick up from this point. Where you are now is kept "
                + "first, so you can come back to it from this list.")
            .setPositiveButton("Restore", (dialog, which) -> {
                try {
                    SuspendBackups.restore(getFilesDir(), romFile, backup);
                    android.widget.Toast.makeText(this, "Restored. PLAY picks up from " + label + ".",
                        android.widget.Toast.LENGTH_LONG).show();
                } catch (IOException e) {
                    android.widget.Toast.makeText(this, "Restore failed: " + e.getMessage(),
                        android.widget.Toast.LENGTH_LONG).show();
                }
            })
            .setNegativeButton("Cancel", null)
            .show();
    }

    /** "25 Oct 2026, 19:45" from mother3_en_v13.20261025-194529.state. */
    private static String backupLabel(File backup) {
        String[] parts = backup.getName().split("\\.");
        if (parts.length >= 3) {
            try {
                Date stamp = new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US)
                    .parse(parts[parts.length - 2]);
                if (stamp != null)
                    return new SimpleDateFormat("d MMM yyyy, HH:mm", Locale.US).format(stamp);
            } catch (java.text.ParseException ignored) {
                // fall back to the file name
            }
        }
        return backup.getName();
    }

    private void refreshCloudButton() {
        if (cloudButton != null) cloudButton.setText(cloudSaves.buttonLabel());
    }

    private void refreshRetroAchievementsButton() {
        if (raButton == null) return;
        String user = RaBridge.savedUser(getFilesDir());
        raButton.setText(user != null ? "RetroAchievements: " + user : "RetroAchievements: log in");
    }

    /** Log in (the password is used once for a token) or out; achievements are softcore. */
    private void showRetroAchievementsDialog() {
        String user = RaBridge.savedUser(getFilesDir());
        if (user != null) {
            new AlertDialog.Builder(this)
                .setTitle("RetroAchievements")
                .setMessage("Logged in as " + user + ".\n\n"
                    + "Achievements unlock in softcore mode: RetroAchievements "
                    + "accepts hardcore unlocks only from approved emulators.")
                .setPositiveButton("OK", null)
                .setNegativeButton("Log out", (dialog, which) -> {
                    RaBridge.logout(getFilesDir());
                    refreshRetroAchievementsButton();
                })
                .show();
            return;
        }
        int pad = Math.round(16 * getResources().getDisplayMetrics().density);
        android.widget.LinearLayout form = new android.widget.LinearLayout(this);
        form.setOrientation(android.widget.LinearLayout.VERTICAL);
        form.setPadding(pad, pad / 2, pad, 0);
        android.widget.EditText userField = new android.widget.EditText(this);
        userField.setHint("Username");
        userField.setSingleLine(true);
        userField.setInputType(android.text.InputType.TYPE_CLASS_TEXT
            | android.text.InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        android.widget.EditText passwordField = new android.widget.EditText(this);
        passwordField.setHint("Password");
        passwordField.setSingleLine(true);
        passwordField.setInputType(android.text.InputType.TYPE_CLASS_TEXT
            | android.text.InputType.TYPE_TEXT_VARIATION_PASSWORD);
        android.widget.TextView status = new android.widget.TextView(this);
        status.setText("Softcore mode. The password is used once to obtain a token and is not stored.");
        form.addView(userField);
        form.addView(passwordField);
        form.addView(status);

        AlertDialog dialog = new AlertDialog.Builder(this)
            .setTitle("RetroAchievements")
            .setView(form)
            .setPositiveButton("Log in", null)
            .setNegativeButton("Cancel", null)
            .create();
        dialog.setOnShowListener(shown -> {
            android.widget.Button login = dialog.getButton(AlertDialog.BUTTON_POSITIVE);
            login.setOnClickListener(view -> {
                String name = userField.getText().toString().trim();
                String password = passwordField.getText().toString();
                if (name.isEmpty() || password.isEmpty()) {
                    status.setText("Enter username and password.");
                    return;
                }
                login.setEnabled(false);
                status.setText("Logging in…");
                new Thread(() -> {
                    String error = null;
                    String loggedIn = null;
                    try {
                        String[] credentials = RaBridge.login(name, password);
                        RaBridge.saveCredentials(getFilesDir(), credentials[0], credentials[1]);
                        loggedIn = credentials[0];
                    } catch (IOException | RuntimeException failure) {
                        error = failure.getMessage() != null ? failure.getMessage()
                                                             : failure.getClass().getSimpleName();
                    }
                    final String message = error;
                    final String account = loggedIn;
                    runOnUiThread(() -> {
                        if (isFinishing() || isDestroyed()) return;
                        if (message == null) {
                            dialog.dismiss();
                            refreshRetroAchievementsButton();
                            android.widget.Toast.makeText(this, "RetroAchievements: logged in as " + account,
                                android.widget.Toast.LENGTH_SHORT).show();
                        } else {
                            status.setText(message);
                            login.setEnabled(true);
                        }
                    });
                }, "ra-login").start();
            });
        });
        dialog.show();
    }

    private void styleOption(android.widget.CompoundButton option) {
        option.setTextColor(android.graphics.Color.rgb(203, 212, 238));
        option.setTextSize(14);
        option.setMinHeight(Math.round(52 * getResources().getDisplayMetrics().density));
    }

    private File modsDir() { return new File(getFilesDir(), "mods"); }

    private static boolean isModFile(String name) {
        String lower = name.toLowerCase(Locale.US);
        return lower.endsWith(".ips") || lower.endsWith(".bps") || lower.endsWith(".ups");
    }

    private List<String> enabledMods() {
        List<String> on = new ArrayList<>();
        File file = new File(modsDir(), "enabled.txt");
        if (!file.isFile()) return on;
        try {
            for (String line : Files.readAllLines(file.toPath(), StandardCharsets.UTF_8)) {
                line = line.trim();
                if (!line.isEmpty() && !line.startsWith("#")) on.add(line);
            }
        } catch (IOException ignored) {
        }
        return on;
    }

    private void saveEnabledMods(List<String> on) {
        StringBuilder text = new StringBuilder("# Mods ticked in the launcher, applied in name order.\n");
        for (String name : on) text.append(name).append('\n');
        try {
            modsDir().mkdirs();
            Files.write(new File(modsDir(), "enabled.txt").toPath(),
                text.toString().getBytes(StandardCharsets.UTF_8));
        } catch (IOException error) {
            android.widget.Toast.makeText(this, "Could not save mods: " + error.getMessage(),
                android.widget.Toast.LENGTH_LONG).show();
        }
    }

    private void refreshMods() {
        if (modList == null) return;
        modList.removeAllViews();
        String[] names = modsDir().list((dir, name) -> isModFile(name));
        if (names == null || names.length == 0) {
            android.widget.TextView none = new android.widget.TextView(this);
            none.setText("No mods installed.");
            none.setTextColor(android.graphics.Color.rgb(132, 143, 173));
            modList.addView(none);
            return;
        }
        Arrays.sort(names);
        final List<String> on = enabledMods();
        for (String name : names) {
            android.widget.CheckBox box = new android.widget.CheckBox(this);
            box.setText(name);
            styleOption(box);
            box.setChecked(on.contains(name));
            box.setOnCheckedChangeListener((view, checked) -> {
                List<String> now = enabledMods();
                now.remove(name);
                if (checked) now.add(name);
                saveEnabledMods(now);
            });
            box.setOnLongClickListener(view -> {
                new AlertDialog.Builder(this)
                    .setTitle("Remove mod?")
                    .setMessage(name)
                    .setPositiveButton("Remove", (dialog, which) -> {
                        new File(modsDir(), name).delete();
                        List<String> now = enabledMods();
                        now.remove(name);
                        saveEnabledMods(now);
                        refreshMods();
                    })
                    .setNegativeButton("Cancel", null)
                    .show();
                return true;
            });
            modList.addView(box);
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, android.content.Intent data) {
        if (requestCode == PICK_SAVE) {
            if (resultCode == RESULT_OK && data != null && data.getData() != null) importSave(data.getData());
            return;
        }
        if (requestCode != PICK_MOD) {
            super.onActivityResult(requestCode, resultCode, data);
            return;
        }
        if (resultCode != RESULT_OK || data == null || data.getData() == null) return;
        try {
            importMod(data.getData());
            refreshMods();
        } catch (IOException error) {
            new AlertDialog.Builder(this)
                .setTitle("Mod not added")
                .setMessage(error.getMessage())
                .setPositiveButton("OK", null)
                .show();
        }
    }

    private void importMod(android.net.Uri uri) throws IOException {
        String name = null;
        try (android.database.Cursor cursor = getContentResolver().query(uri,
                new String[] {android.provider.OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (cursor != null && cursor.moveToFirst()) name = cursor.getString(0);
        }
        // Keep only a plain file name; the native side reads mods/<name>.
        if (name != null) name = name.substring(name.lastIndexOf('/') + 1).trim();
        if (name == null || name.isEmpty() || name.startsWith(".") || !isModFile(name))
            throw new IOException("Choose an .ips, .bps or .ups patch file.");
        File dir = modsDir();
        if (!dir.isDirectory() && !dir.mkdirs()) throw new IOException("Could not create the mods folder.");
        File temporary = File.createTempFile("mod-", ".tmp", dir);
        long total = 0;
        try (InputStream input = getContentResolver().openInputStream(uri);
             FileOutputStream output = new FileOutputStream(temporary)) {
            if (input == null) throw new IOException("Android could not open " + name + ".");
            byte[] buffer = new byte[65536];
            int read;
            while ((read = input.read(buffer)) >= 0) {
                total += read;
                if (total > MAX_MOD_BYTES) throw new IOException("Mods must be 16 MiB or smaller.");
                output.write(buffer, 0, read);
            }
        } catch (IOException error) {
            temporary.delete();
            throw error;
        }
        Files.move(temporary.toPath(), new File(dir, name).toPath(),
            java.nio.file.StandardCopyOption.REPLACE_EXISTING);
        List<String> on = enabledMods();
        if (!on.contains(name)) on.add(name);  // a newly added mod starts ticked
        saveEnabledMods(on);
    }

    /**
     * Copies another emulator's battery save over the game's. Emulators pad
     * flash saves (128 KB flash, RTC footers, VBA-Next's 136 KB .srm); the
     * game's 64 KB always come first. The old save is kept as .sav.bak, and a
     * suspended game no longer matches it, so the next start loads the import.
     */
    private void importSave(android.net.Uri uri) {
        byte[] save = new byte[SAVE_BYTES];
        long total = 0;
        try (InputStream input = getContentResolver().openInputStream(uri)) {
            if (input == null) throw new IOException("Android could not open that file.");
            byte[] buffer = new byte[65536];
            int read;
            while ((read = input.read(buffer)) >= 0) {
                if (total < SAVE_BYTES)
                    System.arraycopy(buffer, 0, save, (int) total, (int) Math.min(read, SAVE_BYTES - total));
                total += read;
                if (total > 4L * SAVE_BYTES) break;
            }
        } catch (IOException error) {
            showSaveError(error.getMessage());
            return;
        }
        if (total < SAVE_BYTES || total > 4L * SAVE_BYTES) {
            showSaveError("That file is not a MOTHER 3 save. Pick the .sav or .srm battery save from your "
                + "emulator (64 KB or a little more). Save states can't be imported.");
            return;
        }
        boolean blank = true;
        for (byte b : save) if (b != save[0]) { blank = false; break; }
        if (blank && (save[0] == 0 || save[0] == (byte) 0xFF)) {
            showSaveError("That save file is empty.");
            return;
        }
        String romFile = org.gbarecomp.GbaGameConfig.load(this).romFile;
        String stem = romFile.contains(".") ? romFile.substring(0, romFile.lastIndexOf('.')) : romFile;
        File target = new File(getFilesDir(), "roms/" + stem + ".sav");
        Runnable write = () -> {
            try {
                File dir = target.getParentFile();
                if (dir != null && !dir.isDirectory() && !dir.mkdirs())
                    throw new IOException("Could not create the save folder.");
                if (target.isFile())
                    Files.copy(target.toPath(), new File(dir, target.getName() + ".bak").toPath(),
                        java.nio.file.StandardCopyOption.REPLACE_EXISTING);
                File temporary = File.createTempFile("save-", ".tmp", dir);
                try (FileOutputStream output = new FileOutputStream(temporary)) {
                    output.write(save);
                    output.getFD().sync();
                } catch (IOException error) {
                    temporary.delete();
                    throw error;
                }
                Files.move(temporary.toPath(), target.toPath(), java.nio.file.StandardCopyOption.REPLACE_EXISTING);
                android.widget.Toast.makeText(this, "Save imported. Choose Load on the title screen.",
                    android.widget.Toast.LENGTH_LONG).show();
            } catch (IOException error) {
                showSaveError(error.getMessage());
            }
        };
        if (!target.isFile()) {
            write.run();
            return;
        }
        new AlertDialog.Builder(this)
            .setTitle("Replace your save?")
            .setMessage("This replaces your current MOTHER 3 save. The current one is kept as "
                + target.getName() + ".bak.")
            .setPositiveButton("Replace", (dialog, which) -> write.run())
            .setNegativeButton("Cancel", null)
            .show();
    }

    private void showSaveError(String message) {
        new AlertDialog.Builder(this)
            .setTitle("Save not imported")
            .setMessage(message)
            .setPositiveButton("OK", null)
            .show();
    }

    /** One value from config.ini, or "" when it is absent. */
    private String iniValue(String section, String key) {
        File file = new File(getFilesDir(), "config.ini");
        if (!file.isFile()) return "";
        try {
            boolean in = false;
            for (String line : Files.readAllLines(file.toPath(), StandardCharsets.UTF_8)) {
                line = line.trim();
                if (line.startsWith("[")) { in = line.equalsIgnoreCase("[" + section + "]"); continue; }
                int eq = line.indexOf('=');
                if (in && eq > 0 && line.substring(0, eq).trim().equalsIgnoreCase(key))
                    return line.substring(eq + 1).trim();
            }
        } catch (IOException ignored) {
        }
        return "";
    }

    /** Rewrites one key of config.ini, keeping every other line. */
    private void setIniValue(String section, String key, String value) {
        File file = new File(getFilesDir(), "config.ini");
        List<String> lines = new ArrayList<>();
        try {
            if (file.isFile()) lines.addAll(Files.readAllLines(file.toPath(), StandardCharsets.UTF_8));
            int start = -1, end = lines.size();
            for (int i = 0; i < lines.size(); ++i) {
                String line = lines.get(i).trim();
                if (!line.startsWith("[")) continue;
                if (start >= 0) { end = i; break; }
                if (line.equalsIgnoreCase("[" + section + "]")) start = i;
            }
            String entry = key + " = " + value;
            if (start < 0) {
                lines.add("[" + section + "]");
                lines.add(entry);
            } else {
                boolean replaced = false;
                for (int i = start + 1; i < end; ++i) {
                    String line = lines.get(i).trim();
                    int eq = line.indexOf('=');
                    if (eq > 0 && line.substring(0, eq).trim().equalsIgnoreCase(key)) {
                        lines.set(i, entry);
                        replaced = true;
                    }
                }
                if (!replaced) lines.add(start + 1, entry);
            }
            File temporary = File.createTempFile("config-", ".tmp", getFilesDir());
            Files.write(temporary.toPath(), (String.join("\n", lines) + "\n")
                .getBytes(StandardCharsets.UTF_8));
            Files.move(temporary.toPath(), file.toPath(),
                java.nio.file.StandardCopyOption.REPLACE_EXISTING,
                java.nio.file.StandardCopyOption.ATOMIC_MOVE);
        } catch (IOException error) {
            android.widget.Toast.makeText(this, "Could not save setting: " + error.getMessage(),
                android.widget.Toast.LENGTH_LONG).show();
        }
    }

    private void copyDiagnostics() {
        String text = "MOTHER 3 | " + android.os.Build.MANUFACTURER + " " + android.os.Build.MODEL
            + " | Android " + android.os.Build.VERSION.RELEASE + "\n";
        try {
            text += "App " + getPackageManager().getPackageInfo(getPackageName(), 0).versionName + "\n";
            text += readTail(new File(getFilesDir(), "android-runtime.log"), 12000);
        } catch (Exception error) {
            text += "No startup log available: " + error.getMessage();
        }
        android.content.ClipboardManager clipboard = (android.content.ClipboardManager)
            getSystemService(CLIPBOARD_SERVICE);
        clipboard.setPrimaryClip(android.content.ClipData.newPlainText("MOTHER 3 diagnostics", text));
        android.widget.Toast.makeText(this, "Diagnostics copied", android.widget.Toast.LENGTH_SHORT).show();
    }

    private static String readTail(File file, int maxBytes) throws IOException {
        long length = file.length();
        int count = (int)Math.min(length, maxBytes);
        byte[] data = new byte[count];
        try (FileInputStream input = new FileInputStream(file)) {
            long skip = Math.max(0L, length - count);
            while (skip > 0) {
                long n = input.skip(skip);
                if (n <= 0) break;
                skip -= n;
            }
            int off = 0;
            while (off < data.length) {
                int n = input.read(data, off, data.length - off);
                if (n < 0) break;
                off += n;
            }
        }
        return new String(data, StandardCharsets.UTF_8);
    }

    private static String lastLines(String text, int maxLines) {
        String[] lines = text.split("\\r?\\n");
        int start = Math.max(0, lines.length - maxLines);
        StringBuilder out = new StringBuilder();
        for (int i = start; i < lines.length; ++i) {
            if (lines[i].isEmpty()) continue;
            out.append(lines[i]).append('\n');
        }
        return out.toString();
    }
}
