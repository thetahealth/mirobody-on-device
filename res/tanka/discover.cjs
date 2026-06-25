#!/usr/bin/env node
// Discover + verify Tanka's request signer (glue + WASM) from live Tanka. This is
// the single discovery engine, used two ways:
//   * the C++ server (src/user/tanka.cpp) runs it on boot + on a timer and adopts
//     the verified build in memory (self-healing when Tanka rebuilds) -- it reads
//     the JSON this prints to stdout;
//   * a developer runs it with --write to refresh the committed PINNED fallback:
//     it writes htdoc/static/tanka-signer.js + rewrites the kTankaWasmPath constant
//     in src/user/tanka.cpp (then `npm run build` in htdoc/ + rebuild the server).
//
// Verification is authoritative: a candidate is accepted ONLY if, after signing a
// real create-QR request with its WASM, Tanka answers code:0. Nothing is trusted
// on shape alone. Requires only Node (no extra packages).
//
// Usage:   node discover.cjs [base] [--write]   (base defaults to https://g.tanka.ai)
// Output:  on success, prints one JSON object {wasmUrl, nargs, signerJs} to stdout
//          and exits 0; with --write it also writes the pinned fallback to disk. On
//          failure (nothing verified / network error) prints nothing and exits 1.
//          All progress/diagnostics go to stderr so stdout stays pure JSON.

const https = require("node:https");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const { randomUUID } = require("node:crypto");

const DEFAULT_BASE = "https://g.tanka.ai";
const GATEWAY = "https://gw-q.tanka.ai";
const CREATE_PATH = "/npc/v2/common/qrcode/login";
// Canonical signer arg order (Tanka's caller), longest known form. A build taking
// fewer string args drops from the end; the self-test confirms the fit.
const CANON_ARGS = ["method", "url", "requestDataStr", "queryStr", "timestamp",
  "timezone", "deviceId", "deviceType", "userId", "orgId", "aiVersion"];

function log(...a) { console.error(...a); }

// GET a URL; resolve to a string (or Buffer when binary). Pins Origin g.tanka.ai
// (Tanka's CDN/gateway only accept it).
function httpGet(url, binary) {
  return new Promise((resolve, reject) => {
    const req = https.get(url, { headers: { Origin: DEFAULT_BASE } }, (res) => {
      const chunks = [];
      res.on("data", (c) => chunks.push(c));
      res.on("end", () => {
        const buf = Buffer.concat(chunks);
        resolve(binary ? buf : buf.toString("utf8"));
      });
    });
    req.on("error", reject);
    req.setTimeout(20000, () => req.destroy(new Error("timeout")));
  });
}

function braceMatch(s, openIdx) {
  let depth = 0;
  for (let j = openIdx; j < s.length; j++) {
    if (s[j] === "{") depth++;
    else if (s[j] === "}") { depth--; if (depth === 0) return s.slice(openIdx, j + 1); }
  }
  throw new Error("unbalanced braces");
}

// All chunk URLs on the site that export get_http_header, as [name, text] pairs.
async function discoverCandidates(base) {
  const html = await httpGet(base + "/login");
  const rtM = html.match(/src=([^ >"']*runtime[^ >"']*\.js)/);
  if (!rtM) throw new Error("runtime chunk not found");
  const rt = rtM[1].split("?")[0];
  const runtime = await httpGet(base + (rt.startsWith("/") ? "" : "/") + rt);

  const names = new Set();
  for (const m of runtime.matchAll(/"([A-Za-z0-9_]+\.[0-9a-f]{8,}\.(?:bundle\.)?js)"/g)) names.add(m[1]);
  for (const m of html.matchAll(/src=\/([A-Za-z0-9_]+\.[0-9a-f]{8,}\.(?:bundle\.)?js)/g)) names.add(m[1]);
  for (const m of runtime.matchAll(/(\d{2,}):"([0-9a-f]{8,})"/g)) names.add(m[1] + "." + m[2] + ".chunk.js");

  const hits = [];
  const list = Array.from(names).sort();
  // Probe in small batches to bound concurrency.
  for (let i = 0; i < list.length; i += 16) {
    const batch = list.slice(i, i + 16);
    const texts = await Promise.all(batch.map((n) =>
      httpGet(base + "/" + n).catch(() => null)));
    batch.forEach((n, k) => {
      const t = texts[k];
      if (t && t.includes("get_http_header") && t.includes("e.v(")) hits.push([n, t]);
    });
  }
  return hits;
}

// From a signer chunk, pull {glueBody, importMap, wasmHash, http, ws, init, nargs}
// or null if it doesn't fit the expected wasm-bindgen shape.
function extract(text) {
  const m = text.match(/var (\w+)=e\((\d+)\);\w+\.exports=e\.v\(\w+,\w+\.id,"([0-9a-f]{12,})",/);
  if (!m) return null;
  const glueId = m[2], wasmHash = m[3];
  const importMap = braceMatch(text, text.indexOf("{", m.index + m[0].length - 1));
  const gm = text.match(new RegExp("\\b" + glueId + "\\(n,_,e\\)\\{"));
  if (!gm) return null;
  const glueBody = braceMatch(text, gm.index + gm[0].length - 1);
  const http = text.match(/get_http_header:\(\)=>(\w+)\.(\w+)/);
  const init = text.match(/\(0,(\w+)\.(\w+)\)\((\w+)\),\3\.__wbindgen_start/);
  if (!http || !init) return null;
  const ws = text.match(/get_ws_params:\(\)=>(\w+)\.(\w+)/);
  // string-arg count: the glue maps the export name to a local wrapper fn whose
  // params are (N strings + 1 bool); count its params minus the bool.
  let nargs = null;
  const wrap = glueBody.match(new RegExp(http[2].replace(/[.*+?^${}()|[\]\\]/g, "\\$&") + ":\\(\\)=>(\\w+)"));
  if (wrap) {
    const fn = glueBody.match(new RegExp("function " + wrap[1] + "\\(([^)]*)\\)"));
    if (fn) nargs = fn[1].split(",").filter((p) => p.trim()).length - 1;
  }
  return { glueBody, importMap, wasmHash, http: http[2], ws: ws ? ws[2] : null, init: init[2], nargs };
}

// Standalone glue. CJS (esm=false) for the in-process self-test; ESM (esm=true)
// for the served htdoc/static/tanka-signer.js (async initSigner(wasmUrl)).
function buildGlue(info, esm) {
  const common =
    "\nconst req = () => ({});\n" +
    "req.o = (o,k)=>Object.prototype.hasOwnProperty.call(o,k);\n" +
    "req.d = (ex,def)=>{for(const k in def) if(req.o(def,k)&&!req.o(ex,k)) Object.defineProperty(ex,k,{enumerable:true,get:def[k]});};\n" +
    "req.r = ()=>{}; req.g = globalThis;\n" +
    "function glueModule(n,_,e) " + info.glueBody + "\n";
  if (esm) {
    return common +
      "\nexport async function initSigner(wasmUrl) {\n" +
      "  const mod = { exports: {} }; glueModule(mod, mod.exports, req);\n" +
      "  const t = mod.exports;\n" +
      "  const resp = await fetch(wasmUrl); const bytes = await resp.arrayBuffer();\n" +
      "  const imports = " + info.importMap + ";\n" +
      "  const { instance } = await WebAssembly.instantiate(bytes, imports);\n" +
      "  (0, t." + info.init + ")(instance.exports);\n" +
      "  if (instance.exports.__wbindgen_start) instance.exports.__wbindgen_start();\n" +
      "  return { get_http_header: t." + info.http +
      (info.ws ? ", get_ws_params: t." + info.ws : "") + " };\n" +
      "}\n";
  }
  return common +
    "\nconst mod = { exports: {} }; glueModule(mod, mod.exports, req);\n" +
    "const t = mod.exports;\n" +
    "const bytes = require('node:fs').readFileSync(require('node:path').join(__dirname, 's.wasm'));\n" +
    "const imports = " + info.importMap + ";\n" +
    "const instance = new WebAssembly.Instance(new WebAssembly.Module(bytes), imports);\n" +
    "(0, t." + info.init + ")(instance.exports);\n" +
    "if (instance.exports.__wbindgen_start) instance.exports.__wbindgen_start();\n" +
    "module.exports = { get_http_header: t." + info.http + " };\n";
}

// POST a signed create-QR built with this candidate and return Tanka's `code`.
function tankaCode(headers, body) {
  return new Promise((resolve) => {
    const u = new URL(GATEWAY + CREATE_PATH);
    const r = https.request({ hostname: u.hostname, path: u.pathname, method: "POST", headers },
      (res) => { let d = ""; res.on("data", (c) => d += c); res.on("end", () => {
        const m = d.match(/"code"\s*:\s*(-?\d+)/); resolve(m ? parseInt(m[1], 10) : null); }); });
    r.on("error", () => resolve(null));
    r.setTimeout(20000, () => { r.destroy(); resolve(null); });
    r.end(body);
  });
}

// Sign a create-QR with this build and POST to Tanka. Returns Tanka's code.
async function selfTest(base, info) {
  const wasm = await httpGet(base + "/" + info.wasmHash + ".module.wasm", true);
  if (!(wasm[0] === 0x00 && wasm[1] === 0x61 && wasm[2] === 0x73 && wasm[3] === 0x6d)) return null;
  // A FRESH temp dir per candidate: require() caches modules by resolved path, so
  // a shared glue.cjs path would make every candidate reuse the first one's WASM.
  const workdir = fs.mkdtempSync(path.join(os.tmpdir(), "tanka-disc-"));
  let get_http_header;
  try {
    fs.writeFileSync(path.join(workdir, "s.wasm"), wasm);
    const gluePath = path.join(workdir, "glue.cjs");
    fs.writeFileSync(gluePath, buildGlue(info, false));
    try { ({ get_http_header } = require(gluePath)); }
    catch (e) { log("    glue load failed:", e.message); return null; }
  } finally {
    try { fs.rmSync(workdir, { recursive: true, force: true }); } catch (e) { /* best-effort */ }
  }

  const n = info.nargs || CANON_ARGS.length;
  const dev = randomUUID();
  const reqBody = JSON.stringify({ qrCodeType: 0, deviceId: dev, deviceType: 7, createCodeStatus: 0 });
  const vals = {
    method: "POST", url: CREATE_PATH, requestDataStr: reqBody, queryStr: "",
    timestamp: String(Date.now()), timezone: "Asia/Shanghai", deviceId: dev,
    deviceType: "7", userId: "none", orgId: "none", aiVersion: "7",
  };
  const args = CANON_ARGS.slice(0, n).map((k) => vals[k]);
  let sig;
  try { sig = JSON.parse(get_http_header.apply(null, args.concat([false]))); }
  catch (e) { log("    sign failed:", e.message); return null; }
  const headers = {
    "Content-Type": "application/json", "Origin": base, "Appid": "E5kta86VM6",
    "Version": "3.64.1", "Aiversion": "7", "Timezone": "Asia/Shanghai", "Deviceid": dev,
    "Devicetype": "7", "Userid": "none", "Orgid": "none", "Timestamp": vals.timestamp,
    "Cookie": "deviceId=" + dev, "x-t-sign": sig["x-t-sign"], "x-t-nonce": sig["x-t-nonce"],
    "x-t-sign-ver": sig["x-t-sign-ver"], "x-t-return-sign-result": "true",
  };
  return tankaCode(headers, reqBody);
}

// Number of get_http_header string args the frontend (htdoc/src/tanka.js) passes;
// must match kTankaExpectedNargs in src/user/tanka.cpp. Used only for the --write
// warning when a discovered build's ABI differs.
const EXPECTED_NARGS = 11;

// --write: refresh the committed pinned fallback on disk (repo paths are relative
// to this script at res/tanka/).
function writeFallback(signerJs, wasmUrl, nargs) {
  const root = path.join(__dirname, "..", "..");
  const signerOut = path.join(root, "htdoc", "static", "tanka-signer.js");
  const tankaCpp = path.join(root, "src", "user", "tanka.cpp");
  fs.mkdirSync(path.dirname(signerOut), { recursive: true });
  fs.writeFileSync(signerOut, signerJs);

  const wasmPath = new URL(wasmUrl).pathname;   // "/<hash>.module.wasm"
  let updated = false;
  try {
    let src = fs.readFileSync(tankaCpp, "utf8");
    const re = /(kTankaWasmPath\s*=\s*")[^"]*(")/;
    if (re.test(src)) {
      fs.writeFileSync(tankaCpp, src.replace(re, "$1" + wasmPath + "$2"));
      updated = true;
    }
  } catch (e) { log("  could not update tanka.cpp:", e.message); }

  log("\n  WROTE pinned fallback:");
  log("  -", path.relative(root, signerOut));
  log("  - set kTankaWasmPath =", wasmPath,
      updated ? "in src/user/tanka.cpp" : "(NOT FOUND -- set the constant manually)");
  log("  Run `npm run build` in htdoc/ to bundle the new signer, then rebuild the server.");
  if (nargs != null && nargs !== EXPECTED_NARGS) {
    log("  *** get_http_header now takes " + nargs + " string args (was " + EXPECTED_NARGS +
        "). Update tanka.js's call AND kTankaExpectedNargs in src/user/tanka.cpp, then rebuild. ***");
  }
}

async function main() {
  const args = process.argv.slice(2);
  const write = args.includes("--write");
  const base = (args.find((a) => !a.startsWith("--")) || DEFAULT_BASE).replace(/\/+$/, "");
  log("Discovering signer chunks on", base, "...");
  const cands = await discoverCandidates(base);
  log("  found", cands.length, "candidate chunk(s) exporting get_http_header");
  for (const [name, text] of cands) {
    const info = extract(text);
    if (!info) { log("  -", name, "-> could not extract; skipping"); continue; }
    log("  -", name, "-> wasm", info.wasmHash, "| args", info.nargs, "-> self-test...");
    const code = await selfTest(base, info);
    if (code === 0) {
      log("    Tanka ACCEPTED (code 0).");
      const signerJs = "// AUTO-DISCOVERED from Tanka's web bundle; verified (create-QR accepted).\n" +
                       buildGlue(info, true);
      const wasmUrl = base + "/" + info.wasmHash + ".module.wasm";
      if (write) writeFallback(signerJs, wasmUrl, info.nargs);
      else process.stdout.write(JSON.stringify({ wasmUrl: wasmUrl, nargs: info.nargs, signerJs: signerJs }));
      return 0;
    }
    log("    Tanka rejected (code " + code + "); skipping.");
  }
  log("No candidate verified against Tanka.");
  return 1;
}

main().then((rc) => process.exit(rc)).catch((e) => { log("discover failed:", e); process.exit(1); });
