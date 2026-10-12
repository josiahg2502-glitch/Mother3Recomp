# PokéSilver PC

A high-resolution PC port front-end for a Game Boy Color cartridge, built on a
from-scratch SM83/CGB emulation core written in freestanding C. One core,
compiled twice: a native shared library for the desktop launcher and a
`wasm32` module for the browser front-end.

```
core/      the emulator (CPU, PPU, APU, MBC, state, patch, scaler)  -- freestanding C11
pc/        native desktop front-end (SDL via pygame)
web/       browser front-end (WebGL upscalers, Web Audio, Gamepad API)
tools/     build script, GB/GBC assembler, demo cartridge + mod generators
tests/     CPU vectors, render/audio/state, mod pipeline, launcher hotkeys
mods/      example IPS patch
assets/    bundled original self-test cartridge
```

Everything in this repository is original work: the emulator, the assembler,
the front-ends, the self-test cartridge's graphics and music (procedurally
generated), the bezel artwork (hand-authored SVG), and the example mod.

**No commercial ROM is included or fetched.** Bring your own legally obtained
cartridge dump. The bundled `assets/pksilver_demo.gbc` is an original
homebrew self-test cart used to prove the pipeline without one.

---

## Build

Requirements: `python3`, `pip install --user ziglang`, and either a browser
(web port) or `pip install pygame numpy` (native port).

```sh
./tools/build.sh all        # native .so + wasm module
./tools/build.sh native     # build/libpksilver_core.so
./tools/build.sh wasm       # web/wasm/pksilver_core.wasm
./tools/build.sh test       # full test suite
```

The wasm build is `wasm32-freestanding`: no libc, no imports, no JS glue
needed beyond the loader in `web/js/core.js`. Memory is sized by the linker;
all JS↔wasm traffic goes through fixed staging arenas declared in
`core/jsbind.c` (state 512 KiB, patch 2 MiB, audio 4096 frames).

## Run

**Browser** — serve `web/` over HTTP (a `file://` page cannot fetch the wasm):

```sh
cd web && python3 -m http.server 8080 --bind 0.0.0.0
# then open http://localhost:8080/
```

Insert a cartridge with the launcher's **Open cartridge** button (`.gb`,
`.gbc`, or a `.zip` containing one — the front-end has its own ZIP reader and
inflates via `DecompressionStream`), or click **Run self-test demo** to boot
the bundled original cart.

**Desktop** — the native launcher:

```sh
python3 pc/launcher.py "your-cartridge.gbc" --mods mods/demo_banner.ips
python3 pc/launcher.py --demo          # bundled self-test cart
python3 pc/launcher.py --selftest      # headless smoke test
```

---

## Controls

### Hotkeys

| Key | Action |
|-----|--------|
| `F10` | Instant toggle: **custom bezel** ↔ **clean 3:2 pillarbox fullscreen**. HUD toast confirms. |
| `F9`  | Cycle controller preset (*Xbox Standard* → *Nintendo Switch* → *Modern Trigger Action* → *Classic 1:1*). Toast appears in the lower bezel terrain. |
| `F11` | Open / close the in-game **settings & assist tools** overlay. Replaces `Esc` for this purpose. |
| `Esc` | Clean, immediate exit. From the overlay it closes the overlay first; otherwise it returns to the launcher with no hang. |
| `F1`  | Quick save — slot 1. HUD confirmation with the state size. |
| `F2`  | Quick load — slot 1. HUD confirmation. |

Overlay-only keys (native port): `1`–`5` upscaler, `g` pixel grid,
`h` scanlines, `m` audio mix, `[` `]` volume, `-` `=` speed, `c` colour
correction.

### Controller presets

The left analog stick drives a **full 8-way directional grid** (deadzone 0.35),
so diagonals register as both components at once — exactly what a d-pad gives
you. Half-pressing a trigger maps to the corresponding shoulder button.

| Physical | Xbox Standard | Nintendo Switch | Modern Trigger Action | Classic 1:1 |
|----------|---------------|-----------------|-----------------------|-------------|
| Left stick | 8-way d-pad | 8-way d-pad | 8-way d-pad | *disabled* |
| D-pad | d-pad | d-pad | d-pad | d-pad |
| **LT** | L — diagonal lock / rotate | L — diagonal lock / rotate | **B** — cancel / run | L |
| **RT** | R — move shortcut / action | R — move shortcut / action | **A** — talk / attack | R |
| LB / RB | L / R | L / R | L / R | L / R |
| **X** | **B** — cancel / run | **A** — talk / attack | **B** — cancel / run | — |
| **Y** | **A** — talk / attack | **B** — cancel / run | **A** — talk / attack | — |
| A (bottom) | A — talk / attack | B — cancel / run | A — talk / attack | A |
| B (right) | B — cancel / run | A — talk / attack | B — cancel / run | B |
| Back / Select | Select | Select | Select | Select |
| Start | Start | Start | Start | Start |

Keyboard fallback: arrows / `WASD` move, `Z` = A, `X` = B, `Enter` or `Space`
= Start, `RShift` or `Backspace` = Select, `Q` / `E` = L / R.

### Audio

16-bit stereo out of the core's APU at 44.1 kHz, with three mix personalities:

- **Authentic** — hardware-shaped, channel crosstalk as modelled
- **Enhanced** *(default)* — wider stereo image, tighter low-pass, less DAC noise
- **Crystal** — bright, clean, modern-mastering bias

The browser front-end pulls from the core via a 2048-frame script processor;
the native port feeds SDL mixer directly.

---

## Video

The core renders a 160×144 framebuffer and exposes five CPU upscalers
(`gb_upscale`): nearest, sharp bilinear, smooth bilinear, OmniScale, Scale3x.
The browser front-end instead uploads the source frame to a WebGL texture and
does the scaling in a shader with an optional LCD pixel-grid and scanline
overlay, then integer-scales the canvas to the bezel's screen well so pixels
stay square.

`F10` swaps between the authored bezel shell (original silver handheld design,
SVG, screen well at viewBox coordinates `400,118 380×342`) and a bare
pillarboxed canvas centred on black.

---

## Mods

Mods are patch files applied over the cartridge image **in memory** — the ROM
on disk is never modified. Supported formats: IPS, IPS32, UPS, BPS.

```js
Mods.addFiles(fileList);   // queue patches
Mods.applyAll();           // push them through gb_apply_patch()
```

```sh
python3 pc/launcher.py cart.gbc --mods a.ips b.ups c.bps
```

Applying a patch re-derives the cartridge identity (`gb_rom_recalc_crc`), so
save states made under one mod set are refused by a different one instead of
silently corrupting the session. Save states are keyed by that CRC.

`mods/demo_banner.ips` is a working example against the bundled self-test
cart; `tools/make_mod.py` regenerates it. See `docs/MODDING.md`.

Cheats use GameShark format (`01XXYYZZ`), added through `gb_add_cheat`.

---

## Tests

```sh
python3 tests/cpu_test.py                    # 499,148 SM83 single-step vectors
python3 tests/render_test.py assets/pksilver_demo.gbc
python3 tests/mod_test.py                    # patch -> verify -> re-render
python3 tests/native_test.py                 # scripted F1/F2/F9/F10/F11 + overlay keys
node    tools/api_check.js                   # JS <-> wasm export cross-reference
```

Current results: **499,148 / 499,148 CPU vectors pass (100.00 %)**; the
self-test cart renders 6 distinct colours with an audio peak of ~18,995/32,767
and save-state replay is byte-exact and deterministic.

`tools/api_check.js` exists because there is no browser in CI: a typo in a
cross-module call would otherwise only surface in the user's tab. It verifies
every `Core.ex.<fn>` against the real wasm export table and every
`Module.member` reference against each module's parsed export surface.

---

## Notes

- The core is `wasm32-freestanding`-clean: no libc, so `core/gb_string.h` and
  `core/freestanding.c` provide the handful of memory helpers it needs.
- `tools/gbasm.py` is a small two-pass GB/GBC assembler used to build the
  self-test cart; it was validated byte-exact against a 50-instruction
  reference program.
- Save-state format: `PKSS` header + version + ROM CRC + payload size +
  payload + CRC32 (181,092 bytes for the demo cart).
