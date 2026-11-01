// Settings persistence and the launcher -> runtime argument mapping.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "m3_settings.h"

namespace fs = std::filesystem;

namespace {

int failures = 0;

void check(bool condition, const char* what, int line) {
    if (condition) return;
    std::fprintf(stderr, "m3_settings_test:%d: %s\n", line, what);
    ++failures;
}

#define CHECK(cond) check((cond), #cond, __LINE__)

bool has_arg(const std::vector<std::string>& args, const std::string& key,
             const std::string& value) {
    for (std::size_t i = 0; i + 1 < args.size(); ++i)
        if (args[i] == key && args[i + 1] == value) return true;
    return false;
}

void write_config(const fs::path& dir, const std::string& body) {
    std::ofstream out(dir / "config.ini", std::ios::trunc);
    out << "[Launcher]\n" << body;
}

}  // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / "m3_settings_test";
    fs::remove_all(dir);
    fs::create_directories(dir);

    // Legacy key: m3_widescreen = 1 still means 284x160, and the derived bool
    // keeps the Android setup in sync.
    write_config(dir, "m3_widescreen = 1\n");
    m3settings::Settings s = m3settings::load(dir.string());
    CHECK(s.view_width == 284);
    CHECK(s.widescreen);
    CHECK(!s.fill_display);
    CHECK(has_arg(m3settings::game_args(s), "--view-width", "284"));

    // An explicit width wins over the legacy flag, and clamping keeps the
    // value inside the range the runtime accepts.
    write_config(dir, "m3_widescreen = 1\nm3_view_width = 256\nm3_fill_display = 1\n");
    s = m3settings::load(dir.string());
    CHECK(s.view_width == 256);
    CHECK(s.widescreen);
    CHECK(s.fill_display);
    std::vector<std::string> args = m3settings::game_args(s);
    CHECK(has_arg(args, "--view-width", "256"));
    CHECK(has_arg(args, "--fill-display", "1"));

    write_config(dir, "m3_view_width = 1024\n");
    s = m3settings::load(dir.string());
    CHECK(s.view_width == 284);

    // Native width sends no view flags at all.
    write_config(dir, "");
    s = m3settings::load(dir.string());
    CHECK(s.view_width == 240);
    CHECK(!s.widescreen);
    args = m3settings::game_args(s);
    CHECK(std::find(args.begin(), args.end(), "--view-width") == args.end());
    CHECK(std::find(args.begin(), args.end(), "--fill-display") == args.end());

    // The launcher saves both keys so a later read agrees with the toggle.
    s.view_width = 256;
    s.widescreen = true;
    m3settings::save(dir.string(), s);
    m3settings::Settings reloaded = m3settings::load(dir.string());
    CHECK(reloaded.view_width == 256);
    CHECK(reloaded.widescreen);

    fs::remove_all(dir);
    return failures == 0 ? 0 : 1;
}
