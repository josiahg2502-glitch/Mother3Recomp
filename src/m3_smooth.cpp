#include "m3_smooth.h"

#include <SDL.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "gba_ppu.h"

namespace m3smooth {
namespace {

using Line = gba::GbaPpu::MotionLine;

struct Obj {
    bool on = false;
    int x = 0, y = 0, shape = 0, size = 0, palette = 0;
};

Obj read_obj(const std::uint8_t* oam, int i) {
    const std::uint8_t* e = oam + 8 * i;
    const int a0 = e[0] | (e[1] << 8), a1 = e[2] | (e[3] << 8), a2 = e[4] | (e[5] << 8);
    Obj o;
    if (!(a0 & 0x100) && (a0 & 0x200)) return o;  // hidden
    o.shape = (a0 >> 14) & 3;
    if (o.shape == 3) return o;
    o.on = true;
    o.size = (a1 >> 14) & 3;
    o.palette = (a2 >> 12) & 15;
    o.x = a1 & 0x1FF;
    if (o.x & 0x100) o.x -= 0x200;
    o.y = a0 & 0xFF;
    if (o.y >= 160) o.y -= 256;
    return o;
}

int wrap(int d, int m) {
    d %= m;
    if (d >= m / 2) d -= m;
    if (d < -m / 2) d += m;
    return d;
}

// Scroll change between frames. MOTHER 3 rebases its 256px BG ring every
// 16px, so the registers only carry the camera's low bits: read big jumps
// modulo 16. Anything still 8px or more is a cut, not motion.
int scroll_step(int now, int before) {
    int d = wrap(now - before, 512);
    if (d >= 8 || d <= -8) d = wrap(d, 16);
    return (d >= 8 || d <= -8) ? 0 : d;
}

bool affine_bg(int dispcnt, int bg) {
    const int mode = dispcnt & 7;
    return (mode == 1 && bg == 2) || (mode == 2 && bg >= 2);
}

struct State {
    bool have_prev = false;
    std::uint64_t frame = 0;
    std::uint32_t w = 0, h = 0;
    std::array<Line, 160> lines{};
    std::array<std::uint8_t, 1024> oam{};
    // Display check, refreshed every couple of seconds (the window can move).
    bool fast_display = false;
    int display_check_in = 0;
    int display_hz = -1;
    bool always = false;  // Frame rate "Smooth (always)": skip the display check
    std::vector<std::uint32_t> depth;
    // Screen velocity (native px per frame) of each BG layer on each line, and
    // of each sprite, from the previous frame to frame `frame`.
    int bg_vx[160][4] = {}, bg_vy[160][4] = {};
    int obj_vx[128] = {}, obj_vy[128] = {};
};
State g;

// The game's own window (the launcher's is gone by now), so the refresh rate
// is read from the display it is actually on, not whichever window has focus.
SDL_Window* game_window() {
    for (Uint32 id = 1; id < 64; ++id)
        if (SDL_Window* w = SDL_GetWindowFromID(id); w && !(SDL_GetWindowFlags(w) & SDL_WINDOW_HIDDEN)) return w;
    return SDL_GetKeyboardFocus();
}

bool display_is_fast() {
    if (--g.display_check_in > 0) return g.fast_display;
    g.display_check_in = 120;
    if (const char* force = std::getenv("M3_SMOOTH_FORCE"); g.always || (force && *force == '1'))
        return g.fast_display = true;
    SDL_Window* win = game_window();
    const int index = win ? SDL_GetWindowDisplayIndex(win) : 0;
    SDL_DisplayMode mode{};
    const int hz = SDL_GetCurrentDisplayMode(index < 0 ? 0 : index, &mode) == 0 ? mode.refresh_rate : 0;
    const bool fast = hz >= 100 || hz == 0;  // 0 = the driver doesn't say; the player chose 120
    if (fast != g.fast_display || g.display_hz != hz) {
        std::fprintf(stderr, "m3-smooth: display %d reports %d Hz -> smooth motion %s\n", index, hz, fast ? "on" : "off");
        std::fflush(stderr);
    }
    g.display_hz = hz;
    return g.fast_display = fast;
}

// Test hook: M3_SMOOTH_DUMP=dir saves in-between/real frame pairs (both at 2x)
// for the 6 frames starting at call M3_SMOOTH_DUMP_AT (default 120).
void dump(const std::uint8_t* mid, std::uint32_t W, std::uint32_t H, const std::uint8_t* rgb,
          std::uint32_t w, std::uint32_t h) {
    static const char* dir = std::getenv("M3_SMOOTH_DUMP");
    static const int at = std::getenv("M3_SMOOTH_DUMP_AT") ? std::atoi(std::getenv("M3_SMOOTH_DUMP_AT")) : 120;
    static int calls = 0;
    const int k = calls++ - at;
    if (!dir || k < 0 || k >= 6) return;
    std::vector<std::uint8_t> real(static_cast<std::size_t>(W) * H * 3);
    for (std::uint32_t Y = 0; Y < H; ++Y)
        for (std::uint32_t X = 0; X < W; ++X) std::memcpy(&real[3 * (Y * W + X)], rgb + 3 * ((Y / 2) * w + X / 2), 3);
    const std::uint8_t* frames[2] = {mid, real.data()};
    for (int f = 0; f < 2; ++f) {
        SDL_Surface* s = SDL_CreateRGBSurfaceWithFormatFrom(const_cast<std::uint8_t*>(frames[f]), W, H, 24, W * 3,
                                                            SDL_PIXELFORMAT_RGB24);
        if (!s) continue;
        char path[512];
        std::snprintf(path, sizeof path, "%s/%s_%d.bmp", dir, f ? "real" : "mid", k);
        SDL_SaveBMP(s, path);
        SDL_FreeSurface(s);
    }
}

// Measures this frame's motion once; the engine draws several moments of it.
void measure(const gba::GbaPpu* ppu, const Line* lines, std::uint32_t w, std::uint32_t h) {
    const std::uint64_t frame = ppu->frame_count();
    if (g.have_prev && frame == g.frame && w == g.w && h == g.h) return;
    const bool consecutive = g.have_prev && frame == g.frame + 1 && w == g.w && h == g.h;
    auto& bg_vx = g.bg_vx;
    auto& bg_vy = g.bg_vy;
    auto& obj_vx = g.obj_vx;
    auto& obj_vy = g.obj_vy;
    std::memset(bg_vx, 0, sizeof bg_vx);
    std::memset(bg_vy, 0, sizeof bg_vy);
    std::memset(obj_vx, 0, sizeof obj_vx);
    std::memset(obj_vy, 0, sizeof obj_vy);
    if (consecutive) {
        for (int y = 0; y < 160; ++y)
            for (int bg = 0; bg < 4; ++bg) {
                const Line &a = lines[y], &b = g.lines[y];
                if (a.bgcnt[bg] != b.bgcnt[bg] || (a.dispcnt & 7) != (b.dispcnt & 7) || affine_bg(a.dispcnt, bg))
                    continue;
                bg_vx[y][bg] = -scroll_step(a.hofs[bg], b.hofs[bg]);
                bg_vy[y][bg] = -scroll_step(a.vofs[bg], b.vofs[bg]);
            }
        const std::uint8_t* oam = ppu->latched_oam();
        Obj before[128];
        for (int i = 0; i < 128; ++i) before[i] = read_obj(g.oam.data(), i);
        for (int i = 0; i < 128; ++i) {
            const Obj now = read_obj(oam, i);
            if (!now.on) continue;
            // OAM slots get reused and re-sorted, so match the nearest sprite of
            // the same shape and palette, preferring the same slot.
            int best = -1, best_cost = 17;
            for (int j = 0; j < 128; ++j) {
                const Obj& o = before[j];
                if (!o.on || o.shape != now.shape || o.size != now.size || o.palette != now.palette) continue;
                const int dx = now.x - o.x, dy = now.y - o.y;
                if (dx > 8 || dx < -8 || dy > 8 || dy < -8) continue;
                const int cost = std::abs(dx) + std::abs(dy) + (j == i ? 0 : 1);
                if (cost < best_cost) best_cost = cost, best = j;
            }
            if (best >= 0) obj_vx[i] = now.x - before[best].x, obj_vy[i] = now.y - before[best].y;
        }
    }
    g.have_prev = true;
    g.frame = frame;
    g.w = w;
    g.h = h;
    std::copy_n(lines, 160, g.lines.begin());
    std::memcpy(g.oam.data(), ppu->latched_oam(), g.oam.size());
}

int interpolate(const gba::GbaPpu* ppu, const std::uint8_t* rgb, std::uint32_t w, std::uint32_t h,
                std::uint8_t* out, float t) {
    const std::uint16_t* tags = ppu->latched_tags();
    const Line* lines = ppu->latched_motion_lines();
    if (!tags || !lines || !display_is_fast()) {
        g.have_prev = false;
        return 0;
    }
    measure(ppu, lines, w, h);
    const int top = static_cast<int>(ppu->view_extra_top());

    // Each native pixel moves back by the part of its motion still to come,
    // on the 2x grid so half-pixel steps show.
    const float back = 2.0f * (1.0f - std::clamp(t, 0.0f, 1.0f));
    auto step = [back](int v) { return static_cast<int>(std::lround(back * static_cast<float>(v))); };
    int bg_dx[160][4], bg_dy[160][4], obj_dx[128], obj_dy[128];
    for (int y = 0; y < 160; ++y)
        for (int bg = 0; bg < 4; ++bg) bg_dx[y][bg] = step(g.bg_vx[y][bg]), bg_dy[y][bg] = step(g.bg_vy[y][bg]);
    for (int i = 0; i < 128; ++i) obj_dx[i] = step(g.obj_vx[i]), obj_dy[i] = step(g.obj_vy[i]);

    // Each native pixel lands as a 2x2 block; the front-most layer wins where blocks overlap.
    const std::uint32_t W = 2 * w, H = 2 * h;
    constexpr std::uint32_t kHole = 0x10000;
    g.depth.assign(static_cast<std::size_t>(W) * H, kHole);
    for (std::uint32_t y = 0; y < h; ++y) {
        const int ny = static_cast<int>(y) - top;
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::uint16_t tag = tags[y * w + x];
            int dx = 0, dy = 0;
            if (tag < 0xFFFE) {
                const int slot = tag & 255;
                if (slot < 128) dx = obj_dx[slot], dy = obj_dy[slot];
                else if (slot < 132 && ny >= 0 && ny < 160) dx = bg_dx[ny][slot - 128], dy = bg_dy[ny][slot - 128];
            }
            const std::uint8_t* c = rgb + 3 * (y * w + x);
            const int X0 = 2 * static_cast<int>(x) - dx, Y0 = 2 * static_cast<int>(y) - dy;
            for (int j = 0; j < 2; ++j) {
                const int Y = Y0 + j;
                if (Y < 0 || Y >= static_cast<int>(H)) continue;
                for (int i = 0; i < 2; ++i) {
                    const int X = X0 + i;
                    if (X < 0 || X >= static_cast<int>(W)) continue;
                    const std::size_t at = static_cast<std::size_t>(Y) * W + X;
                    if (tag >= g.depth[at]) continue;
                    g.depth[at] = tag;
                    std::memcpy(out + 3 * at, c, 3);
                }
            }
        }
    }
    // Gaps open where something moved away; fill them from the farther of the
    // nearest neighbours on the row (the background it uncovered), or the
    // unmoved frame when the whole row around is empty.
    for (std::uint32_t Y = 0; Y < H; ++Y) {
        std::uint32_t* d = g.depth.data() + static_cast<std::size_t>(Y) * W;
        std::uint8_t* row = out + static_cast<std::size_t>(Y) * W * 3;
        for (std::uint32_t X = 0; X < W;) {
            if (d[X] != kHole) { ++X; continue; }
            std::uint32_t end = X;
            while (end < W && d[end] == kHole) ++end;
            const bool has_l = X > 0, has_r = end < W;
            const std::uint8_t* fill = nullptr;
            if (has_l && has_r) fill = row + 3 * (d[X - 1] >= d[end] ? X - 1 : end);
            else if (has_l) fill = row + 3 * (X - 1);
            else if (has_r) fill = row + 3 * end;
            for (std::uint32_t k = X; k < end; ++k)
                std::memcpy(row + 3 * k, fill ? fill : rgb + 3 * ((Y / 2) * w + k / 2), 3);
            X = end;
        }
    }

    dump(out, W, H, rgb, w, h);
    return 1;
}

}  // namespace

void enable(int level) {
    const bool on = level > 0;
    gba::g_ppu_record_motion = on ? 1 : 0;
    gba::g_present_interpolator = on ? &interpolate : nullptr;
    g = State{};
    g.always = level == 2;
}

}  // namespace m3smooth
