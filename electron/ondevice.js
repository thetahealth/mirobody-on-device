// On-device private LLM for the desktop app: Gemma 4 (GGUF) via node-llama-cpp,
// running in the MAIN process (the renderer is sandboxed with no Node access).
// Exposed to the renderer through the preload bridge (window.ondevice) and wired
// in main.js. The desktop analogue of Android/iOS LiteRtLlmEngine + ModelManager.
//
// node-llama-cpp is ESM-only and pulls a native binary, so it is loaded lazily via
// dynamic import(); if it isn't installed the module degrades gracefully (status
// "unavailable", generation reports a friendly error) rather than crashing main.
const fs = require('fs');
const path = require('path');
const { app } = require('electron');

// Gemma 4 (edge-tuned) in GGUF for llama.cpp. node-llama-cpp resolves `hf:` URIs to a
// Hugging Face download with resume + progress. CONFIRM this repo/file path against a
// real Gemma 4 GGUF release before shipping (quant/repo names vary by publisher).
const MODEL_URI = 'hf:ggml-org/gemma-4-E4B-it-GGUF/gemma-4-E4B-it-Q4_K_M.gguf';
const MODEL_FILE = MODEL_URI.slice(MODEL_URI.lastIndexOf('/') + 1);

let _mod = null;            // cached node-llama-cpp module
let _llama = null;          // cached llama instance
let _model = null;          // cached loaded model
const _active = new Map();  // generation id -> { controller, context }
let _downloader = null;     // in-flight ModelDownloader
let _status = 'absent';     // absent | downloading | ready | failed | unavailable

function modelsDir() {
  const dir = path.join(app.getPath('userData'), 'models');
  fs.mkdirSync(dir, { recursive: true });
  return dir;
}
function modelPath() { return path.join(modelsDir(), MODEL_FILE); }
function isReady() {
  try { return fs.statSync(modelPath()).size > 0; } catch (e) { return false; }
}

async function mod() {
  if (_mod) return _mod;
  // eslint-disable-next-line no-eval -- keep this a true dynamic import under CommonJS
  _mod = await import('node-llama-cpp');
  return _mod;
}

function status() {
  if (_status === 'downloading' || _status === 'failed') return { status: _status };
  return { status: isReady() ? 'ready' : 'absent' };
}

// Download the GGUF to userData/models with progress. onProgress({status, progress}).
async function download(onProgress) {
  if (isReady()) { _status = 'ready'; onProgress({ status: 'ready', progress: 1 }); return; }
  if (_status === 'downloading') return;
  _status = 'downloading';
  try {
    const { createModelDownloader } = await mod();
    _downloader = await createModelDownloader({
      modelUri: MODEL_URI,
      dirPath: modelsDir(),
      onProgress: ({ totalSize, downloadedSize }) => {
        onProgress({ status: 'downloading', progress: totalSize ? downloadedSize / totalSize : 0 });
      },
    });
    await _downloader.download();
    _downloader = null;
    _status = 'ready';
    onProgress({ status: 'ready', progress: 1 });
  } catch (e) {
    _downloader = null;
    // Distinguish "library missing" from a transfer failure for a clearer message.
    _status = 'failed';
    onProgress({ status: 'failed', error: e && e.message ? e.message : String(e) });
  }
}

async function cancelDownload() {
  if (_downloader && _downloader.cancel) { try { await _downloader.cancel(); } catch (e) { /* ignore */ } }
  _downloader = null;
  _status = isReady() ? 'ready' : 'absent';
}

function remove() {
  try { fs.unlinkSync(modelPath()); } catch (e) { /* already gone */ }
  _status = 'absent';
  return { status: 'absent' };
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

// Stream a reply. cb = { onChunk(text), onDone(), onError(reason) }. Emits text
// deltas; the caller wraps them into the SSE-style reply chunks the renderer expects.
async function generate(id, history, cb) {
  if (!isReady()) { cb.onError('On-device model not downloaded yet.'); return; }
  const question = lastUserContent(history);
  if (!question) { cb.onDone(); return; }

  let context = null;
  try {
    const { getLlama, LlamaChatSession } = await mod();
    if (!_llama) _llama = await getLlama();
    if (!_model || _model.__path !== modelPath()) {
      _model = await _llama.loadModel({ modelPath: modelPath() });
      _model.__path = modelPath();
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
    const a = _active.get(id);
    if (a) { _active.delete(id); }
    if (context) { try { context.dispose(); } catch (e) { /* ignore */ } }
  }
}

function cancel(id) {
  const a = _active.get(id);
  if (a && a.controller) { try { a.controller.abort(); } catch (e) { /* ignore */ } }
}

module.exports = { status, download, cancelDownload, remove, generate, cancel, modelPath };
