#!/usr/bin/env python3
"""
make_fixture.py -- build the compact CPU test fixture shipped in this repo.

The authoritative vectors live in https://github.com/singleStepTests/sm83
(MIT License, Copyright (c) 2024 SingleStepTests).  That checkout is ~192 MB,
so we sample it down into tests/vectors.bin -- a few hundred KiB that still
covers every opcode and every CB sub-opcode, which is what CI actually needs.

Usage:
    git clone --depth 1 --filter=blob:none --sparse \
        https://github.com/singleStepTests/sm83.git /tmp/sm83
    cd /tmp/sm83 && git sparse-checkout set v1
    python3 tests/make_fixture.py --vectors /tmp/sm83/v1 --per-opcode 20

Layout of vectors.bin (little endian):
    "SM83" | u32 version | u32 count | u32 reserved
    then, per test:
        u8 name_len, name bytes
        initial: u16 pc, u16 sp, u8 a,b,c,d,e,f,h,l, u8 ime, u8 ie
                 u16 ram_count, then ram_count * (u16 addr, u8 val)
        final:   same shape
"""
import argparse
import json
import os
import struct
import sys

VERSION = 1
REGS = ["a", "b", "c", "d", "e", "f", "h", "l"]


def pack_state(st):
    out = struct.pack("<HH", st["pc"] & 0xFFFF, st["sp"] & 0xFFFF)
    out += bytes(st[r] & 0xFF for r in REGS)
    out += bytes([1 if st.get("ime") else 0, st.get("ie", 0) & 0xFF])
    ram = st.get("ram", [])
    out += struct.pack("<H", len(ram))
    for addr, val in ram:
        out += struct.pack("<HB", addr & 0xFFFF, val & 0xFF)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--vectors", required=True, help="directory of v1/*.json")
    ap.add_argument("--out", default=os.path.join(os.path.dirname(
        os.path.abspath(__file__)), "vectors.bin"))
    ap.add_argument("--per-opcode", type=int, default=20)
    args = ap.parse_args()

    files = sorted(f for f in os.listdir(args.vectors) if f.endswith(".json"))
    if not files:
        print(f"no json vectors in {args.vectors}", file=sys.stderr)
        return 1

    tests = []
    for fn in files:
        with open(os.path.join(args.vectors, fn)) as fh:
            data = json.load(fh)
        # even stride sampling keeps the operand sweep representative
        if len(data) > args.per_opcode:
            step = max(1, len(data) // args.per_opcode)
            data = data[::step][:args.per_opcode]
        tests.extend(data)

    blob = bytearray()
    blob += b"SM83"
    blob += struct.pack("<III", VERSION, len(tests), 0)
    for t in tests:
        name = t["name"].encode("ascii", "replace")[:255]
        blob += bytes([len(name)]) + name
        blob += pack_state(t["initial"])
        blob += pack_state(t["final"])

    with open(args.out, "wb") as fh:
        fh.write(blob)

    print(f"wrote {args.out}: {len(tests)} vectors, "
          f"{len(blob) / 1024:.1f} KiB from {len(files)} opcode files")
    return 0


if __name__ == "__main__":
    sys.exit(main())
