#include "m3_settings.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
#include <utility>

namespace m3settings {

const char* const kButtonKeys[kButtonCount] = {"up", "down", "left", "right", "a",
                                               "b",  "l",    "r",    "start", "select"};
const char* const kButtonLabels[kButtonCount] = {"Up", "Down", "Left", "Right", "A",
                                                 "B",  "L",    "R",    "Start", "Select"};
namespace {

// Same defaults as the runtime (host_window.cpp kDefaultBinds), as scancode names.
const char* const kDefaultKeys[kButtonCount] = {"Up", "Down", "Left", "Right", "X",
                                                "Z",  "C",    "V",    "Return", "Right Shift"};
const char* const kScreens[] = {"raw", "backlit", "frontlit", "unlit", "classic"};

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}

std::vector<std::string> read_lines(const std::string& path) {
    std::vector<std::string> lines;
    std::ifstream f(path, std::ios::binary);
    for (std::string l; std::getline(f, l);) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        lines.push_back(l);
    }
    return lines;
}

// key -> value for one section (keys lowercased).
std::map<std::string, std::string> read_section(const std::string& path, const std::string& section) {
    std::map<std::string, std::string> out;
    bool in = false;
    for (const auto& raw : read_lines(path)) {
        const std::string l = trim(raw);
        if (l.empty() || l[0] == '#' || l[0] == ';') continue;
        if (l.front() == '[') { in = lower(l) == "[" + lower(section) + "]"; continue; }
        const auto eq = l.find('=');
        if (in && eq != std::string::npos) out[lower(trim(l.substr(0, eq)))] = trim(l.substr(eq + 1));
    }
    return out;
}

// Rewrites only the given keys of one section; every other line is kept.
void write_section(const std::string& path, const std::string& section,
                   const std::vector<std::pair<std::string, std::string>>& kv) {
    std::vector<std::string> lines = read_lines(path);
    std::vector<bool> done(kv.size(), false);
    int sec_start = -1, sec_end = -1;
    for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
        const std::string l = trim(lines[i]);
        if (!l.empty() && l.front() == '[') {
            if (sec_start >= 0 && sec_end < 0) sec_end = i;
            if (lower(l) == "[" + lower(section) + "]") sec_start = i;
            continue;
        }
        if (sec_start < 0 || sec_end >= 0) continue;
        const auto eq = l.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = lower(trim(l.substr(0, eq)));
        for (std::size_t k = 0; k < kv.size(); ++k)
            if (key == lower(kv[k].first)) { lines[i] = kv[k].first + " = " + kv[k].second; done[k] = true; }
    }
    std::vector<std::string> missing;
    for (std::size_t k = 0; k < kv.size(); ++k)
        if (!done[k]) missing.push_back(kv[k].first + " = " + kv[k].second);
    if (sec_start < 0) {
        if (!lines.empty() && !trim(lines.back()).empty()) lines.push_back("");
        lines.push_back("[" + section + "]");
        lines.insert(lines.end(), missing.begin(), missing.end());
    } else {
        int at = sec_end < 0 ? static_cast<int>(lines.size()) : sec_end;
        while (at > sec_start + 1 && trim(lines[at - 1]).empty()) --at;  // before trailing blanks
        lines.insert(lines.begin() + at, missing.begin(), missing.end());
    }
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    for (const auto& l : lines) f << l << "\n";
}

int to_int(const std::map<std::string, std::string>& m, const char* key, int fallback, int lo, int hi) {
    const auto it = m.find(key);
    if (it == m.end() || it->second.empty()) return fallback;
    return std::clamp(std::atoi(it->second.c_str()), lo, hi);
}

}  // namespace

Settings defaults() {
    Settings s;
    for (int i = 0; i < kButtonCount; ++i) s.keys[i] = kDefaultKeys[i];
    return s;
}

Settings load(const std::string& dir) {
    Settings s = defaults();
    const auto cfg = read_section(dir + "/config.ini", "Launcher");
    s.flavor = to_int(cfg, "m3_window_flavor", s.flavor, 0, 6);
    s.fullscreen = to_int(cfg, "fullscreen", s.fullscreen, 0, 2);
    s.scale = to_int(cfg, "scale", s.scale, 1, 10);
    s.sharp_filter = to_int(cfg, "sharp_filter", 1, 0, 1) != 0;
    s.volume = to_int(cfg, "volume", s.volume, 0, 100);
    s.prompt_style = to_int(cfg, "m3_button_prompts", s.prompt_style, 0, 4);
    s.widescreen = to_int(cfg, "m3_widescreen", 0, 0, 1) != 0;
    s.smooth = to_int(cfg, "m3_smooth_120", 0, 0, 2);
    s.hq_audio = to_int(cfg, "m3_hq_audio", 1, 0, 1) != 0;
    s.auto_rotate = to_int(cfg, "m3_auto_rotate", 0, 0, 1) != 0;
    if (auto it = cfg.find("screen"); it != cfg.end())
        for (const char* name : kScreens)
            if (lower(it->second) == name) s.screen = name;
    const auto keys = read_section(dir + "/keybinds.ini", "player1");
    for (int i = 0; i < kButtonCount; ++i)
        if (auto it = keys.find(kButtonKeys[i]); it != keys.end() && !it->second.empty()) s.keys[i] = it->second;
    return s;
}

void save(const std::string& dir, const Settings& s) {
    write_section(dir + "/config.ini", "Launcher",
                  {{"m3_window_flavor", std::to_string(s.flavor)},
                   {"fullscreen", std::to_string(s.fullscreen)},
                   {"scale", std::to_string(s.scale)},
                   {"sharp_filter", s.sharp_filter ? "1" : "0"},
                   {"screen", s.screen},
                   {"volume", std::to_string(s.volume)},
                   {"m3_button_prompts", std::to_string(s.prompt_style)},
                   {"m3_widescreen", s.widescreen ? "1" : "0"},
                   {"m3_smooth_120", std::to_string(s.smooth)},
                   {"m3_hq_audio", s.hq_audio ? "1" : "0"},
                   {"m3_auto_rotate", s.auto_rotate ? "1" : "0"}});
    std::vector<std::pair<std::string, std::string>> keys;
    for (int i = 0; i < kButtonCount; ++i) keys.emplace_back(kButtonKeys[i], s.keys[i]);
    write_section(dir + "/keybinds.ini", "player1", keys);
}

std::vector<std::string> game_args(const Settings& s) {
    std::vector<std::string> args = {"--fullscreen=" + std::to_string(s.fullscreen), "--scale",
                                      std::to_string(s.scale), "--sharp-filter", s.sharp_filter ? "1" : "0",
                                      "--screen", s.screen, "--volume", std::to_string(s.volume)};
    if (s.widescreen) args.insert(args.end(), {"--view-width", "284"});
    return args;
}

}  // namespace m3settings
