/* ==========================================================================
 *  ui.js -- HUD toasts and the settings/assist overlay (F11)
 * ========================================================================== */
"use strict";

const UI = (() => {
  let hud = null;
  let overlay = null;

  function init() {
    hud = document.getElementById("hud");
    overlay = document.getElementById("overlay");
  }

  /* position: 'top' or 'bottom' (bottom sits in the bezel's lower terrain) */
  function toast(text, position = "top", ms = 1800) {
    const el = document.createElement("div");
    el.className = "toast " + position;
    el.textContent = text;
    el.style.animationDuration = ".18s, .3s";
    el.style.animationDelay = `0s, ${ms}ms`;
    hud.appendChild(el);
    setTimeout(() => el.remove(), ms + 400);
  }

  function openOverlay()  { overlay.classList.remove("hidden"); }
  function closeOverlay() { overlay.classList.add("hidden"); }
  function toggleOverlay() { overlay.classList.contains("hidden") ? openOverlay() : closeOverlay(); }
  function overlayOpen()  { return !overlay.classList.contains("hidden"); }

  return { init, toast, openOverlay, closeOverlay, toggleOverlay, overlayOpen };
})();

window.UI = UI;
