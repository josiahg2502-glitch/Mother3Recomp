/* ==========================================================================
 *  input.js -- keyboard + Gamepad API, with the four controller presets
 *
 *  Game Boy button bits (active high), matching core/gb.h:
 *    RIGHT 0x01  LEFT 0x02  UP 0x04  DOWN 0x08
 *    A     0x10  B    0x20  SELECT 0x40  START 0x80
 *    L     0x100 R    0x200            (assist shoulders)
 * ========================================================================== */
"use strict";

const GB = {
  RIGHT: 0x01, LEFT: 0x02, UP: 0x04, DOWN: 0x08,
  A: 0x10, B: 0x20, SELECT: 0x40, START: 0x80,
  L: 0x100, R: 0x200,
};

/* Standard gamepad layout indices */
const PAD = {
  FACE_BOTTOM: 0, FACE_RIGHT: 1, FACE_LEFT: 2, FACE_TOP: 3,
  LB: 4, RB: 5, LT: 6, RT: 7,
  BACK: 8, START: 9, L3: 10, R3: 11,
  UP: 12, DOWN: 13, LEFT: 14, RIGHT: 15,
};

/* The four presets.  Each maps a standard-layout control to GB buttons.
 *   Xbox Standard      -- faces as labelled: A=A, B=B, X=B, Y=A
 *   Nintendo Switch    -- faces by position: bottom=B, right=A, left(Y)=A, top(X)=B
 *   Modern Trigger Action -- LT/RT drive B/A like shoulder-action shooters,
 *                           faces still live
 *   Classic 1:1        -- the GB's own eight buttons, nothing extra, no stick
 */
const PRESETS = [
  {
    name: "Xbox Standard",
    stick: true,
    buttons: {
      [PAD.FACE_BOTTOM]: GB.A, [PAD.FACE_RIGHT]: GB.B,
      [PAD.FACE_LEFT]: GB.B,  [PAD.FACE_TOP]: GB.A,
      [PAD.LB]: GB.L, [PAD.RB]: GB.R, [PAD.LT]: GB.L, [PAD.RT]: GB.R,
      [PAD.BACK]: GB.SELECT, [PAD.START]: GB.START,
      [PAD.UP]: GB.UP, [PAD.DOWN]: GB.DOWN, [PAD.LEFT]: GB.LEFT, [PAD.RIGHT]: GB.RIGHT,
    },
    axes: {},
  },
  {
    name: "Nintendo Switch",
    stick: true,
    buttons: {
      [PAD.FACE_BOTTOM]: GB.B, [PAD.FACE_RIGHT]: GB.A,
      [PAD.FACE_LEFT]: GB.A,  [PAD.FACE_TOP]: GB.B,
      [PAD.LB]: GB.L, [PAD.RB]: GB.R, [PAD.LT]: GB.L, [PAD.RT]: GB.R,
      [PAD.BACK]: GB.SELECT, [PAD.START]: GB.START,
      [PAD.UP]: GB.UP, [PAD.DOWN]: GB.DOWN, [PAD.LEFT]: GB.LEFT, [PAD.RIGHT]: GB.RIGHT,
    },
    axes: {},
  },
  {
    name: "Modern Trigger Action",
    stick: true,
    buttons: {
      [PAD.FACE_BOTTOM]: GB.A, [PAD.FACE_RIGHT]: GB.B,
      [PAD.FACE_LEFT]: GB.B,  [PAD.FACE_TOP]: GB.A,
      [PAD.LB]: GB.L, [PAD.RB]: GB.R,
      [PAD.LT]: GB.B, [PAD.RT]: GB.A,      /* triggers become the action pair */
      [PAD.BACK]: GB.SELECT, [PAD.START]: GB.START,
      [PAD.UP]: GB.UP, [PAD.DOWN]: GB.DOWN, [PAD.LEFT]: GB.LEFT, [PAD.RIGHT]: GB.RIGHT,
    },
    axes: {},
  },
  {
    name: "Classic 1:1",
    stick: false,                            /* d-pad only, exactly eight buttons */
    buttons: {
      [PAD.FACE_BOTTOM]: GB.A, [PAD.FACE_RIGHT]: GB.B,
      [PAD.LB]: GB.L, [PAD.RB]: GB.R,
      [PAD.BACK]: GB.SELECT, [PAD.START]: GB.START,
      [PAD.UP]: GB.UP, [PAD.DOWN]: GB.DOWN, [PAD.LEFT]: GB.LEFT, [PAD.RIGHT]: GB.RIGHT,
    },
    axes: {},
  },
];

const Input = (() => {
  let presetIndex = 0;
  let kb = 0;                    /* keyboard-derived bitmask */
  const onPresetChange = [];

  const KEYMAP = {
    ArrowRight: GB.RIGHT, KeyD: GB.RIGHT,
    ArrowLeft: GB.LEFT,  KeyA2: 0,
    ArrowUp: GB.UP,      ArrowDown: GB.DOWN,
    KeyZ: GB.A, KeyX: GB.B,
    Enter: GB.START, ShiftRight: GB.SELECT, Space: GB.START,
    Backspace: GB.SELECT,
    KeyS: GB.L, KeyQ: GB.L,
    KeyW: GB.R, KeyE: GB.R,
  };
  /* WASD-ish d-pad without clobbering the L shoulder on S */
  delete KEYMAP.KeyA2;
  KEYMAP.KeyA = GB.LEFT;         /* A = left (L shoulder lives on S) */

  function attach() {
    window.addEventListener("keydown", (e) => {
      const b = KEYMAP[e.code];
      if (b) { kb |= b; if (e.target === document.body) e.preventDefault(); }
    });
    window.addEventListener("keyup", (e) => {
      const b = KEYMAP[e.code];
      if (b) kb &= ~b;
    });
    window.addEventListener("blur", () => { kb = 0; });
    window.addEventListener("gamepadconnected", () => {});
  }

  const DEAD = 0.35;

  function poll() {
    let mask = kb;
    const pads = navigator.getGamepads ? navigator.getGamepads() : [];
    let connected = false;
    for (const gp of pads) {
      if (!gp || !gp.connected) continue;
      connected = true;
      const p = PRESETS[presetIndex];

      for (const [idxStr, bits] of Object.entries(p.buttons)) {
        const btn = gp.buttons[+idxStr];
        if (btn && (btn.pressed || btn.value > 0.5)) mask |= bits;
      }
      if (p.stick && gp.axes && gp.axes.length >= 2) {
        const ax = gp.axes[0], ay = gp.axes[1];
        /* full 8-way grid from the analog stick */
        if (ax > DEAD)  mask |= GB.RIGHT;
        if (ax < -DEAD) mask |= GB.LEFT;
        if (ay > DEAD)  mask |= GB.DOWN;
        if (ay < -DEAD) mask |= GB.UP;
      }
      /* analog triggers: half-press = shoulder, per the preset tables */
      if (gp.buttons[PAD.LT] && gp.buttons[PAD.LT].value > 0.3) mask |= GB.L;
      if (gp.buttons[PAD.RT] && gp.buttons[PAD.RT].value > 0.3) mask |= GB.R;
      break;   /* first connected pad wins */
    }
    return { mask, connected };
  }

  return {
    GB, PRESETS,
    attach,
    poll,
    get presetIndex() { return presetIndex; },
    get presetName() { return PRESETS[presetIndex].name; },
    cyclePreset() {
      presetIndex = (presetIndex + 1) % PRESETS.length;
      onPresetChange.forEach((f) => f(presetIndex));
      return presetIndex;
    },
    setPreset(i) {
      presetIndex = ((i % PRESETS.length) + PRESETS.length) % PRESETS.length;
      onPresetChange.forEach((f) => f(presetIndex));
      return presetIndex;
    },
    onPresetChange: (f) => onPresetChange.push(f),
  };
})();

window.Input = Input;
