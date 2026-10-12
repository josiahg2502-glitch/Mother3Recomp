/* ==========================================================================
 *  gb.c -- CPU (SM83), memory map, timers, interrupts, OAM DMA, cartridge
 * ========================================================================== */
#include "gb_string.h"
#include "gb_internal.h"

/* ------------------------------------------------------------------ ROM  */
static uint8_t  g_rom[GB_ROM_MAX];
static uint32_t g_rom_size;
static uint32_t g_rom_crc;
static uint8_t  g_rom_info_title[17];
static int      g_rom_info_cart = 0, g_rom_info_rombanks = 2;
static int      g_rom_info_rambanks = 0, g_rom_info_cgb = 0, g_rom_info_sgb = 0;

gb_state_t g_gb;

/* --------------------------------------------------------------- helpers */
#define A  (g->cpu.a)
#define F  (g->cpu.f)
#define B  (g->cpu.b)
#define C  (g->cpu.c)
#define D  (g->cpu.d)
#define E  (g->cpu.e)
#define H  (g->cpu.h)
#define L  (g->cpu.l)
#define HL ((uint16_t)((H << 8) | L))
#define BC ((uint16_t)((B << 8) | C))
#define DE ((uint16_t)((D << 8) | E))
#define AF ((uint16_t)((A << 8) | F))

#define FZ 0x80u
#define FN 0x40u
#define FH 0x20u
#define FC 0x10u

static inline void set_hl(gb_state_t *g, uint16_t v) { H = v >> 8; L = v & 0xFF; }
static inline void set_bc(gb_state_t *g, uint16_t v) { B = v >> 8; C = v & 0xFF; }
static inline void set_de(gb_state_t *g, uint16_t v) { D = v >> 8; E = v & 0xFF; }

/* ------------------------------------------------------------ interrupts */

static const uint16_t int_vectors[5] = { 0x40, 0x48, 0x50, 0x58, 0x60 };

/* ----------------------------------------------------------------- RTC   */

static void rtc_decompose(gb_state_t *g)
{
    gb_rtc_t *r = &g->rtc;
    uint64_t t = r->total;
    r->s  = (uint8_t)(t % 60);
    r->m  = (uint8_t)((t / 60) % 60);
    r->h  = (uint8_t)((t / 3600) % 24);
    uint32_t days = (uint32_t)((t / 86400) & 0x1FF);
    r->dl = (uint8_t)(days & 0xFF);
    r->dh = (uint8_t)(((days >> 8) & 1) | (r->halt ? 0x40 : 0) | (r->carry ? 0x80 : 0));
}

static void rtc_recompose(gb_state_t *g)
{
    gb_rtc_t *r = &g->rtc;
    uint32_t days = (uint32_t)r->dl | ((uint32_t)(r->dh & 1) << 8);
    r->total = (uint64_t)days * 86400ULL + (uint64_t)r->h * 3600ULL +
               (uint64_t)r->m * 60ULL + r->s;
}

void gb_rtc_tick(gb_state_t *g, uint64_t now)
{
    gb_rtc_t *r = &g->rtc;
    if (!r->present) return;
    if (now && r->last_unix && now > r->last_unix)
        r->total += (uint64_t)(now - r->last_unix);
    if (now) r->last_unix = now;
}

/* ------------------------------------------------------------- cart/MBC  */

void gb_cart_reset(gb_state_t *g, const uint8_t *rom, size_t size)
{
    memset(&g->cart, 0, sizeof(g->cart));
    memset(&g->rtc, 0, sizeof(g->rtc));
    if (!rom || size < 0x8000) return;

    g->cart.type = rom[0x0147];
    uint8_t rom_size_code = rom[0x0148];
    uint8_t ram_size_code = rom[0x0149];

    static const uint16_t bank_tab[] = { 2, 4, 8, 16, 32, 64, 128, 256, 512 };
    g->cart.rom_banks = (rom_size_code < 9) ? bank_tab[rom_size_code] : 2;
    if (rom_size_code >= 0x52 && rom_size_code <= 0x54) {
        static const uint16_t big[] = { 72, 80, 96 };
        g->cart.rom_banks = big[rom_size_code - 0x52];
    }
    uint32_t actual = (uint32_t)(size / 0x4000);
    if (actual < 2) actual = 2;
    if (g->cart.rom_banks > actual) g->cart.rom_banks = actual;

    switch (ram_size_code) {
        case 0:  g->cart.ram_size = 0;      g->cart.ram_banks = 0; break;
        case 1:  g->cart.ram_size = 0x800;  g->cart.ram_banks = 1; break; /* MBC2 */
        case 2:  g->cart.ram_size = 0x2000; g->cart.ram_banks = 1; break;
        case 3:  g->cart.ram_size = 0x8000; g->cart.ram_banks = 4; break;
        case 4:  g->cart.ram_size = 0x20000;g->cart.ram_banks = 16; break;
        case 5:  g->cart.ram_size = 0x10000;g->cart.ram_banks = 8; break;
        default: g->cart.ram_size = 0x2000; g->cart.ram_banks = 1; break;
    }

    switch (g->cart.type) {
        case 0x00:                                  g->cart.mbc = 0; break;
        case 0x01: case 0x02: case 0x03:            g->cart.mbc = 1; break;
        case 0x05: case 0x06:                       g->cart.mbc = 2; break;
        case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13: g->cart.mbc = 3; break;
        case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E:
                                                    g->cart.mbc = 5; break;
        default:                                    g->cart.mbc = 0; break;
    }

    switch (g->cart.type) {
        case 0x03: case 0x06: case 0x09: case 0x0D: case 0x10: case 0x13:
        case 0x1B: case 0x1E: case 0x22: case 0xFF:
            g->cart.has_battery = 1; break;
        default: break;
    }
    if (g->cart.mbc == 2) { g->cart.has_ram = 1; g->cart.ram_size = 0x200; }
    else if (g->cart.ram_size) g->cart.has_ram = 1;

    if (g->cart.mbc == 3 && (g->cart.type == 0x0F || g->cart.type == 0x10))
        g->cart.has_rtc = 1;

    g->cart.rom_bank = 1;
    g->cart.ram_bank = 0;
    g->cart.ram_enable = 0;
    g->cart.banking_mode = 0;

    g->rtc.present = g->cart.has_rtc;
    g->rtc.halt = 0;
    g->rtc.carry = 0;
}

static inline uint32_t rom_bank_index(gb_state_t *g)
{
    uint32_t b = g->cart.rom_bank | ((uint32_t)g->cart.rom_bank_hi << 5);
    if (g->cart.mbc == 1) b = g->cart.rom_bank | ((uint32_t)g->cart.rom_bank_hi << 5);
    uint32_t banks = g->cart.rom_banks ? g->cart.rom_banks : 2;
    if (b >= banks) b %= banks;
    return b;
}

void gb_cart_write(gb_state_t *g, uint16_t addr, uint8_t val)
{
    gb_cart_t *c = &g->cart;
    if (c->mbc == 0) return;

    if (addr < 0x2000) {
        if (c->has_ram) c->ram_enable = ((val & 0x0F) == 0x0A);
        return;
    }
    if (addr < 0x4000) {
        if (c->mbc == 5) {
            if (addr < 0x3000) {
                uint16_t b = (c->rom_bank & 0x100) | val;
                c->rom_bank = (uint8_t)(b & 0x1FF);
            } else {
                uint16_t b = (c->rom_bank & 0xFF) | ((val & 1) << 8);
                c->rom_bank = (uint8_t)(b & 0x1FF);
            }
        } else if (c->mbc == 2) {
            if (addr & 0x100) return;
            uint8_t b = val & 0x0F;
            if (b == 0) b = 1;
            c->rom_bank = b;
        } else {
            uint8_t b = val & (c->mbc == 3 ? 0x7F : 0x1F);
            if (c->mbc == 1 && (b & 0x1F) == 0) b |= 1;
            if (c->mbc == 3 && b == 0) b = 1;
            c->rom_bank = b;
        }
        return;
    }
    if (addr < 0x6000) {
        if (c->mbc == 1) {
            if (c->banking_mode == 0) c->rom_bank_hi = (uint8_t)(val & 3);
            else c->ram_bank = (uint8_t)(val & 3);
        } else if (c->mbc == 3) {
            if (val <= 0x07) c->ram_bank = val;
            else if (val >= 0x08 && val <= 0x0C) c->ram_bank = val; /* RTC */
        } else if (c->mbc == 5) {
            c->ram_bank = (uint8_t)(val & 0x0F);
        }
        return;
    }
    /* 0x6000 - 0x7FFF */
    if (c->mbc == 1) {
        c->banking_mode = val & 1;
        if (c->banking_mode == 0) c->ram_bank = 0; else c->rom_bank_hi = 0;
    } else if (c->mbc == 3) {
        if (g->rtc.present) {
            if (g->rtc.latch_state == 0 && val == 0x00) g->rtc.latch_state = 1;
            else if (g->rtc.latch_state == 1 && val == 0x01) {
                g->rtc.latch_state = 0;
                rtc_decompose(g);
            } else g->rtc.latch_state = 0;
        }
    }
}

/* ------------------------------------------------------------ memory map */

static uint8_t io_read(gb_state_t *g, uint16_t addr);

uint8_t gb_read8(gb_state_t *g, uint16_t addr)
{
    /* Test-rig mode: the SM83 vectors are pure CPU tests generated against a
     * flat 64 KiB RAM, so bypass the MBC and I/O read-back masks entirely. */
    if (g->dbg_flat) return gb_dbg_read(addr);

    switch (addr >> 12) {
        case 0x0: case 0x1: case 0x2: case 0x3:
            return g_rom[addr & 0x3FFF];
        case 0x4: case 0x5: case 0x6: case 0x7: {
            uint32_t off = rom_bank_index(g) * 0x4000u + (addr - 0x4000u);
            return g_rom[off & (GB_ROM_MAX - 1)];
        }
        case 0x8: case 0x9:
            return g->vram[g->vram_bank & 1][addr - 0x8000];
        case 0xA: case 0xB: {
            gb_cart_t *c = &g->cart;
            if (!c->has_ram) return 0xFF;
            if (c->mbc == 3 && c->ram_bank >= 0x08 && c->ram_bank <= 0x0C) {
                rtc_decompose(g);
                switch (c->ram_bank) {
                    case 0x08: return g->rtc.s;
                    case 0x09: return g->rtc.m;
                    case 0x0A: return g->rtc.h;
                    case 0x0B: return g->rtc.dl;
                    case 0x0C: return g->rtc.dh;
                    default: break;
                }
            }
            if (!c->ram_enable || !c->ram_size) return 0xFF;
            uint32_t off = (uint32_t)c->ram_bank * 0x2000u + (addr - 0xA000u);
            if (off >= c->ram_size) off %= c->ram_size;
            if (c->mbc == 2) return g->cram[off & 0x1FF] | 0xF0;
            return g->cram[off];
        }
        case 0xC:
            return g->wram[0][addr - 0xC000];
        case 0xD:
            return g->wram[g->wram_bank & 7][addr - 0xD000];
        case 0xE:
            if (addr < 0xF000) return g->wram[0][addr - 0xE000];
            break;
        case 0xF:
            if (addr < 0xFE00) {
                if (addr < 0xF000) return g->wram[0][addr - 0xE000];
                return g->wram[g->wram_bank & 7][addr - 0xF000];
            }
            if (addr < 0xFEA0) {
                if (g->dma_active) return 0xFF;
                return g->oam[addr - 0xFE00];
            }
            if (addr < 0xFF00) return 0xFF;
            if (addr < 0xFF80) return io_read(g, addr);
            if (addr < 0xFFFF) return g->hram[addr - 0xFF80];
            return g->ie;
        default: break;
    }
    return 0xFF;
}

static uint8_t io_read(gb_state_t *g, uint16_t addr)
{
    /* Test-rig mode: return the register file verbatim instead of the
     * emulated read-back masks (DIV, P1, NR52, KEY1, ...).                */
    if (g->dbg_raw_io) return g->io[addr - 0xFF00];

    switch (addr) {
        case 0xFF00: { /* P1 */
            uint8_t v = g->joyp | 0xC0;
            if (!(g->joyp & 0x10)) { /* directions */
                if (g->buttons & GB_BTN_RIGHT)  v &= ~0x01;
                if (g->buttons & GB_BTN_LEFT)   v &= ~0x02;
                if (g->buttons & GB_BTN_UP)     v &= ~0x04;
                if (g->buttons & GB_BTN_DOWN)   v &= ~0x08;
            }
            if (!(g->joyp & 0x20)) { /* buttons */
                if (g->buttons & GB_BTN_A)      v &= ~0x01;
                if (g->buttons & GB_BTN_B)      v &= ~0x02;
                if (g->buttons & GB_BTN_SELECT) v &= ~0x04;
                if (g->buttons & GB_BTN_START)  v &= ~0x08;
            }
            return (uint8_t)(v | 0xC0);
        }
        case 0xFF04: return (uint8_t)(g->div >> 8);
        case 0xFF05: return g->tima_reload ? 0 : g->io[0x05];
        case 0xFF0F: return (uint8_t)(g->io[0x0F] | 0xE0);
        case 0xFF10: case 0xFF11: case 0xFF12: case 0xFF13: case 0xFF14:
        case 0xFF16: case 0xFF17: case 0xFF18: case 0xFF19:
        case 0xFF1A: case 0xFF1B: case 0xFF1C: case 0xFF1D: case 0xFF1E:
        case 0xFF20: case 0xFF21: case 0xFF22: case 0xFF23:
        case 0xFF24: case 0xFF25: case 0xFF26:
            return gb_apu_read(g, addr);
        case 0xFF44: return g->io[0x44];
        case 0xFF4D: return (uint8_t)((g->speed & 1) << 7) | (g->speed_prepare ? 1 : 0) | 0x7E;
        case 0xFF4F: return (uint8_t)(0xFE | (g->vram_bank & 1));
        case 0xFF55: return (uint8_t)((g->ppu.hdma_active || g->ppu.hdma_left) ? (g->ppu.hdma_left ? (uint8_t)((g->ppu.hdma_left - 1) >> 4) : 0) : 0xFF);
        case 0xFF68: return (uint8_t)(g->io[0x68] | 0x40);
        case 0xFF69: return g->bgpd[g->io[0x68] & 0x3F];
        case 0xFF6A: return (uint8_t)(g->io[0x6A] | 0x40);
        case 0xFF6B: return g->obpd[g->io[0x6A] & 0x3F];
        case 0xFF70: return (uint8_t)(0xF8 | (g->wram_bank & 7));
        case 0xFF76: return 0x00; /* PCM */
        case 0xFF77: return 0x00;
        default:
            if (addr >= 0xFF30 && addr <= 0xFF3F)
                return g->apu.wave_dac ? g->io[addr - 0xFF00] : g->io[addr - 0xFF00];
            return g->io[addr - 0xFF00];
    }
}

static void io_write(gb_state_t *g, uint16_t addr, uint8_t val);

void gb_write8(gb_state_t *g, uint16_t addr, uint8_t val)
{
    if (g->dbg_flat) { gb_dbg_write(addr, val); return; }

    switch (addr >> 12) {
        case 0x0: case 0x1: case 0x2: case 0x3:
        case 0x4: case 0x5: case 0x6: case 0x7:
            gb_cart_write(g, addr, val);
            return;
        case 0x8: case 0x9:
            g->vram[g->vram_bank & 1][addr - 0x8000] = val;
            return;
        case 0xA: case 0xB: {
            gb_cart_t *c = &g->cart;
            if (!c->has_ram) return;
            if (c->mbc == 3 && c->ram_bank >= 0x08 && c->ram_bank <= 0x0C) {
                switch (c->ram_bank) {
                    case 0x08: g->rtc.s  = val & 0x3F; break;
                    case 0x09: g->rtc.m  = val & 0x3F; break;
                    case 0x0A: g->rtc.h  = val & 0x1F; break;
                    case 0x0B: g->rtc.dl = val; break;
                    case 0x0C: g->rtc.dh = val & 0xC1;
                               g->rtc.halt  = (val >> 6) & 1;
                               g->rtc.carry = (val >> 7) & 1;
                               break;
                    default: break;
                }
                rtc_recompose(g);
                return;
            }
            if (!c->ram_enable || !c->ram_size) return;
            uint32_t off = (uint32_t)c->ram_bank * 0x2000u + (addr - 0xA000u);
            if (off >= c->ram_size) off %= c->ram_size;
            if (c->mbc == 2) val &= 0x0F;
            g->cram[off] = val;
            return;
        }
        case 0xC:
            g->wram[0][addr - 0xC000] = val;
            return;
        case 0xD:
            g->wram[g->wram_bank & 7][addr - 0xD000] = val;
            return;
        case 0xE:
            if (addr < 0xF000) g->wram[0][addr - 0xE000] = val;
            return;
        case 0xF:
            if (addr < 0xFE00) {
                if (addr < 0xF000) g->wram[0][addr - 0xE000] = val;
                else g->wram[g->wram_bank & 7][addr - 0xF000] = val;
                return;
            }
            if (addr < 0xFEA0) {
                if (g->dma_active) return;
                g->oam[addr - 0xFE00] = val;
                return;
            }
            if (addr < 0xFF00) return;
            if (addr < 0xFF80) { io_write(g, addr, val); return; }
            if (addr < 0xFFFF) { g->hram[addr - 0xFF80] = val; return; }
            g->ie = val;
            return;
        default: break;
    }
}

static void io_write(gb_state_t *g, uint16_t addr, uint8_t val)
{
    switch (addr) {
        case 0xFF00:
            g->joyp = (uint8_t)(val & 0x30);
            return;
        case 0xFF01: g->io[0x01] = val; return;
        case 0xFF02:
            g->io[0x02] = (uint8_t)(val & 0x83);
            if ((val & 0x80) && (val & 0x01)) { g->serial_bits = 8; g->serial_timer = 0; }
            return;
        case 0xFF04:
            /* any write resets DIV */
            {
                uint8_t tac = g->io[0x07];
                uint16_t mask = (uint16_t)(1 << ((tac & 3) == 0 ? 9 : (tac & 3) == 1 ? 3 : (tac & 3) == 2 ? 5 : 7));
                if ((tac & 4) && (g->div & mask)) {
                    uint8_t t = g->io[0x05] + 1;
                    if (t == 0) { g->io[0x05] = 0; g->tima_reload = 4; g->io[0x0F] |= 4; }
                    else g->io[0x05] = t;
                }
            }
            g->div = 0;
            return;
        case 0xFF05:
            if (!g->tima_reload) g->io[0x05] = val;
            return;
        case 0xFF06:
            g->io[0x06] = val;
            if (g->tima_reload) g->io[0x05] = val;
            return;
        case 0xFF07: {
            uint8_t old = g->io[0x07];
            uint16_t omask = (uint16_t)(1 << ((old & 3) == 0 ? 9 : (old & 3) == 1 ? 3 : (old & 3) == 2 ? 5 : 7));
            uint16_t nmask = (uint16_t)(1 << ((val & 3) == 0 ? 9 : (val & 3) == 1 ? 3 : (val & 3) == 2 ? 5 : 7));
            if ((old & 4) && !(val & 4) && (g->div & omask)) {
                uint8_t t = g->io[0x05] + 1;
                if (t == 0) { g->io[0x05] = 0; g->tima_reload = 4; g->io[0x0F] |= 4; }
                else g->io[0x05] = t;
            }
            if ((val & 4) && !(g->div & nmask) && (g->div & omask) && (old & 3) != (val & 3)) {
                uint8_t t = g->io[0x05] + 1;
                if (t == 0) { g->io[0x05] = 0; g->tima_reload = 4; g->io[0x0F] |= 4; }
                else g->io[0x05] = t;
            }
            g->io[0x07] = (uint8_t)(val & 0x07);
            return;
        }
        case 0xFF0F: g->io[0x0F] = (uint8_t)(val & 0x1F); return;

        case 0xFF10: case 0xFF11: case 0xFF12: case 0xFF13: case 0xFF14:
        case 0xFF16: case 0xFF17: case 0xFF18: case 0xFF19:
        case 0xFF1A: case 0xFF1B: case 0xFF1C: case 0xFF1D: case 0xFF1E:
        case 0xFF20: case 0xFF21: case 0xFF22: case 0xFF23:
        case 0xFF24: case 0xFF25: case 0xFF26:
            g->io[addr - 0xFF00] = val;
            gb_apu_write(g, addr, val);
            return;
        case 0xFF30: case 0xFF31: case 0xFF32: case 0xFF33: case 0xFF34: case 0xFF35:
        case 0xFF36: case 0xFF37: case 0xFF38: case 0xFF39: case 0xFF3A: case 0xFF3B:
        case 0xFF3C: case 0xFF3D: case 0xFF3E: case 0xFF3F:
            g->io[addr - 0xFF00] = val;
            return;

        case 0xFF40: /* LCDC */
            g->io[0x40] = val;
            if (!(val & 0x80)) {
                /* LCD off: park the PPU at the start of a fresh line */
                g->io[0x44] = 0;
                g->ppu.mode = 0;
                g->ppu.line_cycles = 0;
                g->ppu.win_line = 0;
                g->ppu.in_window = 0;
                g->ppu.line_rendered = 0;
                g->ppu.prev_stat_irq = 0;
            }
            return;
        case 0xFF41: /* STAT */
            g->io[0x41] = (uint8_t)((val & 0xF8) | (g->io[0x41] & 0x07));
            return;
        case 0xFF42: g->io[0x42] = val; return;
        case 0xFF43: g->io[0x43] = val; return;
        case 0xFF44: return; /* LY read only */
        case 0xFF45:         /* LYC */
            g->io[0x45] = val;
            return;
        case 0xFF46: /* OAM DMA */
            g->dma_src = (uint16_t)(val << 8);
            g->dma_active = 1;
            g->dma_pos = 0;
            g->dma_cycles_left = 8; /* 2 cycle startup delay */
            g->io[0x46] = val;
            return;
        case 0xFF47: g->io[0x47] = val; return;
        case 0xFF48: g->io[0x48] = val; return;
        case 0xFF49: g->io[0x49] = val; return;
        case 0xFF4A: g->io[0x4A] = val; return;
        case 0xFF4B: g->io[0x4B] = val; return;

        case 0xFF4D: /* KEY1 */
            if (g->cgb_mode) g->speed_prepare = val & 1;
            g->io[0x4D] = (uint8_t)(val & 0x01);
            return;
        case 0xFF4F: /* VBK */
            if (g->cgb_mode) g->vram_bank = (uint8_t)(val & 1);
            return;
        case 0xFF51: g->io[0x51] = val; return;
        case 0xFF52: g->io[0x52] = (uint8_t)(val & 0xF0); return;
        case 0xFF53: g->io[0x53] = val; return;
        case 0xFF54: g->io[0x54] = (uint8_t)(val & 0x1F); return;
        case 0xFF55: { /* HDMA5 */
            if (!g->cgb_mode) { g->io[0x55] = val; return; }
            uint16_t src = (uint16_t)((g->io[0x51] << 8) | (g->io[0x52] & 0xF0));
            uint16_t dst = (uint16_t)(((g->io[0x53] & 0x1F) << 8) | (g->io[0x54] & 0xF0));
            uint16_t len = (uint16_t)(((val & 0x7F) + 1) * 16);
            g->ppu.hdma_src = src;
            g->ppu.hdma_dst = dst;
            g->ppu.hdma_left = len;
            g->ppu.hdma_hblank = (val & 0x80) ? 1 : 0;
            if (!(val & 0x80)) {
                g->ppu.hdma_active = 1;
            } else {
                g->ppu.hdma_active = 1;
            }
            g->io[0x55] = (uint8_t)(val & 0x80);
            return;
        }
        case 0xFF56: g->io[0x56] = (uint8_t)(val & 0xC1); return;
        case 0xFF68: g->io[0x68] = (uint8_t)(val & 0xBF); return;
        case 0xFF69: {
            uint8_t idx = g->io[0x68] & 0x3F;
            g->bgpd[idx] = val;
            if (g->io[0x68] & 0x80) {
                idx = (uint8_t)((idx + 1) & 0x3F);
                g->io[0x68] = (uint8_t)((g->io[0x68] & 0x80) | idx);
            }
            return;
        }
        case 0xFF6A: g->io[0x6A] = (uint8_t)(val & 0xBF); return;
        case 0xFF6B: {
            uint8_t idx = g->io[0x6A] & 0x3F;
            g->obpd[idx] = val;
            if (g->io[0x6A] & 0x80) {
                idx = (uint8_t)((idx + 1) & 0x3F);
                g->io[0x6A] = (uint8_t)((g->io[0x6A] & 0x80) | idx);
            }
            return;
        }
        case 0xFF6C: g->io[0x6C] = (uint8_t)(val & 1); return;
        case 0xFF70: /* SVBK */
            if (g->cgb_mode) g->wram_bank = (uint8_t)(val & 7 ? val & 7 : 1);
            return;
        case 0xFF76: case 0xFF77: return;
        default:
            g->io[addr - 0xFF00] = val;
            return;
    }
}

uint8_t *gb_vram_ptr(gb_state_t *g, uint16_t addr)
{
    return &g->vram[g->vram_bank & 1][addr - 0x8000];
}

/* ---------------------------------------------------------------- timers */

static inline uint16_t timer_mask(uint8_t tac)
{
    switch (tac & 3) {
        case 0: return 1 << 9;
        case 1: return 1 << 3;
        case 2: return 1 << 5;
        default: return 1 << 7;
    }
}

static inline void tima_inc(gb_state_t *g)
{
    if (g->tima_reload) {
        g->tima_reload--;
        if (!g->tima_reload) g->io[0x05] = g->io[0x06];
        return;
    }
    uint8_t t = (uint8_t)(g->io[0x05] + 1);
    if (t == 0) {
        g->io[0x05] = 0;
        g->tima_reload = 4;
        g->io[0x0F] |= 0x04;
    } else {
        g->io[0x05] = t;
    }
}

static void timer_step(gb_state_t *g, uint32_t n)
{
    uint8_t tac = g->io[0x07];
    uint32_t div = g->div;
    if (tac & 4) {
        uint16_t mask = timer_mask(tac);
        for (uint32_t i = 0; i < n; i++) {
            uint16_t prev = (uint16_t)div;
            div = (div + 1) & 0xFFFF;
            if ((prev & mask) && !(div & mask)) tima_inc(g);
        }
    } else {
        div = (div + n) & 0xFFFF;
    }
    g->div = div;
}

/* ------------------------------------------------------------- OAM DMA   */

static void dma_step(gb_state_t *g, uint32_t n)
{
    if (g->serial_bits) {
        g->serial_timer += n;
        if (g->serial_timer >= 512) {
            g->serial_timer -= 512;
            if (--g->serial_bits == 0) {
                g->io[0x01] = 0xFF;
                g->io[0x02] &= ~0x80;
                g->io[0x0F] |= 0x08;
            }
        }
    }
    if (!g->dma_active) return;
    g->dma_cycles_left += n;
    while (g->dma_active && g->dma_cycles_left >= 4) {
        g->dma_cycles_left -= 4;
        if (g->dma_pos < 0xA0) {
            uint8_t v = gb_read8(g, (uint16_t)(g->dma_src + g->dma_pos));
            g->oam[g->dma_pos] = v;
            g->dma_pos++;
        } else {
            g->dma_active = 0;
        }
    }
    if (g->dma_active && g->dma_pos >= 0xA0) g->dma_active = 0;
}

/* ------------------------------------------------------------- advance   */

void gb_advance(gb_state_t *g, uint32_t cycles)
{
    while (cycles) {
        /* chunk to the next PPU event so STAT/LYC/VBLANK land exactly */
        uint32_t n = gb_ppu_next_event(g);
        if (n == 0) n = 1;
        if (n > cycles) n = cycles;
        if (n > 64) n = 64;   /* keep timer/DMA granularity tight */

        timer_step(g, n);
        gb_apu_step(g, n);
        dma_step(g, n);
        gb_ppu_step(g, n);

        cycles -= n;
    }
}

/* ------------------------------------------------------------------ CPU  */

static inline void push8(gb_state_t *g, uint8_t v)
{
    g->cpu.sp = (uint16_t)(g->cpu.sp - 1);
    gb_write8(g, g->cpu.sp, v);
}

static inline uint8_t pop8(gb_state_t *g)
{
    uint8_t v = gb_read8(g, g->cpu.sp);
    g->cpu.sp = (uint16_t)(g->cpu.sp + 1);
    return v;
}

static inline void push16(gb_state_t *g, uint16_t v)
{
    push8(g, (uint8_t)(v >> 8));
    push8(g, (uint8_t)(v & 0xFF));
}

static inline uint16_t pop16(gb_state_t *g)
{
    uint8_t lo = pop8(g);
    uint8_t hi = pop8(g);
    return (uint16_t)((hi << 8) | lo);
}

static inline uint8_t rd_r(gb_state_t *g, int i)
{
    switch (i) {
        case 0: return B;
        case 1: return C;
        case 2: return D;
        case 3: return E;
        case 4: return H;
        case 5: return L;
        case 6: return gb_read8(g, HL);
        default: return A;
    }
}

static inline void wr_r(gb_state_t *g, int i, uint8_t v)
{
    switch (i) {
        case 0: B = v; break;
        case 1: C = v; break;
        case 2: D = v; break;
        case 3: E = v; break;
        case 4: H = v; break;
        case 5: L = v; break;
        case 6: gb_write8(g, HL, v); break;
        default: A = v; break;
    }
}

static const uint16_t r16_tab[4] = { 0, 0, 0, 0 }; /* unused placeholder */

static inline uint16_t rd_r16(gb_state_t *g, int i)
{
    switch (i) {
        case 0: return BC;
        case 1: return DE;
        case 2: return HL;
        default: return g->cpu.sp;
    }
}

static inline void wr_r16(gb_state_t *g, int i, uint16_t v)
{
    switch (i) {
        case 0: set_bc(g, v); break;
        case 1: set_de(g, v); break;
        case 2: set_hl(g, v); break;
        default: g->cpu.sp = v; break;
    }
}

static const uint8_t cc_tab[4][2] = { { 0, FZ }, { 1, FZ }, { 0, FC }, { 1, FC } };

static inline int cond(gb_state_t *g, int i)
{
    switch (i) {
        case 0: return (F & FZ) == 0;
        case 1: return (F & FZ) != 0;
        case 2: return (F & FC) == 0;
        default: return (F & FC) != 0;
    }
}

static const uint8_t alu_op[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };

void gb_cpu_reset(gb_state_t *g, int cgb)
{
    memset(&g->cpu, 0, sizeof(g->cpu));
    if (cgb) {
        g->cpu.a = 0x11; g->cpu.f = 0x80;
        g->cpu.b = 0x00; g->cpu.c = 0x00;
        g->cpu.d = 0xFF; g->cpu.e = 0x56;
        g->cpu.h = 0x00; g->cpu.l = 0x0D;
    } else {
        g->cpu.a = 0x01; g->cpu.f = 0xB0;
        g->cpu.b = 0x00; g->cpu.c = 0x13;
        g->cpu.d = 0x00; g->cpu.e = 0xD8;
        g->cpu.h = 0x01; g->cpu.l = 0x4D;
    }
    g->cpu.sp = 0xFFFE;
    g->cpu.pc = 0x0100;
    g->cpu.ime = 0;
    g->cpu.ime_pending = 0;
    g->cpu.halt = 0;
    g->cpu.halt_bug = 0;
    g->cpu.stop = 0;
}

/* IF / IE accessors: in flat test mode these live in the flat array, since
 * the vectors model $FF0F and $FFFF as ordinary writable RAM.              */
static inline uint8_t cpu_if(gb_state_t *g)
{
    return g->dbg_flat ? g_flat_if : g->io[0x0F];
}

static inline void cpu_if_set(gb_state_t *g, uint8_t mask)
{
    if (g->dbg_flat) g_flat_if = (uint8_t)(g_flat_if | mask);
    else g->io[0x0F] = (uint8_t)(g->io[0x0F] | mask);
}

static inline void cpu_if_clear(gb_state_t *g, uint8_t mask)
{
    if (g->dbg_flat) g_flat_if = (uint8_t)(g_flat_if & (uint8_t)~mask);
    else g->io[0x0F] = (uint8_t)(g->io[0x0F] & (uint8_t)~mask);
}

static inline uint8_t cpu_ie(gb_state_t *g)
{
    return g->dbg_flat ? g_flat_ie : g->ie;
}

int gb_cpu_step(gb_state_t *g)
{
    uint8_t fired = (uint8_t)(cpu_if(g) & cpu_ie(g) & 0x1F);

    if (g->cpu.halt) {
        if (fired) {
            g->cpu.halt = 0;
            if (!g->cpu.ime) g->cpu.halt_bug = 1;
        } else {
            return 4;
        }
    }

    if (g->cpu.ime && fired) {
        int bit = 0;
        while (!(fired & (1 << bit))) bit++;
        g->cpu.ime = 0;
        cpu_if_clear(g, (uint8_t)(1 << bit));
        push16(g, g->cpu.pc);
        g->cpu.pc = int_vectors[bit];
        return 20;
    }

    /* --- fetch (with the HALT bug's missing increment) --- */
    uint8_t op;
    if (g->cpu.halt_bug) {
        g->cpu.halt_bug = 0;
        op = gb_read8(g, g->cpu.pc);
    } else {
        op = gb_read8(g, g->cpu.pc++);
    }

    if (g->cpu.ime_pending) { g->cpu.ime_pending = 0; g->cpu.ime = 1; }

    uint8_t lo, hi;
    uint16_t nn;

    switch (op) {
        case 0x00: return 4;                                     /* NOP */
        case 0x01: case 0x11: case 0x21: case 0x31: {            /* LD r16,nn */
            lo = gb_read8(g, g->cpu.pc++);
            hi = gb_read8(g, g->cpu.pc++);
            wr_r16(g, (op >> 4) & 3, (uint16_t)((hi << 8) | lo));
            return 12;
        }
        case 0x02: gb_write8(g, BC, A); return 8;                /* LD (BC),A */
        case 0x12: gb_write8(g, DE, A); return 8;
        case 0x22: gb_write8(g, HL, A); set_hl(g, (uint16_t)(HL + 1)); return 8;
        case 0x32: gb_write8(g, HL, A); set_hl(g, (uint16_t)(HL - 1)); return 8;
        case 0x0A: A = gb_read8(g, BC); return 8;
        case 0x1A: A = gb_read8(g, DE); return 8;
        case 0x2A: A = gb_read8(g, HL); set_hl(g, (uint16_t)(HL + 1)); return 8;
        case 0x3A: A = gb_read8(g, HL); set_hl(g, (uint16_t)(HL - 1)); return 8;

        case 0x03: case 0x13: case 0x23: case 0x33:               /* INC r16 */
            wr_r16(g, (op >> 4) & 3, (uint16_t)(rd_r16(g, (op >> 4) & 3) + 1));
            return 8;
        case 0x0B: case 0x1B: case 0x2B: case 0x3B:               /* DEC r16 */
            wr_r16(g, (op >> 4) & 3, (uint16_t)(rd_r16(g, (op >> 4) & 3) - 1));
            return 8;

        case 0x04: case 0x14: case 0x24: case 0x34:
        case 0x0C: case 0x1C: case 0x2C: case 0x3C: {             /* INC r8 */
            int i = (op >> 3) & 7;
            uint8_t v = (uint8_t)(rd_r(g, i) + 1);
            F = (uint8_t)((F & FC) | (v ? 0 : FZ) | ((v & 0x0F) ? 0 : FH));
            wr_r(g, i, v);
            return (i == 6) ? 12 : 4;
        }
        case 0x05: case 0x15: case 0x25: case 0x35:
        case 0x0D: case 0x1D: case 0x2D: case 0x3D: {             /* DEC r8 */
            int i = (op >> 3) & 7;
            uint8_t v = (uint8_t)(rd_r(g, i) - 1);
            F = (uint8_t)((F & FC) | FN | (v ? 0 : FZ) | ((v & 0x0F) == 0x0F ? FH : 0));
            wr_r(g, i, v);
            return (i == 6) ? 12 : 4;
        }
        case 0x06: case 0x16: case 0x26: case 0x36:
        case 0x0E: case 0x1E: case 0x2E: case 0x3E: {             /* LD r8,n */
            int i = (op >> 3) & 7;
            wr_r(g, i, gb_read8(g, g->cpu.pc++));
            return (i == 6) ? 12 : 8;
        }

        case 0x07: { /* RLCA */
            uint8_t c = (uint8_t)(A >> 7);
            A = (uint8_t)((A << 1) | c);
            F = (uint8_t)(c ? FC : 0);
            return 4;
        }
        case 0x0F: { /* RRCA */
            uint8_t c = (uint8_t)(A & 1);
            A = (uint8_t)((A >> 1) | (c << 7));
            F = (uint8_t)(c ? FC : 0);
            return 4;
        }
        case 0x17: { /* RLA */
            uint8_t c = (uint8_t)(A >> 7);
            A = (uint8_t)((A << 1) | ((F & FC) ? 1 : 0));
            F = (uint8_t)(c ? FC : 0);
            return 4;
        }
        case 0x1F: { /* RRA */
            uint8_t c = (uint8_t)(A & 1);
            A = (uint8_t)((A >> 1) | ((F & FC) ? 0x80 : 0));
            F = (uint8_t)(c ? FC : 0);
            return 4;
        }

        case 0x08: { /* LD (nn),SP */
            lo = gb_read8(g, g->cpu.pc++);
            hi = gb_read8(g, g->cpu.pc++);
            nn = (uint16_t)((hi << 8) | lo);
            gb_write8(g, nn, (uint8_t)(g->cpu.sp & 0xFF));
            gb_write8(g, (uint16_t)(nn + 1), (uint8_t)(g->cpu.sp >> 8));
            return 20;
        }
        case 0x09: case 0x19: case 0x29: case 0x39: {             /* ADD HL,r16 */
            uint32_t r = rd_r16(g, (op >> 4) & 3);
            uint32_t res = HL + r;
            F = (uint8_t)((F & FZ) | ((res > 0xFFFF) ? FC : 0) |
                          (((HL & 0x0FFF) + (r & 0x0FFF)) > 0x0FFF ? FH : 0));
            set_hl(g, (uint16_t)res);
            return 8;
        }

        case 0x10: /* STOP
                    * The documented operand byte is always $00, so whether
                    * PC advances by 1 or 2 is indistinguishable in practice
                    * (the extra fetch is a NOP).  We match the SM83
                    * single-step reference, which advances by 1.           */
            if (g->cgb_mode && g->speed_prepare) {
                g->speed ^= 1;
                g->speed_prepare = 0;
                g->io[0x4D] = (uint8_t)(((g->speed & 1) << 7) & 0x80);
            }
            return 4;
        case 0x18: { /* JR r8 */
            int8_t d = (int8_t)gb_read8(g, g->cpu.pc++);
            g->cpu.pc = (uint16_t)(g->cpu.pc + d);
            return 12;
        }
        case 0x20: case 0x28: case 0x30: case 0x38: {             /* JR cc,r8 */
            int8_t d = (int8_t)gb_read8(g, g->cpu.pc++);
            if (cond(g, (op >> 3) & 3)) {
                g->cpu.pc = (uint16_t)(g->cpu.pc + d);
                return 12;
            }
            return 8;
        }

        case 0x27: { /* DAA */
            uint16_t v = A;
            if (!(F & FN)) {
                if ((F & FH) || (v & 0x0F) > 9) v += 6;
                if ((F & FC) || v > 0x9F) v += 0x60;
            } else {
                if (F & FH) v = (uint16_t)((v - 6) & 0xFF);
                if (F & FC) v = (uint16_t)(v - 0x60);
            }
            F = (uint8_t)((F & (FN | FC)) | ((v & 0xFF) == 0 ? FZ : 0));
            if (v & 0x100) F |= FC;
            A = (uint8_t)(v & 0xFF);
            return 4;
        }
        case 0x2F: A = (uint8_t)~A; F |= (FN | FH); return 4;      /* CPL */
        case 0x37: F = (uint8_t)((F & FZ) | FC); return 4;         /* SCF */
        case 0x3F: /* CCF */
            F = (uint8_t)((F & (FZ | FC)) ^ FC);
            return 4;
        case 0x76: /* HALT */
            g->cpu.halt = 1;
            return 4;
        case 0xF3: g->cpu.ime = 0; g->cpu.ime_pending = 0; return 4; /* DI */
        case 0xFB: g->cpu.ime_pending = 1; return 4;                 /* EI */

        /* --- 0x40 - 0x7F : LD r,r -------------------------------- */
        default:
            if (op >= 0x40 && op <= 0x7F) {
                if (op == 0x76) { g->cpu.halt = 1; return 4; }
                wr_r(g, (op >> 3) & 7, rd_r(g, op & 7));
                return ((op & 7) == 6 || ((op >> 3) & 7) == 6) ? 8 : 4;
            }
            break;
    }

    /* --- ALU A,r8 ------------------------------------------------ */
    if (op >= 0x80 && op <= 0xBF) {
        int idx = op & 7;
        uint8_t v = rd_r(g, idx);
        int cycles = (idx == 6) ? 8 : 4;
        switch ((op >> 3) & 7) {
            case 0: { /* ADD */
                uint16_t r = (uint16_t)(A + v);
                F = (uint8_t)(((r & 0xFF) == 0 ? FZ : 0) | ((r > 0xFF) ? FC : 0) |
                              (((A & 0xF) + (v & 0xF)) > 0xF ? FH : 0));
                A = (uint8_t)r;
                break;
            }
            case 1: { /* ADC */
                uint8_t c = (F & FC) ? 1 : 0;
                uint16_t r = (uint16_t)(A + v + c);
                F = (uint8_t)(((r & 0xFF) == 0 ? FZ : 0) | ((r > 0xFF) ? FC : 0) |
                              (((A & 0xF) + (v & 0xF) + c) > 0xF ? FH : 0));
                A = (uint8_t)r;
                break;
            }
            case 2: { /* SUB */
                uint16_t r = (uint16_t)(A - v);
                F = (uint8_t)(FN | ((r & 0xFF) == 0 ? FZ : 0) | ((r > 0xFF) ? FC : 0) |
                              ((A & 0xF) < (v & 0xF) ? FH : 0));
                A = (uint8_t)r;
                break;
            }
            case 3: { /* SBC */
                uint8_t c = (F & FC) ? 1 : 0;
                uint16_t r = (uint16_t)(A - v - c);
                F = (uint8_t)(FN | ((r & 0xFF) == 0 ? FZ : 0) | ((r > 0xFF) ? FC : 0) |
                              ((A & 0xF) < ((v & 0xF) + c) ? FH : 0));
                A = (uint8_t)r;
                break;
            }
            case 4: A = (uint8_t)(A & v); F = (uint8_t)(FH | (A ? 0 : FZ)); break;
            case 5: A = (uint8_t)(A ^ v); F = (uint8_t)(A ? 0 : FZ); break;
            case 6: A = (uint8_t)(A | v); F = (uint8_t)(A ? 0 : FZ); break;
            case 7: { /* CP */
                uint16_t r = (uint16_t)(A - v);
                F = (uint8_t)(FN | ((r & 0xFF) == 0 ? FZ : 0) | ((r > 0xFF) ? FC : 0) |
                              ((A & 0xF) < (v & 0xF) ? FH : 0));
                break;
            }
            default: break;
        }
        return cycles;
    }

    switch (op) {
        case 0xC0: case 0xC8: case 0xD0: case 0xD8: {             /* RET cc */
            if (cond(g, (op >> 3) & 3)) { g->cpu.pc = pop16(g); return 20; }
            return 8;
        }
        case 0xC9: g->cpu.pc = pop16(g); return 16;                /* RET */
        case 0xD9: g->cpu.pc = pop16(g); g->cpu.ime = 1; return 16; /* RETI */
        case 0xC2: case 0xCA: case 0xD2: case 0xDA: {             /* JP cc,nn */
            lo = gb_read8(g, g->cpu.pc++);
            hi = gb_read8(g, g->cpu.pc++);
            if (cond(g, (op >> 3) & 3)) { g->cpu.pc = (uint16_t)((hi << 8) | lo); return 16; }
            return 12;
        }
        case 0xC3:                                                /* JP nn */
            lo = gb_read8(g, g->cpu.pc++);
            hi = gb_read8(g, g->cpu.pc++);
            g->cpu.pc = (uint16_t)((hi << 8) | lo);
            return 16;
        case 0xE9: g->cpu.pc = HL; return 4;                       /* JP (HL) */
        case 0xC4: case 0xCC: case 0xD4: case 0xDC: {             /* CALL cc,nn */
            lo = gb_read8(g, g->cpu.pc++);
            hi = gb_read8(g, g->cpu.pc++);
            if (cond(g, (op >> 3) & 3)) {
                push16(g, g->cpu.pc);
                g->cpu.pc = (uint16_t)((hi << 8) | lo);
                return 24;
            }
            return 12;
        }
        case 0xCD: {                                              /* CALL nn */
            lo = gb_read8(g, g->cpu.pc++);
            hi = gb_read8(g, g->cpu.pc++);
            push16(g, g->cpu.pc);
            g->cpu.pc = (uint16_t)((hi << 8) | lo);
            return 24;
        }
        case 0xC1: case 0xD1: case 0xE1: case 0xF1: {             /* POP */
            uint16_t v = pop16(g);
            if (op == 0xF1) { A = (uint8_t)(v >> 8); F = (uint8_t)(v & 0xF0); }
            else wr_r16(g, (op >> 4) & 3, v);
            return 12;
        }
        case 0xC5: case 0xD5: case 0xE5: case 0xF5: {             /* PUSH */
            uint16_t v = (op == 0xF5) ? AF : rd_r16(g, (op >> 4) & 3);
            push16(g, v);
            return 16;
        }
        case 0xC6: case 0xCE: case 0xD6: case 0xDE:
        case 0xE6: case 0xEE: case 0xF6: case 0xFE: {             /* ALU A,n */
            uint8_t v = gb_read8(g, g->cpu.pc++);
            switch ((op >> 3) & 7) {
                case 0: { uint16_t r = (uint16_t)(A + v);
                    F = (uint8_t)(((r & 0xFF) == 0 ? FZ : 0) | ((r > 0xFF) ? FC : 0) |
                                  (((A & 0xF) + (v & 0xF)) > 0xF ? FH : 0)); A = (uint8_t)r; break; }
                case 1: { uint8_t c = (F & FC) ? 1 : 0; uint16_t r = (uint16_t)(A + v + c);
                    F = (uint8_t)(((r & 0xFF) == 0 ? FZ : 0) | ((r > 0xFF) ? FC : 0) |
                                  (((A & 0xF) + (v & 0xF) + c) > 0xF ? FH : 0)); A = (uint8_t)r; break; }
                case 2: { uint16_t r = (uint16_t)(A - v);
                    F = (uint8_t)(FN | ((r & 0xFF) == 0 ? FZ : 0) | ((r > 0xFF) ? FC : 0) |
                                  ((A & 0xF) < (v & 0xF) ? FH : 0)); A = (uint8_t)r; break; }
                case 3: { uint8_t c = (F & FC) ? 1 : 0; uint16_t r = (uint16_t)(A - v - c);
                    F = (uint8_t)(FN | ((r & 0xFF) == 0 ? FZ : 0) | ((r > 0xFF) ? FC : 0) |
                                  ((A & 0xF) < ((v & 0xF) + c) ? FH : 0)); A = (uint8_t)r; break; }
                case 4: A = (uint8_t)(A & v); F = (uint8_t)(FH | (A ? 0 : FZ)); break;
                case 5: A = (uint8_t)(A ^ v); F = (uint8_t)(A ? 0 : FZ); break;
                case 6: A = (uint8_t)(A | v); F = (uint8_t)(A ? 0 : FZ); break;
                case 7: { uint16_t r = (uint16_t)(A - v);
                    F = (uint8_t)(FN | ((r & 0xFF) == 0 ? FZ : 0) | ((r > 0xFF) ? FC : 0) |
                                  ((A & 0xF) < (v & 0xF) ? FH : 0)); break; }
                default: break;
            }
            return 8;
        }
        case 0xC7: case 0xCF: case 0xD7: case 0xDF:
        case 0xE7: case 0xEF: case 0xF7: case 0xFF:               /* RST */
            push16(g, g->cpu.pc);
            g->cpu.pc = (uint16_t)(op & 0x38);
            return 16;

        case 0xE0: { /* LDH (n),A */
            uint8_t n = gb_read8(g, g->cpu.pc++);
            gb_write8(g, (uint16_t)(0xFF00 + n), A);
            return 12;
        }
        case 0xF0: { /* LDH A,(n) */
            uint8_t n = gb_read8(g, g->cpu.pc++);
            A = gb_read8(g, (uint16_t)(0xFF00 + n));
            return 12;
        }
        case 0xE2: gb_write8(g, (uint16_t)(0xFF00 + C), A); return 8;
        case 0xF2: A = gb_read8(g, (uint16_t)(0xFF00 + C)); return 8;
        case 0xEA: { /* LD (nn),A */
            lo = gb_read8(g, g->cpu.pc++);
            hi = gb_read8(g, g->cpu.pc++);
            gb_write8(g, (uint16_t)((hi << 8) | lo), A);
            return 16;
        }
        case 0xFA: { /* LD A,(nn) */
            lo = gb_read8(g, g->cpu.pc++);
            hi = gb_read8(g, g->cpu.pc++);
            A = gb_read8(g, (uint16_t)((hi << 8) | lo));
            return 16;
        }
        case 0xF8: { /* LD HL,SP+r8 */
            int8_t d = (int8_t)gb_read8(g, g->cpu.pc++);
            uint16_t r = (uint16_t)(g->cpu.sp + d);
            F = (uint8_t)((((g->cpu.sp & 0xFF) + (uint8_t)d) > 0xFF ? FC : 0) |
                          (((g->cpu.sp & 0xF) + (d & 0xF)) > 0xF ? FH : 0));
            set_hl(g, r);
            return 12;
        }
        case 0xF9: g->cpu.sp = HL; return 8;
        case 0xE8: { /* ADD SP,r8 */
            int8_t d = (int8_t)gb_read8(g, g->cpu.pc++);
            uint16_t r = (uint16_t)(g->cpu.sp + d);
            F = (uint8_t)((((g->cpu.sp & 0xFF) + (uint8_t)d) > 0xFF ? FC : 0) |
                          (((g->cpu.sp & 0xF) + (d & 0xF)) > 0xF ? FH : 0));
            g->cpu.sp = r;
            return 16;
        }
        case 0xCB: { /* CB prefix: rotates/shifts, BIT, RES, SET */
            uint8_t cb = gb_read8(g, g->cpu.pc++);
            int i = cb & 7;                 /* register operand            */
            uint8_t v = rd_r(g, i);
            uint8_t res = v;
            int cycles = (i == 6) ? 16 : 8; /* (HL) costs an extra access  */

            if (cb < 0x40) {
                /* --- rotates and shifts: op = (cb >> 3) & 7 ------------- */
                switch ((cb >> 3) & 7) {
                    case 0: { /* RLC */
                        uint8_t c = (uint8_t)(v >> 7);
                        res = (uint8_t)((v << 1) | c);
                        F = (uint8_t)((res ? 0 : FZ) | (c ? FC : 0));
                        break;
                    }
                    case 1: { /* RRC */
                        uint8_t c = (uint8_t)(v & 1);
                        res = (uint8_t)((v >> 1) | (c << 7));
                        F = (uint8_t)((res ? 0 : FZ) | (c ? FC : 0));
                        break;
                    }
                    case 2: { /* RL */
                        uint8_t c = (uint8_t)(v >> 7);
                        res = (uint8_t)((v << 1) | ((F & FC) ? 1 : 0));
                        F = (uint8_t)((res ? 0 : FZ) | (c ? FC : 0));
                        break;
                    }
                    case 3: { /* RR */
                        uint8_t c = (uint8_t)(v & 1);
                        res = (uint8_t)((v >> 1) | ((F & FC) ? 0x80 : 0));
                        F = (uint8_t)((res ? 0 : FZ) | (c ? FC : 0));
                        break;
                    }
                    case 4: { /* SLA */
                        uint8_t c = (uint8_t)(v >> 7);
                        res = (uint8_t)(v << 1);
                        F = (uint8_t)((res ? 0 : FZ) | (c ? FC : 0));
                        break;
                    }
                    case 5: { /* SRA */
                        uint8_t c = (uint8_t)(v & 1);
                        res = (uint8_t)((v >> 1) | (v & 0x80));
                        F = (uint8_t)((res ? 0 : FZ) | (c ? FC : 0));
                        break;
                    }
                    case 6: { /* SWAP */
                        res = (uint8_t)((v >> 4) | (v << 4));
                        F = (uint8_t)(res ? 0 : FZ);
                        break;
                    }
                    default: { /* SRL */
                        uint8_t c = (uint8_t)(v & 1);
                        res = (uint8_t)(v >> 1);
                        F = (uint8_t)((res ? 0 : FZ) | (c ? FC : 0));
                        break;
                    }
                }
                wr_r(g, i, res);
                return cycles;
            }

            int bit = (cb >> 3) & 7;
            if (cb < 0x80) {
                /* --- BIT: Z set from the tested bit, N=0, H=1, C kept --- */
                F = (uint8_t)((F & FC) | FH | ((v & (1 << bit)) ? 0 : FZ));
                return (i == 6) ? 12 : 8;   /* BIT (HL) does not write back */
            }
            if (cb < 0xC0) res = (uint8_t)(v & ~(1 << bit));   /* RES */
            else           res = (uint8_t)(v |  (1 << bit));   /* SET */
            wr_r(g, i, res);
            return cycles;
        }
        default:
            /* undocumented / invalid -> treat as NOP */
            return 4;
    }
}

/* ------------------------------------------------------- public: init   */

static uint32_t crc32_buf(const uint8_t *d, size_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        crc ^= d[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
    }
    return ~crc;
}

void gb_init(void)
{
    memset(&g_gb, 0, sizeof(g_gb));
    g_gb.speed_target = 100;
    g_gb.model = GB_MODEL_AUTO;
    g_gb.color_mode = GB_COLOR_CGB;
    g_gb.audio_mode = GB_AUDIO_ENHANCED;
    g_gb.joyp = 0x30;
    g_gb.wram_bank = 1;
}

void gb_shutdown(void)
{
    g_gb.loaded = 0;
}

uint8_t *gb_rom_buffer(void) { return g_rom; }

int gb_load_rom(size_t size)
{
    if (size < 0x8000 || size > GB_ROM_MAX) return -1;

    /* decode title (0x134 - 0x143) */
    memset(g_rom_info_title, 0, sizeof(g_rom_info_title));
    for (int i = 0; i < 16; i++) {
        uint8_t c = g_rom[0x134 + i];
        if (c == 0 || c == 0x80) break;
        if (c < 0x20 || c > 0x7E) c = '_';
        g_rom_info_title[i] = (char)c;
    }
    g_rom_info_cart = g_rom[0x0147];
    g_rom_info_cgb = (g_rom[0x0143] & 0x80) ? 1 : 0;
    if ((g_rom[0x0143] & 0xC0) == 0xC0) g_rom_info_cgb = 2; /* CGB-only */
    g_rom_info_sgb = (g_rom[0x0146] == 0x03) ? 1 : 0;
    g_rom_size = (uint32_t)size;
    g_rom_crc = crc32_buf(g_rom, size);

    static const uint16_t bank_tab[] = { 2, 4, 8, 16, 32, 64, 128, 256, 512 };
    uint8_t rc = g_rom[0x0148];
    g_rom_info_rombanks = (rc < 9) ? bank_tab[rc] : 2;
    switch (g_rom[0x0149]) {
        case 0: g_rom_info_rambanks = 0; break;
        case 1: g_rom_info_rambanks = 1; break;
        case 2: g_rom_info_rambanks = 1; break;
        case 3: g_rom_info_rambanks = 4; break;
        case 4: g_rom_info_rambanks = 16; break;
        case 5: g_rom_info_rambanks = 8; break;
        default: g_rom_info_rambanks = 1; break;
    }

    gb_reset();
    g_gb.loaded = 1;
    return 0;
}

int gb_is_loaded(void) { return g_gb.loaded; }

void gb_get_rom_info(char *title, int cap, int *cart_type, int *rom_banks,
                     int *ram_banks, int *cgb, int *sgb)
{
    if (title && cap > 0) {
        int i = 0;
        for (; i < cap - 1 && g_rom_info_title[i]; i++) title[i] = g_rom_info_title[i];
        title[i] = 0;
    }
    if (cart_type) *cart_type = g_rom_info_cart;
    if (rom_banks) *rom_banks = g_rom_info_rombanks;
    if (ram_banks) *ram_banks = g_rom_info_rambanks;
    if (cgb) *cgb = g_rom_info_cgb;
    if (sgb) *sgb = g_rom_info_sgb;
}

uint32_t gb_rom_crc32(void) { return g_rom_crc; }

/* Re-derive the cartridge identity from the bytes currently in RAM.  Called
 * after a mod patch mutates the image: without it a patched ROM would still
 * advertise the *base* CRC, and save states made under one mod set would be
 * accepted by a different one (gb_load_state guards on this value).         */
void gb_rom_recalc_crc(void)
{
    if (g_rom_size) g_rom_crc = crc32_buf(g_rom, g_rom_size);
}
uint8_t *gb_rom_data(void) { return g_rom; }
size_t gb_rom_size(void) { return g_rom_size; }

void gb_set_model(int model)
{
    g_gb.model = (uint8_t)model;
}

void gb_reset(void)
{
    gb_state_t *g = &g_gb;
    int cgb = 0;
    if (g->model == GB_MODEL_CGB) cgb = 1;
    else if (g->model == GB_MODEL_AUTO) cgb = (g_rom[0x0143] & 0x80) ? 1 : 0;

    memset(g->vram, 0, sizeof(g->vram));
    memset(g->wram, 0, sizeof(g->wram));
    memset(g->oam, 0, sizeof(g->oam));
    memset(g->io, 0, sizeof(g->io));
    memset(g->hram, 0, sizeof(g->hram));
    memset(g->fb, 0, sizeof(g->fb));

    g->cgb_mode = (uint8_t)cgb;
    g->vram_bank = 0;
    g->wram_bank = 1;
    g->speed = 0;
    g->speed_prepare = 0;
    g->div = 0;
    g->tima_reload = 0;
    g->joyp = 0x30;
    g->dma_active = 0;
    g->serial_bits = 0;
    g->ie = 0;
    g->buttons = 0;
    g->frames = 0;
    g->cycles_frame = 0;

    /* IO power-on defaults (skipping the boot ROM) */
    g->io[0x00] = 0xCF; /* P1  : nothing pressed   */
    g->io[0x05] = 0x00; /* TIMA */
    g->io[0x06] = 0x00; /* TMA  */
    g->io[0x07] = 0x00; /* TAC  */
    g->io[0x0F] = 0x00; /* IF   */
    g->io[0x10] = 0x80; g->io[0x11] = 0xBF; g->io[0x12] = 0xF3;
    g->io[0x14] = 0xBF; g->io[0x16] = 0x3F; g->io[0x17] = 0x00;
    g->io[0x19] = 0xBF; g->io[0x1A] = 0x7F; g->io[0x1B] = 0xFF;
    g->io[0x1C] = 0x9F; g->io[0x1E] = 0xBF; g->io[0x20] = 0xFF;
    g->io[0x21] = 0x00; g->io[0x22] = 0x00; g->io[0x23] = 0xBF;
    g->io[0x24] = 0x77; g->io[0x25] = 0xF3; g->io[0x26] = 0xF1;
    g->io[0x40] = 0x91; g->io[0x41] = 0x85; g->io[0x42] = 0x00;
    g->io[0x43] = 0x00; g->io[0x44] = 0x00; g->io[0x45] = 0x00;
    g->io[0x46] = 0xFF; g->io[0x47] = 0xFC; g->io[0x48] = 0xFF;
    g->io[0x49] = 0xFF; g->io[0x4A] = 0x00; g->io[0x4B] = 0x00;
    g->io[0x4D] = 0xFF;
    g->io[0x4F] = 0xFF;
    g->io[0x56] = 0x02;
    g->io[0x68] = 0xFF; g->io[0x6A] = 0xFF;
    g->io[0x70] = 0xFF;

    /* wave RAM pattern (as the boot ROM leaves it) */
    {
        static const uint8_t wave[16] = {
            0x84,0x40,0x43,0xAA,0x2D,0x78,0x92,0x3C,
            0x60,0x59,0x59,0xB0,0x34,0xB8,0x2E,0xDA
        };
        memcpy(&g->io[0x30], wave, 16);
    }

    /* Cartridge / mapper state comes from the header that gb_load_rom()
     * staged into the ROM buffer.  Without this the MBC is left zeroed and
     * bank 1 reads alias bank 0.                                          */
    gb_cart_reset(g, g_rom, g_rom_size ? g_rom_size : 0x8000);

    gb_cpu_reset(g, cgb);
    gb_ppu_reset(g);
    gb_apu_reset(g);

    /* CGB palettes power-on state (white-ish ramp) */
    if (cgb) {
        for (int i = 0; i < 0x40; i++) { g->bgpd[i] = 0xFF; g->obpd[i] = 0xFF; }
    }

    g->apu.nr50_vol_l = 7; g->apu.nr50_vol_r = 7;
    g->apu.nr51_sel = 0xF3;
    g->apu.apu_on = 1;
}

void gb_set_input(uint16_t buttons) { g_gb.buttons = buttons; }

const uint32_t *gb_framebuffer(void) { return g_gb.fb; }
