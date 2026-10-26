// Launcher settings, stored in the same files the runtime and the classic
// launcher use: config.ini [Launcher] and keybinds.ini [player1].
#pragma once
#include <array>
#include <string>
#include <vector>

namespace m3settings {

// GBA buttons in input.ini / RuiPadSpec order.
constexpr int kButtonCount = 10;
extern const char* const kButtonKeys[kButtonCount];   // ini key names
extern const char* const kButtonLabels[kButtonCount]; // display names

struct Settings {
    int flavor = 0;          // window flavor 0..6 (Plain .. Melon)
    int fullscreen = 1;      // 0 windowed, 1 borderless, 2 exclusive
    int scale = 6;           // window size multiplier when windowed
    bool sharp_filter = true;
    std::string screen = "raw";  // raw, backlit, frontlit, unlit, classic
    int volume = 100;            // 0..100
    bool widescreen = false;     // field widescreen at 16:9 (284x160; 320x180 full screen on Android)
    int prompt_style = 0;        // in-game buttons: 0 auto, 1 Xbox, 2 PlayStation, 3 Nintendo, 4 keyboard
    int smooth = 0;              // 120 fps smooth motion: 0 off, 1 on 100 Hz+ displays, 2 always
    std::array<std::string, kButtonCount> keys;  // SDL scancode names
};

Settings defaults();
// dir is the folder next to the executable (where the runtime reads them).
Settings load(const std::string& dir);
void save(const std::string& dir, const Settings& s);
// Command-line flags that apply the settings to the game.
std::vector<std::string> game_args(const Settings& s);

}  // namespace m3settings
