/* ==========================================================================
 *  ppu.c -- scanline renderer, DMG + CGB, HDMA support
 *
 *  Model: one scanline is 456 T-cycles split into
 *      mode 2 (OAM search)   : 80 cycles
 *      mode 3 (pixel xfer)   : 172 + f(sprites on line) cycles
 *      mode 0 (H-blank)      : remainder
 *  and 10 V-blank lines (144..153) of 456 cycles each in mode 1.
 *
 *  gb_advance() asks the PPU for its next event boundary so STAT / LYC /
 *  V-blank interrupts land on the right cycle.
 * ========================================================================== */
#include "gb_string.h"
#include "gb_internal.h"

#define LCDC_BG_EN   0x01
#define LCDC_OBJ_EN  0x02
#define LCDC_OBJ_SZ  0x04
#define LCDC_BG_MAP  0x08
#define LCDC_TILESEL 0x10
#define LCDC_WIN_EN  0x20
#define LCDC_WIN_MAP 0x40
#define LCDC_ON      0x80

#define STAT_HBLANK_IRQ 0x08
#define STAT_VBLANK_IRQ 0x10
#define STAT_OAM_IRQ    0x20
#define STAT_LYC_IRQ    0x40

/* --- DMG (monochrome) palettes, XRGB8888.  Shade 3 = darkest. --------- */
static const uint32_t dmg_bg_default[4] = {
    0x00E8F8D0, 0x00A8C868, 0x00688838, 0x00283818
};
static const uint32_t dmg_ob_default[4] = {
    0x00000000, 0x00F8B888, 0x00C87848, 0x00703810
};

static uint32_t g_pal_bg[4], g_pal_ob0[4], g_pal_ob1[4];
static int      g_pal_override[3];

void gb_set_palette_override(const uint8_t *pal, int which)
{
    if (which < 0 || which > 2) return;
    if (!pal) { g_pal_override[which] = 0; return; }
    uint32_t out[4];
    for (int i = 0; i < 4; i++)
        out[i] = ((uint32_t)pal[i*4] << 16) | ((uint32_t)pal[i*4+1] << 8) |
                 (uint32_t)pal[i*4+2];
    if (which == 0) memcpy(g_pal_bg,  out, sizeof(out));
    if (which == 1) memcpy(g_pal_ob0, out, sizeof(out));
    if (which == 2) memcpy(g_pal_ob1, out, sizeof(out));
    g_pal_override[which] = 1;
}

static void reset_palettes(void)
{
    memcpy(g_pal_bg,  dmg_bg_default, sizeof(dmg_bg_default));
    memcpy(g_pal_ob0, dmg_ob_default, sizeof(dmg_ob_default));
    memcpy(g_pal_ob1, dmg_ob_default, sizeof(dmg_ob_default));
    g_pal_override[0] = g_pal_override[1] = g_pal_override[2] = 0;
}

/* --- CGB colour: 15 bit -> sRGB --------------------------------------- */

static inline uint8_t clamp255(int v) { return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v)); }

static const uint8_t vivid_curve[32] = {
      0,  10,  21,  31,  42,  52,  63,  73,
     84,  94, 105, 115, 126, 136, 147, 157,
    168, 178, 189, 199, 210, 220, 231, 241,
    252, 252, 253, 253, 254, 254, 255, 255
};

static inline uint32_t cgb_color(uint16_t c, int mode)
{
    int r = c & 0x1F, g = (c >> 5) & 0x1F, b = (c >> 10) & 0x1F;
    int R, G, B;

    if (mode == GB_COLOR_OFF) {
        R = (r << 3) | (r >> 2);
        G = (g << 3) | (g >> 2);
        B = (b << 3) | (b >> 2);
    } else if (mode == GB_COLOR_VIVID) {
        R = vivid_curve[r]; G = vivid_curve[g]; B = vivid_curve[b];
        int luma = (R * 54 + G * 183 + B * 19) >> 8;
        R = luma + ((R - luma) * 130 >> 7);
        G = luma + ((G - luma) * 130 >> 7);
        B = luma + ((B - luma) * 130 >> 7);
        R = clamp255(R); G = clamp255(G); B = clamp255(B);
    } else {
        /* GB_COLOR_CGB -- hardware-like: bit expansion plus a small amount
         * of cross-channel bleed that reproduces the CGB LCD's response.  */
        R = (r << 3) | (r >> 2);
        G = (g << 3) | (g >> 2);
        B = (b << 3) | (b >> 2);
        R = (R * 26 + G *  4 + B * 2) >> 5;
        G = (G * 24 + R *  4 + B * 4) >> 5;
        B = (B * 26 + R *  4 + G * 2) >> 5;
        R = clamp255(R); G = clamp255(G); B = clamp255(B);
    }
    return ((uint32_t)R << 16) | ((uint32_t)G << 8) | (uint32_t)B;
}

void gb_ppu_reset(gb_state_t *g)
{
    memset(&g->ppu, 0, sizeof(g->ppu));
    g->ppu.mode = 0;
    g->ppu.line_cycles = 0;
    g->ppu.mode3_len = 172;
    reset_palettes();
}

/* -------------------------------------------------------------- render  */

static uint8_t lb_idx[GB_LCD_W];  /* colour index 0..3                     */
static uint8_t lb_pal[GB_LCD_W];  /* palette number                        */
static uint8_t lb_obj[GB_LCD_W];  /* 1 = sprite pixel                      */

static void render_bg(gb_state_t *g, uint8_t ly, int *window_used)
{
    uint8_t lcdc = g->io[0x40];
    int cgb = g->cgb_mode;

    uint16_t map_base = (lcdc & LCDC_BG_MAP)  ? 0x1C00 : 0x1800;
    uint16_t win_base = (lcdc & LCDC_WIN_MAP) ? 0x1C00 : 0x1800;
    int unsigned_tiles = (lcdc & LCDC_TILESEL) ? 1 : 0;

    uint8_t scy = g->io[0x42], scx = g->io[0x43];
    uint8_t wy  = g->io[0x4A], wx  = g->io[0x4B];

    *window_used = 0;
    int win_on_line = (lcdc & LCDC_WIN_EN) && g->ppu.in_window && wx <= 166;

    if (!cgb && !(lcdc & LCDC_BG_EN)) {
        /* DMG: BG/window disabled -> shade 0 across the line */
        for (int x = 0; x < GB_LCD_W; x++) {
            lb_idx[x] = 0; lb_pal[x] = 0; lb_obj[x] = 0;
        }
        return;
    }

    uint8_t fy    = (uint8_t)((ly + scy) & 7);
    uint8_t row_y = (uint8_t)(((ly + scy) >> 3) & 31);

    for (int x = 0; x < GB_LCD_W; x++) {
        int use_win = win_on_line && (x + 7) >= (int)wx;
        uint16_t map;
        uint8_t tx, ty, row_in_tile;

        if (use_win) {
            uint8_t wly = (uint8_t)(g->ppu.win_line & 0xFF);
            tx = (uint8_t)(((x + 7 - (int)wx) >> 3) & 31);
            ty = (uint8_t)((wly >> 3) & 31);
            row_in_tile = (uint8_t)(wly & 7);
            map = (uint16_t)(win_base + ty * 32 + tx);
            *window_used = 1;
        } else {
            tx = (uint8_t)(((scx + x) >> 3) & 31);
            ty = row_y;
            row_in_tile = fy;
            map = (uint16_t)(map_base + ty * 32 + tx);
        }

        uint8_t  tile = g->vram[0][map];
        uint16_t attr = cgb ? g->vram[1][map] : 0;
        int      bank = cgb ? ((attr & 0x08) ? 1 : 0) : 0;

        uint16_t taddr;
        if (unsigned_tiles) taddr = (uint16_t)(tile * 16);
        else                taddr = (uint16_t)(0x1000 + (int16_t)(int8_t)tile * 16);

        int row = (attr & 0x40) ? (7 - row_in_tile) : row_in_tile;  /* Y flip */
        const uint8_t *p = &g->vram[bank][taddr + row * 2];
        uint8_t lo = p[0], hi = p[1];

        int bit = (attr & 0x20) ? (x & 7) : (7 - (x & 7));           /* X flip */
        int idx = ((lo >> bit) & 1) | (((hi >> bit) & 1) << 1);

        lb_idx[x] = (uint8_t)idx;
        lb_pal[x] = cgb ? (uint8_t)(attr & 7) : 0;
        lb_obj[x] = 0;
        /* attr bit 7 (BG-to-OAM priority) is folded into the sprite pass by
         * re-reading it, so we stash it in the high nibble of lb_pal.       */
        if (cgb && (attr & 0x80)) lb_pal[x] |= 0x80;
    }
}

static void render_obj(gb_state_t *g, uint8_t ly)
{
    uint8_t lcdc = g->io[0x40];
    if (!(lcdc & LCDC_OBJ_EN)) return;

    int tall = (lcdc & LCDC_OBJ_SZ) ? 16 : 8;
    int cgb  = g->cgb_mode;
    int bg_master_off = cgb && !(lcdc & LCDC_BG_EN);

    /* collect sprites intersecting this line, preserving OAM order */
    uint8_t order[40];
    int count = 0;
    for (int i = 0; i < 40; i++) {
        int sy = g->oam[i * 4] - 16;
        if (ly >= sy && ly < sy + tall) {
            order[count++] = (uint8_t)i;
            if (count == 40) break;
        }
    }

    /* sort by X (stable: ties keep OAM order, which is the CGB rule) */
    for (int i = 1; i < count; i++) {
        uint8_t v = order[i];
        uint8_t vx = g->oam[v * 4 + 1];
        int j = i - 1;
        while (j >= 0 && g->oam[order[j] * 4 + 1] > vx) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = v;
    }
    if (count > 10) count = 10;   /* hardware limit: 10 sprites per line */

    /* paint back-to-front so the front-most sprite wins ties */
    for (int n = count - 1; n >= 0; n--) {
        int i = order[n];
        int sy = g->oam[i * 4] - 16;
        int sx = g->oam[i * 4 + 1] - 8;
        uint8_t tile = g->oam[i * 4 + 2];
        uint8_t attr = g->oam[i * 4 + 3];

        if (tall == 16) tile &= 0xFE;
        int row = ly - sy;
        if (attr & 0x40) row = tall - 1 - row;      /* Y flip */

        int bank = cgb ? ((attr & 0x08) ? 1 : 0) : 0;
        uint16_t taddr = (uint16_t)(tile * 16 + row * 2);
        uint8_t lo = g->vram[bank][taddr];
        uint8_t hi = g->vram[bank][taddr + 1];

        uint8_t pal     = cgb ? (uint8_t)(attr & 7) : ((attr & 0x10) ? 1 : 0);
        int     bg_prio = (attr & 0x80) ? 1 : 0;
        int     xflip   = attr & 0x20;

        for (int px = 0; px < 8; px++) {
            int x = sx + px;
            if (x < 0 || x >= GB_LCD_W) continue;

            int bit = xflip ? px : (7 - px);
            int idx = ((lo >> bit) & 1) | (((hi >> bit) & 1) << 1);
            if (idx == 0) continue;                 /* colour 0 = transparent */

            if (lb_obj[x]) continue;                /* a nearer sprite owns it */

            if (bg_prio && !bg_master_off) {
                int bg_idx = lb_idx[x];
                int bg_behind = (lb_pal[x] & 0x80) ? 1 : 0;
                /* OBJ-behind-BG wins only over BG colour 0, unless the CGB
                 * BG attribute also asks for background priority.          */
                if (bg_idx != 0 || bg_behind) continue;
            }

            lb_idx[x] = (uint8_t)idx;
            lb_pal[x] = pal;
            lb_obj[x] = 1;
        }
    }
}

static void colorize_line(gb_state_t *g, uint8_t ly)
{
    uint32_t *fb = &g->fb[ly * GB_LCD_W];

    if (g->cgb_mode) {
        for (int x = 0; x < GB_LCD_W; x++) {
            int base = (lb_pal[x] & 7) * 8 + lb_idx[x] * 2;
            const uint8_t *pd = lb_obj[x] ? g->obpd : g->bgpd;
            uint16_t c = (uint16_t)(pd[base] | (pd[base + 1] << 8));
            fb[x] = cgb_color(c, g->color_mode);
        }
    } else {
        uint8_t bgp  = g->io[0x47];
        uint8_t obp0 = g->io[0x48], obp1 = g->io[0x49];
        for (int x = 0; x < GB_LCD_W; x++) {
            if (lb_obj[x]) {
                uint8_t p = lb_pal[x] ? obp1 : obp0;
                int shade = (p >> (lb_idx[x] * 2)) & 3;
                fb[x] = (lb_pal[x] ? g_pal_ob1 : g_pal_ob0)[shade];
            } else {
                int shade = (bgp >> (lb_idx[x] * 2)) & 3;
                fb[x] = g_pal_bg[shade];
            }
        }
    }
}

void gb_ppu_render_line(gb_state_t *g, uint8_t ly)
{
    if (ly >= GB_LCD_H) return;
    int window_used = 0;
    render_bg(g, ly, &window_used);
    render_obj(g, ly);
    colorize_line(g, ly);
    if (window_used) g->ppu.win_line++;
    g->ppu.line_rendered = 1;
}

/* ------------------------------------------------------------------ step */

/* cycles until the next PPU event (mode change or end of line) */
uint32_t gb_ppu_next_event(const gb_state_t *g)
{
    if (!(g->io[0x40] & LCDC_ON)) return 456;
    uint32_t lc = g->ppu.line_cycles;
    if (g->ppu.mode == 2) return (80 > lc) ? (80 - lc) : 1;
    if (g->ppu.mode == 3) {
        uint32_t end = 80 + g->ppu.mode3_len;
        return (end > lc) ? (end - lc) : 1;
    }
    return 456 - lc;   /* mode 0 or mode 1 */
}

static void stat_update(gb_state_t *g, uint32_t mode)
{
    g->ppu.mode = mode;
    g->io[0x41] = (uint8_t)((g->io[0x41] & 0xFC) | (mode & 3));

    uint8_t stat = g->io[0x41];
    int irq = 0;
    if (mode == 0 && (stat & STAT_HBLANK_IRQ)) irq = 1;
    if (mode == 1 && (stat & STAT_VBLANK_IRQ)) irq = 1;
    if (mode == 2 && (stat & STAT_OAM_IRQ))    irq = 1;
    if ((stat & STAT_LYC_IRQ) && g->io[0x44] == g->io[0x45]) irq = 1;

    /* edge triggered: only a 0 -> 1 transition raises IF bit 1 */
    if (irq && !g->ppu.prev_stat_irq) g->io[0x0F] |= 0x02;
    g->ppu.prev_stat_irq = (uint8_t)irq;
}

static void lyc_update(gb_state_t *g)
{
    uint8_t stat = g->io[0x41];
    if (g->io[0x44] == g->io[0x45]) {
        g->io[0x41] |= 0x04;
        if (stat & STAT_LYC_IRQ) {
            if (!g->ppu.prev_stat_irq) g->io[0x0F] |= 0x02;
            g->ppu.prev_stat_irq = 1;
        }
    } else {
        g->io[0x41] &= (uint8_t)~0x04;
    }
}

static void begin_line(gb_state_t *g, uint8_t ly)
{
    g->ppu.line_cycles = 0;
    g->ppu.line_rendered = 0;   /* re-arm: mode 3 renders this line once */
    g->io[0x44] = ly;

    /* window latch: WY==LY is compared once at the start of the line */
    if ((g->io[0x40] & LCDC_WIN_EN) && ly == g->io[0x4A]) g->ppu.in_window = 1;

    lyc_update(g);

    if (ly == GB_LCD_H) {
        g->io[0x0F] |= 0x01;          /* V-blank interrupt request */
        g->ppu.frame_done = 1;
        stat_update(g, 1);
    } else {
        /* pre-compute this line's mode-3 length from its sprite load */
        int tall = (g->io[0x40] & LCDC_OBJ_SZ) ? 16 : 8;
        int spr = 0;
        for (int i = 0; i < 40; i++) {
            int sy = g->oam[i * 4] - 16;
            if (ly >= sy && ly < sy + tall) if (++spr >= 10) break;
        }
        uint32_t len = 172 + (uint32_t)spr * 6;
        if (len > 280) len = 280;
        g->ppu.mode3_len = len;
        stat_update(g, 2);
    }
}

void gb_ppu_step(gb_state_t *g, uint32_t cycles)
{
    if (!(g->io[0x40] & LCDC_ON)) {
        g->ppu.mode = 0;
        g->ppu.line_cycles = 0;
        g->io[0x41] &= 0xFC;
        g->ppu.prev_stat_irq = 0;
        return;
    }

    while (cycles) {
        uint32_t n = cycles;
        uint32_t to_event = gb_ppu_next_event(g);
        if (n > to_event) n = to_event;
        g->ppu.line_cycles += n;
        cycles -= n;

        /* --- HDMA -------------------------------------------------- */
        if (g->ppu.hdma_active && g->ppu.hdma_left) {
            int do_burst = !g->ppu.hdma_hblank;
            int do_hblank = g->ppu.hdma_hblank && g->ppu.mode == 0;
            if (do_burst || do_hblank) {
                uint32_t budget = do_hblank ? 16 : n;
                while (g->ppu.hdma_left && budget--) {
                    g->vram[1][g->ppu.hdma_dst & 0x1FFF] = gb_read8(g, g->ppu.hdma_src);
                    g->ppu.hdma_src = (uint16_t)(g->ppu.hdma_src + 1);
                    g->ppu.hdma_dst = (uint16_t)((g->ppu.hdma_dst + 1) & 0x1FFF);
                    g->ppu.hdma_left--;
                }
                if (!g->ppu.hdma_left) g->ppu.hdma_active = 0;
            }
        }

        if (cycles && g->ppu.line_cycles < 456) continue;

        /* --- mode transitions -------------------------------------- */
        uint8_t ly = g->io[0x44];
        if (g->ppu.mode == 2 && g->ppu.line_cycles >= 80) {
            stat_update(g, 3);
            if (cycles) continue;
        }
        if (g->ppu.mode == 3 && g->ppu.line_cycles >= 80 + g->ppu.mode3_len) {
            if (!g->ppu.line_rendered) gb_ppu_render_line(g, ly);
            stat_update(g, 0);
            if (cycles) continue;
        }
        if (g->ppu.line_cycles >= 456) {
            uint8_t next = (uint8_t)(ly + 1);
            if (next >= 154) { next = 0; g->ppu.in_window = 0; g->ppu.win_line = 0; }
            begin_line(g, next);
        }
    }
}
