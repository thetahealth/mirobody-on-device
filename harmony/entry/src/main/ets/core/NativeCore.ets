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

/**
 * BYOK provider id -> the config name the core reads that provider's key from.
 * '' = the core has no client for this provider (openrouter / groq / cerebras
 * stay on the BYOK direct transport until res/agents/baseline.cpp learns them).
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
  return '';
}

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

/** render_chart's arguments shape (see res/mcp_tools/render_chart.cpp). */
interface RenderChartArgs {
  option?: object;
}

/**
 * A render_chart call's arguments -> a fenced ```echarts block, the markdown
 * parser's chart IR. Injecting it into the TEXT stream keeps the chart at its
 * true position in the reply with zero extra plumbing — the same block a model
 * could have written by hand. Returns '' when the arguments hold no option.
 */
function chartFence(argsJson: string): string {
  try {
    const args = JSON.parse(argsJson) as RenderChartArgs;
    if (args.option !== undefined && typeof args.option === 'object') {
      return '\n```echarts\n' + JSON.stringify(args.option) + '\n```\n';
    }
  } catch (e) {
    // Malformed arguments: skip the chart, keep the turn alive.
  }
  return '';
}

export class NativeCore {
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
    const listed = mirobody.nativeGetProviders();
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

  private static async injectAll(): Promise<boolean> {
    let injected = false;
    for (const id of ['zhipu', 'nvidia', 'gemini']) {
      const env = envNameFor(id);
      const key = await SecureKeyStore.getKey(id);
      if (env.length > 0 && key.length > 0) {
        injected = mirobody.nativeSetConfig(env, key) === 0 || injected;
      }
    }
    if (injected) {
      mirobody.nativeReloadProviders();
    }
    return injected;
  }

  /**
   * Can the native transport run `model` for `providerId`? Self-heals for a key
   * saved after startup: when the model is not served but its provider has a
   * stored key, re-inject + reload once and re-check.
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
      // The C ABI drops tool_id, but events are strictly ordered on one stream:
      // a call's queryArguments immediately follows its queryTitle, so tracking
      // the last title is enough to recognize a render_chart call.
      let lastTool = '';
      const turnId = mirobody.nativeChat(shortName(model), JSON.stringify(wire),
        (type: string, content: string) => {
          if (type === 'reply') {
            cb.onDelta(content);
          } else if (type === 'thinking') {
            if (cb.onThinking !== undefined) {
              cb.onThinking(content);
            }
          } else if (type === 'queryTitle') {
            lastTool = content;
          } else if (type === 'queryArguments') {
            if (lastTool === 'render_chart') {
              const fence = chartFence(content);
              if (fence.length > 0) {
                cb.onDelta(fence);   // charts ride the text stream as ```echarts blocks
              }
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
}
