#include "m3_widescreen.h"

#include <SDL.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iterator>

#include "gba_bus.h"
#include "gba_ppu.h"
#include "mod_function_hooks.h"
#include "mod_runtime.h"
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"

namespace m3widescreen {
namespace {

// Field state (English v1.3; found by matching VRAM against the ROM maps).
constexpr std::uint32_t kCamera = 0x02001004;     // s16 x, s16 y: world px of the screen's top-left
constexpr std::uint32_t kLayerPtrs = 0x02005C5C;  // 3 x u32: decompressed layer 0..2 (u16 per 16px cell)
constexpr std::uint32_t kTileDataPtr = 0x02009C88;  // u32: 0x300 x 8-byte 16px tile definitions
constexpr std::uint32_t kMapId = 0x020281E4;
constexpr std::uint32_t kGame = 0x02005C00;  // gGame
// Per layer, gGame+0x1C + 4*layer (MapLayerInfo): width in bits 0-2 and height
// in bits 3-5, in 256px units minus one; bits 6-7 == 3 loop it horizontally,
// and the next byte's bits 0-1 == 3 vertically.
constexpr std::uint32_t kLayerInfo = kGame + 0x1C;
// Per layer, gGame+0x58B0 + 0x38*layer: u16 16px column and row of the screen's
// top-left, then (+8, +10) the fine scroll; bit 4 of +0x35 also loops the layer.
// For the scenery layers this is the camera; a sky or weather layer drifts on
// its own (the clouds of map 12 scroll with the wind).
constexpr std::uint32_t kLayerScroll = kGame + 0x58B0;
constexpr std::uint32_t kMapLoops = kGame + 0x11;  // bit 5 horizontally, bit 4 vertically

struct Field {
    bool ok = false;
    bool bg_ok[4] = {};  // which field BGs this model reproduces
    std::uint32_t layer[3] = {};
    std::uint32_t tiles = 0;
    int width = 0, height = 0;  // in 16px cells
    int map = -1;
    // Layer 0 can be a sky or weather layer with its own size, scroll and
    // looping instead of part of the scenery; then it is "detached".
    bool detached = false;
    int lw = 0, lh = 0;  // layer 0 size in 16px cells
    bool loop_x = false, loop_y = false;
};
Field g;  // written by tick(), read by the provider (both on the game thread)
bool g_battle = false;
Uint32 g_next = 0;
int g_misses = 0;  // consecutive failed checks of the field still in use
int g_margin_left = 0, g_margin_right = 0;  // margin widths of the widened view, set by tick()
int g_margin_top = 0, g_margin_bottom = 0;   // rows above and below the native 160

bool in_ewram(std::uint32_t a) { return a >= 0x02000000u && a < 0x02040000u; }

// Field data lives in EWRAM. The provider runs for every margin pixel of every
// layer, so read the bus's backing store directly instead of paying a bus_read_*
// bridge call (MMIO bookkeeping, BIOS sync, read override) per access.
const std::uint8_t* ewram_at(std::uint32_t a, std::uint32_t n) {
    if (!in_ewram(a) || !in_ewram(a + n - 1)) return nullptr;
    gba::GbaBus* bus = gbarecomp::active_bus();
    return bus ? bus->ewram_ptr() + (a - 0x02000000u) : nullptr;
}
std::uint8_t ew8(std::uint32_t a) {
    const std::uint8_t* p = ewram_at(a, 1);
    return p ? p[0] : bus_read_u8(a);
}
std::uint16_t ew16(std::uint32_t a) {
    const std::uint8_t* p = ewram_at(a, 2);
    return p ? static_cast<std::uint16_t>(p[0] | (p[1] << 8)) : bus_read_u16(a);
}
std::uint32_t ew32(std::uint32_t a) {
    const std::uint8_t* p = ewram_at(a, 4);
    return p ? (p[0] | (p[1] << 8) | (p[2] << 16) | (std::uint32_t(p[3]) << 24)) : bus_read_u32(a);
}

const std::uint8_t* io() {
    gba::GbaBus* bus = gbarecomp::active_bus();
    return bus ? bus->io().raw() : nullptr;
}

std::uint16_t io16(const std::uint8_t* r, int off) { return static_cast<std::uint16_t>(r[off] | (r[off + 1] << 8)); }

// World position of the screen's top-left for BG `bg`: the scroll registers hold
// the camera's low 4 bits (the game rebases its ring every 16px), the camera
// variable the rest. Snapping to the scroll keeps the sub-tile phase exact even
// when the variable lags a frame.
void camera(const std::uint8_t* r, int bg, int* x, int* y) {
    const int lx = io16(r, 0x10 + 4 * bg) & 15, ly = io16(r, 0x12 + 4 * bg) & 15;
    const int cx = static_cast<std::int16_t>(ew16(kCamera));
    const int cy = static_cast<std::int16_t>(ew16(kCamera + 2));
    auto snap = [](int var, int low) {
        const int d = var - low;
        return low + 16 * ((d >= 0 ? d + 8 : d - 8) / 16);
    };
    *x = snap(cx, lx);
    *y = snap(cy, ly);
}

int snap16(int var, int low) {
    const int d = var - low;
    return low + 16 * ((d >= 0 ? d + 8 : d - 8) / 16);
}

// Like camera(), for the BG showing a layer: a detached layer 0 (BG1) scrolls
// by its own column/row, again snapped to the scroll registers.
void origin(const Field& f, const std::uint8_t* r, int bg, int* x, int* y) {
    if (bg != 1 || !f.detached) {
        camera(r, bg, x, y);
        return;
    }
    const int lx = io16(r, 0x14) & 15, ly = io16(r, 0x16) & 15;
    *x = snap16(16 * ew16(kLayerScroll) + (ew16(kLayerScroll + 8) & 15), lx);
    *y = snap16(16 * ew16(kLayerScroll + 2) + (ew16(kLayerScroll + 10) & 15), ly);
}

// BG text entry for world 8px tile (tx, ty) of `layer`, or -1 outside the map.
int entry_at(const Field& f, int layer, int tx, int ty) {
    const bool own = layer == 0 && f.detached;
    const int w = own ? f.lw : f.width, h = own ? f.lh : f.height;
    if (own && f.loop_x) tx = ((tx % (2 * w)) + 2 * w) % (2 * w);
    if (own && f.loop_y) ty = ((ty % (2 * h)) + 2 * h) % (2 * h);
    if (!f.layer[layer] || tx < 0 || ty < 0 || tx >= 2 * w || ty >= 2 * h) return -1;
    const std::uint16_t cell = ew16(f.layer[layer] + 2u * std::uint32_t((ty >> 1) * w + (tx >> 1)));
    const int t = cell & 0x3FF;
    if (t >= 0x300) return 0;
    const int sub = (cell >> 10) & 15, hf = (cell >> 14) & 1, vf = (cell >> 15) & 1;
    const std::uint32_t def = f.tiles + 8u * std::uint32_t(t);
    const int vis = ew16(def + 2);
    const int q = (((ty & 1) ^ vf) << 1) | ((tx & 1) ^ hf);
    if (!(vis & (1 << q))) return 0;
    const int mt = ew8(def + 4 + q);
    return ((t >> 6) * 64 + (mt & 0x3F)) | ((((mt >> 6) & 1) ^ hf) << 10) | ((((mt >> 7) & 1) ^ vf) << 11) | (sub << 12);
}

// True when no layer draws anything in 16px cell (cx, cy): the void between
// rooms. A cell is blank when each of its 8px parts is hidden or uses VRAM
// tile 0 (the empty tile); map tile 0 itself can be real ground outdoors.
bool void_cell(const Field& f, int cx, int cy) {
    if (cx < 0 || cy < 0 || cx >= f.width || cy >= f.height) return true;
    for (int L = f.detached ? 1 : 0; L < 3; ++L)  // a detached layer 0 isn't scenery
        for (int q = 0; q < 4; ++q) {
            const int e = entry_at(f, L, 2 * cx + (q & 1), 2 * cy + (q >> 1));
            if (e > 0 && (e & 0x3FF) != 0) return false;
        }
    return true;
}

// void_cell() for the live field, memoized for one rendered frame: in_room()
// asks about the same few cells for every margin pixel of every layer. A 32x32
// direct-mapped window covers the screen plus margins without collisions.
std::uint32_t g_field_gen = 0;  // bumped by tick() whenever it replaces g
std::uint32_t g_frame = 0;      // bumped by frame_start() as each widened frame renders
bool void_cell_live(int cx, int cy) {
    struct Slot { int cx, cy; std::uint64_t frame; std::uint32_t gen; bool is_void; };
    static Slot s_slot[32 * 32];
    static bool s_init = false;
    if (!s_init) {
        for (Slot& e : s_slot) e = {0, 0, ~std::uint64_t{0}, 0, false};
        s_init = true;
    }
    gba::GbaPpu* ppu = gbarecomp::active_ppu();
    const std::uint64_t frame = ppu ? ppu->frame_count() : 0;
    Slot& e = s_slot[((cy & 31) << 5) | (cx & 31)];
    if (e.cx != cx || e.cy != cy || e.frame != frame || e.gen != g_field_gen)
        e = {cx, cy, frame, g_field_gen, void_cell(g, cx, cy)};
    return e.is_void;
}

// Share of BG `bg`'s visible 240x160 that this field model reproduces from VRAM.
double visible_match(const Field& f, const std::uint8_t* r, const std::uint8_t* vram, int bg, int* samples) {
    int ok = 0, total = 0;
    {
        const int cnt = io16(r, 0x08 + 2 * bg);
        const std::uint32_t screen = ((cnt >> 8) & 31) * 0x800u;
        const int hofs = io16(r, 0x10 + 4 * bg), vofs = io16(r, 0x12 + 4 * bg);
        int cx, cy;
        origin(f, r, bg, &cx, &cy);
        for (int y = 0; y < 160; y += 8)
            for (int x = 0; x < 240; x += 8) {
                const int mine = entry_at(f, bg - 1, (cx + x) >> 3, (cy + y) >> 3);
                if (mine < 0) continue;
                const std::uint32_t at = screen + 2u * std::uint32_t((((y + vofs) >> 3) & 31) * 32 + (((x + hofs) >> 3) & 31));
                const int ring = vram[at] | (vram[at + 1] << 8);
                if (!mine && !ring) continue;
                ++total;
                ok += mine == ring;
            }
    }
    *samples = total;
    return total ? double(ok) / total : 0.0;
}

// The field's BG setup: mode 0, BG2+BG3 on (BG1 optional), 256x256 4bpp text maps.
bool field_layout(const std::uint8_t* r) {
    const int dispcnt = io16(r, 0);
    if ((dispcnt & 7) != 0 || (dispcnt & 0x0C00) != 0x0C00) return false;
    for (int bg = 1; bg <= 3; ++bg) {
        const int cnt = io16(r, 0x08 + 2 * bg);
        if ((cnt >> 14) != 0 || (cnt & 0x80)) return false;
    }
    return true;
}

// Scores the model per BG. The scenery layers (BG2, BG3) must match; BG1 joins
// only when it also matches (or is empty), so a layer we can't reproduce is
// left blank in the margins rather than drawn wrong.
bool score(Field& f, const std::uint8_t* r, const std::uint8_t* vram, double need, int* samples_out) {
    int samples_all = 0;
    bool ok = true;
    for (int bg = 1; bg <= 3; ++bg) {
        int samples = 0;
        const double m = visible_match(f, r, vram, bg, &samples);
        // A detached layer 0 says nothing about the scenery width under test.
        if (bg >= 2 || !f.detached) samples_all += samples;
        f.bg_ok[bg] = f.layer[bg - 1] && (samples == 0 || m >= need);
        if (bg >= 2 && samples > 0 && m < need) ok = false;
    }
    *samples_out = samples_all;
    return ok && samples_all > 0;
}

// Interior maps pack several rooms; a margin point belongs to this one only if
// the path from the nearest native pixel out to it never crosses the void:
// along the native row to its column, then along that column to its row.
bool in_room(int cx, int cy, int hx, int y) {
    const int wx = (cx + hx) >> 4, wy = (cy + y) >> 4;
    const int ex = (cx + std::clamp(hx, 0, 239)) >> 4, ey = (cy + std::clamp(y, 0, 159)) >> 4;
    for (int c = ex; c != wx; c += wx < ex ? -1 : 1)
        if (void_cell_live(c, ey)) return false;
    for (int c = ey; c != wy; c += wy < ey ? -1 : 1)
        if (void_cell_live(wx, c)) return false;
    return !void_cell_live(wx, wy);
}

// Pixels of one margin, counted from its outer edge, that fall outside the map or room.
int void_width(int cx, int cy, int y, bool left) {
    const int m = left ? g_margin_left : g_margin_right;
    for (int d = 1; d <= m; ++d)
        if (!in_room(cx, cy, left ? -d : 239 + d, y)) return m - d + 1;
    return 0;
}

// When the camera sits at a map edge (cutscenes often park it there) one margin is
// void. Blacking the same width on the other side keeps the native picture centred
// instead of looking shoved sideways; the black shrinks evenly as the camera moves on.
// Rows above and below the native screen follow its first and last row.
int black_width(int cx, int cy, int y) {
    static int s_cx = -100000, s_cy = 0, s_map = -1;
    static std::uint32_t s_layer = 0;
    static std::int8_t s_width[160];
    y = std::clamp(y, 0, 159);
    if (cx != s_cx || cy != s_cy || g.map != s_map || g.layer[1] != s_layer) {
        s_cx = cx, s_cy = cy, s_map = g.map, s_layer = g.layer[1];
        std::fill(std::begin(s_width), std::end(s_width), static_cast<std::int8_t>(-1));
    }
    if (s_width[y] < 0)
        s_width[y] = static_cast<std::int8_t>(std::max(void_width(cx, cy, y, true), void_width(cx, cy, y, false)));
    return s_width[y];
}

// Window effects (the circle wipe, spotlight reveals) live inside WIN0/WIN1, which
// the game keeps within the native 240 columns, so the margins are outside every
// window and show only what WINOUT lets through there. The plain field never
// enables a window.
bool hidden_outside_windows(const std::uint8_t* r, int layer_bit) {
    return (io16(r, 0) & 0x6000) && !(io16(r, 0x4A) & layer_bit);
}

bool in_black(int cx, int cy, int hx, int y) {
    const int b = black_width(cx, cy, y);
    return b && (hx < -g_margin_left + b || hx >= 240 + g_margin_right - b);
}

// A sprite's position and size in native-relative pixels. OAM X is 9 bits and
// Y 8 bits, so the hardware reads X 256..511 and Y 160..255 as left of and
// above the screen. The margins reach past those limits (X up to 319 when the
// right margin is 80 wide), so a value is read as right of or below the
// screen when that is where it lands in the view; both readings can't.
bool obj_box(std::uint16_t attr0, std::uint16_t attr1, int* sx, int* sy, int* w, int* h) {
    static const int kW[3][4] = {{8, 16, 32, 64}, {16, 32, 32, 64}, {8, 8, 16, 32}};
    static const int kH[3][4] = {{8, 16, 32, 64}, {8, 8, 16, 32}, {16, 32, 32, 64}};
    const int shape = (attr0 >> 14) & 3, size = (attr1 >> 14) & 3;
    if (shape > 2) return false;
    const int doubled = (attr0 & 0x300) == 0x300 ? 2 : 1;  // affine double-size box
    *w = kW[shape][size] * doubled;
    *h = kH[shape][size] * doubled;
    *sx = attr1 & 0x1FF;
    *sy = attr0 & 0xFF;
    if (*sx >= 256 && (*sx >= 240 + g_margin_right || *sx - 512 + *w > -g_margin_left)) *sx -= 512;
    if (*sy >= 160 + g_margin_bottom) *sy -= 256;
    return true;
}

// True when a sprite whose box reaches into a margin belongs there: its feet
// stand on the native screen, or in this room and outside the centring black.
bool obj_shows_in_margin(int sx, int sy, int w, int h) {
    const std::uint8_t* r = io();
    if (!r) return false;
    const int px = std::clamp(sx + w / 2, -64, 303);
    const int py = std::clamp(sy + h - 1, -g_margin_top, 159 + g_margin_bottom);  // feet
    if (px >= 0 && px < 240 && py >= 0 && py < 160) return true;
    int cx, cy;
    camera(r, 2, &cx, &cy);
    return in_room(cx, cy, px, py) && !in_black(cx, cy, px, py) && !hidden_outside_windows(r, 0x10);
}

// Sprites reaching into a margin from another room (furniture next door, parked
// actors) are moved off the canvas; sprites in this room show as usual.
int place_obj(int, std::uint16_t attr0, std::uint16_t attr1, std::uint16_t, int* out_sx) {
    if (!g.ok) return 0;
    int sx, sy, w, h;
    if (!obj_box(attr0, attr1, &sx, &sy, &w, &h)) return 0;
    const bool native = sx >= 0 && sx + w <= 240;  // within the native columns
    *out_sx = native || obj_shows_in_margin(sx, sy, w, h) ? sx : 100000;  // else off every canvas
    return 1;
}

// The rows above and below the native screen draw only sprites this accepts:
// field sprites reaching there whose feet are in this room. Everything else
// keeps the hardware's reading and stays within the native rows.
int place_obj_y(int, std::uint16_t attr0, std::uint16_t attr1, std::uint16_t, int* out_sy) {
    if (!g.ok || (!g_margin_top && !g_margin_bottom)) return 0;
    int sx, sy, w, h;
    if (!obj_box(attr0, attr1, &sx, &sy, &w, &h)) return 0;
    if (sy >= 0 && sy + h <= 160) return 0;  // within the native rows
    if (sy + h <= -g_margin_top || sy >= 160 + g_margin_bottom) return 0;  // in neither margin
    if (!obj_shows_in_margin(sx, sy, w, h)) return 0;
    *out_sy = sy;
    return 1;
}

// The field culls actors against the native screen: the sprite code builds a
// {camera, 240x160} rect and asks one of two overlap tests (rect, pos, size)
// whether an actor's box touches it. Actors outside never reach OAM, so they
// vanished at the old screen edge while the scenery around them showed. While
// the field is widened, answer those tests against the wider view.
// 0x08035820 also serves looping maps (gGame+0x11 bit 5 horizontally, bit 4
// vertically): an actor before the view's left (top) edge still counts when
// the view runs past the map's far edge and wraps onto it. That test is
// reproduced with the game's own 16-bit arithmetic and the 240 it has built
// in replaced by the widened width; the map size comes from the actor's own
// layer when it has one (+0xCB bit 7, layer in +0xBD bits 5-6), else from the
// main layer (gGame+0x0E bits 6-7).
constexpr std::uint32_t kOverlap = 0x080357D0;
constexpr std::uint32_t kOverlapLooping = 0x08035820;
constexpr const char* kCullHooks[] = {"m3.widescreen.cull", "m3.widescreen.cull-looping"};

int layer_px(int index, bool height) {  // sub_0801A61C / sub_0801A638
    const int info = ew8(kLayerInfo + 4u * index);
    return 256 * (((height ? info >> 3 : info) & 7) + 1);
}

int overlap_view(std::uint32_t addr, int, ArmCpuState* cpu) {
    if (!g.ok) return 0;
    const std::uint32_t rect = cpu->R[0], pos = cpu->R[1], size = cpu->R[2], actor = cpu->R[3];
    if (bus_read_u16(rect + 4) != 240 || bus_read_u16(rect + 6) != 160) return 0;  // not the view
    auto s16 = [](std::uint32_t a) { return static_cast<int>(static_cast<std::int16_t>(bus_read_u16(a))); };
    const int x = s16(rect) - g_margin_left, w = 240 + g_margin_left + g_margin_right;
    const int y = s16(rect + 2) - g_margin_top, h = 160 + g_margin_top + g_margin_bottom;
    const int px = s16(pos), py = s16(pos + 2);
    const int half_w = bus_read_u16(size) >> 1, box_h = bus_read_u16(size + 2);
    const int loops = addr == kOverlapLooping ? ew8(kMapLoops) : 0;
    auto map_px = [actor](bool height) {
        const bool own = actor && (bus_read_u8(actor + 0xCB) >> 7);
        return layer_px(own ? (bus_read_u8(actor + 0xBD) >> 5) & 3 : ew8(kGame + 0x0E) >> 6, height);
    };
    auto wrapped = [](int view, int extent, int map) {  // where the wrapped part of the view ends
        return static_cast<int>(static_cast<std::uint16_t>(extent - (map - view)));
    };
    bool hit = true;
    if (x > px + half_w) hit = (loops & 0x20) && px < wrapped(x, w, map_px(false));
    hit = hit && x + w >= px - half_w;
    if (hit && y > py) hit = (loops & 0x10) && py < wrapped(y, h, map_px(true));
    hit = hit && y + h >= py - box_h;
    cpu->R[0] = hit;
    cpu->R[15] = cpu->R[14] & ~1u;  // return to the caller
    return 1;
}

GBA_MOD_CONSTRUCTOR(register_cull_hooks) {
    gba_mod_register_function_entry_plugin(kCullHooks[0], kOverlap, 1, &overlap_view);
    gba_mod_register_function_entry_plugin(kCullHooks[1], kOverlapLooping, 1, &overlap_view);
}

// BG0 carries text boxes, menus and the cinematic letterbox bars. A bar is a
// row of solid one-colour tiles, so a side margin continues BG0 only where the
// native screen edge shows that colour (side_fill); text boxes (bordered,
// transparent edges) stay put. Above and below the screen BG0 continues only when it covers
// the whole edge row (a bar, or a text box or menu against that edge): those
// rows then take a solid tile of that row, so scenery the box hid natively
// doesn't peek out past it.
std::uint32_t bg0_entry_at(const std::uint8_t* r, int hx, int y) {
    const int cnt = io16(r, 0x08), hofs = io16(r, 0x10) & 0x1FF, vofs = io16(r, 0x12) & 0x1FF;
    return ((cnt >> 8) & 31) * 0x800u + 2u * std::uint32_t((((y + vofs) >> 3) & 31) * 32 + (((hx + hofs) >> 3) & 31));
}

bool solid_tile(const std::uint8_t* r, const std::uint8_t* vram, int hx, int y, std::uint16_t* out) {
    const std::uint32_t at = bg0_entry_at(r, hx, y);
    const std::uint16_t entry = static_cast<std::uint16_t>(vram[at] | (vram[at + 1] << 8));
    const std::uint8_t* tile = vram + ((io16(r, 0x08) >> 2) & 3) * 0x4000u + (entry & 0x3FFu) * 32u;
    const std::uint8_t first = tile[0];
    if ((first & 15) == 0 || (first >> 4) != (first & 15)) return false;
    for (int i = 1; i < 32; ++i)
        if (tile[i] != first) return false;
    *out = entry;
    return true;
}

// BG0's colour (palette bank and index) at native pixel (hx, y); 0 is transparent.
int bg0_color_at(const std::uint8_t* r, const std::uint8_t* vram, int hx, int y) {
    const std::uint32_t at = bg0_entry_at(r, hx, y);
    const std::uint16_t entry = static_cast<std::uint16_t>(vram[at] | (vram[at + 1] << 8));
    int px = (hx + (io16(r, 0x10) & 0x1FF)) & 7, py = (y + (io16(r, 0x12) & 0x1FF)) & 7;
    if (entry & 0x400) px = 7 - px;
    if (entry & 0x800) py = 7 - py;
    const std::uint8_t pair = vram[((io16(r, 0x08) >> 2) & 3) * 0x4000u + (entry & 0x3FFu) * 32u + py * 4 + (px >> 1)];
    const int index = (px & 1) ? pair >> 4 : pair & 15;
    return index ? ((entry >> 12) << 4) | index : 0;
}

// A side margin continues BG0 on native row y when the edge tile is solid, or
// when the edge pixel has the colour of a solid tile further along the row (a
// menu bar whose edge tiles carry a frame line).
bool side_fill(const std::uint8_t* r, const std::uint8_t* vram, int ex, int y, std::uint16_t* out) {
    if (solid_tile(r, vram, ex, y, out)) return true;
    const int color = bg0_color_at(r, vram, ex, y);
    if (!color) return false;
    for (int k = 1; k < 30; ++k) {
        const int hx = ex == 0 ? 8 * k : 239 - 8 * k;
        if (solid_tile(r, vram, hx, y, out) && bg0_color_at(r, vram, hx, y) == color) return true;
    }
    return false;
}

// For the top (y < 0) or bottom edge row: whether BG0 is opaque along all of
// it, and a solid tile on it to continue with. Computed once per frame.
bool covering_edge_row(const std::uint8_t* r, const std::uint8_t* vram, int y, std::uint16_t* fill) {
    struct Edge { std::uint64_t frame = ~std::uint64_t{0}; bool covers = false; std::uint16_t fill = 0; };
    static Edge s_edge[2];
    gba::GbaPpu* ppu = gbarecomp::active_ppu();
    const std::uint64_t frame = ppu ? ppu->frame_count() : 0;
    Edge& e = s_edge[y < 0 ? 0 : 1];
    if (e.frame != frame) {
        e = {frame, false, 0};
        const int ey = y < 0 ? 0 : 159, vofs = io16(r, 0x12) & 0x1FF;
        const std::uint32_t chars = ((io16(r, 0x08) >> 2) & 3) * 0x4000u;
        bool covers = true, found = false;
        for (int hx = 0; hx < 240 && covers; ++hx) {
            const std::uint32_t at = bg0_entry_at(r, hx, ey);
            const std::uint16_t entry = static_cast<std::uint16_t>(vram[at] | (vram[at + 1] << 8));
            int px = (hx + (io16(r, 0x10) & 0x1FF)) & 7, py = (ey + vofs) & 7;
            if (entry & 0x400) px = 7 - px;
            if (entry & 0x800) py = 7 - py;
            const std::uint8_t pair = vram[chars + (entry & 0x3FFu) * 32u + py * 4 + (px >> 1)];
            covers = ((px & 1) ? pair >> 4 : pair & 15) != 0;
            if (covers && !found && (hx & 7) == 0) found = solid_tile(r, vram, hx, ey, &e.fill);
        }
        e.covers = covers && found;
    }
    *fill = e.fill;
    return e.covers;
}

int extend_solid_edge(const std::uint8_t* r, int hx, int y, std::uint16_t* out) {
    gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus) return gba::kWsTilemapUnavailable;
    const std::uint8_t* vram = bus->vram_ptr();
    const int cnt = io16(r, 0x08);
    if ((cnt >> 14) != 0 || (cnt & 0x80)) return gba::kWsTilemapUnavailable;
    const int ex = std::clamp(hx, 0, 239), ey = std::clamp(y, 0, 159);
    if (y < 0 || y >= 160) {
        std::uint16_t fill;
        if (!covering_edge_row(r, vram, y, &fill)) return gba::kWsTilemapUnavailable;
        if (hx >= 0 && hx < 240) {
            *out = fill;
            return gba::kWsTilemapReplace;
        }
    }
    return side_fill(r, vram, ex, ey, out) ? gba::kWsTilemapReplace : gba::kWsTilemapUnavailable;
}

// Battles: BG2/BG3 are tiling background patterns (with per-line distortion),
// so the 256px ring's wrap is their true continuation; BG0/BG1 are the text
// box and HUD and never extend. WIN0 hides the background behind the text and
// HUD rows natively, so margins follow the same rows.
bool battle_layout(const std::uint8_t* r) {
    const int dispcnt = io16(r, 0);
    if ((dispcnt & 7) != 0 || (dispcnt & 0x0C00) != 0x0C00) return false;
    return ((io16(r, 0x0C) >> 8) & 31) == 29 && ((io16(r, 0x0E) >> 8) & 31) == 28 &&
           (io16(r, 0x0C) >> 14) == 0 && (io16(r, 0x0E) >> 14) == 0;
}

int provide_battle(const std::uint8_t* r, int bg, int y) {
    if (bg != 2 && bg != 3) return gba::kWsTilemapUnavailable;
    const int dispcnt = io16(r, 0);
    if (dispcnt & 0x2000) {  // WIN0 spans the native width; extend it across the margins
        const int v = io16(r, 0x44), top = v >> 8, bottom = v & 0xFF;
        const bool inside = top <= bottom ? (y >= top && y < bottom) : (y >= top || y < bottom);
        const int mask = inside ? io16(r, 0x48) : io16(r, 0x4A);
        if (!(mask & (1 << bg))) return gba::kWsTilemapUnavailable;
    }
    return gba::kWsTilemapKeepWrapped;
}

// Weather on BG1 is alpha-blended over the scenery. When it is a detached
// layer exactly 256px wide (the rain of map 11) the BG ring holds all of it, so
// the ring's wrap is its true continuation even where the model can't match it
// (rain moves on its own). Anything else BG1 shows that the model can't explain
// (a text box) stays blank.
bool blended_overlay(const std::uint8_t* r) {
    const int bldcnt = io16(r, 0x50);
    return ((bldcnt >> 6) & 3) == 1 && (bldcnt & 0x02);
}

// What a margin pixel of one field layer shows, from that row's cameras.
struct RowView {
    std::uint32_t frame = ~0u;
    std::uint32_t gen = 0;
    int y = 0;
    bool hidden[4] = {};                   // per BG, outside every window it shows in
    int black_left = 0, black_right = 0;   // margin columns the centring black covers
    int room_cx[4] = {}, room_cy[4] = {};  // per BG, the camera its rooms are measured in
    int org_x[4] = {}, org_y[4] = {};      // per BG, the world position of its top-left
};

int provide_field(const std::uint8_t* r, const RowView& v, int bg, int hx, int y, std::uint16_t* out) {
    if (bg == 0) return extend_solid_edge(r, hx, y, out);
    if (bg == 1 && !g.bg_ok[1] && g.detached && g.lw == 16 && blended_overlay(r))
        return gba::kWsTilemapKeepWrapped;
    if (!g.bg_ok[bg] || !in_room(v.room_cx[bg], v.room_cy[bg], hx, y)) return gba::kWsTilemapUnavailable;
    const int e = entry_at(g, bg - 1, (v.org_x[bg] + hx) >> 3, (v.org_y[bg] + y) >> 3);
    if (e <= 0) return gba::kWsTilemapUnavailable;
    *out = static_cast<std::uint16_t>(e);
    return gba::kWsTilemapReplace;
}

// The PPU asks for every margin pixel of every layer, a row at a time. Along a
// row the cameras are fixed, and the answer changes only where the pixel enters
// another 8px tile of the layer or another 16px room cell (in_room depends on
// nothing else once the row is fixed), so both are kept.
int provide(int bg, int hx, int y, std::uint16_t* out) {
    if (g_battle) {
        const std::uint8_t* r = io();
        return r ? provide_battle(r, bg, y) : gba::kWsTilemapUnavailable;
    }
    if (!g.ok || bg > 3) return gba::kWsTilemapUnavailable;
    static RowView s_row;
    if (s_row.frame != g_frame || s_row.gen != g_field_gen || s_row.y != y) {
        const std::uint8_t* r = io();
        if (!r) return gba::kWsTilemapUnavailable;
        s_row.frame = g_frame;
        s_row.gen = g_field_gen;
        s_row.y = y;
        for (int b = 0; b <= 3; ++b) s_row.hidden[b] = hidden_outside_windows(r, 1 << b);
        int cx, cy;
        camera(r, 2, &cx, &cy);
        const int black = black_width(cx, cy, y);
        s_row.black_left = -g_margin_left + black;
        s_row.black_right = 240 + g_margin_right - black;
        for (int b = 1; b <= 3; ++b) {
            camera(r, b == 1 && g.detached ? 2 : b, &s_row.room_cx[b], &s_row.room_cy[b]);  // rooms are in scenery coordinates
            origin(g, r, b, &s_row.org_x[b], &s_row.org_y[b]);
        }
    }
    if (s_row.hidden[bg] || hx < s_row.black_left || hx >= s_row.black_right)
        return gba::kWsTilemapUnavailable;
    // BG0 answers per side (and, above and below the screen, once for the rest).
    const int cell = bg == 0 ? (hx < 0 ? -1 : hx >= 240 ? -2 : -3) : (s_row.room_cx[bg] + hx) >> 4;
    const int tile = bg == 0 ? 0 : (s_row.org_x[bg] + hx) >> 3;
    struct Memo {
        std::uint32_t frame = ~0u;
        std::uint32_t gen = 0;
        int y = 0, cell = 0, tile = 0, action = 0;
        std::uint16_t entry = 0;
    };
    static Memo s_memo[4];
    Memo& m = s_memo[bg];
    if (m.frame != g_frame || m.gen != g_field_gen || m.y != y || m.cell != cell || m.tile != tile) {
        const std::uint8_t* r = io();
        if (!r) return gba::kWsTilemapUnavailable;
        m = {g_frame, g_field_gen, y, cell, tile, 0, 0};
        m.action = provide_field(r, s_row, bg, hx, y, &m.entry);
    }
    *out = m.entry;
    return m.action;
}

void frame_start();

}  // namespace

void install() {
    gba::g_ws_tilemap_provider = &provide;
    gba::g_ws_obj_attr_x_provider = &place_obj;
    gba::g_ws_obj_attr_y_provider = &place_obj_y;
    gba::g_ws_authored_margin_layers = 1;
    gba::g_ws_pillarbox = 1;  // until the field validates
    gba::g_ws_frame_start = &frame_start;
}

namespace {
// The game's camera stops at the map's edges with the native 240x160 inside the
// map. The widened view stops there too: near an edge the native picture slides
// towards it inside the canvas instead of the margins showing the void past the
// map (which then took matching black on the other side, a black frame in the
// corners). Looping maps, battles and other screens keep it centred.
void place_native_view(gba::GbaPpu* ppu) {
    const int spare_x = static_cast<int>(ppu->render_width()) - 240;
    const int spare_y = static_cast<int>(ppu->render_height()) - 160;
    int left = static_cast<int>(ppu->view_extra_left()), top = static_cast<int>(ppu->view_extra_top());
    const std::uint8_t* r = io();
    const bool placed = g.ok && r;
    if (placed) {
        int cx, cy;
        camera(r, 2, &cx, &cy);
        const int loops = ew8(kMapLoops), map_w = 16 * g.width, map_h = 16 * g.height;
        if (!(loops & 0x20) && map_w >= 240 + spare_x) left = std::clamp(left, cx + 240 + spare_x - map_w, cx);
        if (!(loops & 0x10) && map_h >= 160 + spare_y) top = std::clamp(top, cy + 160 + spare_y - map_h, cy);
        left = std::clamp(left, 0, spare_x);
        top = std::clamp(top, 0, spare_y);
    }
    gba::g_ws_native_view_left = placed ? left : -1;
    gba::g_ws_native_view_top = placed ? top : -1;
    g_margin_left = left;
    g_margin_right = spare_x - left;
    g_margin_top = top;
    g_margin_bottom = spare_y - top;
}

// Each rendered frame: what it shows is settled by now (the game wrote its
// scroll during vblank), so the placement matches the scroll it renders with.
void frame_start() {
    ++g_frame;
    if (gba::GbaPpu* ppu = gbarecomp::active_ppu()) place_native_view(ppu);
}

void set_detached(Field& f) {
    f.detached = f.layer[0] && (f.loop_x || f.loop_y || f.lw != f.width || f.lh != f.height);
}
}  // namespace

void tick() {
    // Only for a widened view; at 240px the PPU path stays untouched. run_game()
    // clears every widescreen hook at startup, so (re)install from here.
    gba::GbaPpu* ppu = gbarecomp::active_ppu();
    if (!ppu || ppu->render_width() <= 240) return;
    if (gba::g_ws_tilemap_provider != &provide) install();
    // Hooks start disabled and a launch disables them again; at 240px they stay off.
    for (const char* id : kCullHooks)
        if (!gba_mod_function_hook_enabled(id)) gba_mod_set_function_hook_enabled(id, 1);
    const Uint32 now = SDL_GetTicks();
    if (now < g_next) return;
    g_next = now + 50;
    gba::GbaBus* bus = gbarecomp::active_bus();
    const std::uint8_t* r = io();
    if (!bus || !r) return;
    const std::uint8_t* vram = bus->vram_ptr();

    Field f;
    f.map = ew16(kMapId);
    for (int L = 0; L < 3; ++L) f.layer[L] = ew32(kLayerPtrs + 4u * L);
    f.tiles = ew32(kTileDataPtr);
    {
        const int info = ew8(kLayerInfo), loops = ew8(kMapLoops);
        const bool own_loop = ew8(kLayerScroll + 0x35) & 0x10;
        f.lw = 16 * ((info & 7) + 1);
        f.lh = 16 * (((info >> 3) & 7) + 1);
        f.loop_x = (info >> 6) == 3 || own_loop || (loops & 0x20);
        f.loop_y = (ew8(kLayerInfo + 1) & 3) == 3 || own_loop || (loops & 0x10);
    }
    // Layers are consecutive equal-size buffers; a map may leave layer 0 out (null).
    int cells = 0;
    for (int L = 0; L < 2; ++L)
        if (f.layer[L] && f.layer[L + 1] > f.layer[L]) cells = static_cast<int>((f.layer[L + 1] - f.layer[L]) / 2);
    const bool sane = field_layout(r) && in_ewram(f.layer[1]) && in_ewram(f.layer[2]) &&
                      (!f.layer[0] || in_ewram(f.layer[0])) && in_ewram(f.tiles) && cells > 0;
    bool ok = false;
    if (sane) {
        int samples = 0;
        const bool same = g.width && g.map == f.map && g.layer[0] == f.layer[0] && g.layer[1] == f.layer[1];
        if (same) {  // keep the known width while it still reproduces the screen
            f.width = g.width;
            f.height = g.height;
            set_detached(f);
            ok = score(f, r, vram, 0.85, &samples) && samples >= 60;
        }
        if (!ok) {  // new map (or lost it): find the width that reproduces the screen
            for (int w = 8; w <= 256 && !ok; ++w) {
                if (cells % w) continue;
                Field t = f;
                t.width = w;
                t.height = cells / w;
                set_detached(t);
                if (score(t, r, vram, 0.9, &samples) && samples >= 150) { f = t; ok = true; }
            }
        }
    }
    // A validated field can fail one check mid-scroll; keep it for a couple of
    // ticks so the margins don't flash black, but still drop it on a real change.
    const bool same_field = g.ok && g.map == f.map && g.layer[0] == f.layer[0] && g.layer[1] == f.layer[1];
    if (ok) {
        g_misses = 0;
    } else if (sane && same_field && ++g_misses < 3) {
        f = g;
        ok = true;
    }
    f.ok = ok;
    if (!ok) f.width = f.height = 0;
    g_battle = !ok && battle_layout(r);
    if (ok != g.ok || f.map != g.map || f.width != g.width)
        SDL_Log("m3-widescreen: map %d field=%d sane=%d width=%d layer0=%s%dx%d", f.map, ok, sane, f.width,
                f.detached ? "detached " : "", f.lw, f.lh);
    g = f;
    ++g_field_gen;
    gba::g_ws_pillarbox = ok || g_battle ? 0 : 1;
}

}  // namespace m3widescreen
