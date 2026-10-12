/* ==========================================================================
 *  main.js -- front-end orchestration: launcher, run loop, hotkeys, layout
 * ========================================================================== */
"use strict";

const App = (() => {
  let running = false;
  let romBytes = null;
  let romCrc = 0;
  let romTitle = "";
  let bezelMode = true;
  let rafId = 0;
  let lastT = 0;
  let frameAcc = 0;
  let fpsAcc = 0, fpsCount = 0, fpsShown = 0;

  const $ = (id) => document.getElementById(id);

  /* ------------------------------------------------------------ zip ----- */
  async function unzipRom(bytes) {
    /* minimal ZIP reader: scan the central directory for the largest
     * .gb/.gbc/.bin entry and inflate it with DecompressionStream.        */
    const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    // find End Of Central Directory
    let eocd = -1;
    for (let i = bytes.length - 22; i >= 0; i--) {
      if (dv.getUint32(i, true) === 0x06054b50) { eocd = i; break; }
    }
    if (eocd < 0) return null;
    const count = dv.getUint16(eocd + 10, true);
    let off = dv.getUint32(eocd + 16, true);
    const entries = [];
    for (let i = 0; i < count; i++) {
      if (dv.getUint32(off, true) !== 0x02014b50) break;
      const method = dv.getUint16(off + 10, true);
      const csize = dv.getUint32(off + 20, true);
      const usize = dv.getUint32(off + 24, true);
      const nlen = dv.getUint16(off + 28, true);
      const elen = dv.getUint16(off + 30, true);
      const clen = dv.getUint16(off + 32, true);
      const lho = dv.getUint32(off + 42, true);
      const name = new TextDecoder().decode(bytes.subarray(off + 46, off + 46 + nlen));
      entries.push({ name, method, csize, usize, lho });
      off += 46 + nlen + elen + clen;
    }
    const rom = entries
      .filter((e) => /\.(gbc|gb|bin|rom)$/i.test(e.name) && !e.name.includes("/"))
      .concat(entries.filter((e) => /\.(gbc|gb|bin|rom)$/i.test(e.name)))
      .sort((a, b) => b.usize - a.usize)[0];
    if (!rom) return null;

    // local header -> data offset
    const lnlen = dv.getUint16(rom.lho + 26, true);
    const lelen = dv.getUint16(rom.lho + 28, true);
    const dataOff = rom.lho + 30 + lnlen + lelen;
    const raw = bytes.subarray(dataOff, dataOff + rom.csize);

    if (rom.method === 0) return raw.slice();
    if (rom.method === 8 && window.DecompressionStream) {
      const ds = new DecompressionStream("deflate-raw");
      const stream = new Blob([raw]).stream().pipeThrough(ds);
      const buf = await new Response(stream).arrayBuffer();
      return new Uint8Array(buf);
    }
    return null;
  }

  /* ---------------------------------------------------------- loading --- */
  function setStatus(msg, cls) {
    const el = $("load-status");
    el.textContent = msg || "";
    el.style.color = cls === "err" ? "#e08a7d" : "#8fd0a0";
  }

  async function loadRomBytes(bytes, label) {
    romBytes = bytes;
    const buf = Core.ex.gb_rom_buffer();
    if (bytes.length > 8 * 1024 * 1024) {
      setStatus("cartridge too large (max 8 MiB)", "err");
      return false;
    }
    Core.poke(buf, bytes);
    const rc = Core.ex.gb_load_rom(bytes.length);
    if (rc !== 0) {
      setStatus("not a Game Boy cartridge image", "err");
      return false;
    }
    romCrc = Core.ex.gb_rom_crc32() >>> 0;

    // re-apply mods over the clean dump
    const results = Mods.applyAll();
    const bad = results.filter((r) => r.rc !== 0);
    if (bad.length) {
      setStatus(`warning: ${bad.length} patch(es) failed to apply`, "err");
    } else if (results.length) {
      setStatus(`${results.length} patch(es) applied`, null);
    }

    romTitle = romTitleOf();
    setStatus(`loaded ${label || romTitle} (${(bytes.length / 1024).toFixed(0)} KiB)`, null);
    rememberSession(label);
    return true;
  }

  function romTitleOf() {
    const t = new Uint8Array(16);
    const titlePtr = Core.ex.gb_rom_data ? Core.ex.gb_rom_data() : 0;
    if (!titlePtr) return "cartridge";
    for (let i = 0; i < 16; i++) t[i] = Core.u8[titlePtr + 0x134 + i];
    let s = "";
    for (let i = 0; i < 16; i++) {
      const c = t[i];
      if (c === 0 || c === 0x80) break;
      s += (c >= 0x20 && c <= 0x7e) ? String.fromCharCode(c) : "_";
    }
    return s || "cartridge";
  }

  function rememberSession(label) {
    try {
      localStorage.setItem("pksilver.session", JSON.stringify({ label, crc: romCrc }));
    } catch {}
  }

  /* ------------------------------------------------------- run / stop --- */
  function startGame() {
    $("launcher").classList.add("hidden");
    $("stage").classList.remove("hidden");

    const canvas = $("lcd");
    if (!Video.attach(canvas)) {
      setStatus("WebGL unavailable in this browser", "err");
      return;
    }
    Bezel.mount($("bezel-svg"), $("screen-well"));
    applyBezelMode();

    Core.ex.gb_set_color_correction(+$("opt-color").value);
    Core.ex.gb_set_audio_mode(+$("opt-audio").value);
    Core.ex.gb_set_volume(+$("opt-vol").value / 100);
    Core.ex.gb_set_speed_multiplier(+$("opt-speed").value);

    AudioOut.start();
    AudioOut.resume();

    running = true;
    lastT = performance.now();
    rafId = requestAnimationFrame(loop);
    UI.toast(`Now playing — ${romTitle}`, "top");
  }

  function stopGame(toLauncher) {
    running = false;
    cancelAnimationFrame(rafId);
    AudioOut.suspend();
    if (toLauncher) {
      $("stage").classList.add("hidden");
      $("launcher").classList.remove("hidden");
      UI.closeOverlay();
    }
  }

  /* ------------------------------------------------------------- loop --- */
  function loop(t) {
    if (!running) return;
    const dt = t - lastT;
    lastT = t;

    // input
    const { mask } = Input.poll();
    Core.ex.gb_set_input(mask);

    // real-time clock feed
    if ($("opt-rtc").checked) {
      Core.ex.gb_set_rtc_now(Math.floor(Date.now() / 1000));
    }

    // one emulated frame per display frame (speed handled in the core)
    Core.ex.gb_run_frame();

    // present
    layoutCanvas();
    Video.present();

    // fps
    fpsAcc += dt; fpsCount++;
    if (fpsAcc > 1000) {
      fpsShown = Math.round(1000 * fpsCount / fpsAcc);
      fpsAcc = 0; fpsCount = 0;
    }

    rafId = requestAnimationFrame(loop);
  }

  /* ----------------------------------------------------------- layout --- */
  function layoutCanvas() {
    const wrap = $("bezel-wrap");
    const well = $("screen-well");
    const canvas = $("lcd");

    if (bezelMode) {
      // the well rectangle is set by Bezel.mount as a % of the SVG box; the
      // SVG is sized to fit the stage, so measure the rendered well.
      const r = well.getBoundingClientRect();
      const [w, h] = Video.fit(r.width, r.height);
      if (canvas.width !== w || canvas.height !== h) {
        canvas.style.width = w + "px";
        canvas.style.height = h + "px";
        canvas.width = w; canvas.height = h;
      }
    } else {
      const r = wrap.getBoundingClientRect();
      const [w, h] = Video.fit(r.width, r.height);
      if (canvas.width !== w || canvas.height !== h) {
        canvas.style.width = w + "px";
        canvas.style.height = h + "px";
        canvas.width = w; canvas.height = h;
      }
    }
  }

  function applyBezelMode() {
    const wrap = $("bezel-wrap");
    if (bezelMode) wrap.classList.remove("clean-mode");
    else wrap.classList.add("clean-mode");
    layoutCanvas();
  }

  /* --------------------------------------------------------- hotkeys ---- */
  function hotkeys(e) {
    if (e.repeat) return;
    switch (e.key) {
      case "F10":
        e.preventDefault();
        bezelMode = !bezelMode;
        applyBezelMode();
        UI.toast(bezelMode ? "Custom bezel — 1080p shell" : "Clean 3:2 pillarbox fullscreen", "top");
        break;
      case "F9": {
        e.preventDefault();
        const i = Input.cyclePreset();
        $("opt-pad").value = String(i);
        UI.toast(`Controller preset — ${Input.presetName}`, "bottom");
        break;
      }
      case "F11":
        e.preventDefault();
        UI.toggleOverlay();
        break;
      case "Escape":
        e.preventDefault();
        if (UI.overlayOpen()) { UI.closeOverlay(); }
        else if (running) { stopGame(true); UI.toast("Exited to launcher", "top"); }
        break;
      case "F1":
        e.preventDefault();
        quickSave();
        break;
      case "F2":
        e.preventDefault();
        quickLoad();
        break;
      default:
        break;
    }
  }

  function quickSave() {
    const r = States.save(romCrc, 1);
    UI.toast(r.ok ? `State saved — slot 1 (${(r.bytes / 1024).toFixed(0)} KiB)`
                  : `Save failed: ${r.why}`, "top");
  }

  function quickLoad() {
    const r = States.load(romCrc, 1);
    UI.toast(r.ok ? "State loaded — slot 1" : `Load failed: ${r.why}`, "top");
  }

  /* ----------------------------------------------------------- wiring --- */
  function wireOptions() {
    $("opt-filter").addEventListener("change", (e) => Video.opts.filter = +e.target.value);
    $("opt-grid").addEventListener("change", (e) => Video.opts.grid = e.target.checked ? 1 : 0);
    $("opt-scan").addEventListener("change", (e) => Video.opts.scan = e.target.checked ? 1 : 0);
    $("opt-intscale").addEventListener("change", (e) => Video.opts.intScale = +e.target.value);
    $("opt-color").addEventListener("change", (e) => Core.ex.gb_set_color_correction(+e.target.value));
    $("opt-audio").addEventListener("change", (e) => Core.ex.gb_set_audio_mode(+e.target.value));
    $("opt-vol").addEventListener("input", (e) => Core.ex.gb_set_volume(+e.target.value / 100));
    $("opt-speed").addEventListener("change", (e) => Core.ex.gb_set_speed_multiplier(+e.target.value));
    $("opt-pad").addEventListener("change", (e) => {
      const i = Input.setPreset(+e.target.value);
      UI.toast(`Controller preset — ${Input.presetName}`, "bottom");
    });

    $("btn-save").addEventListener("click", quickSave);
    $("btn-load").addEventListener("click", quickLoad);
    $("btn-reset").addEventListener("click", async () => {
      if (!romBytes) return;
      const buf = Core.ex.gb_rom_buffer();
      Core.poke(buf, romBytes);
      Core.ex.gb_load_rom(romBytes.length);
      Mods.applyAll();
      UI.toast("Cartridge reset", "top");
    });
    $("btn-quit").addEventListener("click", () => { stopGame(true); });
    $("btn-close-overlay").addEventListener("click", () => UI.closeOverlay());

    // mods
    $("btn-add-mod").addEventListener("click", () => $("file-mod").click());
    $("file-mod").addEventListener("change", async (e) => {
      const names = await Mods.addFiles(e.target.files);
      refreshModList();
      UI.toast(`Added ${names.length} patch(es) — reset to apply`, "top");
      e.target.value = "";
    });

    // cheats
    $("btn-cheat-add").addEventListener("click", () => {
      const v = $("cheat-input").value.trim();
      if (!v) return;
      const rc = Core.ex.gb_add_cheat(v);
      $("cheat-input").value = "";
      refreshCheatList();
      UI.toast(rc === 0 ? `Cheat added: ${v.toUpperCase()}` : "Invalid GameShark code", "top");
    });
    $("btn-cheat-clear").addEventListener("click", () => {
      Core.ex.gb_clear_cheats();
      refreshCheatList();
      UI.toast("Cheats cleared", "top");
    });

    window.addEventListener("resize", layoutCanvas);
    window.addEventListener("keydown", hotkeys);
  }

  function refreshModList() {
    const ul = $("mod-list");
    ul.innerHTML = "";
    Mods.entries().forEach((m, i) => {
      const li = document.createElement("li");
      li.textContent = `${m.name} (${(m.bytes.length / 1024).toFixed(1)} KiB)`;
      const rm = document.createElement("button");
      rm.textContent = "remove";
      rm.style.marginLeft = "8px";
      rm.onclick = () => { Mods.removeAt(i); refreshModList(); };
      li.appendChild(rm);
      ul.appendChild(li);
    });
  }

  function refreshCheatList() {
    const ul = $("cheat-list");
    ul.innerHTML = "";
    const n = Core.ex.gb_cheat_count();
    const li = document.createElement("li");
    li.textContent = n ? `${n} cheat(s) active` : "none";
    ul.appendChild(li);
  }

  /* --------------------------------------------------------- launcher --- */
  function wireLauncher() {
    $("btn-open-rom").addEventListener("click", () => $("file-rom").click());
    $("file-rom").addEventListener("change", async (e) => {
      const f = e.target.files[0];
      if (!f) return;
      let bytes = new Uint8Array(await f.arrayBuffer());
      if (/\.zip$/i.test(f.name)) {
        setStatus("unpacking archive…", null);
        const inner = await unzipRom(bytes);
        if (!inner) { setStatus("no cartridge found inside the zip", "err"); return; }
        bytes = inner;
      }
      const ok = await loadRomBytes(bytes, f.name);
      if (ok) startGame();
      e.target.value = "";
    });

    $("btn-demo").addEventListener("click", async () => {
      setStatus("loading core self-test demo…", null);
      const res = await fetch("assets/pksilver_demo.gbc");
      if (!res.ok) { setStatus("demo cartridge missing", "err"); return; }
      const bytes = new Uint8Array(await res.arrayBuffer());
      const ok = await loadRomBytes(bytes, "PKSILVER DEMO");
      if (ok) startGame();
    });

    // continue button if we have a remembered session
    try {
      const s = JSON.parse(localStorage.getItem("pksilver.session") || "null");
      if (s && s.label) $("btn-continue").classList.remove("hidden");
    } catch {}
    $("btn-continue").addEventListener("click", () => $("btn-open-rom").click());
  }

  async function boot() {
    UI.init();
    try {
      await Core.init("wasm/pksilver_core.wasm");
    } catch (e) {
      setStatus("failed to start the core: " + e.message, "err");
      return;
    }
    Input.attach();
    wireLauncher();
    wireOptions();
    setStatus("core ready — insert a cartridge", null);

    // gamepad status line
    setInterval(() => {
      const pads = navigator.getGamepads ? navigator.getGamepads() : [];
      const gp = [...pads].find((p) => p && p.connected);
      $("pad-status").textContent = gp
        ? `Connected: ${gp.id} — preset: ${Input.presetName}`
        : "No controller detected.";
    }, 1000);
  }

  document.addEventListener("DOMContentLoaded", boot);

  return { startGame, stopGame, quickSave, quickLoad };
})();

window.App = App;
