/* ==========================================================================
 *  audio.js -- WebAudio output.  Pulls 16-bit stereo frames from the core's
 *              enhanced-DSP path into an AudioContext.
 * ========================================================================== */
"use strict";

const AudioOut = (() => {
  let ctx = null;
  let node = null;
  let running = false;
  const BUFFER = 2048;

  function start(sampleRateHint) {
    if (running) return;
    const AC = window.AudioContext || window.webkitAudioContext;
    if (!AC) return;
    ctx = new AC({ latencyHint: "interactive" });
    Core.ex.gb_set_sample_rate(ctx.sampleRate);

    node = ctx.createScriptProcessor(BUFFER, 2, 2);
    const ptr = Core.ex.gb_js_audio_buf();
    node.onaudioprocess = (ev) => {
      const outL = ev.outputBuffer.getChannelData(0);
      const outR = ev.outputBuffer.getChannelData(1);
      const want = ev.outputBuffer.length;
      const got = Core.ex.gb_js_render_audio(want);
      const i16 = Core.i16;
      const base = ptr >> 1;
      for (let i = 0; i < want; i++) {
        if (i < got) {
          outL[i] = i16[base + i * 2] / 32768;
          outR[i] = i16[base + i * 2 + 1] / 32768;
        } else {
          outL[i] = 0;
          outR[i] = 0;
        }
      }
    };
    node.connect(ctx.destination);
    running = true;
  }

  function resume() { if (ctx && ctx.state === "suspended") ctx.resume(); }
  function suspend() { if (ctx && ctx.state === "running") ctx.suspend(); }
  function stop() {
    if (node) { node.onaudioprocess = null; node.disconnect(); node = null; }
    if (ctx) { ctx.close(); ctx = null; }
    running = false;
  }

  return { start, resume, suspend, stop, get running() { return running; } };
})();

window.AudioOut = AudioOut;
