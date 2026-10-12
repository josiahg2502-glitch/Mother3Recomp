/* ==========================================================================
 *  core.js -- wasm loader + typed views over the core's linear memory
 * ========================================================================== */
"use strict";

const Core = (() => {
  let instance = null;
  let views = null;
  const ex = {};

  function refreshViews() {
    const buf = instance.exports.memory.buffer;
    views = {
      u8: new Uint8Array(buf),
      u32: new Uint32Array(buf),
      i16: new Int16Array(buf),
      dv: new DataView(buf),
    };
  }

  function ensure() {
    if (!views || views.u8.buffer !== instance.exports.memory.buffer) refreshViews();
    return views;
  }

  async function init(url) {
    const res = await fetch(url);
    if (!res.ok) throw new Error("failed to fetch core: " + res.status);
    const bytes = await res.arrayBuffer();
    const r = await WebAssembly.instantiate(bytes, {});
    instance = r.instance;
    Object.assign(ex, instance.exports);
    refreshViews();
    ex.gb_init();
    return ex;
  }

  return {
    init,
    ex,
    get u8() { return ensure().u8; },
    get u32() { return ensure().u32; },
    get i16() { return ensure().i16; },
    get dv() { return ensure().dv; },
    /* write a JS byte array into a wasm pointer */
    poke(ptr, bytes) {
      ensure().u8.set(bytes, ptr);
    },
    /* read N bytes from a wasm pointer */
    peek(ptr, n) {
      return ensure().u8.subarray(ptr, ptr + n).slice();
    },
  };
})();

window.Core = Core;
