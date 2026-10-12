#!/usr/bin/env python3
"""
gbasm.py -- a compact SM83 (Game Boy CPU) assembler.

Two passes, label resolution, and just enough directives to author test and
demo cartridges:

    .org  $0150          set the emit address
    .db   1, 2, "txt"    raw bytes
    .dw   $1234, label   raw little-endian words
    .ds   16, $FF        reserve N bytes (optional fill)
    .incbin "file.bin"   splice in a binary blob
    NAME  equ $FF40      numeric constant
    name:                label at the current address

Instruction syntax follows the usual GB documentation, hex with `$` or `0x`,
and accepts `[hl]` or `(hl)` interchangeably.

    python3 tools/gbasm.py input.asm -o out.gb
"""
import argparse
import re
import sys

R8 = {"b": 0, "c": 1, "d": 2, "e": 3, "h": 4, "l": 5, "(hl)": 6, "[hl]": 6, "a": 7}
R16 = {"bc": 0, "de": 1, "hl": 2, "sp": 3}
R16_AF = {"bc": 0, "de": 1, "hl": 2, "af": 3}
CC = {"nz": 0, "z": 1, "nc": 2, "c": 3}

CB_OPS = {"rlc": 0, "rrc": 1, "rl": 2, "rr": 3, "sla": 4, "sra": 5,
          "swap": 6, "srl": 7}
CB_GRP = {"bit": 0x40, "res": 0x80, "set": 0xC0}
ALU = {"add": 0, "adc": 1, "sub": 2, "sbc": 3, "and": 4, "xor": 5, "or": 6, "cp": 7}


class AsmError(Exception):
    pass


class StrLit(str):
    """Marks a .db operand that was written as a quoted string, so it is
    emitted as raw ASCII instead of being evaluated as an expression."""
    pass


def norm(tok):
    """(hl) and [hl] are the same operand; keep the table keys canonical."""
    t = tok.strip().lower()
    t = re.sub(r"\s+", "", t)
    if t.startswith("[") and t.endswith("]"):
        t = "(" + t[1:-1] + ")"
    return t


class Assembler:
    def __init__(self, text):
        self.lines = text.splitlines()
        self.syms = {}
        self.origin = 0
        self.pass_no = 1
        self.errors = []

    # -- expression evaluation -------------------------------------------
    def evaluate(self, expr):
        """Evaluate a constant expression.

        Numeric literals are stashed before label substitution, otherwise the
        symbol regex would happily match the `x0100` inside `0x0100`.
        """
        e = expr.strip().replace("$", "0x")
        # binary literals: %1010_0101
        e = re.sub(r"%([01][01_]*)",
                   lambda m: str(int(m.group(1).replace("_", ""), 2)), e)

        lits = []

        def stash(m):
            lits.append(m.group(0).replace("_", ""))
            return "\x00%d\x00" % (len(lits) - 1)

        e = re.sub(r"0[xX][0-9a-fA-F][0-9a-fA-F_]*|\b\d[\d_]*\b", stash, e)

        def sub_sym(m):
            name = m.group(0)
            if name in self.syms:
                return str(self.syms[name])
            if self.pass_no == 1:
                return "0"          # unresolved now, fixed on the second pass
            raise AsmError(f"undefined symbol '{name}'")

        e = re.sub(r"[A-Za-z_.][A-Za-z0-9_.]*", sub_sym, e)
        e = re.sub(r"\x00(\d+)\x00", lambda m: lits[int(m.group(1))], e)

        e = e.replace("&&", " and ").replace("||", " or ")
        e = re.sub(r"(?<![<>=!])=(?!=)", "==", e)
        try:
            return int(eval(e, {"__builtins__": {}}, {})) & 0xFFFFFFFF
        except AsmError:
            raise
        except Exception as ex:
            raise AsmError(f"bad expression '{expr}' ({ex})")

    def evaluate8(self, expr):
        v = self.evaluate(expr)
        return v & 0xFF

    def evaluate_signed8(self, expr):
        v = self.evaluate(expr)
        if v > 0x7FFFFFFF:
            v -= 0x100000000
        if not (-128 <= v <= 127):
            raise AsmError(f"value {v} does not fit in a signed byte")
        return v & 0xFF

    # -- operand parsing --------------------------------------------------
    def split_operands(self, rest):
        """Split on commas that are not inside brackets."""
        out, depth, cur = [], 0, ""
        for ch in rest:
            if ch in "([":
                depth += 1
            elif ch in ")]":
                depth -= 1
            if ch == "," and depth == 0:
                out.append(cur.strip())
                cur = ""
            else:
                cur += ch
        if cur.strip():
            out.append(cur.strip())
        return out

    def is_mem(self, tok):
        return tok.startswith("(") and tok.endswith(")")

    def mem_inner(self, tok):
        return tok[1:-1].strip()

    # -- emission ---------------------------------------------------------
    def emit_bytes(self, buf, values):
        buf.extend(values)

    def assemble_instruction(self, mn, operands, buf):
        """Emit one instruction.  Returns True if handled."""
        o = operands
        n = len(o)

        def need(k):
            if n != k:
                raise AsmError(f"'{mn}' expects {k} operand(s), got {n}")

        # ---- no-operand forms -------------------------------------------
        simple = {
            "nop": [0x00], "halt": [0x76], "stop": [0x10, 0x00],
            "di": [0xF3], "ei": [0xFB], "cpl": [0x2F], "daa": [0x27],
            "scf": [0x37], "ccf": [0x3F], "ret": [0xC9], "reti": [0xD9],
            "rlca": [0x07], "rrca": [0x0F], "rla": [0x17], "rra": [0x1F],
        }
        if n == 0 and mn in simple:
            self.emit_bytes(buf, simple[mn])
            return True

        if n == 1:
            # jp (hl)
            if mn == "jp" and norm(o[0]) == "(hl)":
                self.emit_bytes(buf, [0xE9])
                return True
            # inc/dec r16
            if mn in ("inc", "dec"):
                t = norm(o[0])
                if t in R16:
                    base = 0x03 if mn == "inc" else 0x0B
                    self.emit_bytes(buf, [base | (R16[t] << 4)])
                    return True
                if t in R8:
                    base = 0x04 if mn == "inc" else 0x05
                    idx = R8[t]
                    self.emit_bytes(buf, [base | (idx << 3)])
                    return True
                raise AsmError(f"bad operand for '{mn}': {o[0]}")
            # push/pop
            if mn in ("push", "pop"):
                t = norm(o[0])
                table = R16_AF
                if t in table:
                    base = (0xC5 if mn == "push" else 0xC1)
                    self.emit_bytes(buf, [base | (table[t] << 4)])
                    return True
                raise AsmError(f"bad operand for '{mn}': {o[0]}")
            # rst
            if mn == "rst":
                v = self.evaluate(o[0])
                if v & 7 or v > 0x38:
                    raise AsmError(f"rst target must be a multiple of 8 (got {v:#x})")
                self.emit_bytes(buf, [0xC7 | v])
                return True
            # alu a, operand  (the implicit-a form)
            if mn in ALU:
                return self.emit_alu(mn, o[0], buf)
            # jp/jr/call unconditional
            if mn in ("jp", "jr", "call"):
                return self.emit_jump(mn, None, o[0], buf)
            # ret cc
            if mn == "ret":
                t = o[0].lower()
                if t in CC:
                    self.emit_bytes(buf, [0xC0 | (CC[t] << 3)])
                    return True
                raise AsmError(f"bad ret condition '{o[0]}'")
            # ld sp, hl handled below (2 operands)
            if mn == "ld" and norm(o[0]) == "sp":
                raise AsmError("ld sp needs a second operand")
            # swap/rotates via CB
            t = norm(o[0])
            if t in R8:
                for name, code in CB_OPS.items():
                    if mn == name:
                        self.emit_bytes(buf, [0xCB, (code << 3) | R8[t]])
                        return True
                for name, base in CB_GRP.items():
                    if mn == name:
                        raise AsmError(f"'{name}' needs a bit index")
            if mn in CB_OPS or mn in CB_GRP:
                raise AsmError(f"bad operand for '{mn}': {o[0]}")

        if n == 2:
            a0, a1 = norm(o[0]), norm(o[1])
            # bit/res/set n, r
            if o[0].lower() in CB_GRP:
                grp = CB_GRP[o[0].lower()]
                bit = self.evaluate(o[0 + "" ] ) if False else self.evaluate(operands[0])
                raise AsmError("unreachable")
            # ld / ldh forms
            if mn in ("ld", "ldh"):
                return self.emit_ld(o, buf, ldh=(mn == "ldh"))
            if mn in ALU:
                # add hl, r16 / add sp, n
                if a0 == "hl" and a1 in R16:
                    self.emit_bytes(buf, [0x09 | (R16[a1] << 4)])
                    return True
                if a0 == "sp" and a1 not in R16:
                    self.emit_bytes(buf, [0xE8, self.evaluate_signed8(o[1])])
                    return True
                if a0 == "a":
                    return self.emit_alu(mn, o[1], buf)
                raise AsmError(f"bad operands for '{mn}': {o}")
            if mn in ("jp", "jr", "call"):
                t = o[0].lower()
                if t in CC:
                    return self.emit_jump(mn, t, o[1], buf)
                raise AsmError(f"bad condition '{o[0]}'")
            if mn in CB_GRP:
                bit = self.evaluate(o[0])
                if not (0 <= bit <= 7):
                    raise AsmError("bit index must be 0..7")
                t = a1
                if t not in R8:
                    raise AsmError(f"bad operand for '{mn}': {o[1]}")
                self.emit_bytes(buf, [0xCB, CB_GRP[mn] | (bit << 3) | R8[t]])
                return True
            if mn in CB_OPS:
                if a1 in R8:
                    self.emit_bytes(buf, [0xCB, (CB_OPS[mn] << 3) | R8[a1]])
                    return True
                raise AsmError(f"bad operand for '{mn}': {o[1]}")

        if n == 3 and mn in CB_GRP:
            raise AsmError("too many operands")

        raise AsmError(f"unhandled instruction '{mn} {', '.join(o)}'")

    def emit_alu(self, mn, operand, buf):
        idx = ALU[mn]
        t = norm(operand)
        if t in R8:
            self.emit_bytes(buf, [0x80 | (idx << 3) | R8[t]])
            return True
        self.emit_bytes(buf, [0xC6 | (idx << 3), self.evaluate8(operand)])
        return True

    def emit_jump(self, mn, cc, operand, buf):
        val = self.evaluate(operand)
        if mn == "jr":
            if val > 0x7FFFFFFF:
                rel = val - 0x100000000
            else:
                rel = val
            # relative to the address *after* the instruction
            here = self.pc + 2
            d = rel - here
            if not (-128 <= d <= 127):
                if self.pass_no == 2:
                    raise AsmError(f"jr target out of range ({d})")
                d = 0
            base = 0x18 if cc is None else (0x20 | (CC[cc] << 3))
            self.emit_bytes(buf, [base, d & 0xFF])
            return True
        base = {"jp": 0xC2, "call": 0xC4}[mn]
        if cc is None:
            base = {"jp": 0xC3, "call": 0xCD}[mn]
            self.emit_bytes(buf, [base, val & 0xFF, (val >> 8) & 0xFF])
        else:
            self.emit_bytes(buf, [base | (CC[cc] << 3), val & 0xFF, (val >> 8) & 0xFF])
        return True

    def emit_ld(self, o, buf, ldh=False):
        """Emit any LD / LDH form.

        Operand *matching* uses the normalised (lower-cased, bracket-canonical)
        text, but every value goes through self.evaluate() with the ORIGINAL
        text so that mixed-case labels and constants still resolve.
        """
        if len(o) != 2:
            raise AsmError("ld needs exactly 2 operands")
        a0, a1 = norm(o[0]), norm(o[1])

        def inner(tok):
            """Inner text of a memory operand, original case preserved."""
            t = tok.strip()
            if t.startswith("(") and t.endswith(")"):
                return t[1:-1].strip()
            if t.startswith("[") and t.endswith("]"):
                return t[1:-1].strip()
            return t

        dst_mem = self.is_mem(a0)
        src_mem = self.is_mem(a1)
        i0 = inner(o[0]) if dst_mem else None
        i1 = inner(o[1]) if src_mem else None
        t0 = i0.lower() if dst_mem else None
        t1 = i1.lower() if src_mem else None

        MEM_REGS = ("bc", "de", "hl", "hli", "hld", "hl+", "hl-", "c")

        # ---- ld (nn), sp -------------------------------------------------
        if a1 == "sp" and dst_mem and t0 not in MEM_REGS:
            v = self.evaluate(i0)
            self.emit_bytes(buf, [0x08, v & 0xFF, (v >> 8) & 0xFF])
            return True

        # ---- ld hl, sp+n / ld sp, n / ld sp, hl --------------------------
        if a0 in ("hl", "sp"):
            if a1 == "sp":
                self.emit_bytes(buf, [0xF9])
                return True
            if a0 == "hl":
                a1n = a1.replace(" ", "")
                if a1n.startswith("sp+") or a1n.startswith("sp-"):
                    self.emit_bytes(buf, [0xF8, self.evaluate_signed8(a1n[2:])])
                    return True
            if a0 in R16:
                v = self.evaluate(o[1])
                self.emit_bytes(buf, [0x01 | (R16[a0] << 4), v & 0xFF, (v >> 8) & 0xFF])
                return True

        # ---- ldh / absolute through A ------------------------------------
        if dst_mem and a1 == "a" and t0 not in MEM_REGS:
            if ldh or t0.startswith("$ff") or t0.startswith("0xff"):
                v = self.evaluate(i0) & 0xFF
                self.emit_bytes(buf, [0xE0, v])
                return True
            v = self.evaluate(i0)
            self.emit_bytes(buf, [0xEA, v & 0xFF, (v >> 8) & 0xFF])
            return True
        if a0 == "a" and src_mem and t1 not in MEM_REGS:
            if ldh or t1.startswith("$ff") or t1.startswith("0xff"):
                v = self.evaluate(i1) & 0xFF
                self.emit_bytes(buf, [0xF0, v])
                return True
            v = self.evaluate(i1)
            self.emit_bytes(buf, [0xFA, v & 0xFF, (v >> 8) & 0xFF])
            return True

        # ---- store through A --------------------------------------------
        if dst_mem and a1 == "a":
            if t0 in ("bc", "de"):
                self.emit_bytes(buf, [0x02 if t0 == "bc" else 0x12])
                return True
            if t0 == "hl":
                self.emit_bytes(buf, [0x77])
                return True
            if t0 in ("hli", "hl+"):
                self.emit_bytes(buf, [0x22])
                return True
            if t0 in ("hld", "hl-"):
                self.emit_bytes(buf, [0x32])
                return True
            if t0 == "c":
                self.emit_bytes(buf, [0xE2])
                return True
        if a0 == "a" and src_mem:
            if t1 in ("bc", "de"):
                self.emit_bytes(buf, [0x0A if t1 == "bc" else 0x1A])
                return True
            if t1 == "hl":
                self.emit_bytes(buf, [0x7E])
                return True
            if t1 in ("hli", "hl+"):
                self.emit_bytes(buf, [0x2A])
                return True
            if t1 in ("hld", "hl-"):
                self.emit_bytes(buf, [0x3A])
                return True
            if t1 == "c":
                self.emit_bytes(buf, [0xF2])
                return True

        # ---- ld (hl), r / ld r, (hl) / ld (hl), n ------------------------
        if dst_mem and t0 == "hl":
            if a1 in R8 and a1 != "a" and R8[a1] != 6:
                self.emit_bytes(buf, [0x70 | R8[a1]])
                return True
            self.emit_bytes(buf, [0x36, self.evaluate8(o[1])])
            return True
        if src_mem and t1 == "hl":
            if a0 in R8:
                self.emit_bytes(buf, [0x40 | (R8[a0] << 3) | 6])
                return True
            raise AsmError(f"bad ld destination '{o[0]}'")

        # ---- register to register / register, immediate ------------------
        if a0 in R8:
            if a1 in R8:
                self.emit_bytes(buf, [0x40 | (R8[a0] << 3) | R8[a1]])
                return True
            self.emit_bytes(buf, [0x06 | (R8[a0] << 3), self.evaluate8(o[1])])
            return True

        # ---- ld r16, nn (fallthrough) ------------------------------------
        if a0 in R16:
            v = self.evaluate(o[1])
            self.emit_bytes(buf, [0x01 | (R16[a0] << 4), v & 0xFF, (v >> 8) & 0xFF])
            return True

        raise AsmError(f"unhandled ld form: {o}")

    # -- pass driver ------------------------------------------------------
    def run(self):
        self.syms = {}
        # pass 1: collect labels and constants, measure sizes
        for _ in range(2):
            self.pass_no = _ + 1
            self.pc = self.origin
            self.out = bytearray()
            for lineno, raw in enumerate(self.lines, 1):
                try:
                    self.assemble_line(raw, lineno)
                except AsmError as e:
                    self.errors.append(f"line {lineno}: {e}\n    {raw.strip()}")
            if self.errors:
                break
        return bytes(self.out), self.errors

    def assemble_line(self, raw, lineno):
        line = raw.split(";", 1)[0].strip()
        if not line:
            return

        # label definition
        m = re.match(r"^([A-Za-z_.][A-Za-z0-9_.]*)\s*:\s*(.*)$", line)
        if m:
            self.syms[m.group(1)] = self.pc
            line = m.group(2).strip()
            if not line:
                return

        # equ
        m = re.match(r"^([A-Za-z_.][A-Za-z0-9_.]*)\s+equ\s+(.+)$", line, re.I)
        if m:
            self.syms[m.group(1)] = self.evaluate(m.group(2))
            return

        parts = re.split(r"\s+", line, maxsplit=1)
        mn = parts[0].lower().lstrip(".")
        rest = parts[1] if len(parts) > 1 else ""

        # directives
        if mn == "org":
            target = self.evaluate(rest)
            if target > self.pc:
                self.out.extend(b"\x00" * (target - self.pc))
                self.pc = target
            elif target < self.pc:
                raise AsmError(f".org {target:#x} goes backwards from {self.pc:#x}")
            return
        if mn == "ds":
            bits = self.split_operands(rest)
            n = self.evaluate(bits[0])
            fill = self.evaluate8(bits[1]) if len(bits) > 1 else 0
            self.out.extend(bytes([fill]) * n)
            self.pc += n
            return
        if mn == "db":
            for item in self.split_strings(rest):
                if isinstance(item, StrLit):
                    b = item.encode("ascii")
                    self.out.extend(b)
                    self.pc += len(b)
                else:
                    v = self.evaluate(item) & 0xFF
                    self.out.append(v)
                    self.pc += 1
            return
        if mn == "dw":
            for item in self.split_operands(rest):
                v = self.evaluate(item) & 0xFFFF
                self.out.extend([v & 0xFF, (v >> 8) & 0xFF])
                self.pc += 2
            return
        if mn == "incbin":
            path = rest.strip().strip('"')
            with open(path, "rb") as fh:
                data = fh.read()
            self.out.extend(data)
            self.pc += len(data)
            return

        operands = self.split_operands(rest) if rest else []
        before = len(self.out)
        if not self.assemble_instruction(mn, operands, self.out):
            raise AsmError(f"unknown instruction '{mn}'")
        self.pc += len(self.out) - before

    def split_strings(self, rest):
        """Split a .db operand list, keeping quoted strings intact."""
        out, cur, inq, depth = [], "", False, 0
        for ch in rest:
            if ch == '"':
                inq = not inq
            if not inq:
                if ch in "([":
                    depth += 1
                elif ch in ")]":
                    depth -= 1
                elif ch == "," and depth == 0:
                    out.append(cur.strip())
                    cur = ""
                    continue
            cur += ch
        if cur.strip():
            out.append(cur.strip())
        res = []
        for item in out:
            if item.startswith('"') and item.endswith('"') and len(item) >= 2:
                res.append(StrLit(item[1:-1]))
            else:
                res.append(item)
        return res


def main():
    ap = argparse.ArgumentParser(description="Assemble an SM83 source file.")
    ap.add_argument("input")
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("--pad", type=lambda x: int(x, 0), default=None,
                    help="pad the image to this size (bytes)")
    ap.add_argument("--fill", type=lambda x: int(x, 0), default=0x00)
    ap.add_argument("--map", action="store_true", help="print the symbol table")
    args = ap.parse_args()

    with open(args.input) as fh:
        text = fh.read()

    asm = Assembler(text)
    data, errors = asm.run()
    if errors:
        for e in errors[:40]:
            print(e, file=sys.stderr)
        print(f"{len(errors)} error(s)", file=sys.stderr)
        return 1

    if args.pad and len(data) < args.pad:
        data += bytes([args.fill]) * (args.pad - len(data))

    with open(args.output, "wb") as fh:
        fh.write(data)

    if args.map:
        for k in sorted(asm.syms, key=lambda s: asm.syms[s]):
            print(f"  {asm.syms[k]:04X}  {k}")
    print(f"wrote {args.output}: {len(data)} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
