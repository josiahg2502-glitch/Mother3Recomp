# Widescreen: reveal more of the world instead of stretching

Status: **current** as of josiahg2502-glitch/Mother3Recomp#43 (all 553 maps
swept). Code: `src/m3_widescreen.cpp`; wiring in `src/main.cpp`. The engine
hooks (`g_ws_*` in `gba_ppu.h`) come from mstan/gbarecomp plus
`patches/gbarecomp-runtime.patch`.

All of it is host presentation. The guest runs unchanged: nothing is written
to guest RAM, VRAM, palettes, OAM, saves or ROM, except the actor-culling
hooks below, which answer the game's own overlap test.

## Views

| Platform | View | On a 1920x1080 panel |
|---|---|---|
| Desktop | 284x160 (`widescreen_view_width` and `max_view_width` 284) | 6.75x, about 1917x1080 |
| Android | 320x180 (`--view-width 320` and `mother3_view_geometry()`: 10 rows above and below) | exactly 6x, no borders |

config.ini `[Launcher] m3_widescreen = 1` turns it on. It applies at the next
launch.

## Field state (English v1.3)

Found by matching VRAM against the ROM maps.

| Address | What |
|---|---|
| 0x02001004 | camera: s16 x, s16 y, world px of the screen's top-left |
| 0x02005C5C | 3 x u32: decompressed layers 0..2 (u16 per 16 px cell); layer 0 may be null |
| 0x02009C88 | u32: 0x300 x 8-byte 16 px tile definitions |
| gGame = 0x02005C00 | |
| gGame+0x67AC | u16 current map (indexes the map info tables). Before #43 the code used 0x020281E4, which is not the map id. |
| gGame+0x1C + 4*layer | MapLayerInfo: width in bits 0-2 and height in bits 3-5 (256 px units minus one); bits 6-7 == 3 loop horizontally, next byte bits 0-1 == 3 vertically |
| gGame+0x58B0 + 0x38*layer | u16 16 px column and row of the screen's top-left, then (+8, +10) the fine scroll; +0x35 bit 4 loops the layer |
| gGame+0x11 | map loops: bit 5 horizontally, bit 4 vertically |

The scroll registers hold the camera's low 4 bits (the game rebases its BG
ring every 16 px). The code snaps the camera variable to them, so the
sub-tile phase stays exact when the variable lags a frame.

## How each part is widened

- **Validation** (`tick()`):
  - The field model is trusted only when it reproduces the visible 240x160 of
    the scenery BGs from VRAM: 0.9 of at least 150 samples to find a width,
    then 0.85 of at least 60 to keep it.
  - Rooms are re-checked every third frame. For 30 frames after a map change
    they are checked every frame, so a map that fades in has its margins as
    it fades.
  - A validated field survives two failed checks mid-scroll.
  - Anything else is pillarboxed (`g_ws_pillarbox`).
- **Layers:** a layer whose size or looping differs from the scenery's (a
  sky, a weather layer, a parallax backdrop) is "detached" and uses its own
  scroll and size. Maps 49 and 97 needed this.
- **Rooms:** interior maps pack several rooms. A margin cell belongs to this
  room only if the path from the native screen to it never crosses the void.
- **Void:** cells outside the room show entry 0, the layer's tile 0 (a dark
  fill in some caves), as the game draws them.
- **Native view placement** (`place_native_view()`, at frame start):
  - At a map edge the native 240x160 slides towards the edge inside the
    canvas (`g_ws_native_view_left`/`top`), instead of the margins showing
    void.
  - Maps narrower than the canvas show whole and centred.
  - Looping maps and battles stay centred.
- **Sprites:**
  - `place_obj` and `place_obj_y` read OAM X 256..319 as right of the
    screen when that is where it lands.
  - Sprites from another room are moved off the canvas.
  - The rows above and below show only field sprites whose feet are in this
    room.
- **Actor culling:**
  - The game culls actors against `{camera, 240x160}` with two overlap tests,
    0x080357D0 and 0x08035820, the second for looping maps.
  - Both are `[[mod_function_hook]]`s in `game.toml`, answered against the
    widened view.
  - Side effect: the off-screen actor flag flips earlier while widescreen is
    on. No gameplay effect has been seen.
- **BG0:** BG0 carries text boxes, menus and the cinematic bars.
  - A side margin continues it only where the edge pixel has a solid tile's
    colour.
  - Above and below the screen it continues only when it covers the whole
    edge row.
- **Weather:** a detached BG1 layer exactly 256 px wide (the rain of map 11)
  continues from the BG ring.
- **Battles:** detected per frame. BG2/BG3 continue from the 256 px ring;
  BG0/BG1 (text, HUD) don't extend; WIN0 rows are followed.

## Testing

`M3_WARP=<map id>` (`src/m3_devwarp.cpp`) takes a door warp to that map's
first entrance on the party's next step, through the game's own transition:
- It uses a `[[mod_function_hook]]` on 0x0800F5EC, the field exit check.
- It is disabled unless the variable is set.
- Android apps get no environment. A test build can read the target from a
  file in the app's files directory instead.

Results of #43:
- Desktop: all 553 maps.
- Android 320x180 on an Anbernic RG556: 22 maps against v1.0.8, with no
  regressions.

## Open points

- Map 428 drops to side bars mid-scene.
- Menus and non-field cutscenes stay pillarboxed by design.
- The rows above and below the native screen use scanline 0's and line
  159's state, so per-line effects only continue the edge line.
- Real looping maps were tested only with the loop bits forced.
- Overlays other than a blended BG1 (fog, darkness) on other layers are
  untested.
