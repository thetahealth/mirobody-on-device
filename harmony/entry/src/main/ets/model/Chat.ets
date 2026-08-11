/**
 * Shared chat + routing types. Both execution lanes (local, cloud/BYOK) expose the same
 * OpenAI Chat/SSE contract, so everything above the router is lane-agnostic
 * (see docs/privacy-tiers.md).
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

  /**
   * Epoch millis, stamped when the turn is sent (user) or finishes (assistant).
   *
   * Optional because sessions written before this field existed have none, and the
   * only honest thing to show for those is nothing — a stamp minted at load time
   * would read as "just now" for a conversation from last week. Same reason it is
   * never backfilled. Rides to disk with the rest of the message (SessionStore
   * JSON-serializes ChatMessage[] whole) and, like `parts`, never to a provider.
   */
  ts?: number;

  /**
   * Assistant turns: which model wrote it, already display-formatted (the picker's
   * own label — "provider · model" for cloud, the bare .gguf name for local). Held
   * as the LABEL rather than an id because it is only ever shown, and because the
   * id alone could not name a local file the provider list has never heard of.
   */
  model?: string;

  /**
   * Ephemeral: drawn in the thread, but never sent to a model and never written to
   * disk. Lives for as long as the screen does.
   *
   * A slash command's answer (`/help`) is the app talking to the user, not a turn.
   * It belongs on screen — that is where it was asked for — but it is not part of
   * the conversation: replaying several KB of documentation as context on every
   * later turn would be absurd, and storing it would put the same KB into every
   * session anyone ever ran the command in. So the transports drop it when building
   * the wire, and persist() drops it when saving.
   *
   * Same instinct as keeping an ECharts option out of `content`: what is worth
   * DISPLAYING, what is worth SENDING and what is worth KEEPING are three different
   * questions, and answering them all with one field is how a message ends up
   * carrying everything.
   */
  local?: boolean;
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

  /**
   * One finished chart: an Apache ECharts option, as a JSON object string ready
   * to parse. Its own callback rather than an onDelta payload because an option
   * must never become part of the ANSWER — see the note in Index.assembleAssistant
   * on why it stays out of `content`.
   *
   * Native lane only, and structurally so: the cloud lane is BYOK straight to the
   * provider (OpenAI-shaped SSE), with no mirobody event protocol and no tool
   * pipeline for `render_chart` to run in.
   */
  onChart?: (optionJson: string) => void;

  /**
   * The model asked the user a multiple-choice question and IS WAITING. `json` is
   * `{ask_id, question, options[], multi_select}`; answering means calling
   * mirobody.nativeChatAnswer(ask_id, ...), which unblocks the turn so it
   * continues in the same reply rather than ending and starting another.
   *
   * The one event that expects something back, so a handler that ignores it parks
   * the turn until the tool's own timeout fires. Native lane only, for the same
   * reason charts are: the cloud lane has no tool pipeline to run ask_user in.
   */
  onAsk?: (json: string) => void;
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
