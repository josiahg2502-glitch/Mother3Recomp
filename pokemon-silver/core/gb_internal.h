/* ==========================================================================
 *  gb_internal.h -- internal state shared by the core translation units
 * ========================================================================== */
#ifndef GB_INTERNAL_H
#define GB_INTERNAL_H

#include "gb.h"

#define GB_CYCLES_PER_FRAME 70224     /* 4.194304 MHz / 59.7275 Hz          */
#define GB_APU_HZ         1048576u    /* one APU tick per 4 T-cycles        */
#define GB_APU_TICK_CYCLES 4u

/* ------------------------------------------------------------------ CPU  */
typedef struct {
    uint16_t pc, sp;
    uint8_t  a, b, c, d, e, f, h, l;
    uint8_t  ime;          /* master interrupt enable                      */
    uint8_t  ime_pending;  /* EI takes effect after the next instruction   */
    uint8_t  halt;         /* HALT entered                                 */
    uint8_t  halt_bug;     /* the famous HALT/IME=0 quirk                  */
    uint8_t  stop;         /* STOP entered (speed switch pending on CGB)   */
} gb_cpu_t;

/* ------------------------------------------------------------------ PPU  */
typedef struct {
    uint32_t mode;         /* 0 hblank, 1 vblank, 2 oam, 3 transfer        */
    uint32_t line_cycles;  /* T-cycles elapsed on the current scanline     */
    uint32_t mode3_len;    /* pixel-transfer length for the current line   */
    uint8_t  line_rendered;
    uint8_t  prev_stat_irq;
    uint8_t  in_window;    /* WY==LY latched for this frame                */
    uint8_t  frame_done;
    uint16_t win_line;     /* internal window line counter                 */
    uint8_t  hdma_active;
    uint16_t hdma_src, hdma_dst;
    uint16_t hdma_left;
    uint8_t  hdma_hblank;
} gb_ppu_t;

/* ------------------------------------------------------------------ APU  */
typedef struct {
    /* square 1 */
    uint8_t  sq1_on, sq1_duty, sq1_vol, sq1_env_dir, sq1_env_period;
    uint16_t sq1_freq;
    uint8_t  sq1_sweep_period, sq1_sweep_dir, sq1_sweep_shift;
    uint8_t  sq1_len, sq1_len_en;
    uint32_t sq1_freq_timer, sq1_duty_pos, sq1_env_timer, sq1_sweep_timer;
    uint8_t  sq1_env_vol, sq1_sweep_en, sq1_sweep_shadow, sq1_dac;

    /* square 2 */
    uint8_t  sq2_on, sq2_duty, sq2_vol, sq2_env_dir, sq2_env_period;
    uint16_t sq2_freq;
    uint8_t  sq2_len, sq2_len_en;
    uint32_t sq2_freq_timer, sq2_duty_pos, sq2_env_timer, sq2_env_vol, sq2_dac;

    /* wave */
    uint8_t  wave_on, wave_vol_code, wave_len_en;
    uint16_t wave_len;
    uint16_t wave_freq;
    uint32_t wave_freq_timer, wave_pos, wave_len_timer;
    uint8_t  wave_dac;

    /* noise */
    uint8_t  noise_on, noise_vol, noise_env_dir, noise_env_period;
    uint8_t  noise_shift, noise_width, noise_div;
    uint8_t  noise_len, noise_len_en;
    uint32_t noise_timer, noise_env_timer, noise_env_vol, noise_lfsr;
    uint8_t  noise_dac;

    /* mixer */
    uint8_t  nr50_vol_l, nr50_vol_r;
    uint8_t  nr51_sel;
    uint8_t  apu_on;

    /* frame sequencer */
    uint32_t fs_timer;
    uint8_t  fs_step;

    /* sub-frame cycle accumulator (transient, not part of a save state)   */
    uint32_t cycles_acc;
    uint8_t  pad1[3];
} gb_apu_t;

/* The output ring + resampler are transient: they are deliberately kept
 * outside gb_state_t so save states stay small and portable.              */
typedef struct {
    int16_t  *buf;
    uint32_t  cap;         /* stereo frames                                */
    uint32_t  head, tail, count;
    float     pos;         /* fractional read cursor                       */
    /* post-processing state */
    float     dc_l, dc_r;
    float     lp_l, lp_r;
    float     hs_l, hs_r;  /* high shelf                                   */
    float     x_ls_l, x_ls_r;
    float     volume;
    int       mode;
    int       out_rate;
} gb_audio_out_t;

extern gb_audio_out_t g_audio_out;

/* ------------------------------------------------------------------ RTC  */
typedef struct {
    uint64_t total;           /* seconds since epoch-of-cart, free running  */
    uint8_t  s, m, h, dl, dh; /* decomposed view / RTC register mirrors    */
    uint8_t  halt;            /* RTC halted (day-high bit 6)               */
    uint8_t  carry;           /* day overflow (day-high bit 7)             */
    uint8_t  latch_state;     /* 0 = none, 1 = primed by the 00->01 pulse  */
    uint64_t last_unix;       /* wall clock when we last synced            */
    int      present;
} gb_rtc_t;

/* --------------------------------------------------------------- cart    */
typedef struct {
    uint8_t  type;
    uint8_t  mbc;
    uint8_t  has_ram, has_battery, has_rtc, has_rumble;
    uint32_t rom_banks, ram_banks, ram_size;
    uint8_t  rom_bank;        /* bank at 0x4000-0x7FFF                     */
    uint16_t rom_bank_hi;     /* high bits for > 2MB (MBC1 multicart)      */
    uint8_t  ram_bank;
    uint8_t  ram_enable;
    uint8_t  banking_mode;    /* MBC1: 0 = ROM banking, 1 = RAM banking    */
    uint8_t  mbc1_zero_hi;    /* 0x4000-0x5FFF mirror high bits            */
} gb_cart_t;

/* ---------------------------------------------------------------- state  */
typedef struct {
    /* memory */
    uint8_t  vram[2][0x2000];
    uint8_t  wram[8][0x1000];
    uint8_t  oam[0xA0];
    uint8_t  io[0x80];        /* FF00 - FF7F                               */
    uint8_t  hram[0x80];      /* FF80 - FFFF (IE is hram[0x7F])            */
    uint8_t  cram[GB_CRAM_MAX];
    uint8_t  bgpd[0x40], obpd[0x40];
    uint8_t  ie;

    /* subsystems */
    gb_cpu_t  cpu;
    gb_ppu_t  ppu;
    gb_apu_t  apu;
    gb_cart_t cart;
    gb_rtc_t  rtc;

    /* machine */
    uint32_t div;             /* 16 bit internal DIV counter (low 16 used) */
    uint8_t  tima_reload;     /* cycles left of the TIMA=TMA delay        */
    uint8_t  tima_pending;    /* value queued into TIMA                   */
    uint8_t  serial_bits;     /* remaining serial transfer bits           */
    uint32_t serial_timer;
    uint16_t dma_src;         /* OAM DMA state                            */
    uint8_t  dma_active, dma_pos;
    uint32_t dma_cycles_left;
    uint8_t  joyp;            /* P1 selection bits                        */
    uint16_t buttons;         /* current input                            */
    uint8_t  vram_bank, wram_bank;
    uint8_t  speed;           /* CGB double speed                          */
    uint8_t  speed_prepare;
    uint8_t  model;           /* effective hardware model                 */
    uint8_t  cgb_mode;        /* running in CGB mode                      */
    uint8_t  loaded;

    /* misc config mirrored here so it survives in states */
    uint8_t  color_mode;
    uint8_t  audio_mode;
    uint8_t  dbg_raw_io;    /* test rig: make I/O reads/writes transparent */
    uint8_t  dbg_flat;      /* test rig: flat 64 KiB RAM memory model      */
    uint8_t  pad0;

    /* front-end counters */
    uint32_t frames;
    uint32_t cycles_frame;
    uint32_t speed_target;    /* cycles to emulate per rendered frame      */
    uint32_t speed_acc;

    /* Rendering output.  Deliberately LAST: save states serialise every
     * byte of gb_state_t *before* the framebuffer, so a state stays small
     * and the frame is simply redrawn on load.                            */
    uint32_t fb[GB_LCD_W * GB_LCD_H];
} gb_state_t;

extern gb_state_t g_gb;

/* --- cross-module entry points ----------------------------------------- */
void gb_cpu_reset(gb_state_t *g, int cgb);
int  gb_cpu_step(gb_state_t *g);                 /* returns T-cycles       */
void gb_advance(gb_state_t *g, uint32_t cycles); /* timers/ppu/apu/dma     */

void gb_ppu_reset(gb_state_t *g);
void gb_ppu_step(gb_state_t *g, uint32_t cycles);
void gb_ppu_render_line(gb_state_t *g, uint8_t ly);
uint32_t gb_ppu_next_event(const gb_state_t *g);

void gb_apu_reset(gb_state_t *g);
void gb_apu_step(gb_state_t *g, uint32_t cycles);
void gb_apu_write(gb_state_t *g, uint16_t addr, uint8_t val);
uint8_t gb_apu_read(gb_state_t *g, uint16_t addr);
void gb_apu_render(gb_state_t *g, int16_t *out, int frames);

void gb_cart_reset(gb_state_t *g, const uint8_t *rom, size_t size);
void gb_cart_write(gb_state_t *g, uint16_t addr, uint8_t val);
void gb_rtc_tick(gb_state_t *g, uint64_t now);

void     gb_rom_recalc_crc(void);
uint8_t  gb_read8(gb_state_t *g, uint16_t addr);
void     gb_write8(gb_state_t *g, uint16_t addr, uint8_t val);
uint8_t *gb_vram_ptr(gb_state_t *g, uint16_t addr);

/* Flat, side-effect-free memory model used by the single-step test rig.
 * In flat mode the whole 0x0000-0xFFFF space is one plain array: no MBC, no
 * echo aliasing, no I/O read-back masks -- exactly the model the vectors were
 * generated against.                                                       */
extern uint8_t g_flat_ram[0x10000];
/* In flat mode IF/IE are standalone registers (as in the reference model),
 * deliberately NOT aliased onto g_flat_ram[$FF0F]/[$FFFF] -- several vectors
 * stage ordinary code or data at those addresses.                          */
extern uint8_t g_flat_if;
extern uint8_t g_flat_ie;
uint8_t  gb_dbg_read(uint16_t addr);
void     gb_dbg_write(uint16_t addr, uint8_t val);

#endif /* GB_INTERNAL_H */
