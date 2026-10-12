#!/usr/bin/env python3
"""
native_test.py -- scripted exercise of the native launcher's hotkey surface.

Posts synthetic SDL key events (F10 / F9 / F11 / F1 / F2 and the overlay
option keys) and asserts the launcher's state machine reacts as specified.
Headless: uses the SDL dummy video/audio drivers.
"""
import argparse
import os
import sys

os.environ["SDL_VIDEODRIVER"] = "dummy"
os.environ["SDL_AUDIODRIVER"] = "dummy"

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

import pygame
import pc.launcher as L


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rom", default=os.path.join(ROOT, "assets", "pksilver_demo.gbc"))
    args = ap.parse_args()

    pygame.init()

    # queue the whole hotkey spec; run() consumes them over its frames
    for key in (pygame.K_F10, pygame.K_F9, pygame.K_F9, pygame.K_F11,
                pygame.K_1, pygame.K_3, pygame.K_g, pygame.K_h,
                pygame.K_m, pygame.K_m, pygame.K_LEFTBRACKET, pygame.K_RIGHTBRACKET,
                pygame.K_MINUS, pygame.K_EQUALS, pygame.K_c,
                pygame.K_F1, pygame.K_F2):
        pygame.event.post(pygame.event.Event(pygame.KEYDOWN, key=key))

    run_args = argparse.Namespace(
        rom=args.rom, mods=[], demo=False, selftest=True, frames=40)
    rc, app = L.run(run_args)

    ok = True

    def check(label, cond, detail=""):
        nonlocal ok
        print(f"  [{'PASS' if cond else 'FAIL'}] {label} {detail}")
        if not cond:
            ok = False

    print("== native launcher hotkey surface ==")
    check("ran frames", app.frames >= 30, f"({app.frames})")
    check("F10 toggled bezel off", app.bezel is False,
          "(True -> False after one press)")
    # F9 fired twice -> preset index 2
    check("F9 cycled presets", app.preset == 2,
          f"({L.PRESETS[app.preset][0]})")
    check("F11 opened overlay", app.overlay is True)
    check("option keys applied", app.filter == 2 and app.grid is False and app.scan is True,
          f"(filter={app.filter} grid={app.grid} scan={app.scan})")
    check("audio mix cycled", app.audio_mode == 0, f"(mode={app.audio_mode}; 1->2->0)")
    check("speed changed", app.speed == 100, f"(speed={app.speed})")
    check("color changed", app.color == 2, f"(color={app.color}; 1->2)")

    # F1 must have written a save-state file
    sp = app.state_path(1)
    check("F1 wrote a quick-save", os.path.exists(sp),
          f"({sp} {os.path.getsize(sp) if os.path.exists(sp) else 0} bytes)")

    pygame.quit()
    print("== RESULT:", "ALL PASS" if ok else "FAILURES", "==")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
