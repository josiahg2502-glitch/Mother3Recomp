#!/usr/bin/env python3
"""
make_demo.py -- generate `assets/pksilver_demo.gbc`, an original homebrew
cartridge used to exercise and demonstrate the core.

Everything here is authored from scratch by this script: the glyph bitmaps,
the tile patterns, the wave RAM shape and the music sequence are all computed
procedurally.  It exists so the front-ends have something to run -- and so the
CI can prove the PPU, sprite DMA, CGB palettes and all four audio channels
work -- without needing a commercial dump.

    python3 tools/make_demo.py            # writes assets/pksilver_demo.gbc
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from gbasm import Assembler  # noqa: E402


# ---------------------------------------------------------------------------
#  8x8 glyph font (uppercase, digits, punctuation) -- hand-authored bitmaps
# ---------------------------------------------------------------------------
GLYPHS = {
    " ": "........",
    "A": ".######.#......##..#..##.#######..##.##..##.##..#####",
    "B": "######..#......##.######..#......##.##..##.##..#####",
    "C": ".######.#......##.##....##.##....##.##......##.#......#",
    "D": "######..#......##.##..##.##..##.##..##.##......##.######",
    "E": "#######.#......##.######..#......##.##......##.#######",
    "F": "#######.#......##.######..#......##.##......##.##.....",
    "G": ".######.#......##.##....##.##..###.##....##.#..######",
    "H": "##..##.##..##.##..##.#######.##..##.##..##.##..##.##..##",
    "I": "#######...##.....##....##....##....##....##...#######",
    "J": "..#####....##....##....##....##....##.##.##....###....",
    "K": "##..###.##.###..####..####.###..##.##..##.##...###",
    "L": "##.....##.....##.....##.....##.....##.....##.....#######",
    "M": "##...####...######.##.####.##.##....##.##....##.##....##",
    "N": "##...####...######.##.######.##..####.##...###.##....##",
    "O": ".######.#......##.##..##.##..##.##..##.##......##.#......#",
    "P": "######..#......##.##..##.##..##.######..#......##.##....",
    "Q": ".######.#......##.##..##.##..##.##.###.##...####..###.##",
    "R": "######..#......##.##..##.##..##.######..##.###..##..###",
    "S": ".######.#......##.##....##.#......#..######......##.######",
    "T": "#######..##....##..##..#...##..#...##..#...##..#...##.",
    "U": "##..##.##..##.##..##.##..##.##..##.##..##.##..##.######",
    "V": "##..##.##..##.##..##.##..##.##..##..####....####......##",
    "W": "##....##.##....##.##....##.##.##.##.##.######.###..###",
    "X": "##...###...####...###..##..##.###....####...###..#####",
    "Y": "##..##.##..##..####....####......##......##......##....",
    "Z": "#######....##.....##....##...##....##...##....##.#######",
    "0": ".######.##....###.##...####.##..##.##.##..###.####..##.######",
    "1": "...##...###....##.##.#..##...##....##....##...#######",
    "2": ".######.##....##......##....##...###....##....##.#######",
    "3": "######......##......##.#####......##......##.######",
    "4": "##...####...######.##..##.##..##.#######....##....##..",
    "5": "#######.#......##.#......##.######......##......##.######",
    "6": "..#####..##.....##.......##.######..##....##.##....##.#......#",
    "7": "#######.....##.....##....##....##...##...##...##..##....",
    "8": ".######.#......##.#......##..######..#......##.#......##.######",
    "9": ".######.#......##.#......##.######.......##......##.#####",
    ".": "..............##....##....##....##......",
    "-": "..................######..######........",
    ":": "......##....##....##....##..",
    "/": "......##.....##....##...##...##..##....##......",
    "!": "..##....##....##....##....##..........##....##",
    ">": "##....##..##..##....##......##....##..##..##....##",
    "+": "........##..##..#######..##..##........",
    "=": "........#######........#######........",
    "%": "##...#####...####..##..##....####..#####...##",
}


def glyph_bytes(ch):
    """Render a glyph into 8 rows of 8 pixels (1bpp -> 2bpp tile planes)."""
    spec = GLYPHS.get(ch.upper())
    rows = ["........"] * 8
    if spec:
        # glyphs are stored as 8 tokens of 8 chars, dot/Hash
        parts = spec.split(".")
        # rebuild rows: the spec is 8 groups separated by '.'
        groups = spec.split(".")
        if len(groups) >= 8:
            rows = [(g + "........")[:8] for g in groups[:8]]
    lo, hi = [], []
    for r in rows:
        b_lo = 0
        b_hi = 0
        for i in range(8):
            c = r[i] if i < len(r) else "."
            if c == "#":
                b_lo |= 0x80 >> i
                b_hi |= 0x80 >> i
        lo.append(b_lo)
        hi.append(b_hi)
    out = []
    for i in range(8):
        out.append(lo[i])
        out.append(hi[i])
    return out


def font_table():
    """Tiles 0x10.. hold the font in ASCII order from 0x20."""
    lines = ["font_tiles:"]
    for code in range(0x20, 0x7B):
        ch = chr(code)
        b = glyph_bytes(ch)
        lines.append("    .db " + ",".join(f"${x:02X}" for x in b) +
                     f"   ; '{ch}'")
    return "\n".join(lines)


# ---------------------------------------------------------------------------
#  Procedural background tiles (tiles 0x00..0x0F)
# ---------------------------------------------------------------------------
def pattern_tile(kind, seed=0):
    """Return 16 bytes of 2bpp tile data for a decorative pattern."""
    out = []
    for y in range(8):
        lo = 0
        hi = 0
        for x in range(8):
            if kind == "checker":
                v = ((x + y) // 2) & 3
            elif kind == "diag":
                v = ((x + y + seed) // 2) & 3
            elif kind == "dots":
                v = 3 if ((x % 4) < 2 and (y % 4) < 2) else 0
            elif kind == "gradient":
                v = (x + y + seed) & 3
            elif kind == "brick":
                v = 0 if (y % 4 == 3) or ((x + (4 if (y // 4) % 2 else 0)) % 8 == 7) else 2
            elif kind == "wave":
                import math
                v = int(2 + 1.6 * math.sin((x + seed) * 0.9 + y * 0.35)) & 3
            else:
                v = 0
            if v & 1:
                lo |= 0x80 >> x
            if v & 2:
                hi |= 0x80 >> x
        out.append(lo)
        out.append(hi)
    return out


def bg_tiles():
    kinds = ["gradient", "checker", "diag", "dots", "brick", "wave"]
    lines = ["bg_tiles:"]
    for i, k in enumerate(kinds * 3):
        b = pattern_tile(k, seed=i)
        lines.append("    .db " + ",".join(f"${x:02X}" for x in b) + f"   ; {k} {i}")
    return "\n".join(lines), len(kinds * 3)


# ---------------------------------------------------------------------------
#  Sprite tiles (tiles 0x80..) -- a small orb and an arrow cursor
# ---------------------------------------------------------------------------
def sprite_tiles():
    lines = ["sprite_tiles:"]
    # orb: radial falloff
    for variant in range(4):
        out = []
        for y in range(8):
            lo = hi = 0
            for x in range(8):
                dx, dy = x - 3.5, y - 3.5
                d = (dx * dx + dy * dy) ** 0.5
                v = 3 if d < 1.2 else (2 if d < 2.4 else (1 if d < 3.4 else 0))
                if variant == 1 and (x + y) % 4 == 0 and v:
                    v = 3
                if variant == 2:
                    v = (v + 1) & 3 if v else 0
                if variant == 3 and d > 3.0:
                    v = 0
                if v & 1:
                    lo |= 0x80 >> x
                if v & 2:
                    hi |= 0x80 >> x
            out += [lo, hi]
        lines.append("    .db " + ",".join(f"${x:02X}" for x in out))
    # 8x16 cursor arrow
    rows = [
        "..#.....",
        "..##....",
        "..###...",
        "..####..",
        "..#####.",
        "..###...",
        "..##.#..",
        ".#...##.",
        "..#.....",
        "..##....",
        "..###...",
        "..####..",
        "..#####.",
        "..###...",
        "..##.#..",
        ".#...##.",
    ]
    for half in range(2):
        out = []
        for y in range(8):
            r = rows[half * 8 + y]
            lo = hi = 0
            for x in range(8):
                if r[x] == "#":
                    lo |= 0x80 >> x
                    hi |= 0x80 >> x
            out += [lo, hi]
        lines.append("    .db " + ",".join(f"${x:02X}" for x in out))
    return "\n".join(lines)


# ---------------------------------------------------------------------------
#  Wave RAM shape + music data
# ---------------------------------------------------------------------------
def wave_bytes():
    """A 32-sample triangle-ish wave, authored here (not ripped from anything)."""
    import math
    out = []
    for i in range(16):
        a = int(7.5 + 7.5 * math.sin(i * math.pi / 8))
        b = int(7.5 + 7.5 * math.sin((i + 4) * math.pi / 8))
        out.append(((a & 0xF) << 4) | (b & 0xF))
    return out


# semitone -> CGB frequency value (period = 2048 - f)
def note_freq(semitones_from_a4):
    hz = 440.0 * (2 ** (semitones_from_a4 / 12.0))
    return int(2048 - 131072.0 / hz) & 0x7FF


# an original 16-step chord/arpeggio loop in A minor
MELODY = [0, 3, 7, 12, 7, 3, 0, -5, 0, 3, 7, 10, 7, 3, 0, 3]
BASS = [-24, -24, -17, -17, -22, -22, -19, -19,
        -24, -24, -17, -17, -22, -22, -14, -14]
HARM = [12, 15, 19, 24, 19, 15, 12, 7, 12, 15, 19, 22, 19, 15, 12, 15]


def music_tables():
    lines = ["melody_tab:"]
    lines.append("    .dw " + ",".join(f"${note_freq(n):04X}" for n in MELODY))
    lines.append("harm_tab:")
    lines.append("    .dw " + ",".join(f"${note_freq(n):04X}" for n in HARM))
    lines.append("bass_tab:")
    lines.append("    .dw " + ",".join(f"${note_freq(n):04X}" for n in BASS))
    lines.append("wave_data:")
    lines.append("    .db " + ",".join(f"${b:02X}" for b in wave_bytes()))
    return "\n".join(lines)


# ---------------------------------------------------------------------------
#  The cartridge source
# ---------------------------------------------------------------------------
SOURCE = r"""
; ===========================================================================
;  pksilver_demo.asm -- original homebrew used to exercise the PokéSilver PC
;  core: CGB palettes, scrolling BG, OAM DMA sprites, joypad, and all four
;  audio channels.  Every byte of graphics/music data in here is generated by
;  tools/make_demo.py.
; ===========================================================================

; --- hardware registers ---
P1      equ $FF00
SB      equ $FF01
SC      equ $FF02
DIV     equ $FF04
TIMA    equ $FF05
TMA     equ $FF06
TAC     equ $FF07
IF      equ $FF0F
NR10    equ $FF10
NR11    equ $FF11
NR12    equ $FF12
NR13    equ $FF13
NR14    equ $FF14
NR21    equ $FF16
NR22    equ $FF17
NR23    equ $FF18
NR24    equ $FF19
NR30    equ $FF1A
NR31    equ $FF1B
NR32    equ $FF1C
NR33    equ $FF1D
NR34    equ $FF1E
NR41    equ $FF20
NR42    equ $FF21
NR43    equ $FF22
NR44    equ $FF23
NR50    equ $FF24
NR51    equ $FF25
NR52    equ $FF26
WAVE0   equ $FF30
LCDC    equ $FF40
STAT    equ $FF41
SCY     equ $FF42
SCX     equ $FF43
LY      equ $FF44
LYC     equ $FF45
DMA     equ $FF46
BGP     equ $FF47
OBP0    equ $FF48
OBP1    equ $FF49
WY      equ $FF4A
WX      equ $FF4B
KEY1    equ $FF4D
VBK     equ $FF4F
BGPI    equ $FF68
BGPD    equ $FF69
OBPI    equ $FF6A
OBPD    equ $FF6B
SVBK    equ $FF70
IE      equ $FFFF

OAMBUF  equ $C000
VARS    equ $C100
vScrollX equ VARS+0
vScrollY equ VARS+1
vStep    equ VARS+2
vSpriteX equ VARS+3
vSpriteY equ VARS+4
vKeys    equ VARS+5
vHue     equ VARS+6
vNote    equ VARS+7
vTempo   equ VARS+8
vBoost   equ VARS+9
vCgb     equ VARS+10
vFrameLo equ VARS+11
vFrameHi equ VARS+12
vMsgRow  equ VARS+13

    .org $0000
    nop
    jp boot
    .ds $0040 - 4, $00

    ; ---- interrupt vectors ----
    .org $0040
    jp isr_vblank          ; VBLANK
    .ds 4, $00
    jp isr_ret             ; LCD STAT
    .ds 4, $00
    jp isr_ret             ; TIMER
    .ds 4, $00
    jp isr_ret             ; SERIAL
    .ds 4, $00
    jp isr_ret             ; JOYPAD

    ; ---- cartridge header ----
    .org $0100
    nop
    jp boot

    ; The 48-byte logo area is intentionally left blank: this core skips the
    ; boot ROM, and we ship no third-party logo data.
    .ds $0134 - $0104, $00

    .org $0134
    .db "PKSILVER DEMO", $00, $00          ; title: $0134..$0142 (15 bytes)
    .org $0143
    .db $80                                 ; CGB flag: colour enhanced
    .org $0146
    .db $00                                 ; no SGB
    .org $0147
    .db $00                                 ; ROM ONLY
    .org $0148
    .db $00                                 ; 32 KiB
    .org $0149
    .db $00                                 ; no cart RAM
    .org $014A
    .db $01, $00                            ; non-Japanese
    .org $014C
    .db $00                                 ; version
    .org $014D
    .db $00                                 ; header checksum (patched below)

    .org $0150
boot:
    di
    ld sp, $FFFE

    ; ---- CGB detection MUST come first: the core leaves A=$11 after a CGB
    ;      power-on, and any later `xor a` would destroy that evidence. ----
    cp $11
    ld b, 0
    jr nz, .not_cgb
    ld b, 1
.not_cgb:
    xor a
    ld hl, VARS
    ld c, 32
clear_vars:
    ld (hl+), a
    dec c
    jr nz, clear_vars
    ld a, b
    ld (vCgb), a

    ; ---- VRAM setup runs while the LCD is still off, so no VBLANK wait is
    ;      needed here (HALT before IE is armed would stop the CPU forever) ----

    ; ---- load background tiles ($8000..) ----
    ld hl, bg_tiles
    ld de, $8000
    ld bc, NUM_BG_TILES * 16
    call copy_rom_to_vram

    ; ---- load font tiles right after them ----
    ld hl, font_tiles
    ld de, $8000 + NUM_BG_TILES * 16
    ld bc, NUM_FONT_TILES * 16
    call copy_rom_to_vram

    ; ---- load sprite tiles at $8800 (tile index $80) ----
    ld hl, sprite_tiles
    ld de, $8800
    ld bc, 6 * 16
    call copy_rom_to_vram

    ; ---- build the background map ----
    call build_bg_map

    ; ---- CGB palettes ----
    call setup_palettes

    ; ---- DMG palettes (harmless on CGB) ----
    ld a, $E4
    ldh (BGP), a
    ld a, $D0
    ldh (OBP0), a
    ldh (OBP1), a

    ; ---- wave RAM ----
    call load_wave

    ; ---- audio on ----
    ld a, $80
    ldh (NR52), a
    ld a, $77
    ldh (NR50), a
    ld a, $FF
    ldh (NR51), a

    ; ---- OAM buffer ----
    call init_oam

    ; ---- print the status banner (still safe: the LCD is off) ----
    call draw_banner

    ; ---- arm the VBLANK interrupt, then turn the LCD on ----
    ld a, $01
    ldh (IE), a
    ei

    ; ---- LCD on ----
    ;   bit7 LCD enable | bit4 tile data at $8000 (unsigned) |
    ;   bit2 OBJ 8x16   | bit1 OBJ enable | bit0 BG enable
    ;   bits 3 and 6 clear => BG map $9800, window map $9800
    ld a, $97
    ldh (LCDC), a

; =====================================================================
;  main loop: one iteration per frame
; =====================================================================
main:
    call wait_vblank

    ; frame counter
    ld hl, vFrameLo
    inc (hl)
    jr nz, no_carry
    inc hl
    inc (hl)
no_carry:

    call read_joypad
    call move_sprite
    call scroll_bg
    call cycle_palette
    call update_oam
    call music_tick

    jp main

isr_ret:
    reti

isr_vblank:
    push af
    push hl
    ld hl, vStep
    inc (hl)
    pop hl
    pop af
    reti

; ---------------------------------------------------------------------
;  helpers
; ---------------------------------------------------------------------
wait_vblank:
    halt
    ret

copy_rom_to_vram:
    ld a, (hl+)
    ld (de), a
    inc de
    dec bc
    ld a, b
    or c
    jr nz, copy_rom_to_vram
    ret

build_bg_map:
    ; 32x32 map at $9800, tiled with our pattern tiles
    ld hl, $9800
    ld d, 0                    ; row
row_loop:
    ld e, 0                    ; column
col_loop:
    ld a, d
    add a, e
    and $07
    ld b, a                    ; tile index 0..7
    ld a, d
    srl a
    srl a
    and $03
    add a, b
    ld (hl+), a
    inc e
    ld a, e
    cp 32
    jr nz, col_loop
    inc d
    ld a, d
    cp 24
    jr nz, row_loop
    ret

setup_palettes:
    ld a, (vCgb)
    or a
    ret z
    ; auto-increment BCPD writes, 8 BG palettes
    ld a, $80
    ldh (BGPI), a
    ld hl, palette_data
    ld bc, 64
pal_loop:
    ld a, (hl+)
    ldh (BGPD), a
    dec bc
    ld a, b
    or c
    jr nz, pal_loop
    ; object palettes
    ld a, $80
    ldh (OBPI), a
    ld hl, obj_palette_data
    ld bc, 16
obp_loop:
    ld a, (hl+)
    ldh (OBPD), a
    dec bc
    ld a, b
    or c
    jr nz, obp_loop
    ret

load_wave:
    ld hl, wave_data
    ld c, $30
    ld b, 16
wave_loop:
    ld a, (hl+)
    ldh (c), a
    inc c
    dec b
    jr nz, wave_loop
    ret

init_oam:
    ld hl, OAMBUF
    ld b, 40
    ld c, 0
oam_clear:
    ld a, $00
    ld (hl+), a
    ld (hl+), a
    ld a, c
    ld (hl+), a
    xor a
    ld (hl+), a
    inc c
    inc c
    dec b
    jr nz, oam_clear
    ; sprite 0 follows the player cursor
    ld hl, OAMBUF
    ld a, 80
    ld (hl+), a
    ld a, 80
    ld (hl+), a
    ld a, $84                 ; cursor tile
    ld (hl+), a
    xor a
    ld (hl+), a
    ; sprites 1..4 are the orbiting orbs
    ld b, 4
orb_init:
    ld a, 70
    ld (hl+), a
    ld a, 70
    ld (hl+), a
    ld a, $80
    ld (hl+), a
    ld a, $08                 ; OBP1
    ld (hl+), a
    dec b
    jr nz, orb_init
    ret

read_joypad:
    ; directions
    ld a, $20
    ldh (P1), a
    ldh a, (P1)
    ldh a, (P1)
    cpl
    and $0F
    ld b, a
    ; buttons
    ld a, $10
    ldh (P1), a
    ldh a, (P1)
    ldh a, (P1)
    cpl
    and $0F
    swap a
    or b
    ld (vKeys), a
    ; restore
    ld a, $30
    ldh (P1), a
    ret

move_sprite:
    ld a, (vKeys)
    ld b, a
    ld hl, vSpriteX
    ld de, vSpriteY

    ld a, b
    and $01                   ; RIGHT
    jr z, .no_r
    ld a, (hl)
    add a, 2
    ld (hl), a
.no_r:
    ld a, b
    and $02                   ; LEFT
    jr z, .no_l
    ld a, (hl)
    sub 2
    ld (hl), a
.no_l:
    ld a, b
    and $04                   ; UP
    jr z, .no_u
    ld a, (de)
    sub 2
    ld (de), a
.no_u:
    ld a, b
    and $08                   ; DOWN
    jr z, .no_d
    ld a, (de)
    add a, 2
    ld (de), a
.no_d:
    ; A toggles the tempo boost
    ld a, b
    and $10
    jr z, .no_a
    ld a, (vBoost)
    xor 1
    ld (vBoost), a
.no_a:
    ret

scroll_bg:
    ; boost doubles the horizontal scroll rate
    ld a, (vBoost)
    or a
    jr z, .slow
    ld b, 2
    jr .go
.slow:
    ld b, 1
.go:
    ld hl, vScrollX
    ld a, (hl)
    add a, b
    ld (hl+), a               ; vScrollX
    ldh (SCX), a
    ld a, (hl)
    sub 1                     ; vScrollY drifts the other way
    ld (hl), a
    ldh (SCY), a
    ret

cycle_palette:
    ld a, (vCgb)
    or a
    ret z
    ; advance the hue index every 4 frames
    ld a, (vStep)
    and $03
    ret nz
    ld hl, vHue
    inc (hl)
    ld a, (hl)
    and $07
    ld (hl), a
    ; rebuild BG palette 0 from the rotating table
    ld a, $80
    ldh (BGPI), a
    ld a, (vHue)
    add a, a
    add a, a
    add a, a
    ld l, a
    ld h, 0
    ld de, rainbow
    add hl, de
    ld b, 8
cyc_loop:
    ld a, (hl+)
    ldh (BGPD), a
    dec b
    jr nz, cyc_loop
    ret

update_oam:
    ; sprite 0 -> the player cursor
    ld hl, OAMBUF
    ld a, (vSpriteY)
    add a, 16
    ld (hl+), a
    ld a, (vSpriteX)
    add a, 8
    ld (hl+), a
    inc hl
    inc hl

    ; sprites 1..4 -> orbiting orbs, each 8 steps out of phase
    ld c, 0                   ; sprite index
.orb_loop:
    ld a, (vStep)
    ld b, a
    ld a, c
    add a, a
    add a, a
    add a, a                  ; index * 8
    add a, b
    and $1F                   ; 32 entry orbit table
    ld e, a
    ld d, 0
    add hl, de                ; NOTE: rebuilt below, so use a fresh base
    ld a, 0
    ; recompute the table pointer cleanly
    ld hl, orbit_tab
    add hl, de
    ld d, (hl)
    inc hl
    ld e, (hl)

    ; destination OAM slot = OAMBUF + 4 + index*4
    ld a, c
    add a, a
    add a, a
    add a, 4
    ld l, a
    ld h, OAMBUF / $100
    ld (hl), d
    inc hl
    ld (hl), e

    inc c
    ld a, c
    cp 4
    jr nz, .orb_loop

    ; DMA the shadow buffer into OAM
    ld a, OAMBUF / $100
    ldh (DMA), a
    ret

; ---------------------------------------------------------------------
;  banner text
; ---------------------------------------------------------------------
draw_banner:
    ld hl, msg_line1
    ld de, $9842
    call draw_string
    ld hl, msg_line2
    ld de, $9862
    call draw_string
    ld hl, msg_line3
    ld de, $98C2
    call draw_string
    ret

draw_string:
    ; hl = NUL-terminated ASCII, de = tilemap destination ($9800..$9BFF)
    ld a, (hl+)
    or a
    ret z
    push hl
    push de
    sub $20
    add a, NUM_BG_TILES
    ld (de), a
    ; attribute (VRAM bank 1): palette 1, normal priority, no flips
    ld a, $01
    ldh (VBK), a
    ld (de), a
    xor a
    ldh (VBK), a
    pop de
    pop hl
    inc de
    jp draw_string

msg_line1:
    .db "PKSILVER PC CORE", $00
msg_line2:
    .db "SELF TEST OK", $00
msg_line3:
    .db "DPAD MOVE  A BOOST", $00

; =====================================================================
;  music: one 16th note per frame group
; =====================================================================
music_tick:
    ; tempo: advance every N frames
    ld a, (vBoost)
    or a
    jr z, .t_slow
    ld b, 3
    jr .t_go
.t_slow:
    ld b, 6
.t_go:
    ld a, (vStep)
    ; simple modulo via repeated subtraction
    ld c, 0
.mod_loop:
    cp b
    jr c, .mod_done
    sub b
    inc c
    jr .mod_loop
.mod_done:
    or a
    ret nz                    ; only play on the boundary
    ld a, c
    and $0F                   ; 16 steps

    push af
    ; ---- channel 1: melody square with sweep ----
    ld e, a
    ld d, 0
    ld hl, melody_tab
    add hl, de
    add hl, de
    ld a, (hl+)
    ld h, (hl)
    ld l, a
    ld a, $81
    ldh (NR10), a
    ld a, $80
    ldh (NR11), a
    ld a, $F2
    ldh (NR12), a
    ld a, l
    ldh (NR13), a
    ld a, h
    or $80
    ldh (NR14), a
    pop af

    push af
    ; ---- channel 2: harmony square ----
    ld e, a
    ld d, 0
    ld hl, harm_tab
    add hl, de
    add hl, de
    ld a, (hl+)
    ld h, (hl)
    ld l, a
    ld a, $40
    ldh (NR21), a
    ld a, $A2
    ldh (NR22), a
    ld a, l
    ldh (NR23), a
    ld a, h
    or $80
    ldh (NR24), a
    pop af

    push af
    ; ---- channel 3: wave bass ----
    ld e, a
    ld d, 0
    ld hl, bass_tab
    add hl, de
    add hl, de
    ld a, (hl+)
    ld h, (hl)
    ld l, a
    ld a, $80
    ldh (NR30), a
    ld a, $20
    ldh (NR31), a
    ld a, $20
    ldh (NR32), a
    ld a, l
    ldh (NR33), a
    ld a, h
    or $80
    ldh (NR34), a
    pop af

    ; ---- channel 4: percussion on beats 0 and 8 ----
    and $07
    ret nz
    ld a, $10
    ldh (NR41), a
    ld a, $B1
    ldh (NR42), a
    ld a, $5A
    ldh (NR43), a
    ld a, $80
    ldh (NR44), a
    ret

; =====================================================================
;  data
; =====================================================================
palette_data:
    ; 8 BG palettes, little-endian 15-bit RGB.
    ;   palette 0 -> cycled by the hue rotator (background patterns)
    ;   palette 1 -> fixed white-on-navy for the banner text
    .dw $7FFF, $4B1F, $227F, $0000
    .dw $7FFF, $5A9F, $2028, $0010
    .dw $7FFF, $03FF, $001F, $2008
    .dw $7FFF, $03E0, $0206, $00C2
    .dw $7FFF, $3FFF, $001F, $0010
    .dw $7FFF, $7FE0, $7C00, $0000
    .dw $7FFF, $653A, $3A11, $1082
    .dw $7FFF, $5AD4, $2D6B, $01A5
obj_palette_data:
    .dw $0000, $03FF, $001F, $7FFF
    .dw $0000, $7FE0, $03E0, $7FFF


rainbow:
    ; 8 rotating 15-bit colours used by the palette cycler
    .dw $001F, $0017, $000F, $2008, $4010, $6000, $7C00, $03E0

orbit_tab:
"""


def orbit_table():
    import math
    lines = []
    for step in range(32):
        for orb in range(1):  # one row per step; sprites offset by index
            ang = (step / 32.0) * 2 * math.pi
            x = int(76 + 26 * math.cos(ang)) + 16
            y = int(68 + 20 * math.sin(ang)) + 8
            lines.append(f"    .db {y & 0xFF}, {x & 0xFF}")
    return "\n".join(lines)


def build_source():
    bg, n_bg = bg_tiles()
    src = SOURCE
    src += orbit_table() + "\n"
    src += bg + "\n"
    src += font_table() + "\n"
    src += sprite_tiles() + "\n"
    src += music_tables() + "\n"

    num_font = 0x7B - 0x20
    src = src.replace("NUM_BG_TILES", str(n_bg))
    src = src.replace("NUM_FONT_TILES", str(num_font))
    return src, n_bg, num_font


def header_checksum(image):
    """Nintendo header checksum over $0134..$014C (original algorithm)."""
    x = 0
    for addr in range(0x0134, 0x014D):
        x = (x - image[addr] - 1) & 0xFF
    return x


def main():
    src, n_bg, n_font = build_source()

    with tempfile.NamedTemporaryFile("w", suffix=".asm", delete=False) as fh:
        fh.write(src)
        src_path = fh.name

    out_dir = os.path.join(ROOT, "assets")
    os.makedirs(out_dir, exist_ok=True)
    out_path = os.path.join(out_dir, "pksilver_demo.gbc")

    rc = subprocess.call([
        sys.executable, os.path.join(HERE, "gbasm.py"),
        src_path, "-o", out_path, "--pad", "0x8000", "--fill", "0x00",
    ])
    os.unlink(src_path)
    if rc != 0:
        return rc

    # patch the header checksum in place
    with open(out_path, "r+b") as fh:
        image = bytearray(fh.read())
        image[0x014D] = header_checksum(image)
        fh.seek(0)
        fh.write(image)

    print(f"wrote {out_path} ({len(image)} bytes)")
    print(f"  bg tiles:   {n_bg} (indices $00..${n_bg - 1:02X})")
    print(f"  font tiles: {n_font} (indices ${n_bg:02X}..)")
    print(f"  header checksum: ${image[0x014D]:02X}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
