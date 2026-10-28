// Mother3Recomp — native host for the statically recompiled
// MOTHER 3 (A3UJ) + MOTHER 3 Fan Translation v1.3 (English) and
// High-Quality Sound Mixing Mod (m4a_hq_mixer).
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "runtime.h"
#include "runtime_arm.h"
#include "view_config.h"
#include "mother3_runtime_hooks.h"
#include "m3_assets.h"
#include "m3_launcher.h"
#include "m3_settings.h"
#include "m3_setup.h"
#include "m3_smooth.h"
#include "m3_widescreen.h"
#if defined(MOTHER3_RETROACHIEVEMENTS)
#include "retroachievements.h"
#endif
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__ANDROID__)
#include <algorithm>
#include <limits>
#include <jni.h>
#include <SDL.h>
#include "ips_patch.h"
#include "mobile_platform.h"
#elif defined(__linux__)
#include <sys/resource.h>
#endif

#if defined(GBAGAME_RECOMP_UI)
#include "game_launcher_boot.h"
#endif

extern "C" const unsigned kDispatchTableLen;

namespace {
// The widescreen field model is checked at vblank, when the game's scroll
// state, layer offsets and VRAM agree. Checked mid-frame it could catch a sky
// layer mid-update (the clouds rebase every 16 px) and drop it for a tick,
// so clouds flickered at the old screen edges. RunOptions::vblank_frame has
// one owner; whatever was installed before runs after.
void (*g_next_vblank)() = nullptr;
void mother3_vblank() {
    m3widescreen::tick();  // throttles itself; nothing to do at 240 px
    if (g_next_vblank) g_next_vblank();
}

// A 320px view also grows 10 rows above and below the native 160: 320x180 is
// 16:9 and a whole 6x of a 1920x1080 handheld panel, so the picture fills the
// screen with no borders. The field model supplies those rows like the side
// margins; other screens keep them black.
void mother3_view_geometry(gbarecomp::ViewGeometry* g, int, int) {
    if (g->width != 320 || g->height != 160) return;
    g->height = 180;
    g->extra_top = 10;
    g->extra_bottom = 10;
}

constexpr const char* kEnglishSha1 = "306fb8874533b0fd0796208faa8bed2db4ae77fb";
constexpr const char* kHqAudioSha1 = "4ffe71ce5c5e1e8d9be5fbd09968b60787169b21";
constexpr const char* kJapanSha1   = "4f0f493e12c2a8c61b2d809af03f7abf87a85776";

std::string arg_value(const std::vector<std::string>& args, const char* flag) {
    for (std::size_t i = 1; i + 1 < args.size(); ++i)
        if (args[i] == flag) return args[i + 1];
    return {};
}

// First line of a path file the classic launcher writes (rom.cfg / bios.cfg).
std::string cfg_path(const std::string& name) {
    std::FILE* f = std::fopen(name.c_str(), "rb");
    if (!f) return {};
    char line[1024] = {};
    if (!std::fgets(line, sizeof line, f)) line[0] = 0;
    std::fclose(f);
    std::string s = line;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

std::vector<std::uint8_t> read_file(const std::string& path) {
    std::vector<std::uint8_t> out;
    if (std::FILE* f = std::fopen(path.c_str(), "rb")) {
        std::uint8_t buf[1 << 16];
        for (std::size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) out.insert(out.end(), buf, buf + n);
        std::fclose(f);
    }
    return out;
}

// The game-owned launcher is the only launcher: on first run (or from Settings >
// Change game files) it asks for the BIOS and ROM itself and applies a language
// patch to a Japanese ROM. --no-launcher skips it; --classic-launcher still opens
// the old setup launcher on request. Returns false when the player quits.
bool m3_launcher_preboot(std::vector<std::string>& args) {
    for (auto it = args.begin(); it != args.end();) {
        if (*it == "--no-launcher") return true;
        if (*it == "--classic-launcher") { args.erase(it); return true; }
        if (*it == "--m3-launcher") it = args.erase(it); else ++it;
    }
    // Config files live next to the executable, where the runtime reads them.
    std::string dir = args[0];
    const auto slash = dir.find_last_of("/\\");
    dir = slash == std::string::npos ? "." : dir.substr(0, slash);
    m3setup::Files files;
    if (const std::string rom = arg_value(args, "--rom"); !rom.empty()) {  // typed paths win
        files.rom_path = rom;
        files.rom = read_file(rom);
        files.bios_path = arg_value(args, "--bios");
        if (files.bios_path.empty()) {
            files.bios_path = cfg_path(dir + "/bios.cfg");
            if (!files.bios_path.empty() && files.bios_path[0] != '/' && files.bios_path.find(':') == std::string::npos)
                files.bios_path = dir + "/" + files.bios_path;  // relative to the game folder, like rom.cfg
        }
        files.drawable = m3assets::Rom(files.rom).ok();
    } else if (!m3setup::load(dir, &files) && !m3setup::choose(dir, &files, false)) {
        return false;  // the player cancelled setup
    }
    m3settings::Settings settings = m3settings::load(dir);
    for (;;) {
        m3assets::Rom rom(files.drawable ? files.rom : std::vector<std::uint8_t>{});
        if (!rom.ok()) {  // its sprite/font/map tables aren't laid out like English v1.3
            std::fprintf(stderr, "m3-launcher: can't draw from '%s'; starting the game directly.\n",
                         files.rom_path.c_str());
            break;
        }
        const m3launcher::Result result = m3launcher::run(rom, settings, dir);
        if (result == m3launcher::Result::Quit) return false;
        if (result == m3launcher::Result::Play) break;
        m3setup::choose(dir, &files, true);  // cancelling keeps the current files
    }
    if (arg_value(args, "--rom").empty()) args.insert(args.end(), {"--rom", files.rom_path});
    if (arg_value(args, "--bios").empty() && !files.bios_path.empty())
        args.insert(args.end(), {"--bios", files.bios_path});
    // Right after argv[0], so flags typed on the command line still win.
    const auto flags = m3settings::game_args(settings);
    args.insert(args.begin() + 1, flags.begin(), flags.end());
    args.push_back("--no-launcher");
    return true;
}

}

int mother3_main(int argc, char** argv) {
#if defined(__linux__) && !defined(__ANDROID__)
    // Generated guest calls require the same stack budget as the Windows build.
    struct rlimit stack;
    constexpr rlim_t required_stack = 256u * 1024u * 1024u;
    if (getrlimit(RLIMIT_STACK, &stack) == 0 && stack.rlim_cur < required_stack) {
        stack.rlim_cur = required_stack;
        if (setrlimit(RLIMIT_STACK, &stack) != 0) {
            std::fprintf(stderr, "Could not reserve the game stack; raise the process stack limit.\n");
            return 1;
        }
    }
#endif
#if defined(_WIN32)
    // The bezel hooks SDL through SDL_DYNAMIC_API, which SDL loads with LoadLibraryA:
    // a folder path with characters outside the ANSI code page, a path over MAX_PATH,
    // or a DLL blocked by antivirus makes SDL pop up an error on every launch. Hand it
    // the 8.3 short path and set the variable only once a test load has worked.
    wchar_t exe_path[32768];
    const DWORD exe_len = GetModuleFileNameW(nullptr, exe_path, 32768);
    if (exe_len > 0 && exe_len < 32768) {
        std::wstring hook(exe_path, exe_len);
        hook = hook.substr(0, hook.find_last_of(L'\\') + 1) + L"mother3_bezel.dll";
        wchar_t short_path[MAX_PATH];
        const DWORD short_len = GetFileAttributesW(hook.c_str()) == INVALID_FILE_ATTRIBUTES
            ? 0 : GetShortPathNameW(hook.c_str(), short_path, MAX_PATH);
        char ansi[MAX_PATH * 4];
        BOOL lossy = FALSE;
        const bool utf8 = GetACP() == CP_UTF8;  // the manifest's activeCodePage (Windows 10 1903+)
        const int ansi_len = short_len > 0 && short_len < MAX_PATH
            ? WideCharToMultiByte(CP_ACP, utf8 ? 0 : WC_NO_BEST_FIT_CHARS, short_path, -1, ansi, sizeof ansi,
                                  nullptr, utf8 ? nullptr : &lossy)
            : 0;
        HMODULE probe = ansi_len > 0 && !lossy ? LoadLibraryA(ansi) : nullptr;
        if (probe) {
            FreeLibrary(probe);
            _putenv_s("SDL_DYNAMIC_API", ansi);
        } else if (short_len > 0) {
            std::fprintf(stderr, "mother3: bezel unavailable (mother3_bezel.dll could not be loaded from this folder)\n");
        }
    }
#endif
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            std::printf(
                "Mother3Recomp [--bios <path>] [--rom <path>] [game.toml]\n\n"
                "Windowed play opens the launcher first; --no-launcher skips it once and\n"
                "--classic-launcher opens the original setup launcher.\n"
                "F10 toggles the bezel, F11 opens the menu, Escape exits.\n");
            return 0;
        }
    }

    if (kDispatchTableLen == 0) {
#if defined(_WIN32)
        _putenv_s("GBARECOMP_FORCE_INTERP", "1");
#else
        setenv("GBARECOMP_FORCE_INTERP", "1", 1);
#endif
    }
    mother3_install_runtime_hooks();

    gbarecomp::RunOptions opts;
    opts.builtin_game_name = "MOTHER 3 (English Fan Translation v1.3)";
    opts.builtin_rom_sha1 = kEnglishSha1;
    opts.allow_rom_sha1_mismatch = true;
    opts.alt_rom_sha1 = kHqAudioSha1;
    opts.runtime_menu_scancode = 68;  // F11; Escape remains the exit key.
    opts.launcher_region = "Japan + fan translations";
    opts.launcher_boxart = "assets/img/boxart_mother3.tga";  // original art, no Nintendo assets
    opts.launcher_game_config = "game.toml";
    opts.launcher_save_path = "saves/mother3_en_v13.sav";
    opts.launcher_expose_widescreen = false;
    opts.launcher_expose_adaptive_view = false;
    opts.widescreen_view_width = 284;
    // Apply HQ audio in memory after the selected language patch is loaded.
    opts.launcher_enable_rom_patches = true;
    opts.launcher_source_rom_sha1 = kJapanSha1;
    opts.launcher_required_patch_sha1 = nullptr;
    opts.launcher_accept_patched_rom = true;   // supported patched language images work directly
    opts.rom_post_load_hook = &mother3_rom_post_load;
    opts.launcher_rom_patch_note =
        "HQ Audio is built in and applied to supported Japanese and fan-translated ROMs. "
        "IPS, IPS32, BPS or UPS language patches can be selected here.";

    // PC conveniences: rewind + fast-forward, save states, sharper scaling.
    opts.expose_assist_tools = true;
    opts.assist_tools_enabled_by_default = true;
    opts.assist_fast_forward_multiplier_default = 4;
    opts.rewind_history_seconds = 30;
    opts.launcher_expose_sharp_filter = true;
    opts.launcher_default_sharp_filter = true;
    opts.freely_resizable_window = true;
    opts.max_view_width = 284;  // ceiling only; --view-width (launcher setting) picks the width
    opts.view_geometry = &mother3_view_geometry;
    opts.runtime_ui_theme = "mother3";  // the runtime menu after the game's own windows

    std::vector<std::string> args(argv, argv + argc);
#if defined(__ANDROID__)
    // App-private storage becomes the working directory, diagnostics go to
    // android-runtime.log and the APK payload's game.toml is appended.
    gbarecomp::MobileProcessOptions mobile;
    mobile.game_config = "variants/mother3en/game.toml";
    mobile.program_name = "./Mother3Recomp";
    gbarecomp::mobile_prepare_process(args, mobile);
    // The setup Activity is Android's launcher: it already ran, and it stores
    // the same config.ini [Launcher] keys the desktop launcher writes.
    args.erase(std::remove(args.begin(), args.end(), std::string("--no-launcher")), args.end());
    // Handhelds get the full-screen 320x180 view (see mother3_view_geometry).
    opts.max_view_width = 320;
    if (m3settings::load(".").widescreen) args.insert(args.begin() + 1, {"--view-width", "320"});
    opts.orientation = gbarecomp::RunOptions::Orientation::Landscape;
    opts.resume_suspend_state_on_launch = true;
    opts.ui_touch_friendly = true;
    // OpenSL ES stops its AudioTrack whenever the 2-block queue runs dry, which chops the
    // sound on devices that deny the low-latency path; SDL's Java AudioTrack driver blocks instead.
    SDL_SetHint(SDL_HINT_AUDIODRIVER, "android");
    opts.touch_pad_default = 1;
    // A pixel font (SIL OFL) for the runtime menu, read from the APK's assets.
    opts.runtime_ui_font = "payload/fonts/pixelify_sans.ttf";
    // shortcut: rewind/fast-forward stay desktop-only until rewind memory is measured on phones.
    opts.expose_assist_tools = false;
    opts.assist_tools_enabled_by_default = false;
#if defined(MOTHER3_RETROACHIEVEMENTS)
    // No-op unless the setup screen stored RetroAchievements credentials.
    mother3::ra_install(opts, MOTHER3_ROM_FILE);
#endif
#else
    if (!m3_launcher_preboot(args)) return 0;
    {
        std::string dir = args[0];
        const auto slash = dir.find_last_of("/\\");
        dir = slash == std::string::npos ? "." : dir.substr(0, slash);
        const char* force = std::getenv("M3_SMOOTH");
        m3smooth::enable(force ? std::atoi(force) : m3settings::load(dir).smooth);
    }
#if defined(GBAGAME_RECOMP_UI)
    if (game_launcher_preboot(args, opts)) return 0;
#endif
#endif
    g_next_vblank = opts.vblank_frame;
    opts.vblank_frame = &mother3_vblank;
    std::vector<char*> av;
    av.reserve(args.size());
    for (auto& arg : args) av.push_back(arg.data());
    const int result = gbarecomp::run_game(static_cast<int>(av.size()), av.data(), opts);
#if defined(MOTHER3_RETROACHIEVEMENTS)
    mother3::ra_shutdown();
#endif
    return result;
}

#if defined(__ANDROID__)
// The setup Activity applies a language patch to the imported Japanese ROM.
extern "C" JNIEXPORT jbyteArray JNICALL
Java_org_gbarecomp_GbaNative_applyRomPatch(JNIEnv* env, jclass, jbyteArray source, jbyteArray patch) {
    if (!source || !patch) return nullptr;
    std::vector<std::uint8_t> source_bytes(static_cast<std::size_t>(env->GetArrayLength(source)));
    std::vector<std::uint8_t> patch_bytes(static_cast<std::size_t>(env->GetArrayLength(patch)));
    env->GetByteArrayRegion(source, 0, static_cast<jsize>(source_bytes.size()),
                            reinterpret_cast<jbyte*>(source_bytes.data()));
    env->GetByteArrayRegion(patch, 0, static_cast<jsize>(patch_bytes.size()),
                            reinterpret_cast<jbyte*>(patch_bytes.data()));
    if (env->ExceptionCheck()) return nullptr;
    std::uint8_t* output = nullptr;
    std::size_t output_size = 0;
    if (!rom_patch_apply(source_bytes.data(), source_bytes.size(), patch_bytes.data(), patch_bytes.size(),
                         &output, &output_size))
        return nullptr;
    if (output_size > static_cast<std::size_t>(std::numeric_limits<jsize>::max())) {
        std::free(output);
        return nullptr;
    }
    jbyteArray result = env->NewByteArray(static_cast<jsize>(output_size));
    if (result)
        env->SetByteArrayRegion(result, 0, static_cast<jsize>(output_size), reinterpret_cast<const jbyte*>(output));
    std::free(output);
    return env->ExceptionCheck() ? nullptr : result;
}

extern "C" int SDL_main(int argc, char** argv) {
    // The generated guest calls need a deeper stack than SDLActivity's thread.
    const int result = gbarecomp::mobile_run_with_stack(mother3_main, argc, argv);
    if (std::FILE* status = std::fopen("android-exit-code", "w")) {  // read by the setup Activity
        std::fprintf(status, "%d", result);
        std::fclose(status);
    }
    return result;
}
#else
int main(int argc, char** argv) { return mother3_main(argc, argv); }
#endif
