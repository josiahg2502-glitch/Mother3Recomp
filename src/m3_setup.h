// First-run and "change game files" setup for the game-owned launcher: picks the
// BIOS and ROM with the OS file dialog, applies a language patch to a Japanese
// ROM, and remembers the result in bios.cfg / rom.cfg next to the executable.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace m3setup {

struct Files {
    std::string rom_path;             // the image the game boots (patched output for a Japanese ROM)
    std::string bios_path;
    std::vector<std::uint8_t> rom;    // rom_path's bytes
    bool drawable = false;            // its tables match English v1.3 (as do translations built on it), so the launcher can draw it
};

// Loads the remembered files (carrying over a classic-launcher Japanese ROM + patch).
// Returns false when the ROM or BIOS is missing or unreadable.
bool load(const std::string& dir, Files* out);

// Asks for the BIOS (when `bios` is set or none is remembered) and the ROM.
// Returns false if the player cancels.
bool choose(const std::string& dir, Files* files, bool bios);

// Asks for a save from another emulator (.sav / .srm) and copies it over the
// game's save, keeping the old one as a .bak. Returns true once imported.
bool import_save(const std::string& dir);

}  // namespace m3setup
