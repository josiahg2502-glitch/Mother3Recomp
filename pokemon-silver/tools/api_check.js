#!/usr/bin/env node
/*
 * api_check.js -- static cross-reference of the web front-end.
 *
 * There is no browser in CI, so a typo in a cross-module call (Video.opts vs
 * Video.options, States.save vs States.saveState) would only surface at
 * runtime in the user's tab.  This walks every module's exported surface and
 * every `Module.member` reference in the front-end, and fails on anything
 * unresolved.  It also checks every Core.ex.<fn> against the real wasm export
 * table so the two halves cannot drift apart.
 *
 *   node tools/api_check.js            (from the pokemon-silver directory)
 */
"use strict";
const fs = require("fs");
const path = require("path");

const ROOT = path.join(__dirname, "..");
const DIR = path.join(ROOT, "web", "js");
const WASM = path.join(ROOT, "web", "wasm", "pksilver_core.wasm");

const files = fs.readdirSync(DIR).filter((f) => f.endsWith(".js"));
const srcs = {};
for (const f of files) srcs[f] = fs.readFileSync(path.join(DIR, f), "utf8");

/* ---- which file defines which global ------------------------------- */
const fileOf = {};      // global name -> file
for (const f of files) {
  for (const m of srcs[f].matchAll(/window\.([A-Za-z_][A-Za-z0-9_]*)\s*=/g)) {
    fileOf[m[1]] = f;
  }
}

/* ---- exported keys of a module: last brace-matched `return { ... }` -- */
function exportKeys(src) {
  const idx = src.lastIndexOf("return {");
  if (idx < 0) return new Set();
  const start = src.indexOf("{", idx);
  let depth = 0, end = -1;
  for (let j = start; j < src.length; j++) {
    const c = src[j];
    if (c === "{") depth++;
    else if (c === "}") { depth--; if (depth === 0) { end = j; break; } }
  }
  if (end < 0) return new Set();

  let body = src.slice(start + 1, end);
  /* a key written as `poke(ptr, bytes) {` may be preceded by a block comment,
   * which would otherwise defeat the name regex -- strip comments first.    */
  body = body.replace(/\/\*[\s\S]*?\*\//g, " ").replace(/^\s*\/\/.*$/gm, " ");
  const parts = [];
  let d = 0, cur = "";
  for (const c of body) {
    if ("{([".includes(c)) d++;
    if ("})]".includes(c)) d--;
    if (c === "," && d === 0) { parts.push(cur); cur = ""; }
    else cur += c;
  }
  parts.push(cur);

  const keys = new Set();
  for (const p of parts) {
    const t = p.trim();
    if (!t) continue;
    const m = t.match(/^get\s+([A-Za-z_$][A-Za-z0-9_$]*)/) ||
              t.match(/^set\s+([A-Za-z_$][A-Za-z0-9_$]*)/) ||
              t.match(/^([A-Za-z_$][A-Za-z0-9_$]*)\s*[:(]/) ||
              t.match(/^([A-Za-z_$][A-Za-z0-9_$]*)$/);
    if (m) keys.add(m[1]);
  }
  return keys;
}

const keys = {};
for (const g of Object.keys(fileOf)) keys[g] = exportKeys(srcs[fileOf[g]]);

/* ---- every `Global.member` reference in the front-end --------------- */
const bad = [];
for (const f of files) {
  for (const m of srcs[f].matchAll(/\b([A-Z][A-Za-z0-9_]*)\.([A-Za-z_$][A-Za-z0-9_$]*)/g)) {
    const [, mod, prop] = m;
    if (!keys[mod]) continue;                 // not one of our modules
    if (keys[mod].size === 0) continue;       // could not parse: skip
    if (!keys[mod].has(prop)) bad.push(`${f} -> ${mod}.${prop}`);
  }
}
// index.html also calls into these modules
const html = fs.readFileSync(path.join(ROOT, "web", "index.html"), "utf8");
for (const m of html.matchAll(/\b([A-Z][A-Za-z0-9_]*)\.([A-Za-z_$][A-Za-z0-9_$]*)/g)) {
  const [, mod, prop] = m;
  if (keys[mod] && keys[mod].size && !keys[mod].has(prop)) bad.push(`index.html -> ${mod}.${prop}`);
}

/* ---- Core.ex.<fn> against the real wasm export table ---------------- */
async function main() {
  const wasm = await WebAssembly.instantiate(fs.readFileSync(WASM), {});
  const ex = new Set(Object.keys(wasm.instance.exports));

  const usedCore = new Set();
  for (const f of files) {
    for (const m of srcs[f].matchAll(/\bCore\.ex\.([A-Za-z_0-9]+)/g)) usedCore.add(m[1]);
    for (const m of srcs[f].matchAll(/\bex\.([A-Za-z_0-9]+)\s*\(/g)) usedCore.add(m[1]);
  }
  const missWasm = [...usedCore].filter((n) => !ex.has(n)).sort();

  let fail = 0;
  console.log("modules:");
  for (const g of Object.keys(keys).sort())
    console.log(`  ${g.padEnd(9)} ${[...keys[g]].sort().join(", ") || "(unparsed)"}`);

  console.log(`\ncore functions referenced by JS: ${usedCore.size}`);
  if (missWasm.length) { console.log("  MISSING from wasm: " + missWasm.join(", ")); fail = 1; }
  else console.log("  all present in wasm exports");

  const uniq = [...new Set(bad)].sort();
  console.log(`\nunresolved cross-module members: ${uniq.length}`);
  if (uniq.length) { console.log("  " + uniq.join("\n  ")); fail = 1; }

  console.log("\n== RESULT:", fail ? "FAILURES" : "ALL PASS", "==");
  process.exit(fail);
}
main();
