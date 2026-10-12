/* ==========================================================================
 *  apu.c -- DMG/CGB sound: 2 squares, wave, noise, frame sequencer, mixer
 *           plus the 16-bit enhanced audio path used by the PC port.
 *
 *  The APU is clocked once every 4 T-cycles (1.048576 MHz).  Samples land in
 *  a large transient ring buffer and gb_apu_render() decimates them to the
 *  host rate with a box anti-alias filter, then applies the selected
 *  personality (authentic / enhanced / crystal).
 * ========================================================================== */
#include "gb_string.h"
#include "gb_internal.h"

gb_audio_out_t g_audio_out;

/* ---------------------------------------------------------------- duty   */

static const uint8_t duty_table[4][8] = {
    { 0, 0, 0, 0, 0, 0, 0, 1 },   /* 12.5% */
    { 1, 0, 0, 0, 0, 0, 0, 1 },   /* 25%   */
    { 1, 0, 0, 0, 0, 1, 1, 1 },   /* 50%   */
    { 0, 1, 1, 1, 1, 1, 1, 0 },   /* 75%   */
};

/* ------------------------------------------------------------ ring buffer */

#define RING_FRAMES (GB_APU_HZ / 60 * 6)   /* ~6 frames of headroom        */

/* Static, not malloc'd: the core must link against wasm32-freestanding,
 * which has no heap.  ~210 KiB is a fair trade for zero dependencies.     */
static int16_t g_ring_storage[RING_FRAMES * 2];

static void ring_init(void)
{
    if (g_audio_out.buf) return;
    g_audio_out.cap = RING_FRAMES;
    g_audio_out.buf = g_ring_storage;
    g_audio_out.head = g_audio_out.tail = g_audio_out.count = 0;
    g_audio_out.volume = 1.0f;
    g_audio_out.mode = GB_AUDIO_ENHANCED;
    g_audio_out.out_rate = 44100;
}

static inline void ring_push(float l, float r)
{
    ring_init();
    if (!g_audio_out.buf) return;
    if (g_audio_out.count == g_audio_out.cap) {
        /* overrun: drop the oldest sample rather than stalling the core */
        g_audio_out.tail = (g_audio_out.tail + 1) % g_audio_out.cap;
        g_audio_out.count--;
    }
    int16_t *p = &g_audio_out.buf[g_audio_out.head * 2];
    int li = (int)(l * 32767.0f);
    int ri = (int)(r * 32767.0f);
    p[0] = (int16_t)(li > 32767 ? 32767 : (li < -32768 ? -32768 : li));
    p[1] = (int16_t)(ri > 32767 ? 32767 : (ri < -32768 ? -32768 : ri));
    g_audio_out.head = (g_audio_out.head + 1) % g_audio_out.cap;
    g_audio_out.count++;
}

static inline float ring_at(uint32_t back, int ch)
{
    uint32_t idx = (g_audio_out.tail + back) % g_audio_out.cap;
    return (float)g_audio_out.buf[idx * 2 + ch] / 32768.0f;
}

static inline void ring_pop(void)
{
    if (!g_audio_out.count) return;
    g_audio_out.tail = (g_audio_out.tail + 1) % g_audio_out.cap;
    g_audio_out.count--;
}

/* ------------------------------------------------------------ frame seq  */

static void clock_length(gb_state_t *g)
{
    gb_apu_t *a = &g->apu;
    if (a->sq1_len_en && a->sq1_len > 0 && --a->sq1_len == 0) a->sq1_on = 0;
    if (a->sq2_len_en && a->sq2_len > 0 && --a->sq2_len == 0) a->sq2_on = 0;
    if (a->wave_len_en && a->wave_len > 0 && --a->wave_len == 0) a->wave_on = 0;
    if (a->noise_len_en && a->noise_len > 0 && --a->noise_len == 0) a->noise_on = 0;
}

static void clock_sweep(gb_state_t *g)
{
    gb_apu_t *a = &g->apu;
    if (!a->sq1_sweep_period) return;
    if (a->sq1_sweep_timer > 0) a->sq1_sweep_timer--;
    if (a->sq1_sweep_timer) return;
    a->sq1_sweep_timer = a->sq1_sweep_period ? a->sq1_sweep_period : 8;

    if (!a->sq1_sweep_en) return;
    uint16_t delta = (uint16_t)(a->sq1_sweep_shadow >> a->sq1_sweep_shift);
    if (a->sq1_sweep_dir) {
        if (delta > a->sq1_sweep_shadow) { a->sq1_on = 0; return; }
        a->sq1_sweep_shadow = (uint16_t)(a->sq1_sweep_shadow - delta);
    } else {
        uint32_t sum = (uint32_t)a->sq1_sweep_shadow + delta;
        if (sum > 2047) { a->sq1_on = 0; return; }
        a->sq1_sweep_shadow = (uint16_t)sum;
    }
    if (a->sq1_sweep_shift) {
        a->sq1_freq = a->sq1_sweep_shadow;
        /* overflow check disables the channel */
        uint32_t check = (uint32_t)a->sq1_sweep_shadow +
                         (a->sq1_sweep_shadow >> a->sq1_sweep_shift);
        if (a->sq1_sweep_dir == 0 && check > 2047) a->sq1_on = 0;
    }
}

static void clock_envelope(gb_state_t *g)
{
    gb_apu_t *a = &g->apu;
    if (a->sq1_env_period && a->sq1_on) {
        if (++a->sq1_env_timer >= a->sq1_env_period) {
            a->sq1_env_timer = 0;
            if (a->sq1_env_dir && a->sq1_env_vol < 15) a->sq1_env_vol++;
            else if (!a->sq1_env_dir && a->sq1_env_vol > 0) a->sq1_env_vol--;
            a->sq1_vol = (uint8_t)a->sq1_env_vol;
        }
    }
    if (a->sq2_env_period && a->sq2_on) {
        if (++a->sq2_env_timer >= a->sq2_env_period) {
            a->sq2_env_timer = 0;
            if (a->sq2_env_dir && a->sq2_env_vol < 15) a->sq2_env_vol++;
            else if (!a->sq2_env_dir && a->sq2_env_vol > 0) a->sq2_env_vol--;
            a->sq2_vol = (uint8_t)a->sq2_env_vol;
        }
    }
    if (a->noise_env_period && a->noise_on) {
        if (++a->noise_env_timer >= a->noise_env_period) {
            a->noise_env_timer = 0;
            if (a->noise_env_dir && a->noise_env_vol < 15) a->noise_env_vol++;
            else if (!a->noise_env_dir && a->noise_env_vol > 0) a->noise_env_vol--;
            a->noise_vol = (uint8_t)a->noise_env_vol;
        }
    }
}

/* ---------------------------------------------------------------- tick   */

static void apu_tick(gb_state_t *g)
{
    gb_apu_t *a = &g->apu;

    if (!a->apu_on) {
        ring_push(0.0f, 0.0f);
        return;
    }

    /* --- frame sequencer (512 Hz -> one step per 2048 ticks) ------- */
    if (++a->fs_timer >= 2048) {
        a->fs_timer = 0;
        switch (a->fs_step & 7) {
            case 0: case 4: clock_length(g); break;
            case 2: case 6: clock_length(g); clock_sweep(g); break;
            case 7: clock_envelope(g); break;
            default: break;
        }
        a->fs_step = (uint8_t)((a->fs_step + 1) & 7);
    }

    /* --- square 1 -------------------------------------------------- */
    float s1 = 0.0f;
    if (a->sq1_dac) {
        if (a->sq1_freq_timer == 0) {
            a->sq1_freq_timer = 2048 - a->sq1_freq;
            a->sq1_duty_pos = (uint8_t)((a->sq1_duty_pos + 1) & 7);
        } else a->sq1_freq_timer--;
        if (a->sq1_on && duty_table[a->sq1_duty][a->sq1_duty_pos])
            s1 = (float)a->sq1_env_vol / 15.0f;
        else if (a->sq1_on)
            s1 = 0.0f;
    }

    /* --- square 2 -------------------------------------------------- */
    float s2 = 0.0f;
    if (a->sq2_dac) {
        if (a->sq2_freq_timer == 0) {
            a->sq2_freq_timer = 2048 - a->sq2_freq;
            a->sq2_duty_pos = (uint8_t)((a->sq2_duty_pos + 1) & 7);
        } else a->sq2_freq_timer--;
        if (a->sq2_on && duty_table[a->sq2_duty][a->sq2_duty_pos])
            s2 = (float)a->sq2_env_vol / 15.0f;
    }

    /* --- wave ------------------------------------------------------ */
    float s3 = 0.0f;
    if (a->wave_dac) {
        if (a->wave_freq_timer == 0) {
            a->wave_freq_timer = 2048 - a->wave_freq;
            a->wave_pos = (a->wave_pos + 1) & 31;
        } else a->wave_freq_timer--;
        if (a->wave_on) {
            uint8_t b = g->io[0x30 + (a->wave_pos >> 1)];
            uint8_t nib = (a->wave_pos & 1) ? (uint8_t)(b & 0x0F) : (uint8_t)(b >> 4);
            static const uint8_t vol_shift[4] = { 4, 0, 1, 2 };
            nib = (uint8_t)(nib >> vol_shift[a->wave_vol_code & 3]);
            s3 = (float)nib / 15.0f;
        }
    }

    /* --- noise ----------------------------------------------------- */
    float s4 = 0.0f;
    if (a->noise_dac) {
        uint32_t div = a->noise_div ? (uint32_t)a->noise_div * 16 : 8;
        uint32_t period = div << a->noise_shift;
        if (period == 0) period = 1;
        if (a->noise_timer == 0) {
            a->noise_timer = period;
            uint16_t xor_bit = (uint16_t)((a->noise_lfsr ^ (a->noise_lfsr >> 1)) & 1);
            a->noise_lfsr = (uint16_t)((a->noise_lfsr >> 1) | (xor_bit << 14));
            if (a->noise_width) a->noise_lfsr = (uint16_t)((a->noise_lfsr & ~(1u << 6)) | (xor_bit << 6));
        } else a->noise_timer--;
        if (a->noise_on && !(a->noise_lfsr & 1))
            s4 = (float)a->noise_env_vol / 15.0f;
    }

    /* --- DAC conversion: unsigned 0..15 -> signed -1..1 ------------- */
    float d1 = a->sq1_dac   ? (s1 * 2.0f - 1.0f) : 0.0f;
    float d2 = a->sq2_dac   ? (s2 * 2.0f - 1.0f) : 0.0f;
    float d3 = a->wave_dac  ? (s3 * 2.0f - 1.0f) : 0.0f;
    float d4 = a->noise_dac ? (s4 * 2.0f - 1.0f) : 0.0f;

    /* --- NR51 panning + NR50 master volume ------------------------- */
    uint8_t sel = a->nr51_sel;
    float l = 0.0f, r = 0.0f;
    if (sel & 0x10) l += d1;
    if (sel & 0x01) r += d1;
    if (sel & 0x20) l += d2;
    if (sel & 0x02) r += d2;
    if (sel & 0x40) l += d3;
    if (sel & 0x04) r += d3;
    if (sel & 0x80) l += d4;
    if (sel & 0x08) r += d4;

    l *= (float)(a->nr50_vol_l + 1) / 8.0f;
    r *= (float)(a->nr50_vol_r + 1) / 8.0f;

    /* four channels summed, normalised so full scale stays inside int16 */
    ring_push(l * 0.22f, r * 0.22f);
}

void gb_apu_step(gb_state_t *g, uint32_t cycles)
{
    gb_apu_t *a = &g->apu;
    a->cycles_acc += cycles;
    while (a->cycles_acc >= GB_APU_TICK_CYCLES) {
        a->cycles_acc -= GB_APU_TICK_CYCLES;
        apu_tick(g);
    }
}

/* --------------------------------------------------------------- regs   */

uint8_t gb_apu_read(gb_state_t *g, uint16_t addr)
{
    gb_apu_t *a = &g->apu;
    uint8_t v = g->io[addr - 0xFF00];
    switch (addr) {
        case 0xFF10: return (uint8_t)(v | 0x80);
        case 0xFF11: return (uint8_t)(v | 0x3F);
        case 0xFF14: return (uint8_t)(v | 0xBF);
        case 0xFF16: return (uint8_t)(v | 0x3F);
        case 0xFF19: return (uint8_t)(v | 0xBF);
        case 0xFF1A: return (uint8_t)(v | 0x7F);
        case 0xFF1B: return (uint8_t)(v | 0xFF);
        case 0xFF1E: return (uint8_t)(v | 0xBF);
        case 0xFF20: return (uint8_t)(v | 0xFF);
        case 0xFF23: return (uint8_t)(v | 0xBF);
        case 0xFF26: {
            uint8_t on = 0;
            if (a->sq1_on && a->sq1_dac)   on |= 0x01;
            if (a->sq2_on && a->sq2_dac)   on |= 0x02;
            if (a->wave_on && a->wave_dac) on |= 0x04;
            if (a->noise_on && a->noise_dac) on |= 0x08;
            return (uint8_t)(on | (a->apu_on ? 0x80 : 0) | 0x70);
        }
        default: return v;
    }
}

void gb_apu_write(gb_state_t *g, uint16_t addr, uint8_t val)
{
    gb_apu_t *a = &g->apu;

    if (addr == 0xFF26) {                       /* NR52 power */
        uint8_t on = (uint8_t)((val >> 7) & 1);
        if (on && !a->apu_on) {
            /* power up: reset the frame sequencer */
            a->fs_timer = 0;
            a->fs_step = 0;
        }
        if (!on && a->apu_on) {
            /* power down clears every register */
            memset(&a->sq1_on, 0, 1);
            a->sq1_on = a->sq2_on = a->wave_on = a->noise_on = 0;
            a->sq1_dac = a->sq2_dac = a->wave_dac = a->noise_dac = 0;
            for (uint16_t r = 0xFF10; r <= 0xFF25; r++) g->io[r - 0xFF00] = 0;
            a->nr50_vol_l = a->nr50_vol_r = 0;
            a->nr51_sel = 0;
            a->sq1_len_en = a->sq2_len_en = a->wave_len_en = a->noise_len_en = 0;
        }
        a->apu_on = on;
        return;
    }

    if (!a->apu_on) return;   /* writes are ignored while the APU is off */

    switch (addr) {
        /* ---- square 1 ---- */
        case 0xFF10:
            a->sq1_sweep_period = (uint8_t)((val >> 4) & 7);
            a->sq1_sweep_dir    = (uint8_t)((val >> 3) & 1);
            a->sq1_sweep_shift  = (uint8_t)(val & 7);
            break;
        case 0xFF11:
            a->sq1_duty = (uint8_t)((val >> 6) & 3);
            a->sq1_len  = (uint8_t)(64 - (val & 0x3F));
            break;
        case 0xFF12:
            a->sq1_vol        = (uint8_t)(val >> 4);
            a->sq1_env_dir    = (uint8_t)((val >> 3) & 1);
            a->sq1_env_period = (uint8_t)(val & 7);
            a->sq1_dac        = (val & 0xF8) ? 1 : 0;
            if (!a->sq1_dac) a->sq1_on = 0;
            break;
        case 0xFF13:
            a->sq1_freq = (uint16_t)((a->sq1_freq & 0x700) | val);
            break;
        case 0xFF14: {
            a->sq1_freq = (uint16_t)(((val & 7) << 8) | (a->sq1_freq & 0xFF));
            a->sq1_len_en = (uint8_t)((val >> 6) & 1);
            if (val & 0x80) {                 /* trigger */
                a->sq1_on = 1;
                if (a->sq1_len == 0) a->sq1_len = 64;
                a->sq1_freq_timer = 2048 - a->sq1_freq;
                a->sq1_env_vol = a->sq1_vol;
                a->sq1_env_timer = 0;
                a->sq1_sweep_shadow = a->sq1_freq;
                a->sq1_sweep_timer = a->sq1_sweep_period ? a->sq1_sweep_period : 8;
                a->sq1_sweep_en = (a->sq1_sweep_period | a->sq1_sweep_shift) ? 1 : 0;
                if (a->sq1_sweep_shift) {
                    uint32_t check = (uint32_t)a->sq1_freq +
                                     (a->sq1_freq >> a->sq1_sweep_shift);
                    if (check > 2047) a->sq1_on = 0;
                }
                if (!a->sq1_dac) a->sq1_on = 0;
            }
            break;
        }
        /* ---- square 2 ---- */
        case 0xFF16:
            a->sq2_duty = (uint8_t)((val >> 6) & 3);
            a->sq2_len  = (uint8_t)(64 - (val & 0x3F));
            break;
        case 0xFF17:
            a->sq2_vol        = (uint8_t)(val >> 4);
            a->sq2_env_dir    = (uint8_t)((val >> 3) & 1);
            a->sq2_env_period = (uint8_t)(val & 7);
            a->sq2_dac        = (val & 0xF8) ? 1 : 0;
            if (!a->sq2_dac) a->sq2_on = 0;
            break;
        case 0xFF18:
            a->sq2_freq = (uint16_t)((a->sq2_freq & 0x700) | val);
            break;
        case 0xFF19:
            a->sq2_freq = (uint16_t)(((val & 7) << 8) | (a->sq2_freq & 0xFF));
            a->sq2_len_en = (uint8_t)((val >> 6) & 1);
            if (val & 0x80) {
                a->sq2_on = 1;
                if (a->sq2_len == 0) a->sq2_len = 64;
                a->sq2_freq_timer = 2048 - a->sq2_freq;
                a->sq2_env_vol = a->sq2_vol;
                a->sq2_env_timer = 0;
                if (!a->sq2_dac) a->sq2_on = 0;
            }
            break;
        /* ---- wave ---- */
        case 0xFF1A:
            a->wave_dac = (uint8_t)((val >> 7) & 1);
            if (!a->wave_dac) a->wave_on = 0;
            break;
        case 0xFF1B:
            a->wave_len = (uint8_t)(256 - val);
            break;
        case 0xFF1C:
            a->wave_vol_code = (uint8_t)((val >> 5) & 3);
            break;
        case 0xFF1D:
            a->wave_freq = (uint16_t)((a->wave_freq & 0x700) | val);
            break;
        case 0xFF1E:
            a->wave_freq = (uint16_t)(((val & 7) << 8) | (a->wave_freq & 0xFF));
            a->wave_len_en = (uint8_t)((val >> 6) & 1);
            if (val & 0x80) {
                a->wave_on = 1;
                if (a->wave_len == 0) a->wave_len = 256;
                a->wave_freq_timer = 2048 - a->wave_freq;
                a->wave_pos = 0;
                if (!a->wave_dac) a->wave_on = 0;
            }
            break;
        /* ---- noise ---- */
        case 0xFF20:
            a->noise_len = (uint8_t)(64 - (val & 0x3F));
            break;
        case 0xFF21:
            a->noise_vol        = (uint8_t)(val >> 4);
            a->noise_env_dir    = (uint8_t)((val >> 3) & 1);
            a->noise_env_period = (uint8_t)(val & 7);
            a->noise_dac        = (val & 0xF8) ? 1 : 0;
            if (!a->noise_dac) a->noise_on = 0;
            break;
        case 0xFF22:
            a->noise_shift = (uint8_t)(val >> 4);
            a->noise_width = (uint8_t)((val >> 3) & 1);
            a->noise_div   = (uint8_t)(val & 7);
            break;
        case 0xFF23:
            a->noise_len_en = (uint8_t)((val >> 6) & 1);
            if (val & 0x80) {
                a->noise_on = 1;
                if (a->noise_len == 0) a->noise_len = 64;
                a->noise_lfsr = 0x7FFF;
                a->noise_timer = 0;
                a->noise_env_vol = a->noise_vol;
                a->noise_env_timer = 0;
                if (!a->noise_dac) a->noise_on = 0;
            }
            break;
        /* ---- mixer ---- */
        case 0xFF24:
            a->nr50_vol_l = (uint8_t)((val >> 4) & 7);
            a->nr50_vol_r = (uint8_t)(val & 7);
            break;
        case 0xFF25:
            a->nr51_sel = val;
            break;
        default:
            break;
    }
}

void gb_apu_reset(gb_state_t *g)
{
    memset(&g->apu, 0, sizeof(g->apu));
    g->apu.apu_on = 1;
    g->apu.nr50_vol_l = 7;
    g->apu.nr50_vol_r = 7;
    g->apu.nr51_sel = 0xF3;
    g->apu.noise_lfsr = 0x7FFF;
    ring_init();
    if (g_audio_out.buf) {
        g_audio_out.head = g_audio_out.tail = g_audio_out.count = 0;
    }
}

/* ------------------------------------------------------ render / DSP    */

void gb_set_sample_rate(int hz)
{
    ring_init();
    if (hz < 8000) hz = 8000;
    if (hz > 192000) hz = 192000;
    g_audio_out.out_rate = hz;
    g_audio_out.pos = 0.0f;
    g_audio_out.dc_l = g_audio_out.dc_r = 0.0f;
    g_audio_out.lp_l = g_audio_out.lp_r = 0.0f;
    g_audio_out.hs_l = g_audio_out.hs_r = 0.0f;
}

void gb_set_audio_mode(int mode) { ring_init(); g_audio_out.mode = mode; }
void gb_set_volume(float v)       { ring_init(); g_audio_out.volume = v; }

int gb_audio_available(void)
{
    ring_init();
    if (!g_audio_out.buf) return 0;
    float ratio = (float)GB_APU_HZ / (float)g_audio_out.out_rate;
    return (int)(g_audio_out.count / ratio);
}

static inline float soft_clip(float x)
{
    /* tanh-ish cubic saturation: keeps 16-bit transients from splatting */
    if (x > 1.0f) x = 1.0f;
    if (x < -1.0f) x = -1.0f;
    return x - (x * x * x) / 3.0f;
}

int gb_render_audio(int16_t *out, int frames)
{
    ring_init();
    if (!g_audio_out.buf || frames <= 0) return 0;

    float ratio = (float)GB_APU_HZ / (float)g_audio_out.out_rate;
    int span = (int)(ratio + 0.5f);
    if (span < 1) span = 1;
    int produced = 0;
    int mode = g_audio_out.mode;
    float vol = g_audio_out.volume;

    /* per-mode DSP coefficients */
    float warm   = (mode == GB_AUDIO_ENHANCED) ? 0.055f :
                   (mode == GB_AUDIO_CRYSTAL)  ? 0.020f : 0.0f;
    float air    = (mode == GB_AUDIO_CRYSTAL)  ? 0.42f  :
                   (mode == GB_AUDIO_ENHANCED) ? 0.26f  : 0.0f;
    float width  = (mode == GB_AUDIO_CRYSTAL)  ? 1.55f  :
                   (mode == GB_AUDIO_ENHANCED) ? 1.35f  : 1.0f;
    float drive  = (mode == GB_AUDIO_AUTHENTIC) ? 0.0f : 0.30f;

    while (produced < frames) {
        if (g_audio_out.count < (uint32_t)span + 2) {
            /* under-run: pad with silence so the host never stalls */
            out[produced * 2] = 0;
            out[produced * 2 + 1] = 0;
            produced++;
            continue;
        }

        /* box-filter decimation from the 1.048576 MHz ring */
        float acc_l = 0.0f, acc_r = 0.0f;
        for (int i = 0; i < span; i++) {
            acc_l += ring_at((uint32_t)i, 0);
            acc_r += ring_at((uint32_t)i, 1);
        }
        for (int i = 0; i < span; i++) ring_pop();
        acc_l /= (float)span;
        acc_r /= (float)span;

        /* DC blocker */
        float dcl = g_audio_out.dc_l, dcr = g_audio_out.dc_r;
        g_audio_out.dc_l = acc_l + 0.995f * dcl;
        g_audio_out.dc_r = acc_r + 0.995f * dcr;
        acc_l = g_audio_out.dc_l - dcl;
        acc_r = g_audio_out.dc_r - dcr;

        /* warmth: one-pole low shelf on the mid signal */
        if (warm > 0.0f) {
            g_audio_out.lp_l += warm * (acc_l - g_audio_out.lp_l);
            g_audio_out.lp_r += warm * (acc_r - g_audio_out.lp_r);
            acc_l += g_audio_out.lp_l * 0.6f;
            acc_r += g_audio_out.lp_r * 0.6f;
        }

        /* mid/side stereo widening */
        if (width != 1.0f) {
            float mid = (acc_l + acc_r) * 0.5f;
            float side = (acc_l - acc_r) * 0.5f;
            side *= width;
            acc_l = mid + side;
            acc_r = mid - side;
        }

        /* air: high-shelf via a cheap difference filter */
        if (air > 0.0f) {
            float hl = acc_l - g_audio_out.hs_l;
            float hr = acc_r - g_audio_out.hs_r;
            g_audio_out.hs_l = acc_l;
            g_audio_out.hs_r = acc_r;
            acc_l += hl * air;
            acc_r += hr * air;
        }

        acc_l *= vol;
        acc_r *= vol;

        if (drive > 0.0f) {
            acc_l = soft_clip(acc_l * (1.0f + drive)) / (1.0f + drive * 0.5f);
            acc_r = soft_clip(acc_r * (1.0f + drive)) / (1.0f + drive * 0.5f);
        }

        int li = (int)(acc_l * 32767.0f);
        int ri = (int)(acc_r * 32767.0f);
        out[produced * 2]     = (int16_t)(li > 32767 ? 32767 : (li < -32768 ? -32768 : li));
        out[produced * 2 + 1] = (int16_t)(ri > 32767 ? 32767 : (ri < -32768 ? -32768 : ri));
        produced++;
    }
    return produced;
}
