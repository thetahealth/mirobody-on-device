/**
 * Router stage — the single replaceable stage that picks local vs cloud per turn
 * (docs/privacy-tiers.md). Strategy: Manual + Heuristic fallback.
 *
 * Default lane is local. Escalate to cloud on a clear "hard" signal: over-long input,
 * a code block, attachments, or the user's deep-thinking toggle. Guard rails always win.
 */
import { RouteInput, RouteDecision, Lane } from '../model/Chat';

// Heuristic threshold — tune on real data (docs/privacy-tiers.md open questions).
const LONG_INPUT_TOKENS = 1200;

function lastUserText(input: RouteInput): string {
  for (let i = input.messages.length - 1; i >= 0; i--) {
    if (input.messages[i].role === 'user') {
      return input.messages[i].content;
    }
  }
  return '';
}

export function selectLane(input: RouteInput): RouteDecision {
  // Guard rails, independent of strategy.
  if (input.offline || !input.byokConfigured) {
    return { lane: Lane.LOCAL, providerId: '', reason: 'cloud-unreachable' };
  }
  if (input.userForcedLane === Lane.CLOUD) {
    return { lane: Lane.CLOUD, providerId: '', reason: 'user-forced' };
  }
  if (input.userForcedLane === Lane.LOCAL) {
    return { lane: Lane.LOCAL, providerId: '', reason: 'user-forced' };
  }

  // Heuristic escalation signals.
  if (input.hasAttachments) {
    return { lane: Lane.CLOUD, providerId: '', reason: 'attachments' };
  }
  if (input.inputTokensEst > LONG_INPUT_TOKENS) {
    return { lane: Lane.CLOUD, providerId: '', reason: 'long-input' };
  }
  if (lastUserText(input).includes('```')) {
    return { lane: Lane.CLOUD, providerId: '', reason: 'code-block' };
  }

  return { lane: Lane.LOCAL, providerId: '', reason: 'default' };
}

/** Rough token estimate — ~4 chars/token. Good enough for the length heuristic. */
export function estimateTokens(messages: string): number {
  return Math.ceil(messages.length / 4);
}
