# Android crash: return to Thumb code in ARM state (EN, RG556)

Status: **resolved** (2026-10-14). Engine-side (gbarecomp) plus the fork's
Android build script; not the fork's Android/cloud/RA code. The pixel glitches
reported at the same time had the same cause. A save failure found while
testing the fix ("unable to save") is recorded at the end.

## Symptom

Build `rg556-tuning` at `0daf5f1` (EN, `--device rg556`, engine
`agray0425/gbarecomp@af47d28` + local patches 0001-0005), Anbernic RG556,
GammaOS Next 1.2 (Android 14). A few minutes into a new game, around naming
the characters, the game aborted:

```
runtime_arm: SELF-HEAL bridge hit interpreter Undefined at pc=0x08010836 while bridging dispatch miss 0x08010832. ...
```

SIGABRT on the game thread: `abort` <- `runtime_bridge_interpret` <-
`runtime_dispatch_miss` <- generated code <- `gbarecomp::run_game`.
0x08010832 is the return address of `bl 0x08090F88` (`LZ77UnCompVram`, a
`swi 0x12` wrapper) in `sub_08010808`, a Thumb entry that exists in the static
corpus; the miss meant CPSR.T was clear on return, so the bridge decoded
Thumb halfwords as ARM. The crash is timing dependent. A second run
(below) aborted the same way at a different PC: 0x08002FDA, Thumb code
near `RemainderNullOnDBZ`, again entered in ARM state.

Scattered pixels and vertical streaks were visible from the title screen on,
while integer scaling was verified pixel exact (presentation not involved).

## Evidence

`src/main.cpp` reads `debug-env.txt` from app storage (only `su`/`run-as` can
create it) and exports its `KEY=VALUE` lines before `run_game()`. With

```
GBARECOMP_SELFHEAL_VERBOSE=1
GBARECOMP_TRACE_ON_DISPATCH_MISS=0x08010832
GBARECOMP_TRACE_DUMP_DEPTH=4096
```

the abort dumped the runtime trace (events carry CPSR). Condensed:

```
swi      pc=0x08090F82 cpsr=0x2000003F     DivMod: swi 6 (Div) from Thumb, System mode
dispatch pc=0x00000008 cpsr=0x20000093     BIOS SWI vector; 0x140 saves SPSR, switches
                                           to System mode with the caller's I bit (IRQs on)
irq      pc=0x00000164 cpsr=0x0000001F     VCount IRQ taken before `push {r2, lr}` at 0x164
...                                        game IRQ handler (IWRAM 0x03002EF0) runs fine
dispatch pc=0x00000138 cpsr=0x00000092     BIOS IRQ wrapper tail: not in the BIOS corpus
                                           -> dispatch miss -> interpreter bridge
mem_w    pc=0x00000164 sp=0x03007C74 lr=0x00000170
                                           native 0x164 runs `push {r2, lr}` AGAIN:
                                           SP should be 0x03007C7C, LR 0x08002FDF
...
call     pc=0x00000170 cpsr=0x4000001F     a later return goes to 0x170 (LR) in ARM state
dispatch pc=0x00000000 cpsr=0x00000000     BIOS SWI return pops garbage -> reset vector
dispatch pc=0x08002FDA cpsr=0x00000000     Thumb code in ARM state -> Undefined -> abort
```

## Root cause

Two defects combined:

1. **Engine: the self-heal bridge ran past an IRQ's return** (`src/runtime/
   runtime_arm_default_aborts.cpp`, `runtime_bridge_interpret`). `runtime_irq()`
   is entered from the interrupted code's `runtime_tick` and returns into it, so
   the interrupted instruction resumes natively. When the handler's return path
   is bridged, the bridge (IRQ-continuation mode) executed the interpreted
   `subs pc, lr, #4` and kept interpreting the interrupted code until the next
   static entry. For game code that is the interrupted PC itself (interior
   dispatch entries), so nothing went wrong; inside the BIOS, whose table lists
   function entries only, the bridge ran 0x164-0x16C (`push {r2, lr}`,
   `add lr, pc, #0`, `bx r12`) and the native SWI handler then ran them again.
   An IRQ inside a BIOS copy or decompression loop (CpuSet, CpuFastSet,
   LZ77) finished the routine in the bridge and then repeated its tail
   natively with r0-r3 restored: wrong data in VRAM, the pixel glitches.

2. **Fork: `tools/build-android.sh` generated the BIOS corpus without
   `--config gbarecomp/bios/gba_bios.toml`.** Only the vectors were seeded: 666
   dispatch entries instead of the 5143 that upstream ships, missing the IRQ
   wrapper tail 0x138 (`after_interrupt`), the SWI-handler resume points and
   every `resume_range`. So every IRQ returned through the bridge, which is what
   exposed defect 1.

## Fix

- `patches/gbarecomp/0006-bridge-stop-at-irq-return.patch`: an IRQ
  continuation stops as soon as the interpreted exception return from IRQ mode
  matches the IRQ's nesting depth.
- `tools/build-android.sh` passes the BIOS config; the regenerated
  `bios_dispatch_table.cpp` is identical to the one in upstream `af47d28`, and
  the BIOS misses (0x138, 0x300, 0x348, 0x34C, 0x2664-0x27BA) are gone.

Verified on the RG556: after 0006 the crash did not recur in a longer session
and the user saw no more pixel glitches; the BIOS-config build then ran with
no BIOS dispatch misses. The engine test suite (30 tests) passes on macOS.

Still bridged by design: the IWRAM code at 0x03006C1C..0x03006D80 (e.g.
0x03006D18, 0x03006D80), which the game builds at runtime (see `game.toml`).

## Save failure: "unable to save" (FLASH512)

Talking to the save frog answered "unable to save", and no `.sav` was written.
gbarecomp reported Macronix MX29L010 (C2/09, a 1 Mbit part) for every flash
size. MOTHER 3's `FLASH512_V131` (0x08CDB124) matches the ID against its setup
table only: SST 39VF512 (BF/D4, 0x08CDB1D8), Macronix MX29L512 (C2/1C,
0x08CDB25C), Panasonic MN63F805MNP (32/1B, 0x08CDB288), and otherwise falls back
to an entry with ID 00/00 whose writes fail. JP and EN were not compared here
(the EN ROM was checked).

`patches/gbarecomp/0007-flash512-chip-id.patch`: 64 KB flash reports the
Panasonic part, as mGBA does (`FLASH_MFG_PANASONIC`); 128 KB keeps the
MX29L010. The ID is re-derived when a savestate loads, so suspend states from
older builds save too. Verified on the RG556: the save succeeded and
`roms/mother3_en.sav` (64 KB) was written. `bus_tests` covers both sizes.

## Engine moved to mstan/gbarecomp (2026-10-14)

`agray0425/gbarecomp` was a fork of `mstan/gbarecomp` 98 commits behind it.
The fork now pins `mstan/gbarecomp@5e89943` with patches 0001-0006 rebased:
0004 was ported by hand (new web-host lines around the savestate lambdas),
0006 is the version proposed upstream as mstan/gbarecomp#44 (it leaves
mstan's resumable multiplayer context alone), and 0007 was dropped because
mstan fixed the flash ID in #28 (Macronix MX29L512, C2/1C, also in MOTHER 3's
table). Engine tests: 50/50 on macOS. On the RG556 the new build resumed a
suspend state written by the old engine and runs cleanly.

## How to collect data from the RG556

adb is authorised and `su` works (GammaOS userdebug):

```
adb logcat -d -b crash                                  # tombstone summary
adb shell "su -c 'cat /data/data/com.ylports.mother3recomp.en.dev/files/android-runtime.log'"
adb exec-out screencap -p > shot.png
```

Tracing: write the variables above to `files/debug-env.txt` (owned by the app
uid, mode 600) and start the game; delete the file afterwards, tracing slows
the emulation.
