/* ==========================================================================
 *  api.c -- frame pacing, options, cheats and the debug/test hooks
 * ========================================================================== */
#include "gb_string.h"
#include "gb_internal.h"

/* ---------------------------------------------------------------- cheats */

typedef struct {
    uint16_t addr;
    uint8_t  value;
    uint8_t  type;       /* 0 = gameshark, 1 = raw */
    uint8_t  enabled;
    uint8_t  pad;
} gb_cheat_t;

#define MAX_CHEATS 64
static gb_cheat_t g_cheats[MAX_CHEATS];
static int        g_cheat_count_active = 0;

/* --------------------------------------------------------------- frames  */

void gb_set_speed_multiplier(int pct)
{
    if (pct < 10)  pct = 10;
    if (pct > 800) pct = 800;
    g_gb.speed_target = (uint32_t)pct;
}

int gb_run_frame(void)
{
    gb_state_t *g = &g_gb;
    if (!g->loaded) return 0;

    g->ppu.frame_done = 0;
    g->cycles_frame = 0;

    uint32_t target = GB_CYCLES_PER_FRAME * g->speed_target / 100u;
    if (target < GB_CYCLES_PER_FRAME / 4u) target = GB_CYCLES_PER_FRAME / 4u;
    uint32_t hard_cap = target * 6u;      /* runaway guard */

    while (!g->ppu.frame_done) {
        int cyc = gb_cpu_step(g);
        if (cyc <= 0) cyc = 4;
        gb_advance(g, (uint32_t)cyc);
        g->cycles_frame += (uint32_t)cyc;

        /* With the LCD switched off the PPU never raises frame_done, so
         * fall back to the cycle budget (this is what the boot sequence of
         * some homebrew does). */
        if (!(g->io[0x40] & 0x80) && g->cycles_frame >= target) break;
        if (g->cycles_frame >= hard_cap) break;
    }

    g->frames++;

    /* RTC: fold elapsed wall time into the cart clock */
    if (g->rtc.present && g->rtc.last_unix) gb_rtc_tick(g, 0);

    /* GameShark style cheats are re-applied once per frame */
    if (g_cheat_count_active) {
        for (int i = 0; i < g_cheat_count_active; i++) {
            if (g_cheats[i].enabled)
                gb_write8(g, g_cheats[i].addr, g_cheats[i].value);
        }
    }
    return 1;
}

/* -------------------------------------------------------------- options  */

void gb_set_color_correction(int mode) { g_gb.color_mode = (uint8_t)mode; }
void gb_set_lcd_grid(int on)            { g_gb.io[0x00] = g_gb.io[0x00]; (void)on; }

void gb_set_rtc_now(uint64_t unix_seconds)
{
    g_gb.rtc.last_unix = unix_seconds;
}

int gb_has_rtc(void) { return g_gb.rtc.present; }

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int gb_add_cheat(const char *code)
{
    if (!code) return -1;
    if (g_cheat_count_active >= MAX_CHEATS) return -2;

    /* strip separators, collect up to 8 hex digits */
    uint8_t digits[8];
    int n = 0;
    for (const char *p = code; *p && n < 8; p++) {
        if (*p == '-' || *p == ':' || *p == ' ' || *p == '+') continue;
        int v = hexval(*p);
        if (v < 0) return -3;
        digits[n++] = (uint8_t)v;
    }
    if (n != 8) return -4;

    /* GameShark (GB) layout: TT BB LL HH  ->  value BB at address HHLL */
    uint16_t addr = (uint16_t)((digits[6] << 4) | digits[7]);
    addr = (uint16_t)(addr << 8);
    addr |= (uint16_t)((digits[4] << 4) | digits[5]);
    uint8_t value = (uint8_t)((digits[2] << 4) | digits[3]);

    gb_cheat_t *c = &g_cheats[g_cheat_count_active++];
    c->addr = addr;
    c->value = value;
    c->type = 0;
    c->enabled = 1;
    c->pad = 0;
    return 0;
}

void gb_clear_cheats(void) { g_cheat_count_active = 0; memset(g_cheats, 0, sizeof(g_cheats)); }
int  gb_cheat_count(void)  { return g_cheat_count_active; }

/* ---------------------------------------------------------------- debug  */

uint8_t g_flat_ram[0x10000];
uint8_t g_flat_if;
uint8_t g_flat_ie;

void gb_dbg_regs(uint16_t *pc, uint16_t *sp, uint8_t *a, uint8_t *b, uint8_t *c,
                 uint8_t *d, uint8_t *e, uint8_t *f, uint8_t *h, uint8_t *l)
{
    gb_cpu_t *cpu = &g_gb.cpu;
    if (pc) *pc = cpu->pc;
    if (sp) *sp = cpu->sp;
    if (a)  *a  = cpu->a;
    if (b)  *b  = cpu->b;
    if (c)  *c  = cpu->c;
    if (d)  *d  = cpu->d;
    if (e)  *e  = cpu->e;
    if (f)  *f  = cpu->f;
    if (h)  *h  = cpu->h;
    if (l)  *l  = cpu->l;
}

void gb_dbg_set_regs(uint16_t pc, uint16_t sp, uint8_t a, uint8_t b, uint8_t c,
                     uint8_t d, uint8_t e, uint8_t f, uint8_t h, uint8_t l)
{
    gb_cpu_t *cpu = &g_gb.cpu;
    cpu->pc = pc; cpu->sp = sp;
    cpu->a = a; cpu->b = b; cpu->c = c; cpu->d = d;
    cpu->e = e; cpu->f = f & 0xF0; cpu->h = h; cpu->l = l;
    /* Staging a machine state implies "about to fetch an instruction", so
     * every transient CPU condition is cleared.  Without this a HALT or STOP
     * vector leaks into the next one and that test silently does nothing.  */
    cpu->halt = 0;
    cpu->halt_bug = 0;
    cpu->stop = 0;
    cpu->ime_pending = 0;
}

/* Raw physical access.  ROM/WRAM/etc. are touched directly, with no MBC
 * banking and no I/O write side effects, so a test vector can stage any
 * machine state and read it back byte-for-byte.                          */
uint8_t gb_dbg_read(uint16_t addr)
{
    gb_state_t *g = &g_gb;
    if (g->dbg_flat) return g_flat_ram[addr];
    if (addr < 0x8000) return gb_rom_data()[addr];
    if (addr < 0xA000) return g->vram[g->vram_bank & 1][addr - 0x8000];
    if (addr < 0xC000) {
        size_t n = g->cart.ram_size ? g->cart.ram_size : sizeof(g->cram);
        uint32_t off = (uint32_t)addr - 0xA000u;
        return (off < n) ? g->cram[off] : 0xFF;
    }
    if (addr < 0xE000) return g->wram[(addr >= 0xD000) ? (g->wram_bank & 7) : 0]
                                    [(addr >= 0xD000) ? addr - 0xD000 : addr - 0xC000];
    if (addr < 0xFE00) return g->wram[(addr >= 0xF000) ? (g->wram_bank & 7) : 0]
                                    [(addr >= 0xF000) ? addr - 0xF000 : addr - 0xE000];
    if (addr < 0xFEA0) return g->oam[addr - 0xFE00];
    if (addr < 0xFF00) return 0xFF;
    if (addr < 0xFF80) return g->io[addr - 0xFF00];
    if (addr < 0xFFFF) return g->hram[addr - 0xFF80];
    return g->ie;
}

void gb_dbg_write(uint16_t addr, uint8_t v)
{
    gb_state_t *g = &g_gb;
    if (g->dbg_flat) { g_flat_ram[addr] = v; return; }
    if (addr < 0x8000) { gb_rom_data()[addr] = v; return; }
    if (addr < 0xA000) { g->vram[0][addr - 0x8000] = v; return; }
    if (addr < 0xC000) {
        size_t n = g->cart.ram_size ? g->cart.ram_size : sizeof(g->cram);
        uint32_t off = (uint32_t)addr - 0xA000u;
        if (off < n) g->cram[off] = v;
        return;
    }
    if (addr < 0xE000) { g->wram[(addr >= 0xD000) ? (g->wram_bank & 7) : 0]
                             [(addr >= 0xD000) ? addr - 0xD000 : addr - 0xC000] = v; return; }
    if (addr < 0xFE00) { g->wram[(addr >= 0xF000) ? (g->wram_bank & 7) : 0]
                             [(addr >= 0xF000) ? addr - 0xF000 : addr - 0xE000] = v; return; }
    if (addr < 0xFEA0) { g->oam[addr - 0xFE00] = v; return; }
    if (addr < 0xFF00) return;
    if (addr < 0xFF80) { g->io[addr - 0xFF00] = v; return; }
    if (addr < 0xFFFF) { g->hram[addr - 0xFF80] = v; return; }
    g->ie = v;
}

void gb_dbg_set_ime(int ime) { g_gb.cpu.ime = ime ? 1 : 0; g_gb.cpu.ime_pending = 0; }
int  gb_dbg_get_ime(void)    { return g_gb.cpu.ime; }

int  gb_dbg_step(void)
{
    int cyc = gb_cpu_step(&g_gb);
    gb_advance(&g_gb, (uint32_t)cyc);
    return cyc;
}

uint8_t gb_dbg_map_read(uint16_t addr)             { return gb_read8(&g_gb, addr); }
void    gb_dbg_map_write(uint16_t addr, uint8_t v) { gb_write8(&g_gb, addr, v); }
void    gb_dbg_set_raw_io(int on)                  { g_gb.dbg_raw_io = on ? 1 : 0; }
void    gb_dbg_set_flat(int on)
{
    g_gb.dbg_flat = on ? 1 : 0;
    g_gb.dbg_raw_io = g_gb.dbg_flat ? 1 : g_gb.dbg_raw_io;
    if (on) {
        memset(g_flat_ram, 0, sizeof(g_flat_ram));
        g_flat_if = 0;
        g_flat_ie = 0;
    }
}
void    gb_dbg_set_if(uint8_t v) { g_flat_if = v; if (!g_gb.dbg_flat) g_gb.io[0x0F] = (uint8_t)(v & 0x1F); }
void    gb_dbg_set_ie(uint8_t v) { g_flat_ie = v; if (!g_gb.dbg_flat) g_gb.ie = v; }

void    gb_dbg_reset_cart(void)
{
    gb_cart_reset(&g_gb, gb_rom_data(), gb_rom_size() ? gb_rom_size() : 0x8000);
}

/* CPU-only step: leaves DIV/TIMA/PPU/APU untouched so a test rig can replay
 * thousands of isolated instruction states without cross-test pollution.  */
int  gb_dbg_step_cpu(void)
{
    return gb_cpu_step(&g_gb);
}
