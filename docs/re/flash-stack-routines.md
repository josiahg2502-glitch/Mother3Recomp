# FLASH512 routines run from the stack (IWRAM 0x03006C1C..0x03006D80)

Status: **resolved** (2026-10-14), EN verified on the RG556; JP addresses from
the symbols only.

## Symptom

Every session with an in-game save closed `NOT_STATIC`: about 20 distinct
dispatch misses in IWRAM 0x03006C1C..0x03006D80 (Thumb) and ~1.1 million
interpreted instructions, with two hot PCs (0x03006CF4 x32768, 0x03006CEC
x8192). `game.toml` had treated the range as code built at runtime because the
bytes there differed between savestates (a pointer table in one, zeros in the
other). mstan's coverage report flags the run as a `jump_table_candidate`,
which only means adjacent same-mode misses; it is not a hint about coverage.

## What runs there

The cartridge's Nintendo flash library (`FLASH512_V131` at 0x08CDB124; code at
0x080911xx-0x080919xx, EN ROM, JP has the same addresses in its symbols) copies
the routines that read flash into a stack buffer on every call and runs them
from there:

| Routine | ROM | Size | Copied by | Called |
|---|---|---|---|---|
| ReadFlash1 (`ldrb r0,[r0]; bx lr`) | 0x080911D4 | 4 | SetReadFlash1 0x080911D8 (buffer = caller's `sub sp`, 0x40/0x60) | through the pointer at 0x02015E14, by the program/erase status polls: thousands of times per save |
| ReadFlash_Core | 0x08091218 | 0x24 | ReadFlash 0x0809123C (`sub sp, #0x80`) | once per read |
| VerifyFlash_Core | 0x080912D8 | 0x30 | 0x0809130A, 0x080913A2 (`sub sp, #0x100`) | once per verify |
| erase verify (0xFF check) | 0x0809184C | 0x24 | referenced at 0x0809176C | after a sector erase (added 2026-10-24) |

Each caller jumps to `buffer | 1` through the `bx r3` helper at 0x08091940.
The library also times flash operations with a timer IRQ, so IRQ returns
resume inside the copies: that is where the interior misses (and most of the
counts) came from. The slot depends on the call depth and is reused for other
data, and copies by different callers overlap (0x03006CEC and 0x03006CF4 are 8
bytes apart), so a fixed `[[extra_func]]` / `[[code_copy]]` would be unsafe.

## Fix

mstan's engine has a game hook for exactly this:
`g_runtime_ram_dispatch_hook`, a "canonicalizer for position-independent code
copied to transient RAM addresses (notably routines executed from a moving
stack frame)". EmeraldRecomp uses it for the same library.
`mother3_ram_dispatch` in `src/main.cpp` takes a Thumb IWRAM PC, finds an
offset at which RAM holds a byte-exact copy of one of the three routines, and
runs `runtime_dispatch(rom + offset)`. ReadFlash1 (4 bytes) must in addition be
the copy that the pointer at 0x02015E14 names. The routines are position
independent (no PC-relative loads, internal branches only), and
`static_resume_all` gives each instruction a static entry (the only gaps are
the padding halfwords 0x0809123A and 0x08091306, never executed), so mid-routine
resumes stay native. Anything else falls through to normal dispatch.

## Evidence (RG556, EN, in-game save each time)

| Hook | Distinct misses | Interpreted instructions |
|---|---|---|
| none | 20 | ~1,100,000 |
| ReadFlash_Core + VerifyFlash_Core | 6 (ReadFlash1 copies) | 81,948 |
| + ReadFlash1 | 0 (`FULLY_STATIC`) | 0 |

## Update 2026-10-24: a fourth routine, and EWRAM copies

The upstream port (josiahg2502-glitch/Mother3Recomp-Android,
`src/mother3_runtime_hooks.cpp`) lists a fourth library routine that runs
from RAM: the erase verify at 0x0809184C, 0x24 bytes. It is
`push {lr}; mov r2, r0`, then a loop that counts down a timeout (literal
0x02015E20 inside the routine) while each byte reads 0xFF. The copier
references it at 0x0809176C. The upstream port also saw the copies in an
EWRAM buffer, not only on the IWRAM stack.

`mother3_ram_dispatch` now:
- includes the erase-verify routine;
- accepts Thumb PCs in EWRAM (0x02000000..0x0203FFFF) as well as IWRAM;
- keeps the same byte-exact match.

The earlier sessions ended FULLY_STATIC with saves, so this path was rare or
not reached. The routine was found by reading the upstream hook and the ROM,
not from a miss here.

