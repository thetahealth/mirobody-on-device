// Context-isolated preload. Exposes:
//  - window.mirobodyDesktop: a small marker the page feature-detects (desktop-only UI).
//  - window.ondevice: a bridge to the main-process on-device LLM (Gemma 4 via
//    node-llama-cpp). The renderer is sandboxed (no Node), so all model work happens
//    in main; this just relays a history and streams reply chunks back. The renderer
//    only shows the on-device provider when this bridge is present, so the browser
//    web client (no preload) is unaffected.
const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('mirobodyDesktop', {
  isDesktop: true,
  electronVersion: process.versions.electron,
});

// Active generations: id -> { onmessage, oncomplete, onerror }.
const gens = new Map();
let genSeq = 0;
let progressCbs = [];

ipcRenderer.on('ondevice:chunk', (_e, { id, data }) => {
  const g = gens.get(id);
  if (g && g.onmessage) g.onmessage(data);
});
ipcRenderer.on('ondevice:done', (_e, { id }) => {
  const g = gens.get(id);
  if (g) { gens.delete(id); if (g.oncomplete) g.oncomplete(); }
});
ipcRenderer.on('ondevice:error', (_e, { id, reason }) => {
  const g = gens.get(id);
  if (g) { gens.delete(id); if (g.onerror) g.onerror(reason); }
});
ipcRenderer.on('ondevice:download:progress', (_e, payload) => {
  progressCbs.forEach((cb) => { try { cb(payload); } catch (err) { /* ignore */ } });
});

contextBridge.exposeInMainWorld('ondevice', {
  available: true,
  // Model registry.
  models: () => ipcRenderer.invoke('ondevice:models'),            // -> [{name,status,remote}]
  suggestions: () => ipcRenderer.invoke('ondevice:suggestions'),  // -> [{name,uri,label}]
  isReady: (name) => ipcRenderer.invoke('ondevice:isReady', name),
  addRemote: (name, uri) => ipcRenderer.send('ondevice:addRemote', { name, uri }),
  addLocal: () => ipcRenderer.invoke('ondevice:addLocal'),        // native file picker -> bool
  remove: (name) => ipcRenderer.send('ondevice:remove', { name }),
  download: (name) => ipcRenderer.send('ondevice:download', { name }),
  cancelDownload: () => ipcRenderer.send('ondevice:download:cancel'),
  // Download progress ({name, status, progress|error}); returns an unsubscribe.
  onDownloadProgress: (cb) => {
    progressCbs.push(cb);
    return () => { progressCbs = progressCbs.filter((x) => x !== cb); };
  },
  // Stream a local turn from `model` (registry entry name). `history` is [{role,content}];
  // onmessage gets SSE-style chunk strings ({"type":"reply",...}); completion/errors via
  // the other callbacks. Returns { cancel }.
  generate: (model, history, onmessage, oncomplete, onerror) => {
    const id = ++genSeq;
    gens.set(id, { onmessage, oncomplete, onerror });
    ipcRenderer.send('ondevice:generate', { id, model, history });
    return { cancel: () => { gens.delete(id); ipcRenderer.send('ondevice:cancel', { id }); } };
  },
});
