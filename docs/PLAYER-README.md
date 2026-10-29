# Setup and controls

## Game files

Supply your own MOTHER 3 ROM. Desktop builds request a GBA BIOS; Android can use its built-in BIOS substitute. ROMs, BIOS dumps and translation patches are not included in the downloads.

- **English:** use the English fan translation v1.3 ROM, or apply its UPS patch to a clean Japanese ROM.
- **Spanish and other patches:** v1.0.4 onward accept IPS, IPS32, BPS and UPS patches through the launcher. Start with the ROM version required by the patch.
- **HQ Audio:** built in; it applies automatically to the Japanese ROM and supported fan translations after the selected language patch. Compatible ROM mods are handled when their sound layout matches.

Unknown patch outputs use the compatibility interpreter and may run more slowly. Support varies by patch.

<details>
<summary>ROM and BIOS identification</summary>

SHA-1 values:

- Japanese ROM: `4f0f493e12c2a8c61b2d809af03f7abf87a85776`
- English v1.3: `306fb8874533b0fd0796208faa8bed2db4ae77fb`
- English + HQ Audio: `4ffe71ce5c5e1e8d9be5fbd09968b60787169b21`
- Retail GBA BIOS: `300c20df6731a33952ded8c436f7f186d25d3492` (16 KiB)

</details>

## Launch

- **Windows:** extract the ZIP and open `Mother3Recomp.exe`.
- **Linux / Steam Deck:** extract the archive and open the included launcher. Ubuntu/Debian users need `libsdl2-2.0-0` and `libgl1`. Steam Deck users can add the launcher as a Non-Steam Game.
- **Android:** install the APK, open the app and select your game files. Preserve app data when updating to keep saves.
  Turn on **Portrait too** on the setup screen to play upright with the controls under the game (it follows the
  phone's rotation lock). Fast-forward and save states are under **Assist Tools** in the in-game menu.

Keep the executable with its bundled libraries and assets. Back up saves before changing builds or patches.

## Launcher

The first launch asks for your BIOS and ROM in your system's file picker. Pick the English v1.3 ROM, or the
Japanese ROM and then its translation patch (the patched ROM is saved in `mods/rom-patches`). After that,
MOTHER 3's own launcher opens: the party walks around outside Tazmily while you pick **Play**, **Settings**,
**Controls** or **Mods**. Everything it shows is drawn from your ROM; no game art ships with the download.

- **Settings:** window flavor (the game's Plain to Melon), display mode, window size, scaling, screen colors,
  volume, **Button prompts**, **Frame rate**, **Import a save file**, and **Change game files** to pick a different
  ROM or BIOS.
- **Import a save file:** bring over a save from another emulator (mGBA, VBA, RetroArch, My Boy!, etc.). Pick its
  battery save (`.sav` or `.srm`, not a save state). Your current save is kept as `saves/mother3_en_v13.sav.bak`.
  On Android, use **Import a save from another emulator** on the setup screen.
- **Frame rate (smooth):** on a 100 Hz or faster screen (120, 144, 165, 240 Hz...), a frame is drawn for every
  refresh of the screen, each at its own moment between two of the game's frames, so scrolling and walking move
  in smaller steps. The launcher shows your screen's rate, such as **144 (smooth)**. The game itself still runs at
  its normal speed. On a 60 Hz screen it has no effect, and the launcher says so. If your screen is fast but the
  game still shows 60 fps, choose **Smooth (always)**, which skips the screen check.
- **Japanese (untranslated):** pick the Japanese ROM and answer **Yes** to play it in Japanese. The launcher opens as
  usual, with its own font, since the Japanese ROM has no English letters.
- **Other translations** (Spanish, Italian, Brazilian Portuguese and others) open the launcher too; when a ROM's font
  differs from English v1.3's, the launcher uses its own font. A ROM the launcher can't draw from starts the game
  directly, using the settings from the last time you used the launcher.
- **Controls:** pick a GBA button, then press the new key or controller button; whichever you press is the one
  that changes (Escape cancels). On controllers the left stick always moves; the D-pad is the rebindable second input.
- **Getting around:** each page shows its buttons at the bottom. B (or Z / Escape) goes back, and LB / RB
  (or Q / E) jumps between Settings, Controls and Mods.
- **Button prompts:** the game's own text ("press the A Button") shows the button you actually use: Xbox,
  PlayStation or Nintendo labels, or your keyboard key, switching when you change controller. Set a fixed style
  in Settings if you prefer.
- **Mods:** put `.ips`, `.bps` or `.ups` patches in the `mods` folder (the page has an **Open mods folder**
  button) and tick them. Mods that change game code are marked *slower*: they run in compatibility mode.
- **Widescreen:** Settings > Widescreen shows 16:9 in towns, fields, rooms and battles: the field draws the real
  map into the sides and battle backgrounds continue their pattern. Menus and anything else stay 4:3 with black
  sides. At the edge of a map both sides go black by the same amount, so the picture stays centred, and screen wipes stay black outside. The bezel is not used in widescreen.
- **Bezel (Windows):** built from your ROM on first launch in your window flavor; F10 hides it.

## Controls

| Action | Keyboard |
| --- | --- |
| Move | Arrow keys |
| Confirm / cancel | X / Z |
| L / R | C / V |
| Start / Select | Enter / Right Shift |
| Toggle bezel (Windows) | F10 |
| Runtime menu (desktop) | F11 |
| Exit (desktop) | Escape |

Controllers move with the left stick (the D-pad works too). Rebind keys and buttons on the launcher's Controls page.

## Known issues

- [Android device-specific performance reports](https://github.com/josiahg2502-glitch/Mother3Recomp/issues/9).
- [Desktop F11 crash reports](https://github.com/josiahg2502-glitch/Mother3Recomp/issues/8).

When reporting a problem, include the app version, device/OS, patch version, steps and any error text. Candidate fixes are under review; device performance still needs confirmation.

[Downloads](https://github.com/josiahg2502-glitch/Mother3Recomp#download) · [Credits](https://github.com/josiahg2502-glitch/Mother3Recomp/blob/master/CREDITS.md)
