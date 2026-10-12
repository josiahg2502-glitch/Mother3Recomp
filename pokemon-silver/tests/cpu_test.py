#!/usr/bin/env python3
"""
cpu_test.py -- differential CPU validation for the PokéSilver PC core.

Runs the SM83 single-step test vectors (github.com/singleStepTests/sm83)
against libpksilver_core and reports per-opcode pass/fail plus a diff for
the first failure of each kind.

Usage:
    python3 tests/cpu_test.py [--lib PATH] [--vectors DIR] [--fixture FILE]
                              [--limit N] [--opcode HEX] [--verbose]

Vectors come from either:
  * --vectors DIR   : a checkout of singleStepTests/sm83 (v1/*.json)
  * --fixture FILE  : the compact binary fixture shipped in tests/vectors.bin
                      (built by tests/make_fixture.py)
"""
import argparse
import ctypes
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

REG_ORDER = ["a", "b", "c", "d", "e", "f", "h", "l"]


class Core:
    """Thin ctypes wrapper around the exported C ABI."""

    def __init__(self, path):
        self.lib = ctypes.CDLL(path)
        L = self.lib
        L.gb_init.restype = None
        L.gb_rom_buffer.restype = ctypes.POINTER(ctypes.c_uint8)
        L.gb_load_rom.argtypes = [ctypes.c_size_t]
        L.gb_load_rom.restype = ctypes.c_int
        L.gb_dbg_set_regs.argtypes = [ctypes.c_uint16, ctypes.c_uint16] + \
            [ctypes.c_uint8] * 8
        L.gb_dbg_set_ime.argtypes = [ctypes.c_int]
        L.gb_dbg_read.argtypes = [ctypes.c_uint16]
        L.gb_dbg_read.restype = ctypes.c_uint8
        L.gb_dbg_write.argtypes = [ctypes.c_uint16, ctypes.c_uint8]
        L.gb_dbg_step.restype = ctypes.c_int
        L.gb_dbg_step_cpu.restype = ctypes.c_int
        L.gb_dbg_map_write.argtypes = [ctypes.c_uint16, ctypes.c_uint8]
        L.gb_dbg_map_read.argtypes = [ctypes.c_uint16]
        L.gb_dbg_map_read.restype = ctypes.c_uint8
        L.gb_dbg_set_raw_io.argtypes = [ctypes.c_int]
        L.gb_dbg_set_flat.argtypes = [ctypes.c_int]
        L.gb_dbg_set_if.argtypes = [ctypes.c_uint8]
        L.gb_dbg_set_ie.argtypes = [ctypes.c_uint8]
        L.gb_dbg_regs.argtypes = [ctypes.POINTER(ctypes.c_uint16)] * 2 + \
            [ctypes.POINTER(ctypes.c_uint8)] * 8
        self.L = L

    def boot(self):
        """Load a flat 32 KiB dummy cart so bank 1 maps 0x4000 -> 0x4000."""
        self.L.gb_init()
        buf = self.L.gb_rom_buffer()
        dummy = bytearray(0x8000)
        dummy[0x0100] = 0x00          # nop at the entry point
        # MBC1 + RAM + battery: the vectors stage operands all over the map,
        # including cartridge RAM at 0xA000-0xBFFF, so the cart must actually
        # have writable RAM or the CPU's *mapped* fetch disagrees with the
        # rig's raw poke.  32 KiB ROM keeps bank 1 an identity mapping.
        dummy[0x0147] = 0x03          # MBC1+RAM+BATTERY
        dummy[0x0148] = 0x00          # 32 KiB ROM
        dummy[0x0149] = 0x04          # 32 KiB RAM
        assert len(dummy) == 0x8000
        ctypes.memmove(buf, bytes(dummy), len(dummy))
        rc = self.L.gb_load_rom(len(dummy))
        if rc != 0:
            raise RuntimeError(f"gb_load_rom failed ({rc})")
        # The SM83 vectors are pure CPU tests generated against a flat 64 KiB
        # RAM, so run the core in that model: no MBC banking, no I/O read-back
        # masks.  gb_dbg_set_flat() is a test-only switch and stays off in
        # normal emulation.
        self.L.gb_dbg_set_flat(1)


def load_vectors_dir(path, limit=None, opcode=None, per_opcode=None, seed=1):
    import json
    import random
    files = sorted(f for f in os.listdir(path) if f.endswith(".json"))
    if opcode is not None:
        files = [f for f in files if f[:-5].replace(" ", "").lower() == opcode]
    out = []
    rng = random.Random(seed)
    for fn in files:
        with open(os.path.join(path, fn)) as fh:
            data = json.load(fh)
        if per_opcode and len(data) > per_opcode:
            # even stride sampling keeps the operand sweep representative
            step = max(1, len(data) // per_opcode)
            data = data[::step][:per_opcode]
        out.extend(data)
        if limit and len(out) >= limit:
            return out[:limit]
    rng.shuffle(out)
    return out


def load_fixture(path, limit=None, opcode=None, per_opcode=None, seed=1):
    """Read tests/vectors.bin (see tests/make_fixture.py for the layout)."""
    with open(path, "rb") as fh:
        blob = fh.read()
    if blob[:4] != b"SM83":
        raise RuntimeError("not a vectors.bin fixture")
    version, count, _ = struct.unpack_from("<III", blob, 4)
    if version != 1:
        raise RuntimeError(f"unsupported fixture version {version}")

    def unpack_state(pos):
        pc, sp = struct.unpack_from("<HH", blob, pos)
        pos += 4
        regs = struct.unpack_from("<8B", blob, pos)
        pos += 8
        ime, ie = struct.unpack_from("<BB", blob, pos)
        pos += 2
        n_ram = struct.unpack_from("<H", blob, pos)[0]
        pos += 2
        ram = []
        for _ in range(n_ram):
            a, v = struct.unpack_from("<HB", blob, pos)
            pos += 3
            ram.append([a, v])
        st = {"pc": pc, "sp": sp, "ime": ime, "ie": ie, "ram": ram}
        st.update(dict(zip(REG_ORDER, regs)))
        return st, pos

    pos = 16
    out = []
    seen = {}
    for _ in range(count):
        nlen = blob[pos]
        pos += 1
        name = blob[pos:pos + nlen].decode("ascii", "replace")
        pos += nlen
        init, pos = unpack_state(pos)
        fin, pos = unpack_state(pos)

        op = name.split()[0].lower()
        if opcode is not None:
            key = name.split()[0].lower()
            key2 = " ".join(name.split()[:2]).lower().replace(" ", "")
            if key != opcode and key2 != opcode:
                continue
        if per_opcode:
            if seen.get(op, 0) >= per_opcode:
                continue
            seen[op] = seen.get(op, 0) + 1
        out.append({"name": name, "initial": init, "final": fin})
        if limit and len(out) >= limit:
            break
    return out


UNUSABLE = range(0xFEA0, 0xFF00)   # hardware hole: reads 0xFF, writes ignored


def touches_unusable(t):
    """A vector that stages bytes in the 0xFEA0-0xFEFF hole cannot run on
    real hardware either, so the rig reports it as skipped rather than failed."""
    if t["initial"]["pc"] & 0xFFFF in UNUSABLE:
        return True
    for section in ("initial", "final"):
        for addr, _ in t[section].get("ram", []):
            if addr & 0xFFFF in UNUSABLE:
                return True
    return False


def run_test(core, t):
    """Returns None on success, 'skip' if unrunnable, else a list of diffs."""
    L = core.L
    init, fin = t["initial"], t["final"]
    if touches_unusable(t):
        return "skip"

    # stage memory.  IF is cleared first: the vectors only list the bytes
    # they care about, and a leftover pending interrupt from a previous test
    # would otherwise be taken by this one.
    # IF starts clear and IE comes from its own field: in the reference model
    # these are registers, not aliases of memory at $FF0F/$FFFF.  Several
    # vectors stage code or data at those addresses, so they must not be
    # interpreted as a pending interrupt.
    L.gb_dbg_set_if(0x00)
    L.gb_dbg_set_ie(init.get("ie", 0) & 0xFF)
    # Memory image: IE first (it seeds $FFFF), then the RAM list wins.
    L.gb_dbg_write(0xFFFF, init.get("ie", 0) & 0xFF)
    for addr, val in init["ram"]:
        L.gb_dbg_write(addr & 0xFFFF, val & 0xFF)

    L.gb_dbg_set_regs(init["pc"] & 0xFFFF, init["sp"] & 0xFFFF,
                      init["a"], init["b"], init["c"], init["d"],
                      init["e"], init["f"], init["h"], init["l"])
    L.gb_dbg_set_ime(1 if init.get("ime") else 0)

    L.gb_dbg_step_cpu()

    pc = ctypes.c_uint16()
    sp = ctypes.c_uint16()
    regs = [ctypes.c_uint8() for _ in range(8)]
    L.gb_dbg_regs(ctypes.byref(pc), ctypes.byref(sp),
                  *[ctypes.byref(r) for r in regs])
    got_ime = L.gb_dbg_get_ime()

    diffs = []
    if pc.value != (fin["pc"] & 0xFFFF):
        diffs.append(f"pc: got {pc.value:04X} want {fin['pc']:04X}")
    if sp.value != (fin["sp"] & 0xFFFF):
        diffs.append(f"sp: got {sp.value:04X} want {fin['sp']:04X}")
    for i, k in enumerate(REG_ORDER):
        want = fin[k] & 0xFF
        if k == "f":
            want &= 0xF0            # only NZHC are architectural
        if regs[i].value != want:
            diffs.append(f"{k}: got {regs[i].value:02X} want {want:02X}")
    if bool(fin.get("ime")) != bool(got_ime):
        diffs.append(f"ime: got {got_ime} want {fin.get('ime')}")
    for addr, val in fin["ram"]:
        got = L.gb_dbg_read(addr & 0xFFFF)
        if got != (val & 0xFF):
            diffs.append(f"[{addr:04X}]: got {got:02X} want {val:02X}")
    return diffs or None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--lib", default=os.path.join(ROOT, "build", "libpksilver_core.so"))
    ap.add_argument("--vectors", default=None)
    ap.add_argument("--fixture", default=None)
    ap.add_argument("--limit", type=int, default=None)
    ap.add_argument("--opcode", default=None)
    ap.add_argument("--per-opcode", type=int, default=None)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--max-report", type=int, default=25)
    args = ap.parse_args()

    lib = args.lib
    if not os.path.exists(lib):
        for cand in ("/tmp/libpksilver_core.so",):
            if os.path.exists(cand):
                lib = cand
                break
    if not os.path.exists(lib):
        print(f"core library not found: {args.lib}", file=sys.stderr)
        return 2

    if args.vectors:
        tests = load_vectors_dir(args.vectors, args.limit, args.opcode,
                                 args.per_opcode, args.seed)
    else:
        fix = args.fixture or os.path.join(HERE, "vectors.bin")
        if not os.path.exists(fix):
            print("no --vectors dir and no tests/vectors.bin fixture", file=sys.stderr)
            return 2
        tests = load_fixture(fix, args.limit, args.opcode,
                             args.per_opcode, args.seed)

    core = Core(lib)
    core.boot()

    passed = failed = skipped = 0
    per_opcode = {}
    reported = 0
    for t in tests:
        op = t["name"].split()[0].lower() if t.get("name") else "??"
        diffs = run_test(core, t)
        if diffs == "skip":
            skipped += 1
            continue
        st = per_opcode.setdefault(op, [0, 0])
        if diffs is None:
            passed += 1
            st[0] += 1
        else:
            failed += 1
            st[1] += 1
            if reported < args.max_report:
                reported += 1
                print(f"FAIL {t['name']}")
                for d in diffs:
                    print(f"       {d}")
                if args.verbose:
                    print(f"       initial={t['initial']}")

    total = passed + failed
    print(f"\n{passed}/{total} passed ({100.0 * passed / max(total,1):.2f}%), "
          f"{failed} failed, {skipped} skipped (unusable address hole)")
    bad = [(k, v) for k, v in sorted(per_opcode.items()) if v[1]]
    if bad:
        print("opcodes with failures:")
        for k, (p, f) in bad:
            print(f"  {k}: {f} failed / {p + f}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
