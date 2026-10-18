// Mother3Recomp â€” native host for the statically recompiled
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
#include "mother3_runtime_hooks.h"
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__linux__)
#include <sys/resource.h>
#endif

#if defined(GBAGAME_RECOMP_UI)
#include "game_launcher_boot.h"
#endif

extern "C" const unsigned kDispatchTableLen;

namespace {
constexpr const char* kEnglishSha1 = "306fb8874533b0fd0796208faa8bed2db4ae77fb";
constexpr const char* kHqAudioSha1 = "4ffe71ce5c5e1e8d9be5fbd09968b60787169b21";
constexpr const char* kJapanSha1   = "4f0f493e12c2a8c61b2d809af03f7abf87a85776";

}

int main(int argc, char** argv) {
#if defined(__linux__)
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
    char hook_path[MAX_PATH];
    if (GetModuleFileNameA(nullptr, hook_path, MAX_PATH)) {
        if (char* slash = std::strrchr(hook_path, '\\')) {
            *slash = 0;
            std::string hook = std::string(hook_path) + "\\mother3_bezel.dll";
            if (GetFileAttributesA(hook.c_str()) != INVALID_FILE_ATTRIBUTES)
                _putenv_s("SDL_DYNAMIC_API", hook.c_str());
        }
    }
#endif
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            std::printf(
                "Mother3Recomp [--bios <path>] [--rom <path>] [game.toml]\n\n"
                "Windowed play opens the launcher first; --no-launcher skips it once.\n"
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
    opts.rom_post_load_hook = &mother3_apply_hq_audio_patch;
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
    opts.max_view_width = 240;

#if defined(GBAGAME_RECOMP_UI)
    std::vector<std::string> args(argv, argv + argc);
    if (game_launcher_preboot(args, opts)) return 0;
    std::vector<char*> av;
    av.reserve(args.size());
    for (auto& arg : args) av.push_back(arg.data());
    return gbarecomp::run_game(static_cast<int>(av.size()), av.data(), opts);
#else
    return gbarecomp::run_game(argc, argv, opts);
#endif
}
