// Decodes launcher art from the player's own MOTHER 3 ROM: overworld sprites,
// the main font and menu window tiles. Only a fallback font is shipped, for
// ROMs without English v1.3's font (the Japanese ROM, translations built on it).
#pragma once
#include <cstdint>
#include <vector>

namespace m3assets {

// ARGB8888 image; 0 is transparent.
struct Image {
    int w = 0, h = 0;
    std::vector<std::uint32_t> px;
};

class Rom {
public:
    // Takes the 32 MiB ROM image; ok() is false when the tables don't match v1.3.
    explicit Rom(std::vector<std::uint8_t> bytes);
    bool ok() const { return ok_; }
    const std::vector<std::uint8_t>& bytes() const { return b_; }

    int sprite_frame_count(int sprite) const;
    // 64x64 canvas with the sprite's feet at (32, 48).
    Image sprite_frame(int sprite, int frame) const;
    // Frame for `step` of the sprite's own walk cycle facing `dir` (0..7, clockwise from down).
    int walk_frame(int sprite, int dir, int step) const;
    // 16x16 glyph for printable ASCII; *advance gets the pixel width to step.
    Image glyph(char c, int* advance) const;
    // Window of w x h tiles (8px each) in one of the menu flavor palettes.
    Image window(int w_tiles, int h_tiles, int flavor) const;
    // Full field map (all three layers); the ROM tables don't store the width
    // where we know to find it, so the caller passes it in 16px tiles.
    Image map(int id, int width_tiles) const;

private:
    std::uint16_t u16(std::size_t o) const;
    std::uint32_t u32(std::size_t o) const;
    bool entry(std::size_t table, int i, std::size_t* off, std::size_t* size) const;
    std::vector<std::uint32_t> sprite_palette(int sprite) const;

    std::vector<std::uint8_t> b_;
    std::vector<std::uint8_t> menu_tiles_;
    bool ok_ = false;
    bool rom_font_ = true;
};

}  // namespace m3assets
