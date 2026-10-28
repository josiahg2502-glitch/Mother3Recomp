#include "m3_setup.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <initializer_list>

#include "ips_patch.h"
#include "m3_assets.h"
#include "sha1.h"
#if defined(GBAGAME_RECOMP_UI)
#include "launcher_files.h"
#include "../third_party/tinyfiledialogs.h"
#endif

namespace fs = std::filesystem;

namespace m3setup {
namespace {

constexpr std::size_t kBiosSize = 16384, kRomSize = 32u << 20;
constexpr std::size_t kSaveSize = 65536;  // 512 Kbit flash (game.toml [save])
constexpr const char* kSavePath = "saves/mother3_en_v13.sav";
constexpr const char* kEnglishSha1 = "306fb8874533b0fd0796208faa8bed2db4ae77fb";
constexpr const char* kHqAudioSha1 = "4ffe71ce5c5e1e8d9be5fbd09968b60787169b21";
constexpr const char* kJapanSha1 = "4f0f493e12c2a8c61b2d809af03f7abf87a85776";

std::vector<std::uint8_t> read(const std::string& path) {
    std::vector<std::uint8_t> out;
    if (path.empty()) return out;
    if (std::FILE* f = std::fopen(path.c_str(), "rb")) {
        std::uint8_t buf[1 << 16];
        for (std::size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) out.insert(out.end(), buf, buf + n);
        std::fclose(f);
    }
    return out;
}

std::string first_line(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return {};
    char line[4096] = {};
    if (!std::fgets(line, sizeof line, f)) line[0] = 0;
    std::fclose(f);
    std::string s = line;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

// rom.cfg / bios.cfg may hold paths relative to their own folder (the classic
// launcher's convention), not to wherever the game was started from.
std::string cfg_path(const std::string& dir, const char* name) {
    const std::string value = first_line(dir + "/" + name);
    if (value.empty() || fs::path(value).is_absolute()) return value;
    return (fs::path(dir) / value).lexically_normal().string();
}

void write_line(const std::string& path, const std::string& value) {
    if (std::FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fprintf(f, "%s\n", value.c_str());
        std::fclose(f);
    }
}

std::string sha1(const std::vector<std::uint8_t>& b) { return gba::sha1(b.data(), b.size()).hex(); }

bool is_english(const std::string& sha) { return sha == kEnglishSha1 || sha == kHqAudioSha1; }

// The launcher draws from the ROM's sprite, map, window and font tables, so any
// image that keeps English v1.3's layout works (Spanish and other translations
// built on it, other dumps, extra patches), not only the exact v1.3 file.
bool drawable(const std::vector<std::uint8_t>& rom) { return m3assets::Rom(rom).ok(); }

// A [Launcher] value from config.ini (only needed to carry over classic-launcher patch setups).
std::string launcher_value(const std::string& dir, const char* key) {
    std::FILE* f = std::fopen((dir + "/config.ini").c_str(), "rb");
    if (!f) return {};
    std::string value;
    bool in = false;
    char line[4096];
    while (std::fgets(line, sizeof line, f)) {
        std::string l = line;
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r' || l.back() == ' ')) l.pop_back();
        if (!l.empty() && l[0] == '[') { in = l == "[Launcher]"; continue; }
        const auto eq = l.find('=');
        if (!in || eq == std::string::npos) continue;
        std::string k = l.substr(0, eq);
        while (!k.empty() && k.back() == ' ') k.pop_back();
        if (k != key) continue;
        const auto v = l.find_first_not_of(' ', eq + 1);
        value = v == std::string::npos ? std::string{} : l.substr(v);
    }
    std::fclose(f);
    return value;
}

bool apply_patch(std::vector<std::uint8_t>& rom, const std::string& patch_path) {
    const auto patch = read(patch_path);
    std::uint8_t* out = nullptr;
    std::size_t out_len = 0;
    if (patch.empty() || !rom_patch_apply(rom.data(), rom.size(), patch.data(), patch.size(), &out, &out_len))
        return false;
    rom.assign(out, out + out_len);
    std::free(out);
    return true;
}

// The patched image goes where the classic launcher cached its patch output.
std::string save_patched(const std::string& dir, const std::string& source, const std::vector<std::uint8_t>& rom) {
    std::error_code ec;
    const fs::path folder = fs::path(dir) / "mods" / "rom-patches";
    fs::create_directories(folder, ec);
    const std::string out = (folder / (fs::path(source).stem().string() + " (patched).gba")).string();
    std::FILE* f = std::fopen(out.c_str(), "wb");
    if (!f) return {};
    const bool ok = std::fwrite(rom.data(), 1, rom.size(), f) == rom.size();
    return std::fclose(f) == 0 && ok ? out : std::string{};
}

#if defined(GBAGAME_RECOMP_UI)
std::string pick(const char* title, std::initializer_list<const char*> patterns, const char* desc) {
    char out[4096] = {};
    return launcher_pick_file(title, patterns.begin(), static_cast<int>(patterns.size()), desc, out, sizeof out)
               ? std::string(out) : std::string{};
}
// tinyfiledialogs drops quote characters on some desktops, so messages avoid them.
void notice(const char* message) { tinyfd_messageBox("MOTHER 3", message, "ok", "info", 1); }
bool ask(const char* message) { return tinyfd_messageBox("MOTHER 3", message, "yesno", "question", 1) == 1; }
#endif

}  // namespace

bool load(const std::string& dir, Files* out) {
    Files f;
    f.bios_path = cfg_path(dir, "bios.cfg");
    f.rom_path = cfg_path(dir, "rom.cfg");
    if (read(f.bios_path).size() != kBiosSize) return false;
    f.rom = read(f.rom_path);
    if (f.rom.size() != kRomSize) return false;
    std::string sha = sha1(f.rom);
    // The classic launcher remembered the Japanese ROM plus its language patch.
    if (sha == kJapanSha1 && launcher_value(dir, "rom_patch_enabled") == "1") {
        const std::string source = f.rom_path;
        if (!apply_patch(f.rom, launcher_value(dir, "rom_patch_path"))) return false;
        f.rom_path = save_patched(dir, source, f.rom);
        if (f.rom_path.empty()) return false;
        write_line(dir + "/rom.cfg", f.rom_path);
        sha = sha1(f.rom);
    }
    // An unpatched Japanese ROM plays as is; the launcher's English font isn't in it.
    f.drawable = sha != kJapanSha1 && drawable(f.rom);
    *out = std::move(f);
    return true;
}

bool choose(const std::string& dir, Files* files, bool bios) {
#if defined(GBAGAME_RECOMP_UI)
    Files next = *files;
    if (bios || read(next.bios_path).size() != kBiosSize) {
        for (;;) {
            const std::string p = pick("Choose your GBA BIOS", {"*.bin", "*.BIN"}, "GBA BIOS (gba_bios.bin)");
            if (p.empty()) return false;
            if (read(p).size() == kBiosSize) { next.bios_path = p; break; }
            notice("That file is not a GBA BIOS. The BIOS is a 16 KB file, usually named gba_bios.bin.");
        }
    }
    for (;;) {
        const std::string p = pick("Choose your MOTHER 3 ROM", {"*.gba", "*.GBA"}, "GBA ROM (.gba)");
        if (p.empty()) return false;
        std::vector<std::uint8_t> rom = read(p);
        if (rom.size() != kRomSize) {
            notice("That file is not a MOTHER 3 ROM. MOTHER 3 ROMs are exactly 32 MB.");
            continue;
        }
        std::string sha = sha1(rom), boot = p;
        if (sha == kJapanSha1 &&
            ask("This is the original Japanese ROM. Play it untranslated, in Japanese? "
                "(The MOTHER 3 launcher needs a translated ROM, so the game will start directly.) "
                "Choose No to apply a translation patch, such as the English v1.3 patch, instead.")) {
            // Played as is.
        } else if (sha == kJapanSha1) {
            const std::string patch = pick("Choose the translation patch", {"*.ups", "*.ips", "*.bps", "*.UPS", "*.IPS", "*.BPS"},
                                           "ROM patch (.ups, .ips, .bps)");
            if (patch.empty()) continue;
            if (!apply_patch(rom, patch)) {
                notice("That patch does not fit this ROM. Use a patch made for the clean Japanese ROM.");
                continue;
            }
            boot = save_patched(dir, p, rom);
            if (boot.empty()) {
                notice("The patched ROM could not be saved in the game folder. Check that the folder is writable.");
                continue;
            }
            sha = sha1(rom);
        } else if (!is_english(sha) && !drawable(rom) &&
                   !ask("This ROM is not one of the known MOTHER 3 versions, so it may run slowly or not at all. "
                        "Use it anyway?")) {
            continue;
        }
        next.rom_path = boot;
        next.drawable = sha != kJapanSha1 && drawable(rom);
        next.rom = std::move(rom);
        break;
    }
    write_line(dir + "/bios.cfg", next.bios_path);
    write_line(dir + "/rom.cfg", next.rom_path);
    *files = std::move(next);
    return true;
#else
    (void)dir, (void)files, (void)bios;
    std::fprintf(stderr, "Pass --rom <MOTHER 3 ROM> and --bios <gba_bios.bin>.\n");
    return false;
#endif
}

bool import_save(const std::string& dir) {
#if defined(GBAGAME_RECOMP_UI)
    const std::string p = pick("Choose a MOTHER 3 save file", {"*.sav", "*.srm", "*.SAV", "*.SRM"},
                               "Save file (.sav, .srm)");
    if (p.empty()) return false;
    std::vector<std::uint8_t> save = read(p);
    // Emulators pad flash saves (128 KB flash, mGBA's RTC footer, VBA-Next's 136 KB .srm);
    // the game's 64 KB always come first.
    if (save.size() < kSaveSize || save.size() > 4 * kSaveSize) {
        notice("That file is not a MOTHER 3 save. Pick the .sav or .srm battery save from your emulator "
               "(64 KB or a little more). Save states can't be imported.");
        return false;
    }
    save.resize(kSaveSize);
    if (std::all_of(save.begin(), save.end(), [](std::uint8_t b) { return b == 0xFF; }) ||
        std::all_of(save.begin(), save.end(), [](std::uint8_t b) { return b == 0x00; })) {
        notice("That save file is empty.");
        return false;
    }
    std::error_code ec;
    const fs::path target = fs::path(dir) / kSavePath;
    if (fs::equivalent(p, target, ec)) return false;
    const bool replacing = fs::exists(target, ec);
    if (replacing && !ask("This replaces your current MOTHER 3 save. "
                          "The current one is kept as mother3_en_v13.sav.bak in the saves folder. Continue?"))
        return false;
    fs::create_directories(target.parent_path(), ec);
    if (replacing) fs::copy_file(target, target.string() + ".bak", fs::copy_options::overwrite_existing, ec);
    if (replacing && ec) {
        notice("The current save could not be backed up, so nothing was changed. Check that the game folder is writable.");
        return false;
    }
    std::FILE* f = std::fopen(target.string().c_str(), "wb");
    bool ok = f && std::fwrite(save.data(), 1, save.size(), f) == save.size();
    if (f && std::fclose(f) != 0) ok = false;
    if (!ok) {
        notice("The save could not be written. Check that the game folder is writable.");
        return false;
    }
    notice("Save imported. Choose Load on the game's title screen to continue it.");
    return true;
#else
    (void)dir;
    return false;
#endif
}

}  // namespace m3setup
