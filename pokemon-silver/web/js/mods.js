/* ==========================================================================
 *  mods.js -- ROM patch (mod) support: IPS / UPS / BPS applied over the clean
 *              dump, in load order, reapplied on every cartridge reset.
 * ========================================================================== */
"use strict";

const Mods = (() => {
  const list = [];      /* {name, bytes} in application order */

  function applyAll() {
    const results = [];
    for (const m of list) {
      const ptr = Core.ex.gb_js_patch_buf();
      if (m.bytes.length > Core.ex.gb_js_patch_cap()) {
        results.push({ name: m.name, rc: -9 });
        continue;
      }
      Core.poke(ptr, m.bytes);
      const rc = Core.ex.gb_js_apply_patch(m.bytes.length);
      results.push({ name: m.name, rc });
    }
    return results;
  }

  async function addFiles(files) {
    const added = [];
    for (const f of files) {
      const buf = new Uint8Array(await f.arrayBuffer());
      list.push({ name: f.name, bytes: buf });
      added.push(f.name);
    }
    return added;
  }

  function removeAt(i) { list.splice(i, 1); }
  function clear() { list.length = 0; }
  function entries() { return list.slice(); }

  return { addFiles, applyAll, removeAt, clear, entries };
})();

window.Mods = Mods;
