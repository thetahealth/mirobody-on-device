// On-device private LLM for the desktop app: any GGUF via node-llama-cpp, running
// in the MAIN process (the renderer is sandboxed with no Node access). Exposed to the
// renderer through the preload bridge (window.ondevice) and wired in main.js. The
// desktop analogue of the Qt client's ModelDownloader + locallmengine.
//
// A user-managed registry of models (each a remote download or a local file); which
// one is used is chosen in the provider picker. Persisted to userData/models.json and
// seeded on first run with a curated default. node-llama-cpp handles each model's chat
// template internally, so any GGUF family works (Gemma, Qwen, Llama, ...).
//
// node-llama-cpp is ESM-only and pulls a native binary, so it is loaded lazily via
// dynamic import(); if it isn't installed the module degrades gracefully rather than
// crashing main.
const fs = require('fs');
const path = require('path');
const { app } = require('electron');

// Curated, verified GGUFs (stable ggml-org / Qwen repos) offered as one-click adds.
// Same set as the Qt client's suggestions().
const SUGGESTIONS = [
  { name: 'Gemma 4 E4B (Q4_0)',           uri: 'hf:ggml-org/gemma-4-E4B-it-GGUF/gemma-4-E4B-it-Q4_0.gguf',            size: '4.6 GB' },
  { name: 'Gemma 4 E2B (Q4_0)',           uri: 'hf:ggml-org/gemma-4-E2B-it-GGUF/gemma-4-E2B-it-Q4_0.gguf',            size: '2.8 GB' },
  { name: 'Qwen2.5 3B Instruct (Q4_0)',   uri: 'hf:Qwen/Qwen2.5-3B-Instruct-GGUF/qwen2.5-3b-instruct-q4_0.gguf',      size: '2.0 GB' },
  { name: 'Qwen2.5 1.5B Instruct (Q4_0)', uri: 'hf:Qwen/Qwen2.5-1.5B-Instruct-GGUF/qwen2.5-1.5b-instruct-q4_0.gguf',  size: '1.1 GB' },
];
const DEFAULT_SUGGESTION = SUGGESTIONS[0];   // seeded on first run

let _mod = null;               // cached node-llama-cpp module
let _llama = null;             // cached llama instance
let _model = null;             // cached loaded model (keyed by __path)
const _active = new Map();     // generation id -> { controller, context }
let _downloader = null;        // in-flight ModelDownloader
let _downloadingName = '';     // entry name currently downloading ('' = none)
let _entries = null;           // [{ name, uri, file, path, remote }]

function modelsDir() {
  const dir = path.join(app.getPath('userData'), 'models');
  fs.mkdirSync(dir, { recursive: true });
  return dir;
}
function registryPath() { return path.join(app.getPath('userData'), 'models.json'); }

// The file name a model URI resolves to (last path segment, minus any query).
function fileFromUri(uri) {
  const clean = String(uri).split('?')[0];
  const seg = clean.slice(clean.lastIndexOf('/') + 1);
  return seg || 'model.gguf';
}

function load() {
  if (_entries) return;
  _entries = [];
  try {
    const j = JSON.parse(fs.readFileSync(registryPath(), 'utf8'));
    if (j && j.entries instanceof Array) {
      _entries = j.entries.filter((e) => e && e.name);
    }
  } catch (e) { /* first run / unreadable → empty */ }
  if (_entries.length === 0) {
    const s = DEFAULT_SUGGESTION;
    _entries.push({ name: fileBaseName(s.uri), uri: s.uri, file: fileFromUri(s.uri),
                    path: path.join(modelsDir(), fileFromUri(s.uri)), remote: true });
    save();
  }
}
function fileBaseName(uri) { return fileFromUri(uri).replace(/\.gguf$/i, ''); }
function save() {
  try { fs.writeFileSync(registryPath(), JSON.stringify({ entries: _entries }, null, 2)); }
  catch (e) { /* best-effort */ }
}
function find(name) { load(); return _entries.find((e) => e.name === name) || null; }
function uniqueName(base) {
  load();
  let b = String(base || '').trim() || 'model';
  let name = b; let n = 2;
  while (_entries.some((e) => e.name === name)) name = b + ' (' + (n++) + ')';
  return name;
}

async function mod() {
  if (_mod) return _mod;
  _mod = await import('node-llama-cpp');
  return _mod;
}

function statusOf(e) {
  if (e.name === _downloadingName) return 'downloading';
  try { return fs.statSync(e.path).size > 0 ? 'ready' : 'absent'; } catch (x) { return 'absent'; }
}
function isReady(name) { const e = find(name); return !!e && statusOf(e) === 'ready'; }
function pathFor(name) { const e = find(name); return e ? e.path : ''; }

function models() {
  load();
  return _entries.map((e) => ({ name: e.name, status: statusOf(e), remote: !!e.remote }));
}
function suggestions() {
  load();
  return SUGGESTIONS
    .filter((s) => !_entries.some((e) => e.uri === s.uri))
    .map((s) => ({ name: s.name, uri: s.uri, label: s.name + ' · ' + s.size }));
}

function addLocal(name, filePath) {
  load();
  try { if (!filePath || fs.statSync(filePath).size <= 0) return; } catch (e) { return; }
  const e = { name: uniqueName(name || path.basename(filePath).replace(/\.gguf$/i, '')),
              uri: '', file: path.basename(filePath), path: filePath, remote: false };
  _entries.push(e); save();
}
function addRemote(name, uri) {
  load();
  const u = String(uri || '').trim();
  if (!u) return;
  const file = fileFromUri(u);
  const e = { name: uniqueName(name || file.replace(/\.gguf$/i, '')),
              uri: u, file, path: path.join(modelsDir(), file), remote: true };
  _entries.push(e); save();
}
function remove(name) {
  load();
  const e = find(name);
  if (!e) return;
  if (_downloadingName === name) { cancelDownload(); }
  if (e.remote) { try { fs.unlinkSync(e.path); } catch (x) { /* gone */ } }  // never delete a local file
  _entries = _entries.filter((x) => x.name !== name);
  save();
}

// Download a remote entry. onProgress({name, status, progress|error}). One at a time.
async function download(name, onProgress) {
  const e = find(name);
  if (!e || !e.remote || _downloadingName) return;
  if (statusOf(e) === 'ready') { onProgress({ name, status: 'ready', progress: 1 }); return; }
  _downloadingName = name;
  onProgress({ name, status: 'downloading', progress: 0 });
  try {
    const { createModelDownloader } = await mod();
    _downloader = await createModelDownloader({
      modelUri: e.uri.split('?')[0],
      dirPath: modelsDir(),
      onProgress: ({ totalSize, downloadedSize }) => {
        onProgress({ name, status: 'downloading', progress: totalSize ? downloadedSize / totalSize : 0 });
      },
    });
    const out = await _downloader.download();
    // node-llama-cpp may name the file from the URI; trust the entry path, but if the
    // downloader reports a different path, adopt it so status/generate find the file.
    if (out && typeof out === 'string' && out !== e.path) { e.path = out; e.file = path.basename(out); save(); }
    _downloader = null; _downloadingName = '';
    onProgress({ name, status: 'ready', progress: 1 });
  } catch (err) {
    _downloader = null; _downloadingName = '';
    onProgress({ name, status: 'failed', error: err && err.message ? err.message : String(err) });
  }
}
async function cancelDownload() {
  if (_downloader && _downloader.cancel) { try { await _downloader.cancel(); } catch (e) { /* ignore */ } }
  _downloader = null; _downloadingName = '';
}

function systemPrompt(history) {
  const base = "You are Mirobody's private on-device health assistant. Answer concisely. "
    + 'You have no internet or tools; rely only on the conversation.';
  const prior = history.slice(0, -1);
  if (!prior.length) return base;
  const transcript = prior
    .map((m) => (m.role === 'user' ? 'User: ' : 'Assistant: ') + (m.content || ''))
    .join('\n');
  return base + '\n\nConversation so far:\n' + transcript;
}
function lastUserContent(history) {
  for (let i = history.length - 1; i >= 0; i--) {
    if (history[i] && history[i].role === 'user') return history[i].content || '';
  }
  return '';
}

// Stream a reply from `model` (registry entry name). cb = { onChunk, onDone, onError }.
async function generate(id, model, history, cb) {
  const mp = pathFor(model);
  if (!mp || !isReady(model)) { cb.onError('On-device model not ready.'); return; }
  const question = lastUserContent(history);
  if (!question) { cb.onDone(); return; }

  let context = null;
  try {
    const { getLlama, LlamaChatSession } = await mod();
    if (!_llama) _llama = await getLlama();
    if (!_model || _model.__path !== mp) {
      if (_model) { try { await _model.dispose(); } catch (e) { /* ignore */ } }
      _model = await _llama.loadModel({ modelPath: mp });
      _model.__path = mp;
    }
    context = await _model.createContext();
    const session = new LlamaChatSession({
      contextSequence: context.getSequence(),
      systemPrompt: systemPrompt(history),
    });
    const controller = new AbortController();
    _active.set(id, { controller, context });
    await session.prompt(question, {
      signal: controller.signal,
      onTextChunk: (chunk) => { if (chunk) cb.onChunk(chunk); },
    });
    cb.onDone();
  } catch (e) {
    if (e && (e.name === 'AbortError' || e.name === 'DOMException')) cb.onDone();
    else cb.onError(e && e.message ? e.message : String(e));
  } finally {
    _active.delete(id);
    if (context) { try { context.dispose(); } catch (e) { /* ignore */ } }
  }
}
function cancel(id) {
  const a = _active.get(id);
  if (a && a.controller) { try { a.controller.abort(); } catch (e) { /* ignore */ } }
}

module.exports = {
  models, suggestions, addLocal, addRemote, remove,
  download, cancelDownload, generate, cancel, isReady, pathFor,
};
