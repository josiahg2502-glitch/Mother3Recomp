/* ==========================================================================
 *  bezel.js -- the in-game bezel: an original, vector-drawn handheld shell
 *              that frames the 160x144 screen.
 *
 *  The artwork is authored here from scratch (gradients, shell curves, an
 *  engraved wordmark) -- it is *inspired by* the Game Boy Color form factor
 *  but is a new design, so the port ships no third-party artwork at all.
 *  A silver/pewter finish nods to the cartridge's own identity.
 * ========================================================================== */
"use strict";

const Bezel = (() => {
  /* viewBox of the bezel SVG; the screen well is placed by CSS inside it */
  const VB_W = 1180, VB_H = 760;

  /* The screen well, in viewBox units.  The CSS positions #screen-well to
   * match this rectangle exactly.                                         */
  const WELL = { x: 400, y: 118, w: 380, h: 342 };   /* 10:9-ish with border */

  function shellSVG() {
    return `
    <defs>
      <linearGradient id="shellTop" x1="0" y1="0" x2="0" y2="1">
        <stop offset="0"   stop-color="#f2f5f8"/>
        <stop offset=".45" stop-color="#c3ccd6"/>
        <stop offset="1"   stop-color="#8b96a3"/>
      </linearGradient>
      <linearGradient id="shellEdge" x1="0" y1="0" x2="1" y2="0">
        <stop offset="0" stop-color="#5f6a76"/>
        <stop offset=".5" stop-color="#aeb8c3"/>
        <stop offset="1" stop-color="#5f6a76"/>
      </linearGradient>
      <linearGradient id="screenBezel" x1="0" y1="0" x2="0" y2="1">
        <stop offset="0" stop-color="#23282e"/>
        <stop offset="1" stop-color="#14181d"/>
      </linearGradient>
      <radialGradient id="btnA" cx=".35" cy=".3" r=".9">
        <stop offset="0" stop-color="#8fa0b0"/><stop offset="1" stop-color="#4a5764"/>
      </radialGradient>
      <radialGradient id="btnB" cx=".35" cy=".3" r=".9">
        <stop offset="0" stop-color="#9aa8b6"/><stop offset="1" stop-color="#525f6c"/>
      </radialGradient>
      <linearGradient id="dpad" x1="0" y1="0" x2="0" y2="1">
        <stop offset="0" stop-color="#3c444d"/><stop offset="1" stop-color="#20262c"/>
      </linearGradient>
      <filter id="soft" x="-20%" y="-20%" width="140%" height="140%">
        <feDropShadow dx="0" dy="10" stdDeviation="14" flood-color="#000" flood-opacity=".55"/>
      </filter>
    </defs>

    <!-- ================= outer shell ================= -->
    <g filter="url(#soft)">
      <path d="
        M 150 60
        H 1030
        Q 1090 60 1105 118
        L 1128 300
        Q 1140 380 1128 460
        L 1105 642
        Q 1090 700 1030 700
        H 150
        Q 90 700 75 642
        L 52 460
        Q 40 380 52 300
        L 75 118
        Q 90 60 150 60 Z"
        fill="url(#shellTop)" stroke="url(#shellEdge)" stroke-width="3"/>

      <!-- brushed highlight across the top face -->
      <path d="M 150 62 H 1030 Q 1088 62 1103 118 L 1106 142 H 74 L 77 118 Q 92 62 150 62 Z"
            fill="#ffffff" opacity=".30"/>
    </g>

    <!-- ================= screen surround ================= -->
    <rect x="${WELL.x - 26}" y="${WELL.y - 30}" width="${WELL.w + 52}" height="${WELL.h + 58}"
          rx="16" fill="url(#screenBezel)"/>
    <rect x="${WELL.x - 12}" y="${WELL.y - 14}" width="${WELL.w + 24}" height="${WELL.h + 28}"
          rx="8" fill="#0a0c0f"/>
    <!-- power LED -->
    <circle cx="${WELL.x - 19}" cy="${WELL.y + WELL.h / 2}" r="5" fill="#e0503c">
      <animate attributeName="opacity" values="1;.55;1" dur="2.6s" repeatCount="indefinite"/>
    </circle>

    <!-- engraved wordmark under the screen -->
    <g fill="#6d7885" font-family="ui-sans-serif, system-ui, sans-serif"
       font-size="27" font-weight="600" letter-spacing="7" text-anchor="middle">
      <text x="${WELL.x + WELL.w / 2}" y="${WELL.y + WELL.h + 46}">S I L V E R</text>
    </g>
    <g fill="#8e99a6" font-family="ui-sans-serif, system-ui, sans-serif"
       font-size="12" letter-spacing="3" text-anchor="middle" opacity=".85">
      <text x="${WELL.x + WELL.w / 2}" y="${WELL.y + WELL.h + 66}">POKESILVER PC &middot; HIGH-RES PORT</text>
    </g>

    <!-- ================= d-pad (left grip) ================= -->
    <g transform="translate(250 470)">
      <g fill="url(#dpad)" stroke="#12161a" stroke-width="2">
        <rect x="-24" y="-70" width="48" height="140" rx="10"/>
        <rect x="-70" y="-24" width="140" height="48" rx="10"/>
      </g>
      <circle r="15" fill="#2b323a"/>
      <!-- direction glyphs -->
      <g fill="#59636e">
        <path d="M0 -56 l9 13 h-18z"/>
        <path d="M0 56 l9 -13 h-18z"/>
        <path d="M-56 0 l13 -9 v18z"/>
        <path d="M56 0 l-13 -9 v18z"/>
      </g>
    </g>

    <!-- ================= face buttons (right grip) ================= -->
    <g transform="translate(930 470) rotate(-18)">
      <circle cx="0"   cy="-46" r="30" fill="url(#btnA)"/>
      <circle cx="46"  cy="0"   r="30" fill="url(#btnB)"/>
      <circle cx="-46" cy="0"   r="30" fill="url(#btnB)"/>
      <circle cx="0"   cy="46"  r="30" fill="url(#btnA)"/>
      <g fill="#dfe6ec" font-family="ui-sans-serif,system-ui,sans-serif" font-size="20"
         font-weight="700" text-anchor="middle">
        <text x="0" y="-39">X</text>
        <text x="46" y="7">A</text>
        <text x="-46" y="7">Y</text>
        <text x="0" y="53">B</text>
      </g>
    </g>

    <!-- ================= start / select pills ================= -->
    <g transform="translate(590 640) rotate(-16)">
      <rect x="-96" y="-14" width="72" height="26" rx="13" fill="#39424c"/>
      <rect x="24"  y="-14" width="72" height="26" rx="13" fill="#39424c"/>
      <g fill="#98a3af" font-family="ui-sans-serif,system-ui,sans-serif" font-size="12"
         letter-spacing="2" text-anchor="middle">
        <text x="-60" y="4">SELECT</text>
        <text x="60"  y="4">START</text>
      </g>
    </g>

    <!-- ================= shoulder hints ================= -->
    <g fill="#4c5661">
      <rect x="96"  y="46" width="150" height="20" rx="10"/>
      <rect x="934" y="46" width="150" height="20" rx="10"/>
    </g>
    <g fill="#c7d0da" font-family="ui-sans-serif,system-ui,sans-serif" font-size="13"
       letter-spacing="3" text-anchor="middle">
      <text x="171" y="60">L</text>
      <text x="1009" y="60">R</text>
    </g>

    <!-- speaker grille -->
    <g transform="translate(1046 596) rotate(-24)" fill="#78828e" opacity=".8">
      <rect x="0"  y="0" width="8" height="64" rx="4"/>
      <rect x="16" y="0" width="8" height="64" rx="4"/>
      <rect x="32" y="0" width="8" height="64" rx="4"/>
      <rect x="48" y="0" width="8" height="64" rx="4"/>
      <rect x="64" y="0" width="8" height="64" rx="4"/>
    </g>
    `;
  }

  /* The CSS rectangle (as a fraction of the SVG box) where the canvas goes. */
  function wellStyle(svgRect) {
    return {
      left: (WELL.x / VB_W) * 100 + "%",
      top: (WELL.y / VB_H) * 100 + "%",
      width: (WELL.w / VB_W) * 100 + "%",
      height: (WELL.h / VB_H) * 100 + "%",
    };
  }

  function mount(svgEl, wellEl) {
    svgEl.innerHTML = shellSVG();
    const st = wellStyle();
    wellEl.style.position = "absolute";
    wellEl.style.left = st.left;
    wellEl.style.top = st.top;
    wellEl.style.width = st.width;
    wellEl.style.height = st.height;
  }

  return { VB_W, VB_H, WELL, mount, wellStyle };
})();

window.Bezel = Bezel;
