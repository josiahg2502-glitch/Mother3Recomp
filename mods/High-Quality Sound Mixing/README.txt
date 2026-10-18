High-Quality Sound Mixing for MOTHER 3 (English Fan Translation v1.3)
========================================================================

Original Mod by Summer Dragonfly & ipatix
Based on 16-bit audio mixing routines from Camelot / Golden Sun
Romhacking.net Entry: https://www.romhacking.net/hacks/6951/
GitHub Repository:   https://github.com/MusicTheorist/MOTHER-3-HQ-Audio

About This Mod:
---------------
The Game Boy Advance hardware mixer processes audio channels at 8-bit resolution,
causing the notorious "GBA background hiss" and quantization noise.

This hack replaces the default m4a mixer with ipatix's custom high-fidelity mixer.
It mixes every individual audio channel at 16-bit resolution before downsampling,
drastically reducing quantization noise and making Shogo Sakai's iconic soundtrack
sound crisper, clearer, and richer.

How to Apply:
-------------
Option 1 (One-Click Automatic Patcher - Recommended):
  1. Make sure your "MOTHER 3 (English v1.3).gba" ROM is placed in the "roms" folder
     (or drag and drop your .gba file directly onto "apply_hq_sound_mod.bat").
  2. Double-click "apply_hq_sound_mod.bat".
  3. The patched ROM "MOTHER 3 (English v1.3) (HQ Audio).gba" will be created and
     your config will automatically be set to boot it.
  4. Double-click "Play Mother 3.cmd" to start playing immediately with 16-bit HQ audio!

Option 2 (Manual / External Patchers):
  1. Apply "mother3_hq_sound_mixing.ups" to your clean English v1.3 ROM using
     any standard UPS patcher (such as Tsukuyomi or Floating IPS / Flips).
  2. Place the resulting patched ROM into the "roms" folder.
  3. In "rom.cfg", put the path to your patched ROM file.
  4. In "config.ini", ensure "skip_launcher = 1" is set under [Launcher].
  5. Run "Play Mother 3.cmd" (or Mother3Recomp.exe).

BUILT-IN MOTHER3RECOMP SUPPORT:
------------------------------
Mother3Recomp applies the HQ mixer in memory after the selected language patch,
including supported fan translations. It does not modify the user's ROM file.
The standalone UPS patch above remains for use with other emulators.

Compatibility & Hashes:
-----------------------
- Base ROM:    MOTHER 3 (English Fan Translation v1.3)
  SHA-1:       306fb8874533b0fd0796208faa8bed2db4ae77fb

- Patched ROM: MOTHER 3 (English v1.3 + HQ Sound Mixing)
  SHA-1:       4ffe71ce5c5e1e8d9be5fbd09968b60787169b21

Note:
This mod repoints internal track players and shares the Mersenne Twister RNG to
free up RAM for the 16-bit mixing buffer. It is fully supported natively by
Mother3Recomp v1.0.1.
