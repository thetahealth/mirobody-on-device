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
 * thread in order. Event types: 'reply' | 'thinking' | 'queryTitle' |
 * 'queryArguments' | 'queryDetail' | 'costStatistics' | 'error', then exactly
 * one terminal 'end' (success) or 'aborted' (after nativeChatCancel).
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
 * no-op.
 */
export const nativeChatCancel: (turnId: number) => void;
