#!/usr/bin/env python3
"""
make_mod.py -- build the bundled example mod: an IPS patch for the demo cart.

Everything this patch writes is original content generated here (new banner
strings and a new banner colour ramp); it never copies artwork from any
commercial title.  It exists to prove the mod pipeline end to end:

    ROM bytes  ->  IPS patch  ->  gb_apply_patch()  ->  patched ROM in RAM

Usage:
    python3 tools/make_mod.py [rom] [-o mods/demo_banner.ips]
"""
import argparse
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# (needle in the unpatched ROM, replacement text)
BANNER = [
    (b"PKSILVER PC CORE", b"PKSILVER MODDED"),
    (b"SELF TEST OK", b"PATCH ACTIVE"),   # exactly 12 bytes: field is fixed-width
    (b"DPAD MOVE  A BOOST", b"DPAD MOVE  A BOOST"),   # unchanged, kept as control
]

# palette 1 (banner: white, light, mid, dark) -> new warm ramp, 15-bit LE
PALETTE_OLD = struct.pack("<HHHH", 0x7FFF, 0x5A9F, 0x2028, 0x0010)
PALETTE_NEW = struct.pack("<HHHH", 0x7FFF, 0x03FF, 0x7FE0, 0x4010)


def find(rom, needle, start=0):
    i = rom.find(needle, start)
    if i < 0:
        raise SystemExit(f"needle not found in ROM: {needle!r}")
    return i


def ips_record(offset, data):
    return struct.pack(">BH", (offset >> 16) & 0xFF, 0)[:1] + \
        struct.pack(">H", offset & 0xFFFF) + struct.pack(">H", len(data)) + data


def ips_offset3(offset):
    return bytes([(offset >> 16) & 0xFF, (offset >> 8) & 0xFF, offset & 0xFF])


def build(rom):
    """Return (ips_bytes, change_list)."""
    recs = []
    changes = []

    for needle, repl in BANNER:
        off = find(rom, needle)
        if repl == needle:
            continue                      # control entry: intentionally untouched
        # pad/truncate to the original field length so no code moves
        data = repl[:len(needle)].ljust(len(needle), b"\x00")
        recs.append(ips_offset3(off) + struct.pack(">H", len(data)) + data)
        changes.append((off, needle.decode(), data.rstrip(b"\x00").decode()))

    off = find(rom, PALETTE_OLD)
    recs.append(ips_offset3(off) + struct.pack(">H", len(PALETTE_NEW)) + PALETTE_NEW)
    changes.append((off, "banner palette", "warm ramp"))

    return b"PATCH" + b"".join(recs) + b"EOF", changes


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("rom", nargs="?",
                    default=os.path.join(ROOT, "assets", "pksilver_demo.gbc"))
    ap.add_argument("-o", "--out",
                    default=os.path.join(ROOT, "mods", "demo_banner.ips"))
    args = ap.parse_args()

    with open(args.rom, "rb") as fh:
        rom = fh.read()
    ips, changes = build(rom)

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "wb") as fh:
        fh.write(ips)

    print(f"wrote {args.out} ({len(ips)} bytes, {len(changes)} changes)")
    for off, old, new in changes:
        print(f"  ${off:04X}: {old!r} -> {new!r}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
