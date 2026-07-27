/**
 * Shared chat + routing types. Both execution lanes (local, cloud/BYOK) expose the same
 * OpenAI Chat/SSE contract, so everything above the router is lane-agnostic
 * (see docs/tiered-inference.md).
 */

/**
 * One ordered piece of an assistant message. `content` alone can't express a
 * reply whose thinking, prose and (later) charts interleave, so the display
 * structure rides beside it. 'thinking' renders as a collapsible dim block;
 * 'text' through the markdown renderer; 'chart' is reserved for the ECharts
 * pipeline.
 */
export interface MessagePart {
  kind: string;      // 'thinking' | 'text' | 'chart'
  content: string;
}

export interface ChatMessage {
  role: string;    // 'system' | 'user' | 'assistant'
  content: string; // the message TEXT — what goes back to the model as context

  // Display structure (assistant messages only; absent => render content as one
  // text part). NEVER serialized to a provider: both transports map messages to
  // bare {role, content} wire objects before sending.
  parts?: MessagePart[];
}

export enum Lane {
  LOCAL = 'local',
  CLOUD = 'cloud'
}

/** Router input — the conversation so far plus the context the router decides on. */
export interface RouteInput {
  messages: ChatMessage[];
  inputTokensEst: number;
  hasAttachments: boolean;
  userForcedLane: string; // 'local' | 'cloud' | '' (empty = let the router decide)
  offline: boolean;
  byokConfigured: boolean; // is a cloud key present for the active provider?
}

/** Router output — which lane, which provider, and why (for telemetry + escalation notice). */
export interface RouteDecision {
  lane: string;       // Lane value
  providerId: string; // set when lane == cloud
  reason: string;
}

/** Streaming callbacks shared by every lane's transport. */
export interface StreamCallbacks {
  onDelta: (text: string) => void;
  onDone: () => void;
  onError: (code: number, message: string) => void;

  /**
   * Chain-of-thought deltas (reasoning models: GLM-4.7, gpt-oss, ...). Optional
   * so existing callers keep compiling; a transport with reasoning output and
   * no handler simply drops it — the pre-parts behavior.
   */
  onThinking?: (text: string) => void;
}

/**
 * Cancel handle for an in-flight turn. The transport sets `cancel` once its request exists;
 * the UI calls it to abort (e.g. a stuck / slow response). Aborting keeps whatever streamed
 * so far and completes normally (no error).
 *
 * `stopped` covers the window BEFORE the transport has a request to destroy — the engine
 * awaits the stored key first, so a stop during that gap would otherwise still fire the
 * request. The transport checks the flag and skips issuing it at all.
 */
export interface Canceler {
  cancel: () => void;
  stopped: boolean;
}
