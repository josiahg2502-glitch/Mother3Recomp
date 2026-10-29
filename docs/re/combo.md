# Battle rhythm combos

Goal: an opt-in latency offset that shifts the combo hit window by the player's
measured audio + display delay (ms), so presses judged "late" by the audible beat
land on time.

## Located (verified 2026-09-24)

All addresses below are byte-identical in the JP and EN ROMs.

| Address | Symbol | Notes |
|---|---|---|
| `0x08070BB4` | `Combo::beatInRange(ComboRhythm&)` | The hit check. Thumb, 0x84 bytes. |
| `0x08070B28` / `0x08070B30` | `Combo::setRhythm(ComboRhythm&)` / `(uint, uint)` | |
| `0x08070B38` / `0x08070B40` | `Combo::setRhythm2(...)` | |
| `0x08070B98` / `0x08070BA0` | `Combo::getRhythm()` / `getRhythm2()` | |
| `0x08070C38` | `Combo::sub_08070C38(ComboRhythm&)` | Unknown; same signature as beatInRange |
| `0x08085FB0` | `RhythmCombo::RhythmCombo(int, UnitObject*, short, ...)` | |
| `0x0807466C`… | `RhythmBgm` (`GetRhythmDataBySongNum`, `getTempo` at `0x0807487C`) | Song beat source |
| `0x080655FC` | `AutoComboUi::AutoComboUi(ComboRhythm&)` | |
| `0x080EC578` | `gRhythmData` (ROM data) | Per-song rhythm table |

`beatInRange` has no direct `BL` callers; it is virtual. The pointer `0x08070BB5`
appears in 9 vtables at `0x09F7E09C`, `0x09F7E20C`, `0x09F8092C`, `0x09F80A9C`,
`0x09F84244`, `0x09F843B4`, `0x09F84524`, `0x09F84694`, `0x09F84804` (same in JP).

## What beatInRange does (from disassembly)

```
bool Combo::beatInRange(ComboRhythm& press):        // r0 = this, r1 = &press
    ComboRhythm start = this->vfunc_0x120();        // via vtable at this+0x1C,
    ComboRhythm len   = this->vfunc_0x128();        // GCC 2.95 {s16 delta; fnptr} slots,
                                                    // called through _call_via_r2 (0x0809193C)
    return press.a >= start.a && press.a < start.a + len.a
        && press.b >= start.b && press.b < start.b + len.b;
```

`ComboRhythm` is (at least) two signed 16-bit fields, `a` at +0 and `b` at +2.
Result: r0 = 1 inside the window, 0 outside.

## Open questions

1. Units of `a` and `b`. Hypothesis: `a` is a beat/tick counter from `RhythmBgm`
   (tempo-dependent) and `b` a second axis (sub-beat or a key/phase). Needs: read
   `gRhythmData`, `RhythmBgm::getTempo`, and watch a live `ComboRhythm` during a
   battle (break at `0x08070BB4`, dump the struct at r1 and the two returned values).
2. Which concrete classes own the 9 vtables (normal combo vs. special songs).
3. Where the press `ComboRhythm` is created from input, i.e. the caller path. A
   caller-side hook may be cleaner than editing the struct in beatInRange.
4. How many ms one tick of `a` is at each tempo (needed to convert the ms setting).

## Implementation plan (for hook-writer, once 1–4 are answered)

- `[[mod_function_hook]] addr = 0x08070BB4, mode = "thumb"` in both game.tomls
  (needs `.\tools\build.ps1 -Regen`).
- Callback: convert the ms offset to ticks at the current tempo; subtract it from the
  press field that encodes time; return 0 so the original check runs on the shifted
  value. Restore the original value afterwards if other code reads the same struct
  (or hook the caller instead).
- Setting: "Combo timing offset (ms)", default 0 (off), opt-in mod package.
- Verify: from a battle save state, a press recorded N ms late fails with offset 0
  and passes with offset ≈ N; offset 0 behaves byte-identically to the unmodded game.
