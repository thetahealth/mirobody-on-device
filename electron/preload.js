// Minimal context-isolated preload. The renderer is the existing web client
// (res/htdoc), which talks to the embedded server over HTTP and needs nothing
// from Node — so this only exposes a small marker the page can feature-detect
// (e.g. to show desktop-only UI). Extend via contextBridge if you later want to
// surface native capabilities to the renderer.
const { contextBridge } = require('electron');

contextBridge.exposeInMainWorld('mirobodyDesktop', {
  isDesktop: true,
  electronVersion: process.versions.electron,
});
