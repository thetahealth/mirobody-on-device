/**
 * NAPI bridge to the embedded mirobody C++ core (the C ABI in src/mirobody.h).
 *
 * Call order per session: nativeSetConfig for every stored API key, then
 * nativeReloadProviders once, then nativeGetProviders to populate the picker;
 * nativeChat per turn.
 */

/** Build stamp of the loaded .so. Proves native code is actually running. */
export const nativeVersion: () => string;

/**
 * Set one core configuration value by its config.yml name (e.g.
 * "ZHIPU_API_KEY"). Values live only in process memory (environment), never on
 * disk. Returns 0 on success. Changes made after chat has already run need a
 * nativeReloadProviders() to take effect.
 */
export const nativeSetConfig: (key: string, value: string) => number;

/**
 * Rebuild the provider clients from the current configuration. Returns the
 * number of providers now available, or -1 on failure. Not safe while a chat
 * turn is in flight.
 */
export const nativeReloadProviders: () => number;

/**
 * Newline-separated provider tokens the core can run right now — the mobile
 * profile lists only providers whose key has been injected, so this IS the
 * model picker's data source for the native lane. '' when none.
 */
export const nativeGetProviders: () => string;

/**
 * Run one chat turn on a worker thread, streaming events back on the ArkTS
 * thread in order. Event types: 'reply' | 'thinking' | 'chart' | 'ask' |
 * 'queryTitle' | 'queryArguments' | 'queryDetail' | 'costStatistics' | 'error',
 * then exactly one terminal 'end' (success) or 'aborted' (after
 * nativeChatCancel).
 *
 * 'ask' is the only event that expects a REPLY: the turn is parked inside the
 * ask_user tool until nativeChatAnswer delivers one (or the tool times out), so
 * an 'end' will not arrive before then.
 *
 * @param provider     a token from nativeGetProviders, passed straight through.
 * @param messagesJson conversation as JSON: [{role, content}, ...] in order,
 *                     ending with the current user turn.
 * @returns a turn id for nativeChatCancel, or throws on bad arguments.
 */
export const nativeChat: (
  provider: string,
  messagesJson: string,
  onEvent: (type: string, content: string) => void
) => number;

/**
 * Request cancellation of an in-flight turn. Takes effect on the turn's next
 * event; the stream then finishes with 'aborted'. Unknown/finished ids are a
 * no-op. Cancels either lane — local turns share the same turn-id table.
 */
export const nativeChatCancel: (turnId: number) => void;

/**
 * Answer an 'ask' event so its parked turn can continue.
 *
 * Routed by the ask id from the event's payload, NOT by turn id: the core
 * resolves the waiting tool itself, so the caller never has to know which turn
 * asked. Must be called from the ArkTS thread while the turn's worker thread is
 * blocked -- which is the normal case, since the event arrived here first.
 *
 * @param answerJson the user's choice as JSON; it becomes the tool's result and
 *                   so reaches the model verbatim. The app sends
 *                   {"selected": string[], "other"?: string}.
 * @returns false when nobody was waiting -- a stale tap after a stop or a
 *          timeout. Safe to ignore.
 */
export const nativeChatAnswer: (askId: string, answerJson: string) => boolean;

/**
 * Can NATIVE code open and mmap this path from inside the app sandbox?
 * Returns JSON `{ok, err, size, mmapOk}`.
 *
 * llama.cpp loads a model by path and mmaps it, so an ArkTS-readable URI or fd
 * is not enough — this is the gate on letting a GGUF live outside app-private
 * storage (which it must, to survive an uninstall).
 */
export const nativeProbePath: (path: string) => string;

/**
 * Neural Network Runtime devices visible to THIS app, as JSON
 * `{available, err, devices: [{name, type, id}]}`.
 *
 * The gate on the NPU question, answerable without converting a model: NNRt is
 * the only route to the Kirin NPU (MindSpore Lite delegates through it; HMS
 * `hiai_foundation`/`CANNKit` expose single-op execution only, not model
 * loading), so if no device here reports `type: 3`
 * (`OH_AI_NNRTDEVICE_ACCELERATOR`) the NPU is not reachable from a third-party
 * app and the whole path is closed. `type` 1 = CPU, 2 = GPU, 0 = other.
 *
 * `available: false` means libmindspore_lite_ndk.so could not be loaded — it is
 * dlopen'd rather than linked so that its absence degrades to this instead of
 * breaking the app's native module.
 */
export const nativeNnrtDevices: () => string;

/**
 * Sequential DRAM read bandwidth this device actually delivers, as JSON
 * `{mb, threads, oneThreadGbs, allThreadGbs}`.
 *
 * A hard ceiling on decode that no accelerator can lift: autoregressive decode
 * reads essentially every weight per token, so `tok/s <= bandwidth /
 * model_bytes`, and an NPU or GPU shares the same LPDDR controller as the CPU.
 * Compare `allThreadGbs` against what the current decode rate already consumes
 * (model size x tok/s). If they are close, decode is at the memory wall and
 * moving it to another compute unit cannot help — which is exactly why the
 * Vulkan backend lost on a Kirin 9020.
 *
 * Uses the thread count set by nativeLocalSetThreads (default 6) because one
 * thread cannot saturate a modern memory controller.
 */
export const nativeMemBandwidth: () => string;

/**
 * On-device lane status as JSON:
 * `{ available, backend, loaded, modelPath, has, built }`.
 *
 * `has` is what the SILICON supports, read from AT_HWCAP —
 * `{fp16, dotprod, i8mm, bf16, sve, sve2, sme}`. `built` is what THIS binary was
 * compiled to emit — `{neon, fma, fp16, dotprod, i8mm, sve, sme}`. Reported apart
 * because conflating them is a trap: ggml's own `ggml_cpu_has_*()` are compile-time
 * `__ARM_FEATURE_*` checks despite the name, so reading them as device capability
 * gives the wrong answer. `has && !built` is unrealized performance — exactly what a
 * cross build with no -march flags produces. Do not infer either from a SoC name; a
 * Kirin 9020 turned out to have i8mm and SVE. /proc/cpuinfo is unreadable to an app
 * on HarmonyOS, so this is the only way to see any of it.
 *
 * `available` is false when the .so was built without llama.cpp (no
 * LLAMA_CPP_DIR) — the app still runs, on-device chat just reports itself
 * unavailable. `loaded` is true once a model is resident, i.e. after one turn.
 */
export const nativeLocalStatus: () => string;

/**
 * Threads for on-device turns; 0 or negative means hardware_concurrency(). Applies
 * from the next turn without reloading the model. Worth setting rather than
 * defaulting: on a big.LITTLE SoC the little cores can cost a matmul more than they
 * add, since the batch waits on the slowest thread.
 */
export const nativeLocalSetThreads: (n: number) => void;

/**
 * Run one chat turn on the on-device model, streaming events back exactly like
 * nativeChat: 'reply' deltas, then a 'stats' event carrying
 * `{loadMs, prefillMs, decodeMs, promptTokens, decodedTokens, threads, nCtx, ctxMs,
 * droppedMsgs, utf8Splits, backend}`, then
 * one terminal 'end' or 'aborted'. A failure emits 'error' first.
 *
 * The model stays loaded between turns and is only reloaded when `modelPath`
 * changes — a multi-GB reload per turn would be unusable.
 *
 * The stats worth acting on: `nCtx` is the context this turn actually got (chosen by
 * walking a ladder down from a ceiling until the buffer allocates, so it varies by
 * model and device) and `ctxMs` is what building it cost, since a fresh context is
 * created per turn. `droppedMsgs` > 0 means history was trimmed to fit, which is why
 * an older turn may seem forgotten. `utf8Splits` counts tokens that ended
 * mid-character — proof the byte-level-BPE carry buffer is doing real work.
 *
 * @param modelPath    absolute path to a .gguf the app can read.
 * @param messagesJson conversation as JSON: [{role, content}, ...].
 * @returns a turn id for nativeChatCancel.
 */
export const nativeLocalChat: (
  modelPath: string,
  messagesJson: string,
  onEvent: (type: string, content: string) => void
) => number;

/**
 * Store health readings on the device as FHIR Observations. `resourcesJson` is a
 * JSON ARRAY of FHIR resources; resolves with
 * `{"stored":<int>,"failed":<int>,"error":"<first failure>"}` and rejects when no
 * database is configured or the argument is not an array.
 *
 * Idempotent on each resource's `id`: give a reading a deterministic id
 * ("hw.<metric>.<startMillis>", see model/HealthMetrics.ets) and re-syncing an
 * overlapping window replaces it instead of adding a duplicate. A resource with
 * no id gets a fresh one, i.e. plain create semantics.
 *
 * Runs on a worker (one SQLite write per resource), which is why it is a Promise.
 * The rows land where the `family_health` MCP tool reads them, so a synced metric
 * is answerable by the model on the native lane with nothing leaving the device.
 */
export const nativeHealthStore: (resourcesJson: string) => Promise<string>;

/**
 * The most recent stored Observations, newest first, as JSON
 * `{"total":<int>,"items":[{"code","display","value","unit","time","source"}...]}`.
 * `count` is clamped to 1..200 (0 => 20). The read-back that lets the settings
 * card show what a sync actually landed, including across a relaunch.
 */
export const nativeHealthRecent: (count: number) => Promise<string>;
