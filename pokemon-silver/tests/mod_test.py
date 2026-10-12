#!/usr/bin/env python3
"""
mod_test.py -- verify the mod pipeline through the real core.

  1. build the example IPS from tools/make_mod.py
  2. push it through gb_apply_patch() (the same call the web/native front-ends use)
  3. read the patched ROM back out of gb_rom_data() and diff it against
     the unpatched image: only the intended regions may differ
  4. run 60 frames of the patched cart and confirm it still renders and
     still produces audio (a patch must not break the game)

Usage: python3 tests/mod_test.py [rom]
"""
import ctypes
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import make_mod  # noqa: E402

LCD_W, LCD_H = 160, 144


def open_core():
    path = os.path.join(ROOT, "build", "libpksilver_core.so")
    if not os.path.exists(path):
        raise SystemExit("run tools/build.sh native first")
    lib = ctypes.CDLL(path)
    lib.gb_init.restype = None
    lib.gb_rom_buffer.restype = ctypes.POINTER(ctypes.c_uint8)
    lib.gb_load_rom.argtypes = [ctypes.c_size_t]
    lib.gb_load_rom.restype = ctypes.c_int
    lib.gb_apply_patch.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t]
    lib.gb_apply_patch.restype = ctypes.c_int
    lib.gb_rom_data.restype = ctypes.POINTER(ctypes.c_uint8)
    lib.gb_run_frame.restype = ctypes.c_int
    lib.gb_framebuffer.restype = ctypes.POINTER(ctypes.c_uint32)
    lib.gb_render_audio.argtypes = [ctypes.POINTER(ctypes.c_int16), ctypes.c_int]
    lib.gb_render_audio.restype = ctypes.c_int
    lib.gb_rom_crc32.restype = ctypes.c_uint32
    lib.gb_init()
    return lib


def read_rom(lib, n):
    p = lib.gb_rom_data()
    return bytes(ctypes.string_at(p, n))


def main():
    rom_path = sys.argv[1] if len(sys.argv) > 1 else \
        os.path.join(ROOT, "assets", "pksilver_demo.gbc")
    with open(rom_path, "rb") as fh:
        rom = fh.read()

    lib = open_core()
    buf = lib.gb_rom_buffer()
    ctypes.memmove(buf, rom, len(rom))
    if lib.gb_load_rom(len(rom)) != 0:
        raise SystemExit("cartridge did not load")

    crc_before = lib.gb_rom_crc32()
    original = read_rom(lib, len(rom))
    assert original == rom, "rom buffer round-trip mismatch"

    ips, changes = make_mod.build(rom)
    pbuf = (ctypes.c_uint8 * len(ips)).from_buffer_copy(ips)
    rc = lib.gb_apply_patch(pbuf, len(ips))

    ok = True

    def check(label, cond, detail=""):
        nonlocal ok
        print(f"  [{'PASS' if cond else 'FAIL'}] {label} {detail}")
        if not cond:
            ok = False

    print("== mod pipeline ==")
    check("gb_apply_patch accepted the IPS", rc == 0, f"(rc={rc})")

    patched = read_rom(lib, len(rom))
    diffs = [i for i in range(len(rom)) if patched[i] != original[i]]
    check("bytes changed", len(diffs) > 0, f"({len(diffs)} bytes)")

    # every diff must fall inside one of the regions we intended to write
    widths = {"PKSILVER PC CORE": 16, "SELF TEST OK": 12, "banner palette": 8}
    regions = [(off, widths[old]) for off, old, _new in changes]
    inside = all(any(s <= i < s + n for s, n in regions) for i in diffs)
    check("only intended regions changed", inside,
          f"(regions {[(hex(s), n) for s, n in regions]})")

    for needle, repl in make_mod.BANNER:
        if repl == needle:
            check(f"control entry intact: {needle.decode()!r}", needle in patched)
        else:
            check(f"banner rewritten -> {repl.decode()!r}", repl in patched)

    check("palette rewritten", make_mod.PALETTE_NEW in patched)
    check("CRC changed", lib.gb_rom_crc32() != crc_before,
          f"({crc_before:08X} -> {lib.gb_rom_crc32():08X})")

    # the patched cart must still run
    fb = lib.gb_framebuffer()
    colours = set()
    peak = 0
    abuf = (ctypes.c_int16 * (1024 * 2))()
    for f in range(60):
        lib.gb_run_frame()
        if f == 59:
            for i in range(LCD_W * LCD_H):
                colours.add(fb[i])
        n = lib.gb_render_audio(abuf, 1024)
        for i in range(n * 2):
            if abs(abuf[i]) > peak:
                peak = abs(abuf[i])

    check("patched cart still renders", len(colours) >= 5, f"({len(colours)} colours)")
    check("patched cart still makes audio", peak > 2000, f"(peak {peak})")

    print("== RESULT:", "ALL PASS" if ok else "FAILURES", "==")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
