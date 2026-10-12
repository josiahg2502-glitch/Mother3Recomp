/* ==========================================================================
 *  gb.h -- PokéSilver PC : portable Game Boy / Game Boy Color core
 *
 *  Zero dependencies (no libc headers beyond stdint/stddef), single-instance,
 *  whole mutable state lives in one POD struct so save-states are a memcpy.
 *  Compiles natively (gcc/clang/zig cc) and to wasm32-freestanding.
 *
 *  Copyright (c) 2026 PokéSilver PC contributors. MIT licensed.
 * ========================================================================== */
#ifndef GB_H
#define GB_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -- exported symbol decoration ------------------------------------------ */
#ifdef GB_BUILD_WASM
#  define GB_API __attribute__((visibility("default")))
#else
#  if defined(_WIN32)
#    define GB_API __declspec(dllexport)
#  else
#    define GB_API __attribute__((visibility("default")))
#  endif
#endif

#define GB_LCD_W 160
#define GB_LCD_H 144

#define GB_ROM_MAX   (8 * 1024 * 1024)  /* largest cart we accept (hacks!)  */
#define GB_CRAM_MAX  (128 * 1024)
#define GB_STATE_VERSION 4

/* joypad bits (the canonical P1 layout, active-high for our API) */
#define GB_BTN_RIGHT   0x01
#define GB_BTN_LEFT    0x02
#define GB_BTN_UP      0x04
#define GB_BTN_DOWN    0x08
#define GB_BTN_A       0x10
#define GB_BTN_B       0x20
#define GB_BTN_SELECT  0x40
#define GB_BTN_START   0x80
/* extras: GBA shoulder buttons (Silver never reads them, they are routed to
 * the assist layer by the frontend, but we keep them in the joypad word).  */
#define GB_BTN_L       0x100
#define GB_BTN_R       0x200

/* models */
#define GB_MODEL_AUTO 0
#define GB_MODEL_DMG  1   /* monochrome, forces DMG behaviour              */
#define GB_MODEL_CGB  2   /* colour (still honours the cart's CGB flag)    */
#define GB_MODEL_SGB  3   /* DMG + Super Game Boy border hook (not used)   */

/* audio personalities */
#define GB_AUDIO_AUTHENTIC 0  /* raw DMG/CGB mix                           */
#define GB_AUDIO_ENHANCED  1  /* 16-bit enhanced: warm + wide + air        */
#define GB_AUDIO_CRYSTAL   2  /* brighter, more present                    */

/* colour correction */
#define GB_COLOR_OFF    0
#define GB_COLOR_CGB    1  /* hardware-accurate CGB -> sRGB curve          */
#define GB_COLOR_VIVID  2  /* saturated "modern" curve                     */

/* ---------------------------------------------------------------------- */
/*  Public API                                                            */
/* ---------------------------------------------------------------------- */

GB_API void     gb_init(void);
GB_API void     gb_shutdown(void);

/* ROM loading.  `dst` is a scratch buffer owned by the core (GB_ROM_MAX);
 * the frontend copies the ROM in, then commits it.                        */
GB_API uint8_t *gb_rom_buffer(void);
GB_API int      gb_load_rom(size_t size);       /* commit buffer -> core   */
GB_API int      gb_is_loaded(void);
GB_API void     gb_get_rom_info(char *title, int title_cap,
                                int *cart_type, int *rom_banks,
                                int *ram_banks, int *cgb, int *sgb);
GB_API uint32_t gb_rom_crc32(void);
GB_API uint8_t *gb_rom_data(void);              /* for patching (mods)     */
GB_API size_t   gb_rom_size(void);

GB_API void     gb_reset(void);                 /* reboots the machine     */
GB_API void     gb_set_model(int model);

/* execution */
GB_API void     gb_set_input(uint16_t buttons);
GB_API int      gb_run_frame(void);             /* 1 = a frame is ready    */
GB_API const uint32_t *gb_framebuffer(void);    /* GB_LCD_W*GB_LCD_H XRGB  */

/* audio: renders `frames` interleaved stereo frames at the configured rate */
GB_API void     gb_set_sample_rate(int hz);
GB_API int      gb_render_audio(int16_t *out, int frames);
GB_API int      gb_audio_available(void);       /* frames buffered         */
GB_API void     gb_set_audio_mode(int mode);
GB_API void     gb_set_volume(float vol);       /* 0.0 .. 1.5              */
GB_API void     gb_set_speed_multiplier(int pct); /* 100 = 1x              */

/* save-states (F1/F2 quick slots live here) */
GB_API size_t   gb_state_size(void);
GB_API int      gb_save_state(uint8_t *dst, size_t cap);
GB_API int      gb_load_state(const uint8_t *src, size_t size);

/* battery-backed cart RAM */
GB_API size_t   gb_sram_size(void);
GB_API size_t   gb_sram_read(uint8_t *dst, size_t cap);
GB_API void     gb_sram_write(const uint8_t *src, size_t size);
GB_API int      gb_has_battery(void);

/* real time clock (MBC3).  Feed wall-clock seconds; the core keeps delta.  */
GB_API void     gb_set_rtc_now(uint64_t unix_seconds);
GB_API int      gb_has_rtc(void);

/* cheats / game genie / gameshark */
GB_API int      gb_add_cheat(const char *code);
GB_API void     gb_clear_cheats(void);
GB_API int      gb_cheat_count(void);

/* battery-free option bits */
GB_API void     gb_set_color_correction(int mode);
GB_API void     gb_set_lcd_grid(int on);        /* LCD grid baked by scaler */
GB_API void     gb_set_palette_override(const uint8_t *pal, int which);
                                                /* which 0=bg 1=ob0 2=ob1  */
/* --- patch engine (mod support: IPS / UPS / BPS) ----------------------- */
GB_API int      gb_apply_patch(const uint8_t *patch, size_t size);

/* --- upscaler (CPU side; the web frontend has a GLSL twin) ------------- */
#define GB_SCALE_NEAREST  0
#define GB_SCALE_SHARP    1   /* sharp bilinear                            */
#define GB_SCALE_SMOOTH   2   /* plain bilinear                            */
#define GB_SCALE_OMNI     3   /* OmniScale-style edge directed 2x/3x       */
#define GB_SCALE_SCALE3X  4   /* AdvMAME 3x                                */
GB_API void     gb_upscale(const uint32_t *src, uint32_t *dst,
                           int src_w, int src_h, int dst_w, int dst_h,
                           int filter, int grid, int scanline);

/* --- debug / test hooks ------------------------------------------------ */
GB_API void     gb_dbg_regs(uint16_t *pc, uint16_t *sp,
                            uint8_t *a, uint8_t *b, uint8_t *c, uint8_t *d,
                            uint8_t *e, uint8_t *f, uint8_t *h, uint8_t *l);
GB_API void     gb_dbg_set_regs(uint16_t pc, uint16_t sp, uint8_t a, uint8_t b,
                                uint8_t c, uint8_t d, uint8_t e, uint8_t f,
                                uint8_t h, uint8_t l);
GB_API void     gb_dbg_set_ime(int ime);
GB_API int      gb_dbg_get_ime(void);
/* Raw physical memory access: bypasses the MBC and all I/O side effects so
 * the single-step test rig can stage arbitrary machine states.            */
GB_API uint8_t  gb_dbg_read(uint16_t addr);
GB_API void     gb_dbg_write(uint16_t addr, uint8_t val);
GB_API int      gb_dbg_step(void);              /* one instruction, cycles */
GB_API int      gb_dbg_step_cpu(void);          /* CPU only: no timers/PPU/APU */
/* Mapped access: goes through the real MBC / I/O logic.  The rig uses these
 * to stage cartridge state (enable RAM, select a bank) before a vector.   */
GB_API uint8_t  gb_dbg_map_read(uint16_t addr);
GB_API void     gb_dbg_map_write(uint16_t addr, uint8_t val);
GB_API void     gb_dbg_set_raw_io(int on);
/* Flat memory model: the whole 64 KiB behaves as writable RAM with no MBC
 * banking and no I/O side effects.  Used to replay SM83 single-step
 * vectors, which are pure CPU tests.  Must stay off during emulation.     */
GB_API void     gb_dbg_set_flat(int on);
GB_API void     gb_dbg_set_if(uint8_t v);   /* flat-mode IF register       */
GB_API void     gb_dbg_set_ie(uint8_t v);   /* flat-mode IE register       */
GB_API void     gb_dbg_reset_cart(void);

/* --- wasm / JavaScript staging buffers (see core/jsbind.c) ------------- */
GB_API uint8_t *gb_js_state_buf(void);
GB_API uint32_t gb_js_state_cap(void);
GB_API uint8_t *gb_js_patch_buf(void);
GB_API uint32_t gb_js_patch_cap(void);
GB_API int      gb_js_save_state(void);
GB_API int      gb_js_load_state(uint32_t size);
GB_API uint32_t gb_js_state_bytes(void);
GB_API int      gb_js_apply_patch(uint32_t size);
GB_API uint8_t *gb_js_pixel_buf(void);
GB_API void     gb_js_grab_frame(void);
GB_API int16_t *gb_js_audio_buf(void);
GB_API int      gb_js_render_audio(uint32_t frames);

#ifdef __cplusplus
}
#endif
#endif /* GB_H */
