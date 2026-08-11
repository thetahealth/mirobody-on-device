/**
 * Slash commands — text the composer answers by itself, without a turn.
 *
 * `/help` renders the built-in guide (resources/rawfile/help/), which is written as
 * a model reply on purpose: it is the user's documentation AND a live check of every
 * construct core/Markdown.ets claims to render. A formatting regression shows up the
 * first time anyone types /help, without a device, a model, or a network.
 *
 * A command NEVER reaches a model. It produces a message flagged `local`, which the
 * transports drop when they build the wire (see NativeCore/Transport), so a help
 * document several thousand characters long costs nothing in context on the next
 * turn — the same reasoning that keeps an ECharts option out of `content`.
 */
import { common } from '@kit.AbilityKit';
import { i18n } from '@kit.LocalizationKit';
import { util } from '@kit.ArkTS';

/** The built-in guide. Documented — it is listed in its own command table. */
export const SLASH_HELP = '/help';

/**
 * The native-core dev probe (pages/NativeProbe.ets).
 *
 * DELIBERATELY UNDOCUMENTED: it is a developer tool with file pickers and a raw
 * event log, temporary until the native transport has proven out on devices, and
 * listing it in user-facing help would offer a debugging screen as a feature. A
 * slash command is the right shape for exactly that — reachable by whoever knows
 * the word, invisible to everyone else, and nothing in the navigation to explain.
 */
export const SLASH_PROBE = '/probe';

/** Start a fresh conversation — the drawer's "new chat", by keyboard. */
export const SLASH_NEW = '/new';

/** Toggle incognito — the drawer's switch, by keyboard. */
export const SLASH_INCOGNITO = '/incognito';

/** One row of the command palette. */
export interface SlashSpec {
  name: string;
  /** app.string id for the one-line description shown beside the name. */
  desc: Resource;
}

/**
 * The commands the palette OFFERS, in the order it lists them.
 *
 * `/help` leads because the palette is the discovery mechanism: whoever opened it
 * by typing a slash is exploring, and the first row should be the one that explains
 * the rest. `/probe` is absent on purpose (see above) — typing it in full still
 * runs it, which is the whole distinction between undocumented and unavailable.
 *
 * Nothing here REPLACES a menu item. Every one of these is also a control in the
 * drawer, and stays one: a command line is a shortcut for people who like them, not
 * a syntax to memorize because the buttons went away.
 */
export const SLASH_MENU: SlashSpec[] = [
  { name: SLASH_HELP, desc: $r('app.string.slash_help_desc') },
  { name: SLASH_NEW, desc: $r('app.string.slash_new_desc') },
  { name: SLASH_INCOGNITO, desc: $r('app.string.slash_incognito_desc') }
];

export class SlashCommand {
  private static cache: Map<string, string> = new Map<string, string>();

  /**
   * The palette's rows for a draft, or [] when the draft is not a command being
   * typed.
   *
   * Opens on a leading `/` and narrows by prefix as the user types, so `/n` shows
   * one row. It closes as soon as a space appears: past that the user is writing a
   * sentence that happens to start with a slash, not choosing a command.
   */
  static suggest(draft: string): SlashSpec[] {
    const t = draft.trimStart();
    if (!t.startsWith('/') || t.indexOf(' ') >= 0 || t.indexOf('\n') >= 0) {
      return [];
    }
    const q = t.toLowerCase();
    const out: SlashSpec[] = [];
    for (const s of SLASH_MENU) {
      if (s.name.startsWith(q)) {
        out.push(s);
      }
    }
    return out;
  }

  /**
   * The command this draft IS, or ''.
   *
   * Matched on the WHOLE trimmed message, so a line that merely mentions "/help"
   * mid-sentence still goes to the model — a user asking "what does /help do" is
   * asking a question, not running one.
   */
  static match(text: string): string {
    const t = text.trim().toLowerCase();
    for (const s of SLASH_MENU) {
      if (t === s.name) {
        return s.name;
      }
    }
    // Not in the palette, still runnable.
    return t === SLASH_PROBE ? SLASH_PROBE : '';
  }

  /**
   * The guide's markdown, in the app's language.
   *
   * Rawfiles carry no locale qualifiers, so the file is picked by name rather than
   * resolved by the resource system — hence the explicit tag test, matching how
   * HistoryDrawer names the same two languages. Cached because the document is a
   * few KB and a user who types /help twice should not pay for it twice.
   */
  static help(ctx: common.UIAbilityContext): string {
    const name = SlashCommand.isChinese() ? 'help-zh.md' : 'help-en.md';
    const hit = SlashCommand.cache.get(name);
    if (hit !== undefined) {
      return hit;
    }
    let text = '';
    try {
      const bytes = ctx.resourceManager.getRawFileContentSync('help/' + name);
      text = util.TextDecoder.create('utf-8').decodeToString(bytes);
    } catch (e) {
      // Packaged with the HAP, so this should not happen; an empty string makes the
      // caller show nothing rather than crash the composer.
      text = '';
    }
    SlashCommand.cache.set(name, text);
    return text;
  }

  private static isChinese(): boolean {
    let tag = '';
    try {
      tag = i18n.System.getAppPreferredLanguage();
    } catch (err) {
      tag = '';
    }
    return tag.startsWith('zh');
  }
}
