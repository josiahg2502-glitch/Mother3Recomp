#!/usr/bin/env python3
"""
render_test.py -- run a cartridge headlessly and dump frames as PNGs.

This is the end-to-end check for the PPU: it proves tiles, the BG map,
sprites, OAM DMA, CGB palettes and scrolling actually produce pixels.

    python3 tests/render_test.py assets/pksilver_demo.gbc \\
        --frames 120 --shots 30,60,120 --out /tmp/frames

Also prints an audio sanity report (peak amplitude per channel mix) so the
APU can be checked without a sound card.
"""
import argparse
import ctypes
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

LCD_W, LCD_H = 160, 144


class Core:
    def __init__(self, path):
        self.lib = ctypes.CDLL(path)
        L = self.lib
        L.gb_init.restype = None
        L.gb_rom_buffer.restype = ctypes.POINTER(ctypes.c_uint8)
        L.gb_load_rom.argtypes = [ctypes.c_size_t]
        L.gb_load_rom.restype = ctypes.c_int
        L.gb_apply_patch.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t]
        L.gb_apply_patch.restype = ctypes.c_int
        L.gb_run_frame.restype = ctypes.c_int
        L.gb_framebuffer.restype = ctypes.POINTER(ctypes.c_uint32)
        L.gb_set_input.argtypes = [ctypes.c_uint16]
        L.gb_get_rom_info.argtypes = [ctypes.c_char_p, ctypes.c_int] + \
            [ctypes.POINTER(ctypes.c_int)] * 5
        L.gb_rom_crc32.restype = ctypes.c_uint32
        L.gb_render_audio.argtypes = [ctypes.POINTER(ctypes.c_int16), ctypes.c_int]
        L.gb_render_audio.restype = ctypes.c_int
        L.gb_set_sample_rate.argtypes = [ctypes.c_int]
        L.gb_set_audio_mode.argtypes = [ctypes.c_int]
        L.gb_set_color_correction.argtypes = [ctypes.c_int]
        L.gb_state_size.restype = ctypes.c_size_t
        L.gb_save_state.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t]
        L.gb_save_state.restype = ctypes.c_int
        L.gb_load_state.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t]
        L.gb_load_state.restype = ctypes.c_int
        L.gb_upscale.argtypes = [ctypes.POINTER(ctypes.c_uint32),
                                 ctypes.POINTER(ctypes.c_uint32),
                                 ctypes.c_int, ctypes.c_int,
                                 ctypes.c_int, ctypes.c_int,
                                 ctypes.c_int, ctypes.c_int, ctypes.c_int]

    def load(self, path):
        with open(path, "rb") as fh:
            data = fh.read()
        buf = self.lib.gb_rom_buffer()
        ctypes.memmove(buf, data, len(data))
        rc = self.lib.gb_load_rom(len(data))
        if rc != 0:
            raise RuntimeError(f"gb_load_rom failed: {rc}")
        return data

    def rom_info(self):
        title = ctypes.create_string_buffer(17)
        ints = [ctypes.c_int() for _ in range(5)]
        self.lib.gb_get_rom_info(title, 17, *[ctypes.byref(i) for i in ints])
        return {
            "title": title.value.decode("ascii", "replace"),
            "cart_type": ints[0].value,
            "rom_banks": ints[1].value,
            "ram_banks": ints[2].value,
            "cgb": ints[3].value,
            "sgb": ints[4].value,
        }

    def frame(self, buttons=0):
        self.lib.gb_set_input(buttons)
        self.lib.gb_run_frame()
        fb = self.lib.gb_framebuffer()
        return [fb[i] for i in range(LCD_W * LCD_H)]


def write_png(path, pixels, w, h):
    """Minimal PNG writer (RGB8) so the test has no third-party dependency."""
    import zlib

    raw = bytearray()
    for y in range(h):
        raw.append(0)                       # filter: none
        for x in range(w):
            p = pixels[y * w + x]
            raw.append((p >> 16) & 0xFF)
            raw.append((p >> 8) & 0xFF)
            raw.append(p & 0xFF)

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        c += struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
        return c

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as fh:
        fh.write(png)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("rom")
    ap.add_argument("--lib", default=os.path.join(ROOT, "build", "libpksilver_core.so"))
    ap.add_argument("--frames", type=int, default=120)
    ap.add_argument("--shots", default="30,60,120")
    ap.add_argument("--out", default="/tmp/pksilver_frames")
    ap.add_argument("--patch", default=None, help="apply an IPS/UPS/BPS first")
    ap.add_argument("--buttons", default="", help="e.g. right,a held for all frames")
    args = ap.parse_args()

    if not os.path.exists(args.lib):
        print(f"core not found: {args.lib}  (run tools/build.sh native)", file=sys.stderr)
        return 2

    NAMES = {"right": 0x01, "left": 0x02, "up": 0x04, "down": 0x08,
             "a": 0x10, "b": 0x20, "select": 0x40, "start": 0x80}
    held = 0
    for tok in args.buttons.replace(",", " ").split():
        held |= NAMES.get(tok.strip().lower(), 0)

    core = Core(args.lib)
    core.lib.gb_init()
    core.lib.gb_set_sample_rate(44100)
    core.lib.gb_set_audio_mode(1)
    core.load(args.rom)

    if args.patch:
        with open(args.patch, "rb") as fh:
            pdata = fh.read()
        buf = (ctypes.c_uint8 * len(pdata)).from_buffer_copy(pdata)
        rc = core.lib.gb_apply_patch(buf, len(pdata))
        print(f"patch {os.path.basename(args.patch)}: rc={rc}")
        if rc != 0:
            return 1

    info = core.rom_info()
    print(f"ROM: '{info['title']}'  type=${info['cart_type']:02X}  "
          f"banks={info['rom_banks']}  cgb={info['cgb']}  "
          f"crc32={core.lib.gb_rom_crc32():08X}")

    os.makedirs(args.out, exist_ok=True)
    shots = {int(x) for x in args.shots.split(",") if x.strip()}

    audio = (ctypes.c_int16 * (735 * 2))()
    peak = 0
    nonzero = 0
    total_samples = 0

    for f in range(1, args.frames + 1):
        px = core.frame(held)
        n = core.lib.gb_render_audio(audio, 735)
        for i in range(n * 2):
            v = abs(audio[i])
            if v > peak:
                peak = v
            if v > 64:
                nonzero += 1
            total_samples += 1
        if f in shots:
            out = os.path.join(args.out, f"frame_{f:04d}.png")
            write_png(out, px, LCD_W, LCD_H)
            uniq = len(set(px))
            print(f"  frame {f:4d}: {uniq} distinct colours -> {out}")

    print(f"\naudio: peak={peak}/32767  "
          f"{100.0 * nonzero / max(total_samples, 1):.1f}% samples above -54 dB")

    # save-state round trip
    sz = core.lib.gb_state_size()
    blob = (ctypes.c_uint8 * sz)()
    rc1 = core.lib.gb_save_state(blob, sz)
    for _ in range(5):
        core.frame(held)
    before = core.frame(held)
    rc2 = core.lib.gb_load_state(blob, sz)
    for _ in range(5):
        core.frame(held)
    after = core.frame(held)
    print(f"state: size={sz} bytes  save={rc1}  load={rc2}  "
          f"replay match={'yes' if before == after else 'NO'}")
    return 0 if rc1 == 0 and rc2 == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
