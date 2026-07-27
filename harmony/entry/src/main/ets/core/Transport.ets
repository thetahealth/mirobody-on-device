/**
 * OpenAI-compatible streaming transport. One code path serves every `openai-chat`
 * provider — only baseUrl, model, and the key differ (docs/byok-providers.md, Request
 * contract). This is the same SSE shape a local Ollama/llama.cpp server exposes, so the
 * local and cloud lanes can share this transport when on-device lands.
 *
 *   POST {baseUrl}/chat/completions
 *   Authorization: Bearer {key}
 *   { "model": ..., "messages": [...], "stream": true }
 *
 * Streams SSE `data:` lines; each chunk is choices[0].delta.content; `[DONE]` ends it.
 */
import { http } from '@kit.NetworkKit';
import { util } from '@kit.ArkTS';
import { BusinessError } from '@kit.BasicServicesKit';
import { ChatMessage, StreamCallbacks, Canceler } from '../model/Chat';
import { ProviderRecord } from '../model/Providers';

interface ChatTemplateKwargs {
  thinking: boolean;
}

interface ChatCompletionBody {
  model: string;
  messages: WireMessage[];
  stream: boolean;
  chat_template_kwargs?: ChatTemplateKwargs;
}

/**
 * Provider/model quirk: NVIDIA NIM's DeepSeek V4 models (flash & pro) are reasoning models
 * whose /chat/completions request HANGS (no output) unless `chat_template_kwargs` is present
 * at the body root. We default to non-thinking so the plain `content` field streams — with
 * thinking enabled the answer arrives in `reasoning_content`, which this UI doesn't render.
 * Returns undefined for models that need no extra fields.
 */
function reasoningKwargs(model: string): ChatTemplateKwargs | undefined {
  if (model.includes('deepseek-v4')) {
    return { thinking: false };
  }
  return undefined;
}

/** Result of a key/config validation probe. */
export interface ValidationResult {
  ok: boolean;
  code: number;
  message: string;
}

// Shape of GET /models (OpenAI-compatible: { data: [{ id, created, owned_by }, ...] }).
interface ModelListItem {
  id?: string;
  created?: number;
  owned_by?: string;
}
interface ModelListResponse {
  data?: ModelListItem[];
}

/**
 * A model surfaced to the "add model" screen: the authoritative API id from /v1/models,
 * enriched (when the provider declares a catalogUrl) with the website catalog's free flag,
 * last-month call volume, and deprecation date.
 */
export interface DiscoveredModel {
  id: string;          // authoritative API model id (from /v1/models)
  vendor: string;      // publisher — id prefix before '/', else owned_by
  displayName: string; // nicer name from the catalog, else the id
  free: boolean;       // PREVIEW / "Free Endpoint" in the website catalog
  popularity: number;  // last-month API invocation count (0 if unknown)
  deprecation: string; // deprecation date string (e.g. "07/27/2026"), '' if none
}

// Catalog metadata for one model (from the build.nvidia.com page), keyed by normalized name.
interface CatalogMeta {
  displayName: string;
  free: boolean;
  popularity: number;
  deprecation: string;
}

function vendorOf(id: string, ownedBy: string): string {
  const slash = id.indexOf('/');
  if (slash > 0) {
    return id.substring(0, slash);
  }
  if (ownedBy.length > 0) {
    return ownedBy;
  }
  return 'other';
}

// Normalize a model name for joining catalog slugs (llama-3_3) with API ids (llama-3.3):
// take the part after '/', lowercase, and fold '_' and '.' to '-'.
export function normalizeModelName(id: string): string {
  const slash = id.indexOf('/');
  const suffix = slash >= 0 ? id.substring(slash + 1) : id;
  return suffix.toLowerCase().replaceAll('_', '-').replaceAll('.', '-');
}

// Pull one quoted-string field value out of a JSON-ish fragment: marker is like '"name":"'.
function afterMarker(w: string, marker: string): string {
  const i = w.indexOf(marker);
  if (i < 0) {
    return '';
  }
  const start = i + marker.length;
  const end = w.indexOf('"', start);
  return end < 0 ? '' : w.substring(start, end);
}

// Error body shapes: NVIDIA `{status,title,detail}`, OpenAI `{error:{message}}`.
interface ErrInner {
  message?: string;
}
interface ErrBody {
  detail?: string;
  title?: string;
  message?: string;
  error?: ErrInner;
}

// Pull a human-readable reason out of a provider error body (empty string if none).
function extractError(raw: string): string {
  try {
    const o = JSON.parse(raw) as ErrBody;
    if (o.detail !== undefined && o.detail.length > 0) {
      return o.detail;
    }
    if (o.error?.message !== undefined && o.error.message.length > 0) {
      return o.error.message;
    }
    if (o.message !== undefined && o.message.length > 0) {
      return o.message;
    }
    if (o.title !== undefined) {
      return o.title;
    }
  } catch (e) {
    // not JSON
  }
  return '';
}

// Shape of a streamed SSE chunk we care about.
interface SseDelta {
  content?: string;
  reasoning_content?: string;   // reasoning models' chain-of-thought (GLM-4.7, ...)
}

// What actually goes on the wire: role + content only. ChatMessage now carries
// a display-only `parts` field which must never reach a provider (unknown
// members are usually ignored, but strict endpoints reject them).
interface WireMessage {
  role: string;
  content: string;
}
interface SseChoice {
  delta?: SseDelta;
}
interface SseChunk {
  choices?: SseChoice[];
}

function baseNoSlash(provider: ProviderRecord): string {
  return provider.baseUrl.endsWith('/')
    ? provider.baseUrl.substring(0, provider.baseUrl.length - 1)
    : provider.baseUrl;
}

function endpoint(provider: ProviderRecord): string {
  return baseNoSlash(provider) + '/chat/completions';
}

function authHeaders(provider: ProviderRecord, key: string): Record<string, string> {
  const headers: Record<string, string> = {
    'Content-Type': 'application/json'
  };
  if (provider.authScheme.startsWith('header:')) {
    const name = provider.authScheme.substring('header:'.length);
    headers[name] = key;
  } else {
    // default: bearer
    headers['Authorization'] = 'Bearer ' + key;
  }
  return headers;
}

/**
 * Stream a chat completion. Resolves when the stream ends (or errors are reported via
 * callbacks). Never throws for provider-side failures — those surface through onError so
 * the caller can degrade per docs/byok-providers.md error handling.
 */
export async function streamChat(
  provider: ProviderRecord,
  model: string,
  apiKey: string,
  messages: ChatMessage[],
  cb: StreamCallbacks,
  canceler?: Canceler
): Promise<void> {
  // Stopped while the caller was still resolving the key — never issue the request.
  if (canceler !== undefined && canceler.stopped) {
    cb.onDone();
    return;
  }

  const wire: WireMessage[] = [];
  for (const m of messages) {
    wire.push({ role: m.role, content: m.content });
  }
  const body: ChatCompletionBody = { model: model, messages: wire, stream: true };
  const kw = reasoningKwargs(model);
  if (kw !== undefined) {
    body.chat_template_kwargs = kw;
  }
  const request = http.createHttp();
  const decoder = util.TextDecoder.create('utf-8');

  let buffer = '';
  let raw = '';       // full body, kept so an error response's detail can be surfaced
  let finished = false;
  let aborted = false;

  // Let the caller abort this request (slow/stuck response). Aborting is not an error.
  if (canceler !== undefined) {
    canceler.cancel = () => {
      aborted = true;
      request.destroy();
    };
  }

  // onDone and onError are mutually exclusive and fire at most once. A stream that already
  // saw `[DONE]` is complete, so a late transport failure (e.g. the connection dropping
  // after the final chunk) must not be reported as an error on top of a good answer.
  const finish = (): void => {
    if (!finished) {
      finished = true;
      cb.onDone();
    }
  };

  const fail = (code: number, message: string): void => {
    if (!finished) {
      finished = true;
      cb.onError(code, message);
    }
  };

  const handleLine = (rawLine: string): void => {
    const line = rawLine.trim();
    if (line.length === 0 || !line.startsWith('data:')) {
      return;
    }
    const payload = line.substring('data:'.length).trim();
    if (payload === '[DONE]') {
      finish();
      return;
    }
    try {
      const chunk = JSON.parse(payload) as SseChunk;
      const content = chunk.choices?.[0]?.delta?.content;
      if (content !== undefined && content.length > 0) {
        cb.onDelta(content);
      }
      // Reasoning models (GLM-4.7 streams this unconditionally) put their
      // chain-of-thought in a separate field. Forward it so the UI can show
      // progress instead of dead air until the answer starts.
      const thinking = chunk.choices?.[0]?.delta?.reasoning_content;
      if (thinking !== undefined && thinking.length > 0 && cb.onThinking !== undefined) {
        cb.onThinking(thinking);
      }
    } catch (e) {
      // Ignore keep-alive / non-JSON data lines.
    }
  };

  request.on('dataReceive', (data: ArrayBuffer) => {
    const chunk = decoder.decodeToString(new Uint8Array(data), { stream: true });
    raw += chunk;
    buffer += chunk;
    let idx = buffer.indexOf('\n');
    while (idx >= 0) {
      const oneLine = buffer.substring(0, idx);
      buffer = buffer.substring(idx + 1);
      handleLine(oneLine);
      idx = buffer.indexOf('\n');
    }
  });

  const options: http.HttpRequestOptions = {
    method: http.RequestMethod.POST,
    header: authHeaders(provider, apiKey),
    extraData: JSON.stringify(body),
    expectDataType: http.HttpDataType.STRING,
    connectTimeout: 30000,
    readTimeout: 120000
  };

  try {
    const code = await request.requestInStream(endpoint(provider), options);
    if (code >= 400) {
      fail(code, extractError(raw));
    } else {
      // Flush any trailing buffered line, then complete.
      if (buffer.length > 0) {
        handleLine(buffer);
        buffer = '';
      }
      finish();
    }
  } catch (err) {
    if (aborted) {
      // User-initiated cancel — keep whatever streamed and finish cleanly.
      if (buffer.length > 0) {
        handleLine(buffer);
        buffer = '';
      }
      finish();
    } else {
      const e = err as BusinessError;
      fail(e.code, e.message);
    }
  } finally {
    request.off('dataReceive');
    request.destroy();
  }
}

/**
 * Fetch the provider's live model catalog via GET /models (OpenAI-compatible). This is the
 * authoritative list of what's actually deployed on the user's key — so the picker never
 * offers a model that 404/410s. Returns [] on any failure (offline, no /models support,
 * bad key); discoverModels then unions in the provider's curated `models` as the fallback.
 */
async function rawModels(provider: ProviderRecord, apiKey: string): Promise<DiscoveredModel[]> {
  const request = http.createHttp();
  try {
    const options: http.HttpRequestOptions = {
      method: http.RequestMethod.GET,
      header: authHeaders(provider, apiKey),
      expectDataType: http.HttpDataType.STRING,
      connectTimeout: 15000,
      readTimeout: 30000
    };
    const resp = await request.request(baseNoSlash(provider) + '/models', options);
    if (resp.responseCode < 200 || resp.responseCode >= 300) {
      return [];
    }
    const parsed = JSON.parse(resp.result as string) as ModelListResponse;
    const out: DiscoveredModel[] = [];
    if (parsed.data !== undefined) {
      for (const item of parsed.data) {
        if (item.id !== undefined && item.id.length > 0) {
          // Google returns ids like "models/gemini-3.6-flash"; the OpenAI-compatible chat
          // endpoint wants the short name. Strip that literal prefix (Google-specific; other
          // providers use real "publisher/model" prefixes we must keep).
          const id = item.id.startsWith('models/') ? item.id.substring('models/'.length) : item.id;
          const ownedBy = item.owned_by !== undefined ? item.owned_by : '';
          out.push({
            id: id,
            vendor: vendorOf(id, ownedBy),
            displayName: id,
            free: false,
            popularity: 0,
            deprecation: ''
          });
        }
      }
    }
    return out;
  } catch (err) {
    return [];
  } finally {
    request.destroy();
  }
}

/**
 * Fetch the provider's website catalog page and parse the model metadata embedded in it
 * (the build.nvidia.com models page ships its data as escaped JSON in the HTML). Returns a
 * map keyed by normalized model name → { free, popularity, deprecation, displayName }. No
 * auth — the website is public and must not receive the user's key. Empty map on any failure
 * (the page format is undocumented and may change, so this degrades gracefully).
 */
async function fetchCatalogMeta(url: string): Promise<Map<string, CatalogMeta>> {
  const request = http.createHttp();
  const out: Map<string, CatalogMeta> = new Map();
  try {
    const options: http.HttpRequestOptions = {
      method: http.RequestMethod.GET,
      header: { 'User-Agent': 'Mozilla/5.0', 'Accept': 'text/html' } as Record<string, string>,
      expectDataType: http.HttpDataType.STRING,
      connectTimeout: 15000,
      readTimeout: 40000
    };
    const resp = await request.request(url, options);
    if (resp.responseCode < 200 || resp.responseCode >= 300) {
      return out;
    }
    const text = (resp.result as string).replaceAll('\\"', '"');
    const parts = text.split('"resourceType":"ENDPOINT"');
    for (let i = 1; i < parts.length; i++) {
      const w = parts[i].substring(0, 4000);
      const name = afterMarker(w, '"name":"');
      if (name.length === 0) {
        continue;
      }
      const popStr = afterMarker(w, '"key":"last_month_api_invocation_count","value":"');
      const disp = afterMarker(w, '"displayName":"');
      const dep = afterMarker(w, '"key":"DEPRECATION","value":"');
      const free = w.indexOf('"key":"PREVIEW","value":"true"') >= 0;
      const meta: CatalogMeta = {
        displayName: disp.length > 0 ? disp : name,
        free: free,
        popularity: popStr.length > 0 ? Number.parseInt(popStr) : 0,
        deprecation: dep
      };
      out.set(normalizeModelName(name), meta);
    }
    return out;
  } catch (err) {
    return out;
  } finally {
    request.destroy();
  }
}

const MONTHS: Map<string, number> = new Map<string, number>([
  ['January', 1], ['February', 2], ['March', 3], ['April', 4], ['May', 5], ['June', 6],
  ['July', 7], ['August', 8], ['September', 9], ['October', 10], ['November', 11], ['December', 12]
]);

// "October 16, 2026" -> 20261016 (comparable), 0 if unparseable.
function ymdOf(dateStr: string): number {
  const parts = dateStr.replaceAll(',', '').trim().split(' ');
  if (parts.length < 3) {
    return 0;
  }
  const mon = MONTHS.get(parts[0]);
  if (mon === undefined) {
    return 0;
  }
  const day = Number.parseInt(parts[1]);
  const year = Number.parseInt(parts[2]);
  if (Number.isNaN(day) || Number.isNaN(year)) {
    return 0;
  }
  return year * 10000 + mon * 100 + day;
}

function todayYmd(): number {
  const d = new Date();
  return d.getFullYear() * 10000 + (d.getMonth() + 1) * 100 + d.getDate();
}

function stripTags(s: string): string {
  return s.replace(/<[^>]+>/g, ' ').replace(/&amp;/g, '&').replace(/\s+/g, ' ').trim();
}

/**
 * Fetch a provider's official deprecations page (Google's docs table:
 * columns Model | Release date | Shutdown date | Recommended replacement) and return
 * modelId -> shutdown-date string, skipping rows with no announced shutdown. Empty on failure.
 */
async function fetchDeprecations(url: string): Promise<Map<string, string>> {
  const request = http.createHttp();
  const out: Map<string, string> = new Map();
  try {
    const options: http.HttpRequestOptions = {
      method: http.RequestMethod.GET,
      header: { 'User-Agent': 'Mozilla/5.0', 'Accept': 'text/html' } as Record<string, string>,
      expectDataType: http.HttpDataType.STRING,
      connectTimeout: 15000,
      readTimeout: 40000
    };
    const resp = await request.request(url, options);
    if (resp.responseCode < 200 || resp.responseCode >= 300) {
      return out;
    }
    const h = (resp.result as string).replace(/\s+/g, ' ');
    const rowRe = new RegExp('<tr[^>]*>(.*?)</tr>', 'g');
    let row = rowRe.exec(h);
    while (row !== null) {
      const cellRe = new RegExp('<t[dh][^>]*>(.*?)</t[dh]>', 'g');
      const cells: string[] = [];
      let c = cellRe.exec(row[1]);
      while (c !== null) {
        cells.push(stripTags(c[1]));
        c = cellRe.exec(row[1]);
      }
      if (cells.length >= 3) {
        const model = cells[0];
        const shutdown = cells[2];
        if (model.length > 0 && shutdown.length > 0 && shutdown.indexOf('No shutdown') < 0) {
          out.set(model, shutdown);
        }
      }
      row = rowRe.exec(h);
    }
    return out;
  } catch (err) {
    return out;
  } finally {
    request.destroy();
  }
}

/**
 * The list for the "add model" screen: the authoritative /v1/models catalog (real API ids),
 * unioned with the provider's curated `models` (so a known-good id the listing omits stays
 * selectable — e.g. Zhipu's glm-4.7-flash, whose probe returns a 429 rate-limit rather than a
 * 404, so it exists), then two optional live enrichments — the website catalog
 * (free/popularity/deprecation, via `catalogUrl`) and the official deprecations table
 * (`deprecationsUrl`), which HIDES models already past their shutdown date and annotates
 * upcoming ones. Returns [] only if /v1/models fails AND the provider curates no models; each
 * enrichment degrades independently.
 */
export async function discoverModels(provider: ProviderRecord, apiKey: string): Promise<DiscoveredModel[]> {
  let list = await rawModels(provider, apiKey);

  // Union in curated ids the live listing didn't return. Marked free:false to stay neutral to
  // the dialog's "only free" filter (which only engages when a provider exposes free flags).
  const seen: Set<string> = new Set<string>();
  for (const m of list) {
    seen.add(m.id);
  }
  for (const curated of provider.models) {
    if (!seen.has(curated)) {
      seen.add(curated);
      list.push({
        id: curated,
        vendor: vendorOf(curated, ''),
        displayName: curated,
        free: false,
        popularity: 0,
        deprecation: ''
      });
    }
  }

  if (list.length === 0) {
    return list;
  }

  const catUrl = provider.catalogUrl;
  if (catUrl !== undefined && catUrl.length > 0) {
    const meta = await fetchCatalogMeta(catUrl);
    if (meta.size > 0) {
      const merged: DiscoveredModel[] = [];
      for (const m of list) {
        const cm = meta.get(normalizeModelName(m.id));
        if (cm !== undefined) {
          merged.push({
            id: m.id,
            vendor: m.vendor,
            displayName: cm.displayName,
            free: cm.free,
            popularity: cm.popularity,
            deprecation: cm.deprecation
          });
        } else {
          merged.push(m);
        }
      }
      list = merged;
    }
  }

  const depUrl = provider.deprecationsUrl;
  if (depUrl !== undefined && depUrl.length > 0) {
    const dep = await fetchDeprecations(depUrl);
    if (dep.size > 0) {
      const today = todayYmd();
      const kept: DiscoveredModel[] = [];
      for (const m of list) {
        const sd = dep.get(m.id);
        if (sd !== undefined) {
          const ymd = ymdOf(sd);
          if (ymd > 0 && ymd <= today) {
            continue; // already shut down — hide it
          }
          if (ymd > today) {
            m.deprecation = sd; // upcoming retirement — annotate
          }
        }
        kept.push(m);
      }
      list = kept;
    }
  }

  return list;
}

/**
 * Validate a key by hitting `GET /models` — this checks auth only, so it doesn't depend on
 * any particular model id being current (a stale default model must not fail key validation).
 * Called before persisting a key so a bad paste is caught in the dialog.
 */
export async function validateKey(
  provider: ProviderRecord,
  apiKey: string
): Promise<ValidationResult> {
  const request = http.createHttp();
  try {
    const options: http.HttpRequestOptions = {
      method: http.RequestMethod.GET,
      header: authHeaders(provider, apiKey),
      expectDataType: http.HttpDataType.STRING,
      connectTimeout: 15000,
      readTimeout: 30000
    };
    const resp = await request.request(baseNoSlash(provider) + '/models', options);
    const code = resp.responseCode;
    if (code >= 200 && code < 300) {
      return { ok: true, code: code, message: 'key 可用' };
    }
    if (code === 401 || code === 403) {
      return { ok: false, code: code, message: 'key 无效或未授权' };
    }
    if (code === 429) {
      // Auth passed; provider is just rate-limited right now.
      return { ok: true, code: code, message: 'key 可用(当前被限流)' };
    }
    const detail = extractError(resp.result as string);
    return { ok: false, code: code, message: detail.length > 0 ? detail : 'HTTP ' + code };
  } catch (err) {
    const e = err as BusinessError;
    return { ok: false, code: e.code, message: e.message };
  } finally {
    request.destroy();
  }
}
