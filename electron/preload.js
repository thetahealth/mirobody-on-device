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
  getStatus: () => ipcRenderer.invoke('ondevice:status'),
  startDownload: () => ipcRenderer.send('ondevice:download'),
  cancelDownload: () => ipcRenderer.send('ondevice:download:cancel'),
  deleteModel: () => ipcRenderer.invoke('ondevice:delete'),
  // Subscribe to download progress ({status, progress|error}); returns an unsubscribe.
  onDownloadProgress: (cb) => {
    progressCbs.push(cb);
    return () => { progressCbs = progressCbs.filter((x) => x !== cb); };
  },
  // Stream a local turn. `history` is [{role,content}]; onmessage receives SSE-style
  // chunk strings ({"type":"reply","content":...}); completion/errors come via the
  // other callbacks. Returns { cancel }.
  generate: (history, onmessage, oncomplete, onerror) => {
    const id = ++genSeq;
    gens.set(id, { onmessage, oncomplete, onerror });
    ipcRenderer.send('ondevice:generate', { id, history });
    return { cancel: () => { gens.delete(id); ipcRenderer.send('ondevice:cancel', { id }); } };
  },
});
