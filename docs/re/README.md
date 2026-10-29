# Reverse-engineering notes

Notes on MOTHER 3 (English v1.3 unless a note says otherwise) and on engine
problems met while porting it. Each note records addresses (and whether JP ==
EN), what the code does, the evidence, and open questions; unverified claims
are marked as hypotheses.

Most were written during the Anbernic RG556 Android port
(savemosca/Mother3Recomp-Android, 2026-10), before its changes moved here. In
those notes, `tools/build-android.sh`, `variants/mother3en/` and
`patches/gbarecomp/00NN-*.patch` refer to that repository. The engine patches
it carried are now upstream:

| Port patch | Upstream |
|---|---|
| 0006 self-heal bridge stops at its own IRQ `iret` | mstan/gbarecomp#44 |
| 0007 "Quit game" in the runtime menu (an earlier 0007, the FLASH512 chip ID, was dropped: mstan fixed it in #28) | mstan/gbarecomp#46 |
| 0008 session health line | mstan/gbarecomp#47 |
| 0009 sensor re-init per session | mstan/gbarecomp#45 |
| 0010 suspend state survives a session ended in the background | mstan/gbarecomp#48 |
| 0011-0016 MP2K shadow mixer (Enhanced audio) | mstan/gbarecomp#49 |
| 0017 pure black letterbox | mstan/gbarecomp#50 |

This repo pins mstan/gbarecomp and carries its own engine changes in
`patches/gbarecomp-runtime.patch`; the game config is `game.toml`.

| Note | Topic | Status |
|---|---|---|
| [widescreen.md](widescreen.md) | 16:9 widescreen: how the field, sprites, BG0 and battles are widened, the addresses, testing with `M3_WARP` | Current (with #43) |
| [hq-audio-patch.md](hq-audio-patch.md) | The HQ Audio patch (code changes, RetroAchievements hash) vs the engine's MP2K shadow mixer; how the shadow's level and crackle were fixed | Both kept: the HQ switch and the runtime menu's Enhanced audio |
| [flash-stack-routines.md](flash-stack-routines.md) | FLASH512 routines copied to IWRAM 0x03006C1C..0x03006D80 | Resolved here by `dispatch_flash_ram_variant` (FULLY_STATIC) |
| [android-thumb-return-crash.md](android-thumb-return-crash.md) | Android crash returning to Thumb code in ARM state (0x08010832), pixel glitches, FLASH512 "unable to save" | Resolved: mstan/gbarecomp#44, the BIOS corpus built with `gba_bios.toml` (as `tools/regen.sh` does), the flash ID in mstan |
| [combo.md](combo.md) | Battle rhythm combos: hit-window check, latency offset | Hit check located; units unknown |
