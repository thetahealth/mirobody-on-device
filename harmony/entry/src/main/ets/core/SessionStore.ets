/**
 * Locally persisted chat sessions — what the left drawer lists.
 *
 * htdoc's drawer reads GET /api/history from the mirobody server. Harmony has no
 * server connection at all: it talks straight to each provider's baseUrl (BYOK) and
 * has no account, so there is nothing to sync with and no user to scope by. History
 * is therefore purely local, which also fixes a real defect on its own — until now
 * `Index.history` was in-memory only, so closing the app discarded the conversation.
 *
 * Storage: one preferences store, same pattern as ConfigStore. An INDEX key holds the
 * session list (id/title/timestamps, small), and each session's messages live under
 * their own `msg.<id>` key. Two reasons to split them: the drawer needs only the index
 * to render, so opening it never deserializes megabytes of transcript; and one huge
 * conversation cannot make every other session's read slower.
 *
 * Preferences is a key-value store with everything held in memory once loaded, so it
 * suits tens of sessions, not thousands. MAX_SESSIONS trims the oldest rather than
 * letting that grow without bound; a relational store is the answer if this ever needs
 * search or real paging.
 */
import { preferences } from '@kit.ArkData';
import { common } from '@kit.AbilityKit';
import { ChatMessage } from '../model/Chat';

const STORE_NAME = 'chat_sessions';
const KEY_INDEX = 'index';
const KEY_MSG_PREFIX = 'msg.';

/** Oldest sessions beyond this are dropped when a new one is recorded. */
const MAX_SESSIONS = 100;

/** Characters of the first user message kept as the session title. */
const TITLE_LEN = 60;

/** Drawer row: enough to list and sort, without the transcript. */
export interface SessionMeta {
  id: string;
  title: string;
  updatedAt: number;   // epoch millis; the list sorts on this
  count: number;       // messages, shown as a subtitle
}

export class SessionStore {
  private static store: preferences.Preferences | null = null;

  private static async prefs(ctx: common.UIAbilityContext): Promise<preferences.Preferences> {
    if (SessionStore.store === null) {
      SessionStore.store = await preferences.getPreferences(ctx, STORE_NAME);
    }
    return SessionStore.store;
  }

  /**
   * Session ids are timestamp+counter, not random: preferences has no ordering, so an
   * id that sorts chronologically makes a corrupt index recoverable by inspection.
   * The counter breaks ties when two sessions start inside the same millisecond.
   */
  private static seq: number = 0;

  static newId(): string {
    SessionStore.seq++;
    return new Date().getTime().toString() + '-' + SessionStore.seq.toString();
  }

  /** First user message, trimmed to one line — the drawer's row label. */
  static titleFor(messages: ChatMessage[]): string {
    for (const m of messages) {
      if (m.role === 'user') {
        const flat = m.content.replace(new RegExp('\\s+', 'g'), ' ').trim();
        return flat.length > TITLE_LEN ? flat.substring(0, TITLE_LEN) + '…' : flat;
      }
    }
    return '';
  }

  /** Newest first. */
  static async list(ctx: common.UIAbilityContext): Promise<SessionMeta[]> {
    const p = await SessionStore.prefs(ctx);
    const raw = await p.get(KEY_INDEX, '[]') as string;
    try {
      const list = JSON.parse(raw) as SessionMeta[];
      list.sort((a: SessionMeta, b: SessionMeta) => b.updatedAt - a.updatedAt);
      return list;
    } catch (err) {
      // A corrupt index must not make the drawer unopenable; an empty list is
      // recoverable (the next save rewrites it), a thrown error is not.
      return [];
    }
  }

  private static async putIndex(ctx: common.UIAbilityContext, list: SessionMeta[]): Promise<void> {
    const p = await SessionStore.prefs(ctx);
    await p.put(KEY_INDEX, JSON.stringify(list));
    await p.flush();
  }

  /** Full transcript for a session; empty when it is missing. */
  static async messages(ctx: common.UIAbilityContext, id: string): Promise<ChatMessage[]> {
    const p = await SessionStore.prefs(ctx);
    const raw = await p.get(KEY_MSG_PREFIX + id, '[]') as string;
    try {
      return JSON.parse(raw) as ChatMessage[];
    } catch (err) {
      return [];
    }
  }

  /**
   * Write (or overwrite) one session. Called after every completed turn, so it is an
   * upsert keyed on id rather than an append — resuming a session and continuing it
   * must update that row, not create a second one.
   *
   * An empty transcript is treated as a delete: a session the user opened but never
   * sent anything in should not litter the drawer.
   */
  static async save(ctx: common.UIAbilityContext, id: string, messages: ChatMessage[]): Promise<void> {
    if (messages.length === 0) {
      await SessionStore.remove(ctx, id);
      return;
    }
    const p = await SessionStore.prefs(ctx);
    await p.put(KEY_MSG_PREFIX + id, JSON.stringify(messages));

    const list = await SessionStore.list(ctx);
    const meta: SessionMeta = {
      id: id,
      title: SessionStore.titleFor(messages),
      updatedAt: new Date().getTime(),
      count: messages.length
    };
    const kept: SessionMeta[] = list.filter((s: SessionMeta) => s.id !== id);
    kept.unshift(meta);

    // Trim the tail, deleting each dropped session's transcript too -- an orphaned
    // `msg.<id>` would otherwise stay in the store forever, invisible.
    while (kept.length > MAX_SESSIONS) {
      const dropped = kept.pop();
      if (dropped !== undefined) {
        await p.delete(KEY_MSG_PREFIX + dropped.id);
      }
    }
    await p.put(KEY_INDEX, JSON.stringify(kept));
    await p.flush();
  }

  static async remove(ctx: common.UIAbilityContext, id: string): Promise<void> {
    const p = await SessionStore.prefs(ctx);
    await p.delete(KEY_MSG_PREFIX + id);
    const list = await SessionStore.list(ctx);
    await SessionStore.putIndex(ctx, list.filter((s: SessionMeta) => s.id !== id));
  }
}
