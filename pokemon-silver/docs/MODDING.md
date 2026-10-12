# Modding guide

Mods are patch files applied over the cartridge image **in memory**. The ROM
on disk is never touched, so a modded session and a clean session can share
the same source file, and removing a mod is just unloading it.

## Formats

| Format | Magic | Notes |
|--------|-------|-------|
| IPS   | `PATCH` | 3-byte offsets — cartridges up to 16 MiB |
| IPS32 | `IPS32` | 4-byte offsets |
| UPS   | `BPS1` | variable-length, checksummed |
| BPS   | `BPS1` | variable-length, checksummed, delta-oriented |

UPS and BPS share the `BPS1` magic and differ only in where they put their
metadata (BPS up front, UPS at the end). Rather than snapshot the whole ROM to
allow a rollback, `core/patch.c` runs each parser in **dry mode** first — full
structure and bounds validation with zero writes — and only mutates the image
once a format has positively identified itself. That keeps the wasm build
inside its memory budget while still leaving the ROM untouched on a bad patch.

## Applying

Core API:

```c
int gb_apply_patch(const uint8_t *patch, size_t size);   /* 0 on success */
uint8_t *gb_rom_data(void);                              /* live image  */
uint32_t gb_rom_crc32(void);                             /* identity    */
```

Order matters: load the cartridge, then patch, then play.

```js
const buf = Core.ex.gb_rom_buffer();
Core.poke(buf, romBytes);
Core.ex.gb_load_rom(romBytes.length);
Mods.addFiles(fileList);
Mods.applyAll();            // each patch -> gb_apply_patch
```

Native:

```sh
python3 pc/launcher.py cart.gbc --mods gfx.ips balance.ups
```

Patches are applied in list order, so a chain behaves the way a layered patch
set does: later patches see the output of earlier ones.

## Identity and save states

A successful `gb_apply_patch` calls `gb_rom_recalc_crc()`, re-deriving the
cartridge identity from the bytes actually in RAM. This is deliberate:

- save states embed the ROM CRC in their `PKSS` header
- `gb_load_state` refuses a state whose CRC does not match (`-4`)

Without the recompute, a patched ROM would keep advertising the *base* CRC and
a state made under one mod set would be accepted by a different one — a silent
corruption path. With it, `cart.gbc` and `cart.gbc + gfx.ips` are two distinct
identities with two distinct save slots.

Front-ends key their state storage on that CRC, so browser save states live
under `"<crc>:<slot>"` and native ones under `saves/<crc>.slot<N>.state`.

## Cheats

GameShark codes (`01XXYYZZ` — bank/value/address form) go through
`gb_add_cheat()` and are re-applied every frame:

```js
Core.ex.gb_add_cheat("0163D1DA");   // 0 = accepted
Core.ex.gb_cheat_count();
Core.ex.gb_clear_cheats();
```

Cheats are a runtime overlay and do **not** change the ROM CRC, so they do not
invalidate save states.

## Writing a patch

Any tool that emits IPS/UPS/BPS works. For a quick hand-rolled IPS the layout
is:

```
"PATCH"
repeat:  offset (3 bytes BE) | size (2 bytes BE) | data
         size == 0 means RLE: rle_len (2 BE) | value (1)
"EOF"
```

`tools/make_mod.py` builds the bundled example that way and is a readable
reference implementation — including the fixed-width detail that trips people
up: when you overwrite a text field in place, the replacement must fit the
original length or you have to account for the code shifting. The example pads
with NULs to the original field width so nothing downstream moves:

```python
data = repl[:len(needle)].ljust(len(needle), b"\x00")
```

## Verifying a mod

`tests/mod_test.py` is the template. It applies the example patch through the
real core and asserts:

1. `gb_apply_patch` accepted it
2. bytes actually changed, and **only** inside the intended regions
3. an untouched control entry is still intact (guards against over-broad writes)
4. the CRC changed
5. the patched cartridge still renders and still produces audio

That last point is the one worth keeping: a patch that applies cleanly can
still break the game, and "renders 6 colours, audio peak > 2000 after 60
frames" catches that immediately.
