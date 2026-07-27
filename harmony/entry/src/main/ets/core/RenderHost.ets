/**
 * RenderHost — LaTeX and ECharts rendering through ONE hidden Web component
 * (resources/rawfile/render/). The chat list itself never holds a webview:
 * formulas render to SVG files, charts to PNG files, and the UI draws native
 * Images from file:// paths — so the list scrolls native and the files double
 * as a disk cache across launches.
 *
 * Why serialized payloads instead of screenshots: the host page hands SVG/PNG
 * back as STRINGS (window.renderTex / window.renderChart -> JSON), so nothing
 * depends on snapshotting a Web component's surface — historically the
 * unreliable part — and math stays resolution-independent.
 *
 * Flow: RichMessage asks lookupMath/lookupChart; a miss queues the job and
 * returns undefined (the block shows its source meanwhile). When the host
 * finishes, the result lands in the cache and AppStorage 'renderEpoch' bumps;
 * RichMessage watches it with @StorageProp, so the affected messages rebuild
 * and find their image. Failures are cached too — a formula MathJax rejects
 * (or an option ECharts throws on) would otherwise re-queue on every rebuild.
 */
import { webview } from '@kit.ArkWeb';
import fs from '@ohos.file.fs';
import { util } from '@kit.ArkTS';

interface RenderReply {
  ok: boolean;
  svg?: string;    // math: SVG text
  png?: string;    // chart: base64 PNG (no data: prefix)
  w?: number;
  h?: number;
}

export interface RenderedImage {
  ok: boolean;
  url: string;    // file:// path of the rendered SVG/PNG
  w: number;      // math: px at MathJax's base font (scale by fontScale); chart: logical px
  h: number;
}

const EPOCH_KEY = 'renderEpoch';
const CACHE_CAP = 200;

/** djb2 — cache filenames for arbitrary sources. */
function hashOf(s: string): string {
  let h = 5381;
  for (let i = 0; i < s.length; i++) {
    h = ((h << 5) + h + s.charCodeAt(i)) >>> 0;
  }
  return h.toString(16);
}

export class RenderHost {
  static readonly controller: webview.WebviewController = new webview.WebviewController();

  private static dir: string = '';
  private static ready: boolean = false;
  private static cache: Map<string, RenderedImage> = new Map<string, RenderedImage>();
  private static queued: Set<string> = new Set<string>();
  private static waiting: string[] = [];
  private static rendering: boolean = false;

  /** Call once with the app's filesDir before the host page loads. */
  static init(filesDir: string): void {
    if (RenderHost.dir.length > 0) {
      return;
    }
    RenderHost.dir = filesDir + '/render-cache';
    try {
      if (!fs.accessSync(RenderHost.dir)) {
        fs.mkdirSync(RenderHost.dir, true);
      }
    } catch (e) {
      // Renders will fail file writes and cache as failed; blocks fall back to
      // showing their raw source. Not fatal.
    }
  }

  /** Wire into the hidden Web component's onPageEnd. */
  static onHostReady(): void {
    RenderHost.ready = true;
    RenderHost.pump();
  }

  /**
   * Cached formula render, or undefined (then queued; watch 'renderEpoch').
   * `display` distinguishes $$ blocks from inline math — different layout.
   */
  static lookupMath(tex: string, display: boolean): RenderedImage | undefined {
    return RenderHost.lookup((display ? 'D:' : 'I:') + tex);
  }

  /** Cached chart render for an ECharts option JSON, or undefined (queued). */
  static lookupChart(optionJson: string): RenderedImage | undefined {
    return RenderHost.lookup('C:' + optionJson);
  }

  private static lookup(key: string): RenderedImage | undefined {
    const hit = RenderHost.cache.get(key);
    if (hit !== undefined) {
      return hit;
    }
    if (!RenderHost.queued.has(key)) {
      RenderHost.queued.add(key);
      RenderHost.waiting.push(key);
      RenderHost.pump();
    }
    return undefined;
  }

  /** One render at a time: the host page's document state is shared. */
  private static pump(): void {
    if (!RenderHost.ready || RenderHost.rendering) {
      return;
    }
    const key = RenderHost.waiting.shift();
    if (key === undefined) {
      return;
    }
    RenderHost.rendering = true;

    const payload = key.substring(2);
    let script: string;
    if (key.startsWith('C:')) {
      script = 'window.renderChart(' + JSON.stringify(payload) + ')';
    } else {
      script = 'window.renderTex(' + JSON.stringify(payload) + ',' +
               (key.startsWith('D:') ? 'true' : 'false') + ')';
    }

    RenderHost.controller.runJavaScript(script)
      .then((raw: string) => {
        RenderHost.store(key, raw);
      })
      .catch(() => {
        RenderHost.store(key, '');
      });
  }

  private static store(key: string, raw: string): void {
    let img: RenderedImage = { ok: false, url: '', w: 0, h: 0 };
    try {
      // runJavaScript returns the page's string result JSON-encoded — unwrap,
      // then parse the reply object itself.
      const unwrapped: string | RenderReply = raw.length > 0
        ? JSON.parse(raw) as string | RenderReply : '';
      const reply: RenderReply = typeof unwrapped === 'string'
        ? JSON.parse(unwrapped) as RenderReply
        : unwrapped;

      if (reply.ok && RenderHost.dir.length > 0) {
        let path = '';
        if (reply.svg !== undefined) {
          path = RenderHost.dir + '/' + hashOf(key) + '.svg';
          const f = fs.openSync(path, fs.OpenMode.CREATE | fs.OpenMode.READ_WRITE | fs.OpenMode.TRUNC);
          fs.writeSync(f.fd, reply.svg);
          fs.closeSync(f);
        } else if (reply.png !== undefined) {
          path = RenderHost.dir + '/' + hashOf(key) + '.png';
          const bytes = new util.Base64Helper().decodeSync(reply.png);
          const f = fs.openSync(path, fs.OpenMode.CREATE | fs.OpenMode.READ_WRITE | fs.OpenMode.TRUNC);
          fs.writeSync(f.fd, bytes.buffer as ArrayBuffer);
          fs.closeSync(f);
        }
        if (path.length > 0) {
          img = { ok: true, url: 'file://' + path, w: reply.w ?? 0, h: reply.h ?? 0 };
        }
      }
    } catch (e) {
      // fall through: cached as failed
    }

    if (RenderHost.cache.size >= CACHE_CAP) {
      // Drop the oldest entry (Map preserves insertion order).
      const first = RenderHost.cache.keys().next();
      if (!first.done) {
        RenderHost.cache.delete(first.value);
      }
    }
    RenderHost.cache.set(key, img);

    RenderHost.rendering = false;
    AppStorage.setOrCreate<number>(EPOCH_KEY, (AppStorage.get<number>(EPOCH_KEY) ?? 0) + 1);
    RenderHost.pump();
  }
}
