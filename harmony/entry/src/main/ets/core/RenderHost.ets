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
 * In MEMORY only, deliberately: a failure leaves no file, so the next launch
 * retries rather than inheriting a bad render forever.
 *
 * Two cache tiers, and the disk one is load-bearing. `cache` is this session's
 * map, capped; `disk` is an index of what previous launches already rendered,
 * built from ONE directory listing at init(). Without that index the files were
 * write-only — every cold start re-rendered the whole history through the
 * webview, one formula at a time (pump() is serialized on purpose), and so did
 * anything the memory cap had evicted. The dimensions ImageSpan needs cannot be
 * recovered from an SVG/PNG cheaply, so they ride in the FILE NAME
 * (<hash>-<w>x<h>.svg|png) and the listing alone rebuilds the whole index.
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
/**
 * Files kept on disk. Above the memory cap because the whole point of the disk tier
 * is to outlive both the session and an eviction; below anything that would make the
 * prune scan (which stats every file) worth worrying about.
 */
const DISK_CAP = 600;

/** One cache file and its age, for the prune sort. */
interface CacheFile {
  name: string;
  at: number;    // mtime, seconds
}

function pad8(n: number): string {
  const h = n.toString(16);
  return '00000000'.substring(h.length) + h;
}

/**
 * Is this SVG source one we are willing to hand to the rasterizer?
 *
 * VALIDATE AND REFUSE, never rewrite. The source is model-authored and untrusted,
 * and stripping dangerous constructs out of untrusted markup by text surgery is a
 * game of whack-a-mole against an adversary who writes the input. Refusing is
 * total, auditable, and costs a model nothing: a diagram needs none of this.
 *
 * ArkUI rasterizes SVG itself rather than through a browser, so there is no script
 * engine to attack. What is left is what this checks for:
 *
 *   external references  <image href="https://…"> fetches when the reply is read,
 *                        which is the read-receipt problem markdown images are
 *                        refused for. Fragment refs (#gradient) are fine and
 *                        common, so they stay allowed.
 *   DOCTYPE / ENTITY     entity expansion is a parser-level denial of service that
 *                        no renderer feature depends on.
 *   script, foreignObject, event handlers
 *                        inert here by construction, refused anyway — the cost is
 *                        three string searches and it stops this from silently
 *                        becoming unsafe if the SVG ever reaches a WebView.
 */
function svgIsSafe(source: string): boolean {
  const s = source.toLowerCase();
  if (s.indexOf('<script') >= 0 || s.indexOf('<foreignobject') >= 0
      || s.indexOf('<!doctype') >= 0 || s.indexOf('<!entity') >= 0
      || s.indexOf('<use') >= 0) {
    return false;
  }
  // Any on*= handler attribute. Checked as ' on…=' so "button-on=" and prose
  // containing "on" do not trip it.
  for (let i = 1; i < s.length - 3; i++) {
    const c = s.charAt(i - 1);
    if ((c === ' ' || c === '\t' || c === '\n') && s.charAt(i) === 'o' && s.charAt(i + 1) === 'n') {
      let j = i + 2;
      while (j < s.length && s.charAt(j) !== '=' && s.charAt(j) !== ' ' && s.charAt(j) !== '>') {
        j++;
      }
      if (j < s.length && s.charAt(j) === '=') {
        return false;
      }
    }
  }
  // href / src that is not a same-document fragment.
  return !hasExternalRef(s, 'href') && !hasExternalRef(s, 'src');
}

function hasExternalRef(lower: string, attr: string): boolean {
  let from = 0;
  while (true) {
    const at = lower.indexOf(attr + '=', from);
    if (at < 0) {
      return false;
    }
    let j = at + attr.length + 1;
    while (j < lower.length && (lower.charAt(j) === '"' || lower.charAt(j) === '\'')) {
      j++;
    }
    if (j < lower.length && lower.charAt(j) !== '#') {
      return true;
    }
    from = at + attr.length + 1;
  }
}

/**
 * The quoted value of an attribute, or ''.
 *
 * The name must start a real attribute — preceded by whitespace or `<` — because a
 * bare indexOf('width=') also matches `stroke-width="2"`, and a figure sized off a
 * stroke would be shaped by whichever rectangle happened to be drawn first.
 */
function attrValue(lower: string, source: string, attr: string): string {
  let from = 0;
  while (true) {
    const at = lower.indexOf(attr + '=', from);
    if (at < 0) {
      return '';
    }
    const before = at === 0 ? '<' : lower.charAt(at - 1);
    if (before === ' ' || before === '\t' || before === '\n' || before === '<') {
      const q = source.charAt(at + attr.length + 1);
      if (q === '"' || q === '\'') {
        const end = source.indexOf(q, at + attr.length + 2);
        if (end > 0) {
          return source.substring(at + attr.length + 2, end);
        }
      }
      return '';
    }
    from = at + attr.length + 1;
  }
}

/** An absolute CSS length -> px. A percentage (or anything unusable) -> 0. */
function plainPixels(v: string): number {
  const t = v.trim();
  if (t.length === 0 || t.endsWith('%')) {
    return 0;
  }
  const n = Number.parseFloat(t);
  return Number.isNaN(n) || n <= 0 ? 0 : Math.round(n);
}

/**
 * The figure's intrinsic size, for an aspect ratio.
 *
 * viewBox first, and width/height only as a fallback: a hand-written SVG reliably
 * carries a viewBox, while its width/height are as often absent or a percentage —
 * and a percentage says nothing about shape, so it counts as unknown rather than as
 * the number in front of the sign. {0,0} means unknown and the renderer defaults.
 */
function svgSize(source: string): RenderedImage {
  const lower = source.toLowerCase();

  const vb = attrValue(lower, source, 'viewbox');
  if (vb.length > 0) {
    const parts: string[] = [];
    let cur = '';
    for (let i = 0; i < vb.length; i++) {
      const c = vb.charAt(i);
      if (c === ' ' || c === ',' || c === '\t' || c === '\n') {
        if (cur.length > 0) {
          parts.push(cur);
          cur = '';
        }
      } else {
        cur += c;
      }
    }
    if (cur.length > 0) {
      parts.push(cur);
    }
    if (parts.length === 4) {
      const w = Number.parseFloat(parts[2]);
      const h = Number.parseFloat(parts[3]);
      if (!Number.isNaN(w) && !Number.isNaN(h) && w > 0 && h > 0) {
        return { ok: true, url: '', w: Math.round(w), h: Math.round(h) };
      }
    }
  }

  const w = plainPixels(attrValue(lower, source, 'width'));
  const h = plainPixels(attrValue(lower, source, 'height'));
  if (w > 0 && h > 0) {
    return { ok: true, url: '', w: w, h: h };
  }
  return { ok: true, url: '', w: 0, h: 0 };
}

/**
 * djb2 and sdbm concatenated — 64 bits of filename.
 *
 * One 32-bit hash was a live hazard rather than a theoretical one: two keys that
 * collide name the SAME file, the second render clobbers the first, and the first
 * message's ImageSpan then silently draws the second's formula. Two independent
 * mixes put that back under the noise floor, and the disk index makes the stakes
 * higher than they were — a wrong file now survives the launch that wrote it.
 */
function hashOf(s: string): string {
  let a = 5381;   // djb2
  let b = 0;      // sdbm
  for (let i = 0; i < s.length; i++) {
    const c = s.charCodeAt(i);
    a = ((a << 5) + a + c) >>> 0;
    b = (c + (b << 6) + (b << 16) - b) >>> 0;
  }
  return pad8(a) + pad8(b);
}

export class RenderHost {
  static readonly controller: webview.WebviewController = new webview.WebviewController();

  private static dir: string = '';
  private static ready: boolean = false;
  private static cache: Map<string, RenderedImage> = new Map<string, RenderedImage>();
  /** hash -> image already on disk, from the init() scan and this session's writes. */
  private static disk: Map<string, RenderedImage> = new Map<string, RenderedImage>();
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
        return;                     // first run: nothing to index
      }
    } catch (e) {
      // Renders will fail file writes and cache as failed; blocks fall back to
      // showing their raw source. Not fatal.
      return;
    }
    RenderHost.scan();
  }

  /**
   * Index what earlier launches left behind, and hold the directory to DISK_CAP.
   *
   * One listing, no file reads: everything lookup() needs is in the name. Anything
   * that does not parse is deleted rather than kept — it is either an older naming
   * scheme (whose dimensions are unrecoverable, so it can never be served) or not
   * ours, and this directory belongs to nothing else.
   */
  private static scan(): void {
    let names: string[] = [];
    try {
      names = fs.listFileSync(RenderHost.dir);
    } catch (e) {
      return;
    }
    const kept: string[] = [];
    for (const name of names) {
      const img = RenderHost.imageOf(name);
      if (img === undefined) {
        RenderHost.remove(name);
        continue;
      }
      RenderHost.disk.set(name.substring(0, name.indexOf('-')), img);
      kept.push(name);
    }
    RenderHost.prune(kept);
  }

  /** `<hash>-<w>x<h>.svg|png` -> the image it holds, or undefined if it is not one. */
  private static imageOf(name: string): RenderedImage | undefined {
    const dash = name.indexOf('-');
    const dot  = name.lastIndexOf('.');
    if (dash <= 0 || dot <= dash) {
      return undefined;
    }
    const dims = name.substring(dash + 1, dot);
    const x = dims.indexOf('x');
    if (x <= 0) {
      return undefined;
    }
    const w = Number.parseInt(dims.substring(0, x), 10);
    const h = Number.parseInt(dims.substring(x + 1), 10);
    if (Number.isNaN(w) || Number.isNaN(h)) {
      return undefined;
    }
    return { ok: true, url: 'file://' + RenderHost.dir + '/' + name, w: w, h: h };
  }

  /**
   * Oldest files out until the directory is back under DISK_CAP.
   *
   * Nothing else ever deletes: a memory eviction drops the Map entry but KEEPS the
   * file, which is exactly what makes the disk tier worth having, so this is the only
   * thing standing between a long-lived install and unbounded growth. The mtime stat
   * costs one syscall per file, and only on the launches that are actually over.
   */
  private static prune(names: string[]): void {
    if (names.length <= DISK_CAP) {
      return;
    }
    const aged: CacheFile[] = [];
    for (const name of names) {
      let at = 0;                   // unreadable: sorts oldest, so it goes first
      try {
        at = fs.statSync(RenderHost.dir + '/' + name).mtime;
      } catch (e) {
        // keep 0
      }
      aged.push({ name: name, at: at });
    }
    aged.sort((x: CacheFile, y: CacheFile) => x.at - y.at);
    const drop = aged.length - DISK_CAP;
    for (let i = 0; i < drop; i++) {
      const name = aged[i].name;
      RenderHost.remove(name);
      RenderHost.disk.delete(name.substring(0, name.indexOf('-')));
    }
  }

  private static remove(name: string): void {
    try {
      fs.unlinkSync(RenderHost.dir + '/' + name);
    } catch (e) {
      // Best effort — a file we cannot delete just stays and counts next time.
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
   *
   * The color mode is part of the key, not a re-render trigger: the ink is baked
   * into the SVG/PNG, so light and dark are two different images. Keying on the
   * mode means flipping the theme misses the cache, queues a re-render, and the
   * other mode's image stays cached for the flip back.
   */
  static lookupMath(tex: string, display: boolean, dark: boolean): RenderedImage | undefined {
    return RenderHost.lookup(RenderHost.keyOf(display ? 'D' : 'I', dark) + tex);
  }

  /** Cached chart render for an ECharts option JSON, or undefined (queued). */
  static lookupChart(optionJson: string, dark: boolean): RenderedImage | undefined {
    return RenderHost.lookup(RenderHost.keyOf('C', dark) + optionJson);
  }

  /**
   * Cached figure for a ```svg block's source, or undefined (queued).
   *
   * Colour-mode independent, unlike math and charts: an author-written SVG carries
   * its own colours, and there is nothing here that could recolour it honestly. So
   * it is cached once rather than twice, and `dark` is not part of the key.
   */
  static lookupSvg(source: string): RenderedImage | undefined {
    return RenderHost.lookup(RenderHost.keyOf('S', false) + source);
  }

  /**
   * The renderer could not draw a figure we accepted and wrote. Demote it so the
   * block falls back to showing its source.
   *
   * Validation (see svgSize/svgIsSafe) only proves the source holds nothing we
   * refuse to draw — it cannot prove ArkUI's rasterizer will succeed. Without this
   * a file that writes fine and draws blank leaves an empty hole in the reply,
   * which is worse than the XML it replaced.
   */
  static markSvgFailed(source: string): void {
    const key = RenderHost.keyOf('S', false) + source;
    const hash = hashOf(key);
    const hit = RenderHost.cache.get(key);
    if (hit !== undefined && !hit.ok) {
      return;                     // already demoted; do not loop on a re-render
    }
    RenderHost.disk.delete(hash);
    RenderHost.cache.set(key, { ok: false, url: '', w: 0, h: 0 });
    AppStorage.setOrCreate<number>(EPOCH_KEY, (AppStorage.get<number>(EPOCH_KEY) ?? 0) + 1);
  }

  // 3-char prefix: kind, mode, ':'. hashOf() covers the whole key, so the two
  // modes also land in different cache files.
  private static keyOf(kind: string, dark: boolean): string {
    return kind + (dark ? '1' : '0') + ':';
  }

  private static lookup(key: string): RenderedImage | undefined {
    const hit = RenderHost.cache.get(key);
    if (hit !== undefined) {
      return hit;
    }
    // Rendered by an earlier launch, or evicted from memory but never from disk.
    // Promoting it into `cache` keeps the hot path a single Map hit.
    const onDisk = RenderHost.disk.get(hashOf(key));
    if (onDisk !== undefined) {
      RenderHost.remember(key, onDisk);
      return onDisk;
    }
    if (!RenderHost.queued.has(key)) {
      RenderHost.queued.add(key);
      RenderHost.waiting.push(key);
      RenderHost.pump();
    }
    return undefined;
  }

  /** Session cache write, oldest out at the cap. The FILE stays — see prune(). */
  private static remember(key: string, img: RenderedImage): void {
    if (RenderHost.cache.size >= CACHE_CAP) {
      // Drop the oldest entry (Map preserves insertion order).
      const first = RenderHost.cache.keys().next();
      if (!first.done) {
        RenderHost.cache.delete(first.value);
      }
    }
    RenderHost.cache.set(key, img);
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

    const payload = key.substring(3);

    // A ```svg figure needs no renderer — the source IS the file. It still goes
    // through the queue rather than straight into lookup(), because lookup() runs
    // inside a build pass and a synchronous write there would put disk I/O on the
    // frame. setTimeout gets it off the build the way runJavaScript does for the
    // other two.
    if (key.charAt(0) === 'S') {
      setTimeout(() => {
        RenderHost.storeSvg(key, payload);
      }, 0);
      return;
    }

    const dark = key.charAt(1) === '1' ? 'true' : 'false';
    let script: string;
    if (key.charAt(0) === 'C') {
      script = 'window.renderChart(' + JSON.stringify(payload) + ',' + dark + ')';
    } else {
      script = 'window.renderTex(' + JSON.stringify(payload) + ',' +
               (key.charAt(0) === 'D' ? 'true' : 'false') + ',' + dark + ')';
    }

    RenderHost.controller.runJavaScript(script)
      .then((raw: string) => {
        RenderHost.store(key, raw);
      })
      .catch(() => {
        RenderHost.store(key, '');
      });
  }

  /**
   * Validate a ```svg body and write it as the figure's own file.
   *
   * Refused source caches as a failure, exactly like a formula MathJax rejects, so
   * the block shows its XML and the check never runs twice on the same source.
   */
  private static storeSvg(key: string, source: string): void {
    let img: RenderedImage = { ok: false, url: '', w: 0, h: 0 };
    if (svgIsSafe(source) && RenderHost.dir.length > 0) {
      try {
        const size = svgSize(source);
        const path = RenderHost.dir + '/' + hashOf(key) + '-' + size.w + 'x' + size.h + '.svg';
        const f = fs.openSync(path, fs.OpenMode.CREATE | fs.OpenMode.READ_WRITE | fs.OpenMode.TRUNC);
        fs.writeSync(f.fd, source);
        fs.closeSync(f);
        img = { ok: true, url: 'file://' + path, w: size.w, h: size.h };
        RenderHost.disk.set(hashOf(key), img);
      } catch (e) {
        // Unwritable: cached as failed, so the block shows its source.
      }
    }
    RenderHost.remember(key, img);
    RenderHost.rendering = false;
    AppStorage.setOrCreate<number>(EPOCH_KEY, (AppStorage.get<number>(EPOCH_KEY) ?? 0) + 1);
    RenderHost.pump();
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
        const w = reply.w ?? 0;
        const h = reply.h ?? 0;
        // Dimensions in the name, so init() can index this file next launch without
        // opening it. Same key -> same name, so a re-render overwrites rather than
        // orphaning what it replaces.
        const stem = RenderHost.dir + '/' + hashOf(key) + '-' + w + 'x' + h;
        let path = '';
        if (reply.svg !== undefined) {
          path = stem + '.svg';
          const f = fs.openSync(path, fs.OpenMode.CREATE | fs.OpenMode.READ_WRITE | fs.OpenMode.TRUNC);
          fs.writeSync(f.fd, reply.svg);
          fs.closeSync(f);
        } else if (reply.png !== undefined) {
          path = stem + '.png';
          const bytes = new util.Base64Helper().decodeSync(reply.png);
          const f = fs.openSync(path, fs.OpenMode.CREATE | fs.OpenMode.READ_WRITE | fs.OpenMode.TRUNC);
          fs.writeSync(f.fd, bytes.buffer as ArrayBuffer);
          fs.closeSync(f);
        }
        if (path.length > 0) {
          img = { ok: true, url: 'file://' + path, w: w, h: h };
          RenderHost.disk.set(hashOf(key), img);
        }
      }
    } catch (e) {
      // fall through: cached as failed
    }

    RenderHost.remember(key, img);

    RenderHost.rendering = false;
    AppStorage.setOrCreate<number>(EPOCH_KEY, (AppStorage.get<number>(EPOCH_KEY) ?? 0) + 1);
    RenderHost.pump();
  }
}
