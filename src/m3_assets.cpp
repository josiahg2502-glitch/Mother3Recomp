#include "m3_assets.h"
#include "m3_launcher_font.h"

#include <algorithm>
#include <string_view>
#include <utility>

namespace m3assets {
namespace {

// ROM offsets (English v1.3 keeps the Japanese graphics layout).
constexpr std::size_t kRomSize    = 0x02000000;
constexpr std::size_t kGfxBank0   = 0x014383E4;  // u32 count + offsets, raw 4bpp tiles
constexpr std::size_t kSpriteInfo = 0x01A442A4;  // frame lists
constexpr std::size_t kSpritePal  = 0x01A41548;  // one 16-colour palette per entry
constexpr std::size_t kSpriteRec  = 0x01433D7C + 0x0C;  // 12-byte record per sprite
constexpr std::size_t kFont       = 0x00CE39F8;  // 16x16 1bpp glyphs, ASCII from space
constexpr std::size_t kMenuGfx    = 0x01B90140;  // LZ77 menu/window tiles
constexpr std::size_t kMenuPal    = 0x01B90824;  // window flavor palettes
constexpr int kMenuPalCount = 42;
constexpr std::size_t kMapInfo     = 0x00D34F44 + 12;  // 26 bytes: 12 x s16 tileset, u16 palette
constexpr std::size_t kMapGfx      = 0x00D3B4E0;  // LZ77, 64 tiles per tileset
constexpr std::size_t kMapPal      = 0x00F3C344;  // 16 x 16-colour palettes, raw
constexpr std::size_t kMapTilemap  = 0x00F9003C;  // LZ77, 3 layers per map, u16 per 16px tile
constexpr std::size_t kMapTileData = 0x0104D9CC;  // LZ77, 0x300 x 8-byte 16px tiles per map
constexpr int kWinCorner = 2, kWinTop = 3, kWinLeft = 34, kWinFill = 25;

// GBA OAM shape/size -> pixel size.
constexpr int kObjW[3][4] = {{8, 16, 32, 64}, {16, 32, 32, 64}, {8, 8, 16, 32}};
constexpr int kObjH[3][4] = {{8, 16, 32, 64}, {8, 8, 16, 32}, {16, 32, 32, 64}};

std::uint32_t bgr555(std::uint16_t c) {
    auto e = [](unsigned v) { return (v << 3) | (v >> 2); };
    return 0xFF000000u | (e(c & 31) << 16) | (e((c >> 5) & 31) << 8) | e((c >> 10) & 31);
}

// GBA BIOS LZ77 (type 0x10). Returns empty on malformed input.
std::vector<std::uint8_t> lz77(const std::vector<std::uint8_t>& b, std::size_t o) {
    if (o + 4 > b.size() || b[o] != 0x10) return {};
    const std::size_t size = b[o + 1] | (b[o + 2] << 8) | (b[o + 3] << 16);
    std::vector<std::uint8_t> out;
    out.reserve(size);
    std::size_t p = o + 4;
    while (out.size() < size) {
        if (p >= b.size()) return {};
        const std::uint8_t flags = b[p++];
        for (int bit = 0; bit < 8 && out.size() < size; ++bit) {
            if (flags & (0x80 >> bit)) {
                if (p + 1 >= b.size()) return {};
                const std::size_t n = (b[p] >> 4) + 3;
                const std::size_t d = (((b[p] & 15) << 8) | b[p + 1]) + 1;
                p += 2;
                if (d > out.size()) return {};
                for (std::size_t k = 0; k < n; ++k) out.push_back(out[out.size() - d]);
            } else {
                if (p >= b.size()) return {};
                out.push_back(b[p++]);
            }
        }
    }
    out.resize(size);
    return out;
}

void blit_tile(Image& img, const std::uint8_t* tile, const std::vector<std::uint32_t>& pal,
               int dx, int dy, bool hflip, bool vflip) {
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) {
            const std::uint8_t byte = tile[y * 4 + x / 2];
            const int v = (x & 1) ? byte >> 4 : byte & 15;
            if (!v) continue;
            const int X = dx + (hflip ? 7 - x : x), Y = dy + (vflip ? 7 - y : y);
            if (X >= 0 && X < img.w && Y >= 0 && Y < img.h) img.px[Y * img.w + X] = pal[v];
        }
}

}  // namespace

Rom::Rom(std::vector<std::uint8_t> bytes) : b_(std::move(bytes)) {
    if (b_.size() != kRomSize || std::string_view(reinterpret_cast<char*>(&b_[0xAC]), 4) != "A3UJ")
        return;
    if (u32(kGfxBank0) != 1001 || u32(kSpriteInfo) != 1000 || u32(kSpritePal) != 365) return;
    menu_tiles_ = lz77(b_, kMenuGfx);
    ok_ = menu_tiles_.size() >= 64 * 32;
    // The Japanese ROM and translations built on it keep these tables but not
    // v1.3's Latin font (FNV-1a of its 'A'-'Z' glyphs), so they get the bundled one.
    std::uint64_t h = 0xCBF29CE484222325ull;
    for (std::size_t i = kFont + 33 * 32; i < kFont + 59 * 32; ++i) h = (h ^ b_[i]) * 0x100000001B3ull;
    rom_font_ = h == 0xB8067095DE201A9Bull;
}

std::uint16_t Rom::u16(std::size_t o) const {
    return o + 2 <= b_.size() ? static_cast<std::uint16_t>(b_[o] | (b_[o + 1] << 8)) : 0;
}
std::uint32_t Rom::u32(std::size_t o) const { return u16(o) | (std::uint32_t(u16(o + 2)) << 16); }

// Tables are u32 count + u32 offsets from the table start; 0 marks a missing
// entry, so an entry runs to the next larger offset.
bool Rom::entry(std::size_t table, int i, std::size_t* off, std::size_t* size) const {
    const std::uint32_t n = u32(table);
    if (i < 0 || std::uint32_t(i) >= n) return false;
    const std::uint32_t o = u32(table + 4 + 4 * i);
    if (!o) return false;
    for (std::uint32_t k = i + 1; k <= n; ++k) {
        const std::uint32_t e = u32(table + 4 + 4 * k);
        if (e > o) {
            *off = table + o;
            *size = e - o;
            return *off + *size <= b_.size();
        }
    }
    return false;
}

// Own palette when the sprite has one and bit 7 of record bytes 5 and 6 is
// clear, else a slot of shared palette 0 (game code 0x08036E9C).
std::vector<std::uint32_t> Rom::sprite_palette(int sprite) const {
    std::size_t o, sz;
    std::size_t base = 0;
    const std::uint8_t* rec = &b_[kSpriteRec + 12 * sprite];
    const bool shared = (rec[5] | rec[6]) & 0x80;
    if (!shared && entry(kSpritePal, sprite, &o, &sz) && sz >= 32) {
        base = o;
    } else if (entry(kSpritePal, 0, &o, &sz)) {
        const std::size_t slot = b_[kSpriteRec + 12 * sprite + 1] & 15;
        if ((slot + 1) * 32 <= sz) base = o + slot * 32;
    }
    std::vector<std::uint32_t> pal(16, 0);
    if (base)
        for (int k = 1; k < 16; ++k) pal[k] = bgr555(u16(base + 2 * k));
    return pal;
}

namespace {
struct Piece { int y, x, tile, attr; };
}

static std::vector<std::vector<Piece>> frames_of(const std::vector<std::uint8_t>& b, std::size_t o,
                                                 std::size_t sz) {
    std::vector<std::vector<Piece>> out;
    const std::size_t anim = b[o + 4] | (b[o + 5] << 8) | (b[o + 6] << 16) | (b[o + 7] << 24);
    const std::size_t end = std::min(o + sz, o + anim);
    std::size_t p = o + 8;
    while (p + 4 <= end) {
        const int n = b[p + 2] | (b[p + 3] << 8);
        p += 4;
        if (n == 0 || n > 32 || p + 4 * n > end) break;
        std::vector<Piece> f;
        for (int k = 0; k < n; ++k, p += 4)
            f.push_back({static_cast<std::int8_t>(b[p]), static_cast<std::int8_t>(b[p + 1]), b[p + 2], b[p + 3]});
        out.push_back(std::move(f));
    }
    return out;
}

int Rom::sprite_frame_count(int sprite) const {
    std::size_t o, sz;
    if (!ok_ || !entry(kSpriteInfo, sprite, &o, &sz) || sz < 8) return 0;
    return static_cast<int>(frames_of(b_, o, sz).size());
}

// The info entry's animation table (offset at +4): a count, then records of
// u16 steps + steps x (frame, duration). Records 8..15 walk in each direction.
int Rom::walk_frame(int sprite, int dir, int step) const {
    std::size_t o, sz;
    if (!ok_ || !entry(kSpriteInfo, sprite, &o, &sz) || sz < 8) return dir;
    std::size_t a = o + u32(o + 4) + 4;
    for (int rec = 0; rec < 8 + dir; ++rec) a += 2 + 2 * std::size_t(u16(a));
    const int steps = u16(a);
    if (steps == 0 || a + 2 + 2 * std::size_t(steps) > o + sz) return dir;
    return b_[a + 2 + 2 * std::size_t(step % steps)];
}

Image Rom::sprite_frame(int sprite, int frame) const {
    Image img{64, 64, std::vector<std::uint32_t>(64 * 64, 0)};
    std::size_t io, isz, go, gsz;
    if (!ok_ || !entry(kSpriteInfo, sprite, &io, &isz) || isz < 8 || !entry(kGfxBank0, sprite, &go, &gsz))
        return img;
    const auto frames = frames_of(b_, io, isz);
    if (frame < 0 || frame >= static_cast<int>(frames.size())) return img;
    const auto pal = sprite_palette(sprite);
    for (const Piece& pc : frames[frame]) {
        const int shape = pc.attr >> 6, size = (pc.attr >> 4) & 3;
        if (shape > 2) continue;
        const int w = kObjW[shape][size], h = kObjH[shape][size];
        const bool hf = pc.attr & 4, vf = pc.attr & 8;
        const int first = pc.tile | ((pc.attr & 3) << 8);  // attr bits 0-1 are tile bits 8-9
        for (int ty = 0; ty < h / 8; ++ty)
            for (int tx = 0; tx < w / 8; ++tx) {
                const std::size_t t = std::size_t(first + ty * (w / 8) + tx) * 32;
                if (t + 32 > gsz) continue;
                const int dx = hf ? w - 8 - tx * 8 : tx * 8, dy = vf ? h - 8 - ty * 8 : ty * 8;
                blit_tile(img, &b_[go + t], pal, 32 + pc.x + dx, 48 + pc.y + dy, hf, vf);
            }
    }
    return img;
}

Image Rom::glyph(char c, int* advance) const {
    Image img{16, 16, std::vector<std::uint32_t>(16 * 16, 0)};
    *advance = 4;
    if (!ok_ || c < 32 || c > 126) return img;
    const std::size_t o = kFont + std::size_t(c - 32) * 32;
    int right = -1;
    if (!rom_font_) {
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 16; ++x)
                if ((kLauncherFont[c - 32][y] << x) & 0x8000) {
                    img.px[y * 16 + x] = 0xFFFFFFFFu;
                    right = std::max(right, x);
                }
        if (right >= 0) *advance = right + 2;
        return img;
    }
    for (int t = 0; t < 4; ++t)
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x)
                if ((b_[o + t * 8 + y] >> (7 - x)) & 1) {
                    const int X = (t & 1) * 8 + x, Y = (t >> 1) * 8 + y;
                    img.px[Y * 16 + X] = 0xFFFFFFFFu;
                    right = std::max(right, X);
                }
    if (right >= 0) *advance = right + 2;
    return img;
}

Image Rom::window(int w, int h, int flavor) const {
    Image img{w * 8, h * 8, std::vector<std::uint32_t>(std::size_t(w) * h * 64, 0)};
    if (!ok_ || w < 2 || h < 2) return img;
    flavor = std::clamp(flavor, 0, kMenuPalCount - 1);
    std::vector<std::uint32_t> pal(16, 0);
    for (int k = 1; k < 16; ++k) pal[k] = bgr555(u16(kMenuPal + 32 * flavor + 2 * k));
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const bool top = y == 0, bot = y == h - 1, lef = x == 0, rig = x == w - 1;
            int t = kWinFill;
            if ((top || bot) && (lef || rig)) t = kWinCorner;
            else if (top || bot) t = kWinTop;
            else if (lef || rig) t = kWinLeft;
            blit_tile(img, &menu_tiles_[t * 32], pal, x * 8, y * 8, rig, bot);
        }
    return img;
}

Image Rom::map(int id, int width_tiles) const {
    Image img;
    if (!ok_ || id < 0 || id >= 1000 || width_tiles <= 0) return img;
    auto blob = [&](std::size_t table, int i) {
        std::size_t o, sz;
        return entry(table, i, &o, &sz) ? lz77(b_, o) : std::vector<std::uint8_t>{};
    };
    std::vector<std::vector<std::uint8_t>> tilesets(12);
    for (int k = 0; k < 12; ++k) {
        const auto set = static_cast<std::int16_t>(u16(kMapInfo + 26 * id + 2 * k));
        if (set >= 0) tilesets[k] = blob(kMapGfx, set);
    }
    std::size_t po, psz;
    const auto tdata = blob(kMapTileData, id);
    if (tdata.size() < 0x300 * 8 || !entry(kMapPal, u16(kMapInfo + 26 * id + 24), &po, &psz) || psz < 512)
        return img;
    std::vector<std::vector<std::uint32_t>> pals(16, std::vector<std::uint32_t>(16, 0));
    for (int p = 0; p < 16; ++p)
        for (int c = 1; c < 16; ++c) pals[p][c] = bgr555(u16(po + 32 * p + 2 * c));

    std::vector<std::vector<std::uint8_t>> layers;
    for (int L = 0; L < 3; ++L) layers.push_back(blob(kMapTilemap, 3 * id + L));
    const std::size_t cells = std::max({layers[0].size(), layers[1].size(), layers[2].size()}) / 2;
    if (!cells || cells % width_tiles) return img;
    const int h_tiles = static_cast<int>(cells / width_tiles);
    img = {width_tiles * 16, h_tiles * 16, std::vector<std::uint32_t>(std::size_t(width_tiles) * h_tiles * 256, 0xFF000000u)};

    for (int L = 2; L >= 0; --L) {  // layer 2 is the bottom of the stack
        const auto& lm = layers[L];
        if (lm.size() / 2 != cells) continue;
        for (std::size_t i = 0; i < cells; ++i) {
            const std::uint16_t v = lm[2 * i] | (lm[2 * i + 1] << 8);
            const int t = v & 0x3FF;
            if (t >= 0x300 || tilesets[t >> 6].size() < 64 * 32) continue;
            const bool hf = v & 0x4000, vf = v & 0x8000;
            const auto& pal = pals[(v >> 10) & 15];
            const std::uint8_t* e = &tdata[t * 8];
            const int vis = e[2] | (e[3] << 8);
            for (int q = 0; q < 4; ++q) {
                if (!(vis & (1 << q))) continue;
                const std::uint8_t mt = e[4 + q];
                const int qx = (q & 1) ^ hf, qy = (q >> 1) ^ vf;
                const int x = static_cast<int>(i % width_tiles) * 16 + qx * 8;
                const int y = static_cast<int>(i / width_tiles) * 16 + qy * 8;
                // Whole-tile and minitile flips combine; flipping twice cancels.
                blit_tile(img, &tilesets[t >> 6][(mt & 0x3F) * 32], pal, x, y,
                          bool(mt & 0x40) != hf, bool(mt & 0x80) != vf);
            }
        }
    }
    return img;
}

}  // namespace m3assets
