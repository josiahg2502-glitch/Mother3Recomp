/* ==========================================================================
 *  state.js -- save states (F1 / F2 quick slots) persisted in localStorage
 * ========================================================================== */
"use strict";

const States = (() => {
  const KEY = "pksilver.states.v1";

  function readAll() {
    try { return JSON.parse(localStorage.getItem(KEY) || "{}"); }
    catch { return {}; }
  }

  function writeAll(obj) {
    try { localStorage.setItem(KEY, JSON.stringify(obj)); return true; }
    catch (e) { return false; }
  }

  function slotKey(romCrc, slot) { return romCrc.toString(16) + ":" + slot; }

  function save(romCrc, slot) {
    const n = Core.ex.gb_js_save_state();
    if (n <= 0) return { ok: false, why: "core refused to save" };
    const ptr = Core.ex.gb_js_state_buf();
    const bytes = Core.u8.subarray(ptr, ptr + n);
    let bin = "";
    const CH = 0x8000;
    for (let i = 0; i < bytes.length; i += CH)
      bin += String.fromCharCode.apply(null, bytes.subarray(i, i + CH));
    const all = readAll();
    all[slotKey(romCrc, slot)] = {
      b64: btoa(bin),
      when: Date.now(),
      bytes: n,
    };
    return { ok: writeAll(all), bytes: n };
  }

  function load(romCrc, slot) {
    const all = readAll();
    const rec = all[slotKey(romCrc, slot)];
    if (!rec) return { ok: false, why: "no state in this slot yet" };
    const bin = atob(rec.b64);
    const bytes = new Uint8Array(bin.length);
    for (let i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
    const ptr = Core.ex.gb_js_state_buf();
    Core.poke(ptr, bytes);
    const rc = Core.ex.gb_js_load_state(bytes.length);
    return { ok: rc === 0, rc };
  }

  function has(romCrc, slot) {
    return !!readAll()[slotKey(romCrc, slot)];
  }

  return { save, load, has };
})();

window.States = States;
