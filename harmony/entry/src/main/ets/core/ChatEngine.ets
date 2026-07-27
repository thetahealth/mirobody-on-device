/**
 * ChatEngine — composes the pipeline for one turn:
 *
 *   turn ─▶ router.select ─▶ { lane, provider? } ─▶ transport ─▶ stream
 *
 * Cloud turns pick between two transports on the user's key: the embedded C++ core
 * (NativeCore, when it serves the selected model) and the BYOK direct SSE client
 * (everything else). Local turns are a STUB on HarmonyOS today (no on-device model wired
 * yet) — they emit a clear notice and, when a cloud key exists, the UI can offer to
 * escalate. Everything downstream of the router is identical regardless of lane, so
 * swapping in a real local runtime later is a drop-in.
 */
import { common } from '@kit.AbilityKit';
import { ChatMessage, StreamCallbacks, RouteInput, RouteDecision, Lane, Canceler } from '../model/Chat';
import { ProviderRecord, findProvider } from '../model/Providers';
import { selectLane, estimateTokens } from './Router';
import { streamChat } from './Transport';
import { SecureKeyStore } from './SecureKeyStore';
import { NativeCore } from './NativeCore';

/**
 * Everything about ONE turn. The provider/model are the caller's snapshot, not a fresh read
 * of the persisted selection — the UI sets its state synchronously but persists it behind an
 * await, so re-loading here could run the turn on the previously selected model while the
 * composer (and the failure accounting) already point at the new one.
 */
export interface TurnContext {
  providerId: string;     // provider this turn runs on
  model: string;          // model id this turn runs on
  userForcedLane: string; // 'local' | 'cloud' | ''
  offline: boolean;
}

export interface TurnResult {
  decision: RouteDecision;
}

export class ChatEngine {
  /**
   * Run one turn. Streams assistant text through `cb`. Returns the routing decision so the
   * UI can show the escalation notice (docs/tiered-inference.md escalation UX).
   */
  static async send(
    ctx: common.UIAbilityContext,
    messages: ChatMessage[],
    turn: TurnContext,
    cb: StreamCallbacks,
    canceler?: Canceler
  ): Promise<TurnResult> {
    const provider: ProviderRecord | undefined = findProvider(turn.providerId);
    const apiKey: string = provider !== undefined ? await SecureKeyStore.getKey(provider.id) : '';

    let joined = '';
    for (const m of messages) {
      joined += m.content;
    }
    const input: RouteInput = {
      messages: messages,
      inputTokensEst: estimateTokens(joined),
      hasAttachments: false,
      userForcedLane: turn.userForcedLane,
      offline: turn.offline,
      byokConfigured: provider !== undefined && apiKey.length > 0
    };

    const decision: RouteDecision = selectLane(input);

    if (decision.lane === Lane.CLOUD && provider !== undefined) {
      decision.providerId = provider.id;
      // Two transports for the same lane. The embedded C++ core is preferred
      // when it serves this model (its provider's key has been injected): same
      // upstream call, but with the core's agent pipeline (tools; charts later).
      // Everything else — providers the core doesn't know, models it doesn't
      // offer — stays on the BYOK direct SSE client.
      if (await NativeCore.offerFor(ctx, provider.id, turn.model)) {
        await NativeCore.chat(turn.model, messages, cb, canceler);
      } else {
        await streamChat(provider, turn.model, apiKey, messages, cb, canceler);
      }
    } else {
      // Local lane stub — on-device model not wired on HarmonyOS yet.
      ChatEngine.emitLocalStub(ctx, cb, input.byokConfigured);
    }

    return { decision };
  }

  private static emitLocalStub(ctx: common.UIAbilityContext, cb: StreamCallbacks, byokConfigured: boolean): void {
    const res: Resource = byokConfigured
      ? $r('app.string.engine_local_stub_byok')
      : $r('app.string.engine_local_stub_nokey');
    const hint = ctx.resourceManager.getStringSync(res.id);
    cb.onDelta(hint);
    cb.onDone();
  }
}
