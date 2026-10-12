# Architecture

## One core, two targets

```
                    core/*.c   (freestanding C11, no libc)
                          |
          +---------------+----------------+
          |                                |
   zig cc -target native            zig cc -target wasm32-freestanding
          |                                |
 build/libpksilver_core.so        web/wasm/pksilver_core.wasm
          |                                |
     pc/launcher.py                   web/js/*.js
     (ctypes, SDL/pygame)             (WebGL, Web Audio, Gamepad)
```

The core knows nothing about windows, audio devices, or input hardware. It
exposes a C ABI (`core/gb.h`) and the front-ends do everything else. That is
why the same 100 %-passing CPU test suite validates both targets.

**Freestanding means no libc.** `core/gb_string.h` supplies the handful of
memory helpers (`memcpy`, `memset`, `memcmp`, `strlen`) and
`core/freestanding.c` the few runtime support symbols the wasm target needs.
There is no `malloc` in the core: every buffer is a static arena with an
explicit size, which is what makes the wasm memory footprint predictable.

| file | responsibility |
|------|----------------|
| `gb.c`      | top-level machine, MBC routing, ROM loading, cartridge identity |
| `ppu.c`     | pixel FIFO-free scanline renderer, CGB palettes, OAM |
| `apu.c`     | four channels + wave table, three mix personalities, decimation |
| `state.c`   | `PKSS` serialisation, CRC guard |
| `patch.c`   | IPS / IPS32 / UPS / BPS with dry-run format detection |
| `scaler.c`  | five CPU upscalers, LCD grid, scanlines |
| `api.c`     | the `GB_API` surface |
| `jsbind.c`  | fixed staging arenas so JS never juggles wasm pointers |

## The JS ↔ wasm boundary

A wasm module cannot hand JavaScript a pointer it allocated on JS's behalf,
and JS-side `malloc` into wasm memory is fragile. `core/jsbind.c` solves this
with **static staging arenas** and a verb-per-operation API:

```c
uint8_t *gb_js_state_buf(void);  uint32_t gb_js_state_cap(void);   // 512 KiB
int      gb_js_save_state(void);          // -> byte count, or negative
int      gb_js_load_state(uint32_t size); // -> 0 on success
uint8_t *gb_js_patch_buf(void);  uint32_t gb_js_patch_cap(void);   // 2 MiB
int      gb_js_apply_patch(uint32_t size);
uint8_t *gb_js_pixel_buf(void);  void gb_js_grab_frame(void);      // RGBA
int16_t *gb_js_audio_buf(void);  int  gb_js_render_audio(uint32_t frames);
```

Return-value conventions differ per verb and this is a real footgun:
`gb_js_save_state` returns the **byte count** on success (negative on
failure), while `gb_js_load_state` returns **0** on success. `web/js/state.js`
encodes both correctly (`if (n <= 0)` for save, `rc === 0` for load).

Audio caps at 4096 frames per call; the front-end asks for 2048.

## Export collection — a bug worth recording

`tools/build.sh` derives the wasm export list from `core/gb.h` so the two
cannot drift. The first version of that collector anchored on the return type:

```sh
grep -o 'GB_API[[:space:]]\+[A-Za-z_][A-Za-z0-9_ *]*\**[[:space:]]\+\(gb_...\)['
```

That requires whitespace between the type and the name. Every pointer-returning
declaration writes it as `uint8_t *gb_rom_buffer(void)` — the `*` is flush
against the name — so the regex silently skipped **seven** functions:
`gb_rom_buffer`, `gb_rom_data`, `gb_framebuffer`, and the four jsbind staging
buffers. The module linked fine and reported 57 exports. The web front-end
could not have loaded a ROM or read a single frame.

The native build was unaffected (it exports by visibility attribute, not by
name list), which is exactly why the failure did not show up in the native
test suite.

The collector now takes the name as *the `gb_*` identifier immediately before
the `(`*:

```sh
tr -d '\n' < gb.h | grep -o 'GB_API[^;]*' | grep -oE 'gb_[a-z0-9_]+[[:space:]]*\(' | ...
```

and `tools/api_check.js` fails the build if any `Core.ex.<fn>` referenced by
JavaScript is absent from the real wasm export table.

## Timing

The machine advances in frames for the front-ends' benefit
(`gb_run_frame` → exactly one vblank), with the APU ticking at the hardware
1.048576 MHz and decimating through a box filter into whatever sample rate the
host asks for. Speed multiplier is applied in the core, so both front-ends get
identical behaviour from `gb_set_speed_multiplier`.

## Input

The core takes a single 16-bit mask (`gb_set_input`) — 8 d-pad/action bits
plus the two extra shoulder bits this port adds for `LT`/`RT`:

```
bit 0 RIGHT  bit 4 A       bit 8  L
bit 1 LEFT   bit 5 B       bit 9  R
bit 2 UP     bit 6 SELECT
bit 3 DOWN   bit 7 START
```

Everything above that — analog deadzones, preset tables, keyboard fallbacks —
lives in the front-end, so a new preset needs no core change. The stick is
quantised to a full 8-way grid at 0.35 deadzone, meaning both components can
be set at once, matching a real d-pad.

## Testing layers

1. **CPU**: 499,148 single-step vectors (`tests/cpu_test.py`, fixture in
   `tests/vectors.bin`). The harness runs in a *flat* memory model — no MBC, no
   echo aliasing, no I/O read-back masks — with `IF`/`IE` as standalone
   registers rather than aliased onto `$FF0F`/`$FFFF`, because several vectors
   stage ordinary code at those addresses.
2. **Integration**: `tests/render_test.py` boots the self-test cart and checks
   distinct colour count, audio peak/RMS, and save-state round-trip with
   deterministic replay.
3. **Mods**: `tests/mod_test.py` — apply, diff against intended regions only,
   confirm identity changed, confirm the cart still renders and makes sound.
4. **Front-end**: `tests/native_test.py` posts synthetic SDL events for the
   whole hotkey spec and asserts the resulting state machine.
5. **Static**: `tools/api_check.js` — no browser in CI, so cross-module calls
   and the wasm export surface are cross-referenced by parsing instead.
