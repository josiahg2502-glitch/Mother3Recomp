/* ==========================================================================
 *  video.js -- GPU presentation: uploads the 160x144 framebuffer and renders
 *              it with a selectable high-res upscaler, optional LCD pixel
 *              grid and scanlines.
 *
 *  The shaders are original GLSL written for this port.  The "sharp
 *  bilinear" path reproduces the classic trick of snapping the sample point
 *  to integer pixel centres so the picture stays crisp while edges blend;
 *  "pixel softening" rounds pixel corners for a CRT-ish bloom.
 * ========================================================================== */
"use strict";

const Video = (() => {
  const SRC_W = 160, SRC_H = 144;

  const VERT = `
    attribute vec2 aPos;
    varying vec2 vUv;
    void main() {
      vUv = aPos * 0.5 + 0.5;
      gl_Position = vec4(aPos, 0.0, 1.0);
    }`;

  const FRAG = `
    precision highp float;
    varying vec2 vUv;
    uniform sampler2D uTex;
    uniform vec2 uSrc;        /* 160 x 144 */
    uniform vec2 uOut;        /* destination size in px */
    uniform int  uFilter;     /* 0 nearest 1 sharp 2 smooth 3 soft */
    uniform float uGrid;      /* 0..1 LCD grid strength */
    uniform float uScan;      /* 0..1 scanline strength */

    vec2 sharpBilinear(vec2 uv, vec2 src) {
      vec2 p = uv * src - 0.5;
      vec2 i = floor(p);
      vec2 f = p - i;
      /* ease f toward the centre so edges stay hard */
      f = f * f * (3.0 - 2.0 * f);
      return (i + 0.5 + (f - 0.5) * 0.35) / src;
    }

    void main() {
      vec2 uv = vUv;
      vec3 col;

      if (uFilter == 0) {
        col = texture2D(uTex, uv).rgb;
      } else if (uFilter == 1) {
        col = texture2D(uTex, sharpBilinear(uv, uSrc)).rgb;
      } else if (uFilter == 2) {
        col = texture2D(uTex, uv).rgb;
      } else {
        /* pixel softening: bilinear plus a slight corner falloff */
        vec2 p = uv * uSrc;
        vec2 f = fract(p);
        vec2 c = clamp(min(f, 1.0 - f) * 6.0, 0.0, 1.0);
        float m = mix(1.0, c.x * c.y, 0.35);
        col = texture2D(uTex, uv).rgb * (0.72 + 0.28 * m);
      }

      /* LCD pixel grid: darken source-pixel boundaries */
      if (uGrid > 0.001) {
        vec2 g = fract(vUv * uSrc);
        vec2 edge = smoothstep(0.0, 0.14, g) * smoothstep(1.0, 0.86, g);
        float k = mix(1.0, edge.x * edge.y, uGrid * 0.5);
        col *= k;
      }

      /* scanlines across the whole output */
      if (uScan > 0.001) {
        float s = 0.5 + 0.5 * sin(vUv.y * uOut.y * 3.14159);
        col *= mix(1.0, 0.72 + 0.28 * s, uScan);
      }

      gl_FragColor = vec4(col, 1.0);
    }`;

  let gl = null, prog = null, tex = null, buf = null;
  let canvas = null;
  const opts = { filter: 1, grid: 1, scan: 0, intScale: 1 };

  function compile(type, src) {
    const sh = gl.createShader(type);
    gl.shaderSource(sh, src);
    gl.compileShader(sh);
    if (!gl.getShaderParameter(sh, gl.COMPILE_STATUS))
      throw new Error(gl.getShaderInfoLog(sh));
    return sh;
  }

  function attach(c) {
    canvas = c;
    gl = canvas.getContext("webgl", { antialias: false, alpha: false }) ||
         canvas.getContext("experimental-webgl");
    if (!gl) return false;

    prog = gl.createProgram();
    gl.attachShader(prog, compile(gl.VERTEX_SHADER, VERT));
    gl.attachShader(prog, compile(gl.FRAGMENT_SHADER, FRAG));
    gl.linkProgram(prog);
    if (!gl.getProgramParameter(prog, gl.LINK_STATUS))
      throw new Error(gl.getProgramInfoLog(prog));
    gl.useProgram(prog);

    buf = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, buf);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1,-1, 3,-1, -1,3]), gl.STATIC_DRAW);
    const loc = gl.getAttribLocation(prog, "aPos");
    gl.enableVertexAttribArray(loc);
    gl.vertexAttribPointer(loc, 2, gl.FLOAT, false, 0, 0);

    tex = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, tex);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    return true;
  }

  /* pick the destination size for the current mode */
  function fit(availW, availH) {
    const aspect = SRC_W / SRC_H;               /* 10:9 */
    let w = availW, h = w / aspect;
    if (h > availH) { h = availH; w = h * aspect; }
    if (opts.intScale) {
      const s = Math.max(1, Math.floor(Math.min(w / SRC_W, h / SRC_H)));
      w = SRC_W * s; h = SRC_H * s;
    }
    return [Math.floor(w), Math.floor(h)];
  }

  function present() {
    if (!gl) return;
    const px = Core.ex.gb_js_pixel_buf();
    Core.ex.gb_js_grab_frame();
    gl.bindTexture(gl.TEXTURE_2D, tex);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, SRC_W, SRC_H, 0, gl.RGBA,
                  gl.UNSIGNED_BYTE, Core.u8.subarray(px, px + SRC_W * SRC_H * 4));

    gl.viewport(0, 0, canvas.width, canvas.height);
    gl.useProgram(prog);
    gl.uniform2f(gl.getUniformLocation(prog, "uSrc"), SRC_W, SRC_H);
    gl.uniform2f(gl.getUniformLocation(prog, "uOut"), canvas.width, canvas.height);
    gl.uniform1i(gl.getUniformLocation(prog, "uFilter"), opts.filter);
    gl.uniform1f(gl.getUniformLocation(prog, "uGrid"), opts.grid);
    gl.uniform1f(gl.getUniformLocation(prog, "uScan"), opts.scan);
    gl.drawArrays(gl.TRIANGLES, 0, 3);
  }

  return {
    attach, present, fit, opts,
    SRC_W, SRC_H,
    get ok() { return !!gl; },
  };
})();

window.Video = Video;
