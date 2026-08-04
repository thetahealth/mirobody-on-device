/**
 * ChatEngine — composes the pipeline for one turn:
 *
 *   turn ─▶ router.select ─▶ { lane, provider? } ─▶ transport ─▶ stream
 *
 * Cloud turns pick between two transports on the user's key: the embedded C++ core
 * (NativeCore, when it serves the selected model) and the BYOK direct SSE client
 * (everything else). Local turns run llama.cpp on a GGUF the user supplied, in-process
 * through the same NAPI bridge; only when no local model is configured do they fall back
 * to the explanatory stub. Everything downstream of the router is identical regardless
 * of lane — the UI cannot tell which one answered.
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
  /**
   * Absolute path of the on-device model this turn should run, or '' for none.
   * Snapshotted by the caller rather than resolved here for the same reason
   * providerId/model are: the selection can change mid-turn.
   */
  localModelPath: string;
  /**
   * Fired the moment the lane is chosen and BEFORE any bytes leave the device. The cloud
   * notice ("this message left the device, on your key") is a heads-up, not a receipt —
   * returning the decision only after the stream finished showed it once the data was
   * already gone. This is the single channel for the decision; there is no return value to
   * read it from twice.
   */
  onDecision: (decision: RouteDecision) => void;
}

export class ChatEngine {
  /**
   * Run one turn. Streams assistant text through `cb`; the routing decision goes to
   * `turn.onDecision` the moment it's made (docs/privacy-tiers.md escalation UX).
   */
  static async send(
    ctx: common.UIAbilityContext,
    messages: ChatMessage[],
    turn: TurnContext,
    cb: StreamCallbacks,
    canceler?: Canceler
  ): Promise<void> {
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
    const cloudProvider: ProviderRecord | undefined =
      decision.lane === Lane.CLOUD ? provider : undefined;
    if (cloudProvider !== undefined) {
      decision.providerId = cloudProvider.id;
    }
    // Announce the lane here — before offerFor()'s awaits and before either transport can
    // put a byte on the wire, so the cloud notice warns rather than reports.
    turn.onDecision(decision);

    if (cloudProvider !== undefined) {
      // Two transports for the same lane. The embedded C++ core is preferred
      // when it serves this model (its provider's key has been injected): same
      // upstream call, but with the core's agent pipeline (tools; charts later).
      // Everything else — providers the core doesn't know, models it doesn't
      // offer — stays on the BYOK direct SSE client.
      if (await NativeCore.offerFor(ctx, cloudProvider.id, turn.model)) {
        await NativeCore.chat(turn.model, messages, cb, canceler);
      } else {
        await streamChat(cloudProvider, turn.model, apiKey, messages, cb, canceler);
      }
    } else if (turn.localModelPath.length > 0) {
      // On-device: llama.cpp + a GGUF inside libmirobody.so (src/llm/local.cpp).
      await NativeCore.localChat(turn.localModelPath, messages, cb, canceler);
    } else {
      // Local lane with nothing to run it on.
      ChatEngine.emitLocalStub(ctx, cb, input.byokConfigured);
    }
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
