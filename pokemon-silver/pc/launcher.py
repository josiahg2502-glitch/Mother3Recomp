#!/usr/bin/env python3
"""
launcher.py -- PokéSilver PC : the native desktop front-end.

Runs the same freestanding C core (build/libpksilver_core.so) that the web
port uses, presented through SDL/pygame with:

  * a launcher (pick a cartridge, or run the bundled core self-test demo)
  * the custom 1080p-class bezel, or clean 3:2 pillarbox      [F10]
  * four controller presets                                    [F9]
  * a settings & assist overlay                                [F11]
  * quick save / load state slot 1                             [F1 / F2]
  * clean exit                                                 [Esc]
  * CPU upscalers (nearest / sharp / smooth / OmniScale / 3x)
  * 16-bit enhanced audio through SDL
  * IPS / UPS / BPS mod loading and GameShark cheats

Usage:
    python3 pc/launcher.py [cartridge.gbc] [--mods a.ips b.ups]
    python3 pc/launcher.py --demo
    python3 pc/launcher.py --selftest      # headless: 240 frames, no window
"""
import argparse
import ctypes
import os
import sys
import time
import zipfile

import pygame  # noqa: E402  (SDL 2.x)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tests"))

LCD_W, LCD_H = 160, 144

GB_RIGHT, GB_LEFT, GB_UP, GB_DOWN = 0x01, 0x02, 0x04, 0x08
GB_A, GB_B, GB_SELECT, GB_START = 0x10, 0x20, 0x40, 0x80
GB_L, GB_R = 0x100, 0x200

PRESETS = [
    ("Xbox Standard", {
        "a": GB_A, "b": GB_B, "x": GB_B, "y": GB_A,
        "leftshoulder": GB_L, "rightshoulder": GB_R,
        "lefttrigger": GB_L, "righttrigger": GB_R,
        "back": GB_SELECT, "start": GB_START,
        "dpup": GB_UP, "dpdown": GB_DOWN, "dpleft": GB_LEFT, "dpright": GB_RIGHT,
        "stick": True,
    }),
    ("Nintendo Switch", {
        "a": GB_B, "b": GB_A, "x": GB_A, "y": GB_B,
        "leftshoulder": GB_L, "rightshoulder": GB_R,
        "lefttrigger": GB_L, "righttrigger": GB_R,
        "back": GB_SELECT, "start": GB_START,
        "dpup": GB_UP, "dpdown": GB_DOWN, "dpleft": GB_LEFT, "dpright": GB_RIGHT,
        "stick": True,
    }),
    ("Modern Trigger Action", {
        "a": GB_A, "b": GB_B, "x": GB_B, "y": GB_A,
        "leftshoulder": GB_L, "rightshoulder": GB_R,
        "lefttrigger": GB_B, "righttrigger": GB_A,
        "back": GB_SELECT, "start": GB_START,
        "dpup": GB_UP, "dpdown": GB_DOWN, "dpleft": GB_LEFT, "dpright": GB_RIGHT,
        "stick": True,
    }),
    ("Classic 1:1", {
        "a": GB_A, "b": GB_B,
        "leftshoulder": GB_L, "rightshoulder": GB_R,
        "back": GB_SELECT, "start": GB_START,
        "dpup": GB_UP, "dpdown": GB_DOWN, "dpleft": GB_LEFT, "dpright": GB_RIGHT,
        "stick": False,
    }),
]

FILTERS = ["Nearest", "Sharp bilinear", "Smooth bilinear", "OmniScale", "Scale3x"]


# --------------------------------------------------------------------------- core
class Core:
    def __init__(self, path):
        if not os.path.exists(path):
            raise SystemExit(f"core not found: {path}\n  run:  tools/build.sh native")
        self.lib = ctypes.CDLL(path)
        L = self.lib
        L.gb_init.restype = None
        L.gb_rom_buffer.restype = ctypes.POINTER(ctypes.c_uint8)
        L.gb_load_rom.argtypes = [ctypes.c_size_t]; L.gb_load_rom.restype = ctypes.c_int
        L.gb_apply_patch.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t]
        L.gb_apply_patch.restype = ctypes.c_int
        L.gb_run_frame.restype = ctypes.c_int
        L.gb_framebuffer.restype = ctypes.POINTER(ctypes.c_uint32)
        L.gb_set_input.argtypes = [ctypes.c_uint16]
        L.gb_set_sample_rate.argtypes = [ctypes.c_int]
        L.gb_set_audio_mode.argtypes = [ctypes.c_int]
        L.gb_set_volume.argtypes = [ctypes.c_float]
        L.gb_set_speed_multiplier.argtypes = [ctypes.c_int]
        L.gb_set_color_correction.argtypes = [ctypes.c_int]
        L.gb_render_audio.argtypes = [ctypes.POINTER(ctypes.c_int16), ctypes.c_int]
        L.gb_render_audio.restype = ctypes.c_int
        L.gb_rom_crc32.restype = ctypes.c_uint32
        L.gb_rom_data.restype = ctypes.POINTER(ctypes.c_uint8)
        L.gb_state_size.restype = ctypes.c_size_t
        L.gb_save_state.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t]
        L.gb_save_state.restype = ctypes.c_int
        L.gb_load_state.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t]
        L.gb_load_state.restype = ctypes.c_int
        L.gb_set_rtc_now.argtypes = [ctypes.c_uint64]
        L.gb_add_cheat.argtypes = [ctypes.c_char_p]; L.gb_add_cheat.restype = ctypes.c_int
        L.gb_upscale.argtypes = [ctypes.POINTER(ctypes.c_uint32), ctypes.POINTER(ctypes.c_uint32),
                                 ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                 ctypes.c_int, ctypes.c_int, ctypes.c_int]
        L.gb_init()

    def load(self, data):
        buf = self.lib.gb_rom_buffer()
        ctypes.memmove(buf, data, len(data))
        return self.lib.gb_load_rom(len(data))


# --------------------------------------------------------------------------- app
class App:
    def __init__(self, args):
        self.args = args
        self.core = Core(os.path.join(ROOT, "build", "libpksilver_core.so"))
        self.preset = 0
        self.bezel = True
        self.filter = 1
        self.grid = True
        self.scan = False
        self.speed = 100
        self.audio_mode = 1
        self.volume = 1.0
        self.color = 1
        self.overlay = False
        self.mods = list(args.mods or [])
        self.rom_bytes = None
        self.rom_crc = 0
        self.toasts = []          # (text, until, position)
        self.running = False
        self.frames = 0

    # ---- helpers ---------------------------------------------------------
    def toast(self, text, position="top", ms=1800):
        self.toasts.append([text, time.time() + ms / 1000.0, position])

    def load_rom(self, path):
        with open(path, "rb") as fh:
            data = fh.read()
        if path.lower().endswith(".zip"):
            data = self._unzip(data)
            if data is None:
                print("no cartridge inside that zip", file=sys.stderr)
                return False
        if self.core.load(data) != 0:
            print("not a Game Boy cartridge image", file=sys.stderr)
            return False
        self.rom_bytes = data
        self.rom_crc = self.core.lib.gb_rom_crc32()
        for m in self.mods:
            with open(m, "rb") as fh:
                pb = fh.read()
            buf = (ctypes.c_uint8 * len(pb)).from_buffer_copy(pb)
            rc = self.core.lib.gb_apply_patch(buf, len(pb))
            print(f"mod {os.path.basename(m)}: {'ok' if rc == 0 else f'FAILED ({rc})'}")
        return True

    @staticmethod
    def _unzip(data):
        import io
        with zipfile.ZipFile(io.BytesIO(data)) as z:
            names = [n for n in z.namelist()
                     if n.lower().endswith((".gbc", ".gb", ".bin", ".rom"))]
            if not names:
                return None
            names.sort(key=lambda n: z.getinfo(n).file_size, reverse=True)
            return z.read(names[0])

    # ---- state -----------------------------------------------------------
    def state_path(self, slot=1):
        d = os.path.join(ROOT, "saves")
        os.makedirs(d, exist_ok=True)
        return os.path.join(d, f"{self.rom_crc:08x}.slot{slot}.state")

    def quick_save(self):
        n = self.core.lib.gb_state_size()
        buf = (ctypes.c_uint8 * n)()
        if self.core.lib.gb_save_state(buf, n) != 0:
            self.toast("Save failed", "top"); return
        with open(self.state_path(1), "wb") as fh:
            fh.write(bytes(buf))
        self.toast(f"State saved — slot 1 ({n // 1024} KiB)", "top")

    def quick_load(self):
        p = self.state_path(1)
        if not os.path.exists(p):
            self.toast("No state in slot 1", "top"); return
        with open(p, "rb") as fh:
            blob = fh.read()
        buf = (ctypes.c_uint8 * len(blob)).from_buffer_copy(blob)
        rc = self.core.lib.gb_load_state(buf, len(blob))
        self.toast("State loaded — slot 1" if rc == 0 else "Load failed", "top")


# --------------------------------------------------------------------------- run
def run(args):
    app = App(args)

    pygame.init()
    pygame.display.set_caption("PokéSilver PC")

    clock = pygame.time.Clock()
    screen = None
    surf = None
    audio_ready = False
    audio_buf = None

    def open_window():
        nonlocal screen, surf
        w, h = (1180, 760) if app.bezel else (960, 720)
        flags = pygame.RESIZABLE
        if args.selftest:
            flags = 0
        screen = pygame.display.set_mode((w, h), flags)
        surf = pygame.Surface((LCD_W, LCD_H))

    def present(fb_ptr):
        # upscale into a scratch buffer then blit
        scale = max(1, min(screen.get_width() // LCD_W, screen.get_height() // LCD_H))
        dw, dh = LCD_W * scale, LCD_H * scale
        out = (ctypes.c_uint32 * (dw * dh))()
        app.core.lib.gb_upscale(fb_ptr, out, LCD_W, LCD_H, dw, dh,
                                app.filter, 1 if app.grid else 0, 1 if app.scan else 0)
        big = pygame.image.frombuffer(bytes(out), (dw, dh), "RGBA")
        screen.fill((0, 0, 0))
        if app.bezel:
            draw_bezel(screen, big)
        else:
            screen.blit(big, big.get_rect(center=screen.get_rect().center))
        draw_toasts(screen, app)
        if app.overlay:
            draw_overlay(screen, app)
        pygame.display.flip()

    # ---------------------------------------------------------------- loop
    rom = args.rom
    if not rom and args.demo:
        rom = os.path.join(ROOT, "assets", "pksilver_demo.gbc")
    if not rom:
        rom = pick_rom_interactive(pygame)
    if not rom or not app.load_rom(rom):
        return 1, app

    open_window()
    app.core.lib.gb_set_sample_rate(44100)
    app.core.lib.gb_set_audio_mode(app.audio_mode)
    app.core.lib.gb_set_volume(app.volume)
    app.core.lib.gb_set_color_correction(app.color)
    app.core.lib.gb_set_speed_multiplier(app.speed)

    if not args.selftest:
        try:
            pygame.mixer.init(frequency=44100, size=-16, channels=2, buffer=1024)
            audio_ready = True
            audio_buf = (ctypes.c_int16 * (1024 * 2))()
        except Exception as e:
            print("audio unavailable:", e)

    app.running = True
    app.toast(f"Now playing — {os.path.basename(rom)}", "top")
    target_frames = args.frames if args.selftest else None
    t0 = time.time()

    joysticks = [pygame.joystick.Joystick(i)
                 for i in range(pygame.joystick.get_count())]
    for j in joysticks:
        j.init()

    while app.running:
        for ev in pygame.event.get():
            if ev.type == pygame.QUIT:
                app.running = False
            elif ev.type == pygame.VIDEORESIZE:
                open_window()
            elif ev.type == pygame.KEYDOWN:
                k = ev.key
                if k == pygame.K_F10:
                    app.bezel = not app.bezel
                    open_window()
                    app.toast("Custom bezel — 1080p shell" if app.bezel
                              else "Clean 3:2 pillarbox fullscreen", "top")
                elif k == pygame.K_F9:
                    app.preset = (app.preset + 1) % len(PRESETS)
                    app.toast(f"Controller preset — {PRESETS[app.preset][0]}", "bottom")
                elif k == pygame.K_F11:
                    app.overlay = not app.overlay
                elif k == pygame.K_ESCAPE:
                    if app.overlay:
                        app.overlay = False
                    else:
                        app.running = False
                elif k == pygame.K_F1:
                    app.quick_save()
                elif k == pygame.K_F2:
                    app.quick_load()
                elif k in (pygame.K_1, pygame.K_2, pygame.K_3, pygame.K_4, pygame.K_5):
                    app.filter = k - pygame.K_1
                    app.core.lib.gb_upscale  # no-op; scaler chosen at present()
                    app.toast(f"Upscaler — {FILTERS[app.filter]}", "bottom")
                elif k == pygame.K_g:
                    app.grid = not app.grid
                    app.toast(f"Pixel grid — {'on' if app.grid else 'off'}", "bottom")
                elif k == pygame.K_h:
                    app.scan = not app.scan
                    app.toast(f"Scanlines — {'on' if app.scan else 'off'}", "bottom")
                elif k == pygame.K_m:
                    app.audio_mode = (app.audio_mode + 1) % 3
                    app.core.lib.gb_set_audio_mode(app.audio_mode)
                    app.toast("Audio mix — " +
                              ["Authentic", "Enhanced", "Crystal"][app.audio_mode], "bottom")
                elif k in (pygame.K_LEFTBRACKET, pygame.K_RIGHTBRACKET):
                    app.volume = max(0.0, min(1.5,
                                 app.volume + (-0.1 if k == pygame.K_LEFTBRACKET else 0.1)))
                    app.core.lib.gb_set_volume(app.volume)
                    app.toast(f"Volume — {app.volume:.0%}", "bottom")
                elif k in (pygame.K_MINUS, pygame.K_EQUALS):
                    app.speed = max(25, min(400,
                                app.speed + (-25 if k == pygame.K_MINUS else 25)))
                    app.core.lib.gb_set_speed_multiplier(app.speed)
                    app.toast(f"Speed — {app.speed}%", "bottom")
                elif k == pygame.K_c:
                    app.color = (app.color + 1) % 3
                    app.core.lib.gb_set_color_correction(app.color)
                    app.toast("Colour — " + ["raw hardware", "CGB palette", "vivid"][app.color],
                              "bottom")
            elif ev.type == pygame.JOYBUTTONDOWN or ev.type == pygame.JOYAXISMOTION:
                pass

        mask = read_keyboard(pygame) | read_gamepad(joysticks, app.preset)
        app.core.lib.gb_set_input(mask)
        app.core.lib.gb_set_rtc_now(int(time.time()))
        app.core.lib.gb_run_frame()
        app.frames += 1

        if audio_ready:
            n = app.core.lib.gb_render_audio(audio_buf, 1024)
            if n > 0:
                try:
                    snd = pygame.sndarray.make_sound(
                        __import__("numpy").frombuffer(bytes(audio_buf), dtype="<i2")
                        [:n * 2].reshape(-1, 2))
                    snd.play()
                except Exception:
                    audio_ready = False

        present(app.core.lib.gb_framebuffer())
        clock.tick(0 if app.speed > 100 else 60)

        if target_frames and app.frames >= target_frames:
            app.running = False

    dt = time.time() - t0
    print(f"ran {app.frames} frames in {dt:.2f}s "
          f"({app.frames / max(dt, 1e-9):.1f} fps)")
    pygame.quit()
    return 0, app


# --------------------------------------------------------------------------- input
KEYMAP = {
    pygame.K_RIGHT: GB_RIGHT, pygame.K_d: GB_RIGHT,
    pygame.K_LEFT: GB_LEFT,   pygame.K_a: GB_LEFT,
    pygame.K_UP: GB_UP,       pygame.K_w: GB_UP,
    pygame.K_DOWN: GB_DOWN,   pygame.K_s: GB_DOWN,
    pygame.K_z: GB_A, pygame.K_x: GB_B,
    pygame.K_RETURN: GB_START, pygame.K_SPACE: GB_START,
    pygame.K_RSHIFT: GB_SELECT, pygame.K_BACKSPACE: GB_SELECT,
    pygame.K_q: GB_L, pygame.K_e: GB_R,
}


def read_keyboard(pygame):
    held = pygame.key.get_pressed()
    m = 0
    for k, b in KEYMAP.items():
        if held[k]:
            m |= b
    return m


PAD_NAMES = ["a", "b", "x", "y", "leftshoulder", "rightshoulder",
             "lefttrigger", "righttrigger", "back", "start",
             "leftstick", "rightstick", "dpup", "dpdown", "dpleft", "dpright"]


def read_gamepad(joysticks, preset_idx):
    name, table = PRESETS[preset_idx]
    m = 0
    for j in joysticks:
        for i in range(min(j.get_numbuttons(), 16)):
            if j.get_button(i):
                nm = PAD_NAMES[i] if i < len(PAD_NAMES) else None
                if nm in table:
                    m |= table[nm]
        # d-pad via hats
        for h in range(j.get_numhats()):
            hx, hy = j.get_hat(h)
            if hx > 0: m |= GB_RIGHT
            if hx < 0: m |= GB_LEFT
            if hy > 0: m |= GB_UP
            if hy < 0: m |= GB_DOWN
        # analog stick -> full 8-way grid
        if table.get("stick") and j.get_numaxes() >= 2:
            ax, ay = j.get_axis(0), j.get_axis(1)
            if ax > 0.35: m |= GB_RIGHT
            if ax < -0.35: m |= GB_LEFT
            if ay > 0.35: m |= GB_DOWN
            if ay < -0.35: m |= GB_UP
        # analog triggers
        if j.get_numaxes() >= 6:
            if j.get_axis(4) > 0.3: m |= GB_L
            if j.get_axis(5) > 0.3: m |= GB_R
    return m


# --------------------------------------------------------------------------- bezel
def draw_bezel(screen, game_img):
    w, h = screen.get_size()
    # shell gradient
    for y in range(h):
        t = y / max(h, 1)
        c = (int(242 - 100 * t), int(245 - 100 * t), int(248 - 96 * t))
        pygame.draw.line(screen, c, (0, y), (w, y))
    # rounded shell border
    pygame.draw.rect(screen, (95, 106, 118), (10, 10, w - 20, h - 20), 4, border_radius=28)

    # screen surround
    gw, gh = game_img.get_size()
    gx = (w - gw) // 2
    gy = (h - gh) // 2 - 24
    pygame.draw.rect(screen, (20, 24, 29), (gx - 22, gy - 22, gw + 44, gh + 44),
                     border_radius=14)
    pygame.draw.rect(screen, (8, 10, 12), (gx - 10, gy - 10, gw + 20, gh + 20),
                     border_radius=8)
    screen.blit(game_img, (gx, gy))

    # power LED
    pygame.draw.circle(screen, (224, 80, 60), (gx - 16, gy + gh // 2), 4)

    font = pygame.font.SysFont("ui-sans-serif,arial,helvetica", 26, bold=True)
    small = pygame.font.SysFont("ui-sans-serif,arial,helvetica", 13)
    word = font.render("S I L V E R", True, (109, 120, 133))
    screen.blit(word, word.get_rect(center=(w // 2, gy + gh + 40)))
    sub = small.render("POKESILVER PC · HIGH-RES PORT", True, (142, 153, 166))
    screen.blit(sub, sub.get_rect(center=(w // 2, gy + gh + 60)))

    # d-pad
    cx, cy = w // 2 - 320, gy + gh // 2 + 40
    pygame.draw.rect(screen, (40, 46, 53), (cx - 16, cy - 52, 32, 104), border_radius=8)
    pygame.draw.rect(screen, (40, 46, 53), (cx - 52, cy - 16, 104, 32), border_radius=8)
    pygame.draw.circle(screen, (43, 50, 58), (cx, cy), 11)

    # face buttons
    bx, by = w // 2 + 320, gy + gh // 2 + 40
    for i, (dx, dy, lab) in enumerate([(0, -34, "X"), (34, 0, "A"), (-34, 0, "Y"), (0, 34, "B")]):
        col = (110, 124, 138) if lab in "XA" else (128, 140, 152)
        pygame.draw.circle(screen, col, (bx + dx, by + dy), 22)
        t = small.render(lab, True, (230, 236, 242))
        screen.blit(t, t.get_rect(center=(bx + dx, by + dy)))

    # start / select
    for i, lab in enumerate(("SELECT", "START")):
        rx = w // 2 - 60 + i * 70
        pygame.draw.rect(screen, (57, 66, 76), (rx, gy + gh + 78, 56, 18), border_radius=9)


def draw_toasts(screen, app):
    now = time.time()
    app.toasts = [t for t in app.toasts if t[1] > now]
    big = pygame.font.SysFont("ui-sans-serif,arial,helvetica", 17)
    w, h = screen.get_size()
    for i, (text, _, pos) in enumerate(app.toasts):
        img = big.render(text, True, (238, 243, 248))
        rect = img.get_rect(center=(w // 2, 40 + i * 34 if pos == "top" else h - 46 - i * 34))
        pad = pygame.Rect(rect.x - 16, rect.y - 7, rect.w + 32, rect.h + 14)
        s = pygame.Surface((pad.w, pad.h), pygame.SRCALPHA)
        s.fill((12, 16, 21, 210))
        screen.blit(s, pad)
        pygame.draw.rect(screen, (60, 70, 82), pad, 1, border_radius=pad.h // 2)
        screen.blit(img, rect)


def draw_overlay(screen, app):
    w, h = screen.get_size()
    s = pygame.Surface((w, h), pygame.SRCALPHA)
    s.fill((5, 8, 11, 185))
    screen.blit(s, (0, 0))
    card = pygame.Rect(w // 2 - 300, 60, 600, h - 120)
    pygame.draw.rect(screen, (21, 26, 33), card, border_radius=14)
    pygame.draw.rect(screen, (38, 48, 60), card, 2, border_radius=14)

    title = pygame.font.SysFont("ui-sans-serif,arial,helvetica", 22, bold=True)
    body = pygame.font.SysFont("ui-sans-serif,arial,helvetica", 15)
    screen.blit(title.render("Settings & assist tools", True, (231, 236, 242)),
                (card.x + 26, card.y + 22))

    lines = [
        ("Video", f"upscaler: {FILTERS[app.filter]}   grid: {'on' if app.grid else 'off'}   "
                  f"scanlines: {'on' if app.scan else 'off'}"),
        ("", "1-5 = upscaler   g = grid   h = scanlines"),
        ("Audio", f"mix: {['Authentic', 'Enhanced', 'Crystal'][app.audio_mode]}   "
                  f"volume: {app.volume:.0%}"),
        ("", "m = cycle mix   [ / ] = volume"),
        ("Emulation", f"speed: {app.speed}%   colour: {['raw', 'CGB', 'vivid'][app.color]}"),
        ("", "- / = = speed   c = colour correction"),
        ("Input", f"preset: {PRESETS[app.preset][0]}"),
        ("", "F9 cycle preset   F10 bezel   F1/F2 state   Esc close/exit"),
    ]
    y = card.y + 70
    for head, text in lines:
        if head:
            screen.blit(body.render(head.upper(), True, (147, 160, 173)), (card.x + 26, y))
            y += 22
        screen.blit(body.render(text, True, (214, 222, 231)), (card.x + 40, y))
        y += 26


def pick_rom_interactive(pygame):
    print("No cartridge given.  Pass one on the command line, e.g.")
    print("    python3 pc/launcher.py \"Pokemon - Silver Version (USA).gbc\"")
    print("or run the bundled self-test:  python3 pc/launcher.py --demo")
    return None


def main():
    ap = argparse.ArgumentParser(description="PokéSilver PC native front-end")
    ap.add_argument("rom", nargs="?", help="cartridge image (.gb/.gbc/.zip)")
    ap.add_argument("--mods", nargs="*", default=[], help="IPS/UPS/BPS patches")
    ap.add_argument("--demo", action="store_true", help="run the bundled self-test demo")
    ap.add_argument("--selftest", action="store_true", help="headless smoke test")
    ap.add_argument("--frames", type=int, default=240)
    args = ap.parse_args()
    if args.selftest:
        os.environ.setdefault("SDL_VIDEODRIVER", "dummy")
        os.environ.setdefault("SDL_AUDIODRIVER", "dummy")
    rc, _app = run(args)
    return rc


if __name__ == "__main__":
    sys.exit(main())
