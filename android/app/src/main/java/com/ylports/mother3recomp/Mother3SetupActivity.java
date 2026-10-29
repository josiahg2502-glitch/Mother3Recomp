package com.ylports.mother3recomp;

import android.app.AlertDialog;
import android.content.Intent;
import android.graphics.Bitmap;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.StateListDrawable;
import android.os.Bundle;
import android.text.InputType;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.widget.Button;
import android.widget.EditText;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;

import com.ylports.mother3recomp.cloud.CloudSaves;

import org.gbarecomp.GbaGameActivity;
import org.gbarecomp.GbaGameConfig;
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
import java.util.function.Consumer;

/**
 * MOTHER 3's Android front end, built for gamepads (D-pad focus, START plays,
 * B goes back) as well as touch.
 *
 * The engine's setup screen asks for the ROM and BIOS the first time. Once
 * they are in place this replaces it with three screens:
 * - Home: a card like the game's file select (room, leader, party, play
 *   time; see GameCard), a warning when the in-game save is much older than
 *   the suspended game, and a menu: Continue / Play, Saves, Achievements,
 *   Settings.
 * - Settings: display, audio, controls, mods, game files, help.
 * - Saves: the in-game save, the suspended game and its backups, cloud saves.
 * RetroAchievements opens a dialog from Home.
 *
 * Settings live where the native side reads them on every platform:
 * config.ini ([Launcher], [Display], [Audio], [Touch]) and mods/enabled.txt.
 * If SDL_main returns with an error, the log tail is shown on return.
 */
public final class Mother3SetupActivity extends GbaSetupActivity {
    private static final int PICK_MOD = 2001;
    private static final int PICK_SAVE = 2002;
    private static final long MAX_MOD_BYTES = 16L * 1024 * 1024;
    private static final int SAVE_BYTES = 65536;  // 512 Kbit flash
    private static final String[] PROMPT_STYLES =
        {"Auto", "Xbox", "PlayStation", "Nintendo", "On-screen buttons"};

    // After the game's own file select (its colours and shapes, none of its
    // art): beige cards with a white rim on black, dark text, a ▶ cursor.
    private static final int BACKGROUND = Color.BLACK;
    private static final int SURFACE = Color.rgb(231, 214, 206);
    private static final int SURFACE_FOCUSED = Color.rgb(214, 193, 182);
    private static final int OUTLINE = Color.WHITE;
    private static final int ACCENT = Color.rgb(248, 216, 0);
    private static final int FOCUS = Color.rgb(248, 216, 0);
    private static final int WARNING = Color.rgb(255, 211, 78);
    private static final int TEXT = Color.WHITE;            // on the black background
    private static final int TEXT_SECONDARY = Color.rgb(214, 214, 214);
    private static final int TEXT_MUTED = Color.rgb(150, 150, 150);
    private static final int INK = Color.rgb(33, 33, 33);   // on cards
    private static final int INK_MUTED = Color.rgb(104, 92, 86);

    private enum Page { HOME, SETTINGS, SAVES }

    private Page page = Page.HOME;
    private GbaGameConfig gameConfig;
    private CloudSaves cloudSaves;

    @Override
    protected boolean supportsAutomaticLaunch() { return false; }

    @Override
    protected boolean setupShowsOptions() { return false; }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // Before the setup screen checks for the ROM and builds the home screen.
        gameConfig = GbaGameConfig.load(this);
        // Before PLAY: the engine resumes only markers that name the battery save.
        SuspendMarker.upgrade(getFilesDir(), gameConfig.romFile);
        cloudSaves = new CloudSaves(this, gameConfig.romFile,
            gameConfig.romSha1.length > 0 ? gameConfig.romSha1[0] : "",
            this::redraw);
        super.onCreate(savedInstanceState);
    }

    @Override
    protected void onResume() {
        super.onResume();
        redraw();
        showNativeLogIfChanged();
        // Only after a game started from PLAY (remembered across recreation).
        cloudSaves.syncAfterPlay();
    }

    /** PLAY goes through here: sync the save first, then keep the suspend state safe. */
    @Override
    public void startActivity(Intent intent) {
        if (intent.getComponent() == null
                || !GbaGameActivity.class.getName().equals(intent.getComponent().getClassName())) {
            super.startActivity(intent);
            return;
        }
        cloudSaves.syncBeforePlay(() -> {
            // A sync can outlive this screen; a newer one handles PLAY then.
            if (isDestroyed()) return;
            // After the sync: a download leaves the state on disk but no longer
            // resumes it, and the game's next suspend overwrites it.
            SuspendBackups.keep(getFilesDir(), gameConfig.romFile);
            cloudSaves.gameStarting();
            Mother3SetupActivity.super.startActivity(intent);
        });
    }

    /** START plays from Home; B (or Back) returns to Home from any other screen. */
    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        final int key = event.getKeyCode();
        final boolean back = key == KeyEvent.KEYCODE_BACK || key == KeyEvent.KEYCODE_BUTTON_B;
        final boolean away = page != Page.HOME || isSetupScreenShown();
        if (back && away && filesReady()) {
            if (event.getAction() == KeyEvent.ACTION_UP) showPage(Page.HOME);
            return true;
        }
        if (key == KeyEvent.KEYCODE_BUTTON_START && !away && filesReady()) {
            if (event.getAction() == KeyEvent.ACTION_UP) play();
            return true;
        }
        return super.dispatchKeyEvent(event);
    }

    // ── Screens ───────────────────────────────────────────────────────────

    @Override
    protected View buildHomeContent() {
        page = Page.HOME;
        return buildHome();
    }

    private void showPage(Page target) {
        if (target == Page.HOME) {
            page = Page.HOME;
            showHomeScreen();
            return;
        }
        page = target;
        setContentView(target == Page.SETTINGS ? buildSettings() : buildSaves());
    }

    /** Rebuilds the screen on show, so dates and statuses are current. */
    private void redraw() {
        if (!filesReady() || isSetupScreenShown()) return;
        showPage(page);
    }

    private View buildHome() {
        LinearLayout root = column();
        root.setPadding(dp(48), dp(24), dp(48), dp(14));

        LinearLayout header = new LinearLayout(this);
        header.setGravity(Gravity.CENTER_VERTICAL);
        TextView title = text(gameConfig.title, 26, TEXT);
        title.setTypeface(bold());
        header.addView(title, new LinearLayout.LayoutParams(0, -2, 1));
        String user = RaBridge.savedUser(getFilesDir());
        header.addView(text(user != null ? "RetroAchievements · " + user : "", 14, TEXT_MUTED));
        root.addView(header);

        boolean suspended = suspendMarker().isFile() && suspendState().isFile();
        root.addView(fileCard(suspended), size(-1, -2, dp(16), 0));

        String warning = saveWarning();
        if (warning != null) {
            TextView banner = text("!   " + warning, 14, WARNING);
            // No border: a yellow border means "selected" everywhere else.
            banner.setBackground(rounded(Color.argb(40, 255, 211, 78), Color.TRANSPARENT, 8));
            banner.setPadding(dp(18), dp(10), dp(18), dp(10));
            root.addView(banner, size(-1, -2, dp(10), 0));
        }

        LinearLayout menu = new LinearLayout(this);
        menu.setOrientation(LinearLayout.VERTICAL);
        menu.setBackground(rounded(SURFACE, OUTLINE, 8));
        menu.setPadding(dp(10), dp(10), dp(10), dp(10));
        menuItems.clear();
        View primary = menuItem(suspended ? "Continue" : "Play", this::play);
        menu.addView(primary);
        menu.addView(menuItem("Saves", () -> showPage(Page.SAVES)));
        menu.addView(menuItem("Achievements", this::showRetroAchievementsDialog));
        menu.addView(menuItem("Settings", () -> showPage(Page.SETTINGS)));
        LinearLayout center = new LinearLayout(this);
        center.setGravity(Gravity.CENTER);
        center.addView(menu, new LinearLayout.LayoutParams(dp(360), -2));
        root.addView(center, new LinearLayout.LayoutParams(-1, 0, 1));

        LinearLayout footer = new LinearLayout(this);
        footer.addView(text("START  play", 13, TEXT_MUTED), new LinearLayout.LayoutParams(0, -2, 1));
        footer.addView(text("Unofficial fan project. Not affiliated with Nintendo.", 13, TEXT_MUTED));
        root.addView(footer);

        pointAt(primary);  // before any focus: Android starts in touch mode
        primary.post(primary::requestFocus);
        return root;
    }

    /**
     * The game's file select, as text: where, who and how long, from the
     * suspended game when Continue resumes one, else from the in-game save
     * (its slot with the most play time).
     */
    private View fileCard(boolean suspended) {
        File rom = new File(getFilesDir(), "roms/" + gameConfig.romFile);
        GameCard card = suspended ? GameCard.fromSuspendState(suspendState(), rom) : null;
        String when = suspended ? "Suspended " + when(suspendState().lastModified()) : null;
        if (card == null && !suspended) {
            for (int slot = 0; slot < 2; ++slot) {
                GameCard saved = GameCard.fromSave(saveFile(), slot, rom);
                if (saved != null && (card == null || saved.playFrames > card.playFrames)) card = saved;
            }
            if (card != null) when = "Saved " + when(saveFile().lastModified());
        }

        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setBackground(rounded(SURFACE, OUTLINE, 8));
        box.setPadding(dp(24), dp(14), dp(24), dp(14));
        if (card == null) {
            TextView empty = text(suspended ? "Suspended game" : saveFile().isFile() ? "In-game save" : "New game",
                20, INK);
            empty.setTypeface(bold());
            box.addView(empty);
            if (when != null) box.addView(text(when, 14, INK_MUTED));
            return box;
        }

        LinearLayout row = new LinearLayout(this);
        row.setGravity(Gravity.CENTER_VERTICAL);
        // The party's portraits, from the player's ROM, at a whole multiple
        // with no smoothing so the pixels stay sharp.
        int scale = Math.max(1, dp(56) / Portraits.HEIGHT);
        for (int number : card.party) {
            Bitmap face = Portraits.load(rom, number);
            if (face == null) continue;
            ImageView view = new ImageView(this);
            view.setImageBitmap(Bitmap.createScaledBitmap(face, Portraits.WIDTH * scale,
                Portraits.HEIGHT * scale, false));
            row.addView(view, size(-2, -2, 0, 0, 0, dp(6)));
        }
        LinearLayout info = new LinearLayout(this);
        info.setOrientation(LinearLayout.VERTICAL);
        row.addView(info, new LinearLayout.LayoutParams(0, -2, 1));
        box.addView(row);

        LinearLayout top = new LinearLayout(this);
        TextView who = text(card.leader + "    Lv " + card.level, 20, INK);
        who.setTypeface(bold());
        top.addView(who, new LinearLayout.LayoutParams(0, -2, 1));
        if (card.place != null) {
            TextView place = text(card.place, 20, INK);
            place.setTypeface(bold());
            top.addView(place);
        }
        info.addView(top, size(-1, -2, 0, 0, dp(10), 0));

        LinearLayout middle = new LinearLayout(this);
        middle.setGravity(Gravity.CENTER_VERTICAL);
        String with = card.companions.isEmpty() ? "" : "with " + String.join(", ", card.companions);
        middle.addView(text(with, 15, INK_MUTED), new LinearLayout.LayoutParams(0, -2, 1));
        TextView label = text("TIME  ", 15, ACCENT);
        label.setTypeface(bold());
        label.setShadowLayer(1, dp(1), dp(1), INK);
        middle.addView(label);
        TextView time = text(card.playTime(), 18, INK);
        time.setTypeface(bold());
        middle.addView(time);
        info.addView(middle, size(-1, -2, dp(4), 0, dp(10), 0));

        if (when != null) box.addView(text(when, 14, INK_MUTED), size(-1, -2, dp(6), 0));
        return box;
    }

    private final List<TextView> menuItems = new ArrayList<>();

    /** One line of the menu card: the ▶ cursor marks the focused one, as in the game. */
    private View menuItem(String label, Runnable onPick) {
        TextView item = text(label, 20, INK);
        item.setTag(label);
        item.setTypeface(bold());
        item.setFocusable(true);
        item.setClickable(true);
        item.setPadding(dp(14), dp(8), dp(14), dp(8));
        StateListDrawable states = new StateListDrawable();
        states.addState(new int[] {android.R.attr.state_pressed}, rounded(SURFACE_FOCUSED, Color.TRANSPARENT, 6));
        states.addState(new int[] {android.R.attr.state_focused}, rounded(SURFACE_FOCUSED, Color.TRANSPARENT, 6));
        states.addState(new int[] {}, rounded(Color.TRANSPARENT, Color.TRANSPARENT, 6));
        item.setBackground(states);
        item.setOnFocusChangeListener((view, focused) -> { if (focused) pointAt(item); });
        item.setOnClickListener(view -> onPick.run());
        menuItems.add(item);
        pointAt(null);
        return item;
    }

    /** Puts the ▶ cursor on one menu line (none: null) and clears the others. */
    private void pointAt(View target) {
        for (TextView item : menuItems)
            item.setText((item == target ? "▶   " : "      ") + item.getTag());
    }

    private View buildSettings() {
        LinearLayout body = column();

        body.addView(section("Display"));
        body.addView(toggle("Widescreen (16:9)",
            "Fills the 16:9 screen with more of the field and battles. Applies when the game starts.",
            "1".equals(iniValue("Launcher", "m3_widescreen")),
            on -> setIniValue("Launcher", "m3_widescreen", on ? "1" : "0")));
        body.addView(toggle("Integer scaling",
            "Whole-pixel multiples: the sharpest picture, with thin borders.",
            "1".equals(iniValue("Display", "integer_scale")),
            on -> setIniValue("Display", "integer_scale", on ? "1" : "0")));
        body.addView(toggle("Stretch to fill",
            "Fills the whole screen; pixels are no longer square.",
            fillDisplay(), this::setFillDisplay));
        body.addView(toggle("Hide system bars", "Full screen, here and in the game.",
            hideSystemBars(), this::setHideSystemBars));

        body.addView(section("Audio"));
        final int mode = audioMode();
        String[][] modes = {
            {"Enhanced", "A high-precision mix of the original music, without the 8-bit hiss."},
            {"HQ (default)", "The game's own mixer patched to high quality. Changes the game's random numbers."},
            {"Original", "The GBA's sound as it was."}};
        for (int i = 0; i < modes.length; ++i) {
            final int choice = i;
            body.addView(choice(modes[i][0], modes[i][1], mode == i, () -> chooseAudio(choice)));
        }

        body.addView(section("Controls"));
        body.addView(toggle("On-screen buttons", "Touch controls over the game.",
            onScreenButtons(), this::setOnScreenButtons));
        int style = 0;
        try { style = Integer.parseInt(iniValue("Launcher", "m3_button_prompts")); }
        catch (NumberFormatException ignored) {}
        final int prompts = Math.max(0, Math.min(PROMPT_STYLES.length - 1, style));
        body.addView(value("Button names in the game", "How the game's text names the buttons.",
            PROMPT_STYLES[prompts], () -> {
                setIniValue("Launcher", "m3_button_prompts",
                    Integer.toString((prompts + 1) % PROMPT_STYLES.length));
                showPage(Page.SETTINGS);
            }));

        body.addView(section("Mods"));
        addMods(body);

        body.addView(section("Game files"));
        body.addView(note("ROM " + (romInstalled() ? "verified" : "missing")
            + "    ·    BIOS " + (biosInstalled() ? "verified" : "not provided")));
        Button files = button("Change ROM, BIOS or language patch", false);
        files.setOnClickListener(view -> showSetupScreen());
        body.addView(files, size(-1, dp(52), dp(6), 0));

        body.addView(section("Help"));
        Button diagnostics = button("Copy diagnostics", false);
        diagnostics.setOnClickListener(view -> copyDiagnostics());
        body.addView(diagnostics, size(-1, dp(52), 0, 0));

        return screen("Settings", body);
    }

    private View buildSaves() {
        LinearLayout body = column();

        body.addView(section("In-game save"));
        body.addView(note(saveFile().isFile()
            ? "Last saved " + when(saveFile().lastModified()) + ", at a save point in the game."
            : "No in-game save yet."));
        String warning = saveWarning();
        if (warning != null) {
            TextView alert = note(warning);
            alert.setTextColor(WARNING);
            body.addView(alert);
        }
        Button importSave = button("Import a save from another emulator", false);
        importSave.setOnClickListener(view -> {
            Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            intent.setType("*/*");
            startActivityForResult(intent, PICK_SAVE);
        });
        body.addView(importSave, size(-1, dp(52), dp(6), 0));

        body.addView(section("Suspended game"));
        boolean suspended = suspendMarker().isFile() && suspendState().isFile();
        body.addView(note(suspended
            ? "Suspended " + when(suspendState().lastModified()) + ". Continue picks up there."
            : "None. Play starts from the in-game save."));
        int copies = SuspendBackups.list(getFilesDir(), gameConfig.romFile).length;
        body.addView(note(copies == 0
            ? "A copy is kept each time a suspended game is resumed."
            : copies + (copies == 1 ? " earlier point kept." : " earlier points kept.")));
        Button restore = button("Resume from an earlier point", false);
        restore.setOnClickListener(view -> showSuspendBackups());
        body.addView(restore, size(-1, dp(52), dp(6), 0));

        body.addView(section("Cloud saves"));
        body.addView(note(cloudSaves.configured()
            ? cloudSaves.buttonLabel().replace("Cloud saves: ", "Syncing with ")
                + ", when you press Play and when the game closes."
            : "Off. Keep the in-game save in sync with your own RomM or Hoard server."));
        Button cloud = button(cloudSaves.configured() ? "Cloud settings" : "Set up cloud saves", false);
        cloud.setOnClickListener(view -> cloudSaves.showSettings());
        body.addView(cloud, size(-1, dp(52), dp(6), 0));

        return screen("Saves", body);
    }

    // ── Building blocks ───────────────────────────────────────────────────

    /** A titled screen: Back, the title, and a scrolling body. */
    private View screen(String title, LinearLayout body) {
        LinearLayout root = column();
        root.setPadding(dp(48), dp(20), dp(48), 0);
        LinearLayout header = new LinearLayout(this);
        header.setGravity(Gravity.CENTER_VERTICAL);
        Button back = button("‹  Back", false);
        back.setOnClickListener(view -> showPage(Page.HOME));
        header.addView(back, size(dp(140), dp(48), 0, 0, 0, dp(24)));
        TextView heading = text(title, 24, TEXT);
        heading.setTypeface(bold());
        header.addView(heading);
        root.addView(header);

        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        body.setPadding(0, dp(4), dp(14), dp(32));  // room for the scroll bar
        scroll.addView(body);
        root.addView(scroll, new LinearLayout.LayoutParams(-1, 0, 1));
        back.post(back::requestFocus);
        return root;
    }

    private LinearLayout column() {
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setBackgroundColor(BACKGROUND);
        return layout;
    }

    private TextView section(String title) {
        TextView view = text(title.toUpperCase(Locale.US), 13, ACCENT);
        view.setTypeface(bold());
        view.setLetterSpacing(0.14f);
        view.setPadding(0, dp(22), 0, dp(6));
        return view;
    }

    private TextView note(String value) {
        TextView view = text(value, 14, TEXT_SECONDARY);
        view.setPadding(0, dp(2), 0, dp(2));
        return view;
    }

    /** A focusable row: title, detail, and a switch the whole row toggles. */
    private View toggle(String title, String detail, boolean checked, Consumer<Boolean> onChange) {
        LinearLayout row = row(title, detail);
        Switch control = new Switch(this);
        control.setChecked(checked);
        control.setFocusable(false);
        control.setClickable(false);
        row.addView(control);
        row.setOnClickListener(view -> {
            control.setChecked(!control.isChecked());
            onChange.accept(control.isChecked());
        });
        return row;
    }

    /** A focusable row in a single-choice group. */
    private View choice(String title, String detail, boolean selected, Runnable onPick) {
        LinearLayout row = row(title, detail);
        TextView mark = text(selected ? "●" : "○", 26, selected ? INK : INK_MUTED);
        row.addView(mark);
        row.setOnClickListener(view -> onPick.run());
        return row;
    }

    /** A focusable row that cycles a value. */
    private View value(String title, String detail, String current, Runnable onNext) {
        LinearLayout row = row(title, detail);
        TextView shown = text(current + "  ›", 15, INK);
        row.addView(shown);
        row.setOnClickListener(view -> onNext.run());
        return row;
    }

    private LinearLayout row(String title, String detail) {
        LinearLayout row = new LinearLayout(this);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setFocusable(true);
        row.setClickable(true);
        row.setBackground(states(SURFACE, Color.TRANSPARENT));
        row.setPadding(dp(18), dp(10), dp(18), dp(10));
        LinearLayout texts = new LinearLayout(this);
        texts.setOrientation(LinearLayout.VERTICAL);
        TextView name = text(title, 16, INK);
        texts.addView(name);
        if (detail != null && !detail.isEmpty()) texts.addView(text(detail, 13, INK_MUTED));
        row.addView(texts, new LinearLayout.LayoutParams(0, -2, 1));
        row.setLayoutParams(size(-1, -2, dp(4), 0));
        return row;
    }

    private Button button(String label, boolean primary) {
        Button button = new Button(this);
        button.setText(label);
        button.setAllCaps(false);
        button.setTypeface(bold());
        button.setTextSize(primary ? 18 : 15);
        button.setTextColor(INK);
        button.setFocusable(true);
        button.setBackground(states(SURFACE, OUTLINE));
        return button;
    }

    /** Background with a visible focus border: the platform highlight is hidden by custom fills. */
    private Drawable states(int fill, int edge) {
        StateListDrawable states = new StateListDrawable();
        states.addState(new int[] {android.R.attr.state_pressed}, rounded(fill, FOCUS, 8));
        states.addState(new int[] {android.R.attr.state_focused}, rounded(fill, FOCUS, 8));
        states.addState(new int[] {}, rounded(fill, edge, 8));
        return states;
    }

    private GradientDrawable rounded(int fill, int stroke, int radius) {
        GradientDrawable shape = new GradientDrawable();
        shape.setColor(fill);
        shape.setCornerRadius(dp(radius));
        shape.setStroke(dp(3), stroke);
        return shape;
    }

    private Typeface font;

    /** Pixelify Sans (SIL Open Font License, res/raw/pixelify_sans_ofl.txt). */
    private Typeface font() {
        if (font == null) {
            // By name: the resource package follows the application id.
            int id = getResources().getIdentifier("pixelify_sans", "font", getPackageName());
            try {
                font = id != 0 ? getResources().getFont(id) : Typeface.DEFAULT;
            } catch (RuntimeException missing) {
                font = Typeface.DEFAULT;
            }
        }
        return font;
    }

    private Typeface bold() {
        return Typeface.create(font(), 600, false);
    }

    private TextView text(String value, float size, int color) {
        TextView view = new TextView(this);
        view.setTypeface(font());
        view.setText(value);
        view.setTextSize(size);
        view.setTextColor(color);
        return view;
    }

    private LinearLayout.LayoutParams size(int width, int height, int top, int bottom) {
        return size(width, height, top, bottom, 0, 0);
    }

    private LinearLayout.LayoutParams size(int width, int height, int top, int bottom, int left, int right) {
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(width, height);
        params.setMargins(left, top, right, bottom);
        return params;
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    private static String when(long millis) {
        return new SimpleDateFormat("d MMM yyyy, HH:mm", Locale.US).format(new Date(millis));
    }

    // ── Files ─────────────────────────────────────────────────────────────

    private String stem() {
        String name = gameConfig.romFile;
        int dot = name.lastIndexOf('.');
        return dot >= 0 ? name.substring(0, dot) : name;
    }

    private File saveFile() { return new File(getFilesDir(), "roms/" + stem() + ".sav"); }
    private File suspendState() { return new File(getFilesDir(), "roms/" + stem() + ".suspend.state"); }
    private File suspendMarker() { return new File(getFilesDir(), "roms/" + stem() + ".suspend.pending"); }

    /** The old-save warning, or null when the in-game save is recent enough. */
    private String saveWarning() {
        if (!suspendState().isFile() || !suspendMarker().isFile()) return null;
        String text;
        if (!saveFile().isFile()) {
            text = "No in-game save yet.";
        } else {
            long days = (suspendState().lastModified() - saveFile().lastModified()) / (24L * 60 * 60 * 1000);
            if (days < 1) return null;
            text = "Your in-game save is " + days + (days == 1 ? " day" : " days")
                + " older than the suspended game.";
        }
        return text + " Save at a save point: progress since then lives only in the suspended game.";
    }

    // ── Audio ─────────────────────────────────────────────────────────────

    /**
     * 0 = Enhanced (the engine's MP2K shadow mixer, config.ini [Audio]
     * enhanced), 1 = HQ (the game's mixer patched into the ROM, [Launcher]
     * m3_hq_audio), 2 = Original. The two are never on together: the shadow
     * mixer models the stock driver, and HQ moves its buffers.
     */
    private int audioMode() {
        if (!"0".equals(iniValue("Launcher", "m3_hq_audio"))) return 1;
        return "1".equals(iniValue("Audio", "enhanced")) ? 0 : 2;
    }

    private void setAudioMode(int mode) {
        setIniValue("Launcher", "m3_hq_audio", mode == 1 ? "1" : "0");
        setIniValue("Audio", "enhanced", mode == 0 ? "1" : "0");
    }

    private void chooseAudio(int choice) {
        final int current = audioMode();
        if (choice == current) return;
        // HQ patches the ROM image, so a suspended game made with it on cannot
        // resume with it off, and the other way round.
        boolean romChanges = (current == 1) != (choice == 1);
        if (!romChanges || !suspendMarker().isFile()) {
            setAudioMode(choice);
            showPage(Page.SETTINGS);
            return;
        }
        new AlertDialog.Builder(this)
            .setTitle(choice == 1 ? "Turn HQ audio on?" : "Turn HQ audio off?")
            .setMessage("HQ audio changes the game image, so the suspended game can't continue "
                + "after this switch: the game starts from your in-game save. A copy of the "
                + "suspended game is kept in Saves; switch back and restore it to continue "
                + "from there.")
            .setPositiveButton("Switch", (dialog, which) -> {
                SuspendBackups.keep(getFilesDir(), gameConfig.romFile);
                setAudioMode(choice);
                showPage(Page.SETTINGS);
            })
            .setNegativeButton("Cancel", null)
            .show();
    }

    // ── Suspended game backups ────────────────────────────────────────────

    /** Lists the suspend-state backups and puts one back for the next Continue. */
    private void showSuspendBackups() {
        if (org.libsdl.app.SdlSession.nativeRunning()) {
            // It would write its own state over the restored one when it suspends.
            Toast.makeText(this, "The game is still closing. Try again in a moment.",
                Toast.LENGTH_SHORT).show();
            return;
        }
        File[] backups = SuspendBackups.list(getFilesDir(), gameConfig.romFile);
        if (backups.length == 0) {
            new AlertDialog.Builder(this)
                .setTitle("Earlier points")
                .setMessage("None yet. A copy is kept each time a suspended game is resumed.")
                .setPositiveButton("OK", null)
                .show();
            return;
        }
        String[] labels = new String[backups.length];
        for (int i = 0; i < backups.length; ++i) labels[i] = backupLabel(backups[i]);
        new AlertDialog.Builder(this)
            .setTitle("Resume from an earlier point")
            .setItems(labels, (dialog, which) -> confirmRestore(backups[which], labels[which]))
            .setNegativeButton("Cancel", null)
            .show();
    }

    private void confirmRestore(File backup, String label) {
        new AlertDialog.Builder(this)
            .setTitle("Resume from " + label + "?")
            .setMessage("Continue will pick up from this point. Where you are now is kept "
                + "first, so you can come back to it from this list.")
            .setPositiveButton("Restore", (dialog, which) -> {
                try {
                    SuspendBackups.restore(getFilesDir(), gameConfig.romFile, backup);
                    Toast.makeText(this, "Restored. Continue picks up from " + label + ".",
                        Toast.LENGTH_LONG).show();
                } catch (IOException e) {
                    Toast.makeText(this, "Restore failed: " + e.getMessage(),
                        Toast.LENGTH_LONG).show();
                }
                redraw();
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
                if (stamp != null) return when(stamp.getTime());
            } catch (java.text.ParseException ignored) {
                // fall back to the file name
            }
        }
        return backup.getName();
    }

    // ── RetroAchievements ─────────────────────────────────────────────────

    private void refreshRetroAchievementsButton() {
        redraw();
    }

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
        LinearLayout form = new LinearLayout(this);
        form.setOrientation(LinearLayout.VERTICAL);
        form.setPadding(pad, pad / 2, pad, 0);
        EditText userField = new EditText(this);
        userField.setHint("Username");
        userField.setSingleLine(true);
        userField.setInputType(InputType.TYPE_CLASS_TEXT
            | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        EditText passwordField = new EditText(this);
        passwordField.setHint("Password");
        passwordField.setSingleLine(true);
        passwordField.setInputType(InputType.TYPE_CLASS_TEXT
            | InputType.TYPE_TEXT_VARIATION_PASSWORD);
        TextView status = new TextView(this);
        status.setText("Softcore mode. The password is used once to obtain a "
            + "token and is not stored.");
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
            Button login = dialog.getButton(AlertDialog.BUTTON_POSITIVE);
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
                        error = CloudSaves.describe(failure);
                    }
                    final String message = error;
                    final String account = loggedIn;
                    runOnUiThread(() -> {
                        if (isFinishing() || isDestroyed()) return;
                        if (message == null) {
                            dialog.dismiss();
                            refreshRetroAchievementsButton();
                            Toast.makeText(this, "RetroAchievements: logged in as " + account,
                                Toast.LENGTH_SHORT).show();
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

    // ── Mods ──────────────────────────────────────────────────────────────

    /** One row per installed patch (select to switch it on or off, long-press to remove), then Add. */
    private void addMods(LinearLayout body) {
        String[] names = modsDir().list((dir, name) -> isModFile(name));
        if (names == null || names.length == 0) {
            body.addView(note("No mods installed. IPS, BPS and UPS patches are applied in name order."));
        } else {
            Arrays.sort(names);
            final List<String> on = enabledMods();
            for (String name : names) {
                View row = toggle(name, null, on.contains(name), checked -> {
                    List<String> now = enabledMods();
                    now.remove(name);
                    if (checked) now.add(name);
                    saveEnabledMods(now);
                });
                row.setOnLongClickListener(view -> {
                    new AlertDialog.Builder(this)
                        .setTitle("Remove mod?")
                        .setMessage(name)
                        .setPositiveButton("Remove", (dialog, which) -> {
                            new File(modsDir(), name).delete();
                            List<String> now = enabledMods();
                            now.remove(name);
                            saveEnabledMods(now);
                            showPage(Page.SETTINGS);
                        })
                        .setNegativeButton("Cancel", null)
                        .show();
                    return true;
                });
                body.addView(row);
            }
        }
        Button add = button("Add mod", false);
        add.setOnClickListener(view -> {
            Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            intent.setType("*/*");
            startActivityForResult(intent, PICK_MOD);
        });
        body.addView(add, size(-1, dp(52), dp(6), 0));
    }

    // ── Settings files and diagnostics ────────────────────────────────────

    private File modsDir() { return new File(getFilesDir(), "mods"); }

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
            showPage(Page.SETTINGS);
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
                // The engine would no longer resume the suspended game, so Home
                // stops offering it; Saves can still put it back from its backup.
                SuspendBackups.keep(getFilesDir(), gameConfig.romFile);
                suspendMarker().delete();
                android.widget.Toast.makeText(this, "Save imported. Choose Load on the title screen.",
                    android.widget.Toast.LENGTH_LONG).show();
                redraw();
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
