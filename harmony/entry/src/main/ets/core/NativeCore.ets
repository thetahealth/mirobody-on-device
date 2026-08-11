/**
 * NativeCore — the embedded mirobody C++ core behind the NAPI bridge
 * (libmirobody.so; see entry/src/main/cpp/). A TRANSPORT for the cloud lane,
 * not a lane of its own: inference still happens at the provider, only the
 * request runs through the in-process core (curl) instead of the ArkTS SSE
 * client, gaining the C++ agent pipeline (tools, and later charts) for free.
 *
 * Keys: the core reads provider credentials from its process-wide config, which
 * consults the environment. ensureReady() reads every stored BYOK key from
 * SecureKeyStore and injects it via nativeSetConfig — process memory only,
 * nothing lands on disk — then builds the provider clients once. A key saved
 * mid-session is picked up lazily: offerFor() re-injects and reloads when it
 * sees a key the core does not serve yet.
 *
 * The core offers a provider only when its key is present (the mobile build
 * profile), so nativeGetProviders() doubles as "what can the native transport
 * run": membership of a model's short name in that set IS the routing test.
 */
import mirobody from 'libmirobody.so';
import fs from '@ohos.file.fs';
import { common } from '@kit.AbilityKit';
import { ChatMessage, StreamCallbacks, Canceler } from '../model/Chat';
import { SecureKeyStore } from './SecureKeyStore';
import { findProvider } from '../model/Providers';

/**
 * BYOK provider id -> the config name the core reads that provider's key from.
 * '' = the core has no client for this provider, so its turns stay on the BYOK
 * direct SSE transport — same upstream call, but outside the agent pipeline and
 * therefore without tools.
 */
function envNameFor(providerId: string): string {
  if (providerId === 'zhipu') {
    return 'ZHIPU_API_KEY';
  }
  if (providerId === 'nvidia') {
    return 'NVIDIA_API_KEY';
  }
  if (providerId === 'gemini') {
    return 'GOOGLE_API_KEY';
  }
  if (COMPAT_PROVIDERS.has(providerId)) {
    return providerId.toUpperCase() + '_API_KEY';
  }
  return '';
}

/**
 * Providers the core serves through its generic OpenAI-compatible block rather
 * than a curated model list (see res/agents/baseline.cpp).
 *
 * The difference that matters: a curated provider's models are named at BUILD time,
 * so injecting its key is enough. These are BYOK catalogs — the user picks models
 * the core has never heard of — so the model ids have to be injected too, or the
 * core offers the key and nothing to run with it.
 */
const COMPAT_PROVIDERS: Set<string> = new Set<string>(['openrouter', 'groq', 'cerebras']);

/**
 * The core keys providers by slash-free model short name ("glm-5.2"), while the
 * BYOK lane carries full API ids ("z-ai/glm-5.2") — take the last path segment.
 */
function shortName(model: string): string {
  const slash = model.lastIndexOf('/');
  return slash >= 0 ? model.substring(slash + 1) : model;
}

/** One native-lane message on the wire; mirrors the C ABI's messages_json. */
interface WireMessage {
  role: string;
  content: string;
}

export class NativeCore {
  /**
   * Answer a question from an `ask` event, unblocking the turn parked in the tool.
   *
   * An EMPTY answer is the documented way to say "nobody replied": the broker
   * settles the wait with no payload and the tool reports that to the model, which
   * is exactly what a stopped turn wants. So releasing a question needs no separate
   * cancel call — answering it with nothing is the release.
   */
  static answerAsk(askId: string, answerJson: string): boolean {
    if (askId.length === 0) {
      return false;
    }
    return mirobody.nativeChatAnswer(askId, answerJson);
  }

  /** Slash-free tokens the core can run right now. */
  private static providers: Set<string> = new Set<string>();
  private static ready: boolean = false;

  /**
   * Point the core at the app sandbox, inject every stored key, and build the
   * provider clients. Idempotent; call once before the first turn (ChatEngine
   * awaits it per send).
   *
   * Path config is computed FRESH from the context every launch and never
   * persisted -- the sandbox path can change across reinstalls. The DDL the
   * core's SQLite needs ships as rawfile/sql/ (a copy of res/sql/sqlite/ --
   * keep them in sync when the schema changes) and is (re)extracted on every
   * launch, so an app update refreshes it.
   */
  static async ensureReady(ctx: common.UIAbilityContext): Promise<void> {
    if (NativeCore.ready) {
      return;
    }
    NativeCore.ready = true;   // even a keyless init counts: refresh() keeps working
    NativeCore.injectPaths(ctx);
    await NativeCore.injectAll();
    NativeCore.refresh();
  }

  /** Sandbox paths + DDL for the embedded core's database and object store. */
  private static injectPaths(ctx: common.UIAbilityContext): void {
    const base = ctx.filesDir;

    // res/sql layout: SQL_DIR is the BASE dir; the core appends the backend
    // subdirectory ("sqlite"), so the files land in <base>/sql/sqlite/.
    const sqlDir = base + '/sql/sqlite';
    try {
      if (!fs.accessSync(sqlDir)) {
        fs.mkdirSync(sqlDir, true);
      }
      const names = ctx.resourceManager.getRawFileListSync('sql');
      for (const name of names) {
        const bytes = ctx.resourceManager.getRawFileContentSync('sql/' + name);
        const file = fs.openSync(sqlDir + '/' + name,
          fs.OpenMode.CREATE | fs.OpenMode.READ_WRITE | fs.OpenMode.TRUNC);
        fs.writeSync(file.fd, bytes.buffer as ArrayBuffer);
        fs.closeSync(file);
      }
    } catch (e) {
      // Schema extraction failing is not fatal: the core's db-backed tools
      // degrade and chat still runs. Log through the status the caller sees.
      console.error('NativeCore: DDL extraction failed: ' + JSON.stringify(e));
    }

    mirobody.nativeSetConfig('SQL_DIR', base + '/sql');
    mirobody.nativeSetConfig('SQLITE_PATH', base + '/mirobody.db');
    mirobody.nativeSetConfig('LOCAL_STORAGE_DIR', base + '/storage');
  }

  /** Re-read the provider set from the core. */
  private static refresh(): void {
    const listed: string = mirobody.nativeGetProviders();
    const next = new Set<string>();
    if (listed.length > 0) {
      for (const line of listed.split('\n')) {
        if (line.length > 0) {
          next.add(shortName(line));
        }
      }
    }
    NativeCore.providers = next;
  }

  /**
   * Every model this app has asked the core to serve for a compat provider,
   * per provider id.
   *
   * Accumulated rather than replaced: the config value is the WHOLE list, so
   * injecting only the model being asked for would silently unregister the ones
   * offered a moment ago and make every switch back a reload.
   */
  private static compatModels: Map<string, Set<string>> = new Map<string, Set<string>>();

  private static async injectAll(): Promise<boolean> {
    let injected = false;
    for (const id of ['zhipu', 'nvidia', 'gemini', 'openrouter', 'groq', 'cerebras']) {
      const env = envNameFor(id);
      const key = await SecureKeyStore.getKey(id);
      if (env.length === 0 || key.length === 0) {
        continue;
      }
      injected = mirobody.nativeSetConfig(env, key) === 0 || injected;

      // A compat provider also needs its endpoint and its model ids: the core
      // registers a client per model, and it has never heard of these.
      if (COMPAT_PROVIDERS.has(id)) {
        const prefix = id.toUpperCase();
        const rec = findProvider(id);
        if (rec !== undefined) {
          mirobody.nativeSetConfig(prefix + '_BASE_URL', rec.baseUrl);
        }
        const models = NativeCore.compatModels.get(id);
        if (models !== undefined && models.size > 0) {
          const list: string[] = [];
          models.forEach((m: string) => { list.push(m); });
          mirobody.nativeSetConfig(prefix + '_MODELS', list.join(','));
        }
      }
    }
    if (injected) {
      mirobody.nativeReloadProviders();
    }
    return injected;
  }

  /**
   * Can the native transport run `model` for `providerId`? Self-heals for a key
   * saved after startup, and for a compat model the core has not been told about:
   * record it, re-inject + reload once, and re-check.
   */
  static async offerFor(ctx: common.UIAbilityContext, providerId: string, model: string): Promise<boolean> {
    await NativeCore.ensureReady(ctx);
    const token = shortName(model);
    if (NativeCore.providers.has(token)) {
      return true;
    }
    if (envNameFor(providerId).length === 0) {
      return false;   // provider unknown to the core; don't churn clients
    }
    // Remember it BEFORE injecting, so this pass registers it. A model picked from
    // a live catalog is unknown at build time — this is the moment the core learns
    // it exists.
    if (COMPAT_PROVIDERS.has(providerId)) {
      let seen = NativeCore.compatModels.get(providerId);
      if (seen === undefined) {
        seen = new Set<string>();
        NativeCore.compatModels.set(providerId, seen);
      }
      seen.add(model);
    }
    if (await NativeCore.injectAll()) {
      NativeCore.refresh();
    }
    return NativeCore.providers.has(token);
  }

  /**
   * Run one turn through the core. Resolves when the stream finishes (matching
   * the BYOK transport's contract). Event mapping: reply -> onDelta; thinking
   * -> onThinking; error -> onError; end/aborted -> onDone. Tool steps and
   * costStatistics are dropped until the chart pipeline lands.
   */
  static chat(
    model: string,
    messages: ChatMessage[],
    cb: StreamCallbacks,
    canceler?: Canceler
  ): Promise<void> {
    const wire: WireMessage[] = [];
    for (const m of messages) {
      // A slash command's answer is the app talking, not a turn: display only.
      if (m.local === true) {
        continue;
      }
      wire.push({ role: m.role, content: m.content });
    }

    return new Promise<void>((resolve) => {
      // A stop can land before the turn exists (the engine awaits the key store
      // first on the BYOK path; keep the same contract here).
      if (canceler !== undefined && canceler.stopped) {
        cb.onDone();
        resolve();
        return;
      }

      let errored = false;
      const turnId: number = mirobody.nativeChat(shortName(model), JSON.stringify(wire),
        (type: string, content: string) => {
          if (type === 'reply') {
            cb.onDelta(content);
          } else if (type === 'thinking') {
            if (cb.onThinking !== undefined) {
              cb.onThinking(content);
            }
          } else if (type === 'chart') {
            // The core's ChartFilter already collapsed the render_chart tool
            // triple into this one event and suppressed the raw steps, so
            // `content` is the ECharts option and nothing here has to know a
            // tool name. This used to sniff queryTitle/queryArguments and
            // synthesize a ```echarts fence into the TEXT stream — which put the
            // option in the answer, and therefore in every later prompt.
            if (cb.onChart !== undefined) {
              cb.onChart(content);
            }
          } else if (type === 'ask') {
            // The turn is BLOCKED inside the tool right now: nothing else arrives
            // on this stream until Index answers or ask_user gives up waiting.
            if (cb.onAsk !== undefined) {
              cb.onAsk(content);
            }
          } else if (type === 'error') {
            errored = true;
            cb.onError(0, content);
          } else if (type === 'end' || type === 'aborted') {
            // In-band provider errors still end with 'end'; the error callback
            // already fired, so only a clean stream completes normally.
            if (!errored) {
              cb.onDone();
            }
            resolve();
          }
          // queryDetail / costStatistics: dropped until the tool-step UI lands.
        });

      if (canceler !== undefined) {
        canceler.cancel = () => {
          mirobody.nativeChatCancel(turnId);
        };
      }
    });
  }

  /**
   * Run one turn on the ON-DEVICE model. Same contract as chat() above — blocking
   * native call on a worker thread, events marshalled back in order, one terminal
   * event — so the caller cannot tell the lanes apart.
   *
   * `modelPath` is an absolute path the app has been granted (see
   * core/LocalModelStore.ets); the engine mmaps it, and it stays resident between
   * turns unless the path changes.
   */
  static localChat(
    modelPath: string,
    messages: ChatMessage[],
    cb: StreamCallbacks,
    canceler?: Canceler
  ): Promise<void> {
    const wire: WireMessage[] = [];
    for (const m of messages) {
      // A slash command's answer is the app talking, not a turn: display only.
      if (m.local === true) {
        continue;
      }
      wire.push({ role: m.role, content: m.content });
    }

    return new Promise<void>((resolve) => {
      if (canceler !== undefined && canceler.stopped) {
        cb.onDone();
        resolve();
        return;
      }

      let errored = false;
      const turnId: number = mirobody.nativeLocalChat(modelPath, JSON.stringify(wire),
        (type: string, content: string) => {
          if (type === 'reply') {
            cb.onDelta(content);
          } else if (type === 'thinking') {
            if (cb.onThinking !== undefined) {
              cb.onThinking(content);
            }
          } else if (type === 'error') {
            errored = true;
            cb.onError(0, content);
          } else if (type === 'stats') {
            // Timings for the probe/diagnostics only; not part of the reply.
            console.info('mirobody local stats ' + content);
          } else if (type === 'end' || type === 'aborted') {
            if (!errored) {
              cb.onDone();
            }
            resolve();
          }
        });

      if (canceler !== undefined) {
        canceler.cancel = () => {
          mirobody.nativeChatCancel(turnId);
        };
      }
    });
  }
}
