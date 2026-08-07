/**
 * The app chrome's type and glyph scales.
 *
 * WHY THIS EXISTS: the same handful of sizes was written as bare numbers at 92 call
 * sites across seven files -- `fontSize(12)` alone appeared 20 times, `fontSize(13)`
 * 17 times. Colours and strings in this app already go through resources
 * (`$r('app.color.*')`, `$r('app.string.*')`, with zero hardcoded hex anywhere), so
 * sizes were the one axis of the design with no single source.
 *
 * NOT resources/base/element/float.json, which would be the obvious parallel: a
 * `$r('app.float.*')` yields a Resource, and a Resource cannot be multiplied. Chat
 * content sizes are all `<n> * this.fontScale`, so a resource-based scale would force
 * two different mechanisms for one type scale. Plain numbers serve both.
 *
 * THE SPLIT between the two groups is what a size means, not what it is attached to:
 *
 *   FONT  text the user reads. An inline glyph that pairs with adjacent text (the ✓
 *         beside a settings row, ✕ excepted) takes the FONT token of that text, so it
 *         tracks the row instead of drifting from it.
 *   ICON  glyph affordances that stand on their own -- SymbolGlyph, the ✕ that closes a
 *         dialog, the ↑ that sends, the ■ that stops. Sized as icons, not as text.
 *
 * NOT the chat content scale. RichMessage.ets carries its own set (11 .. 16, plus
 * heading steps and a 0.92 code-span factor), every one of them multiplied by
 * `fontScale` so the conversation honours the font-size setting while the chrome around
 * it stays put -- that is deliberate, see AppSettingsDialogs.ets. Those numbers are
 * tuned against each other for markdown rendering and never repeat outside that file,
 * so they stay there: hoisting them here would make them harder to tune and dedupe
 * nothing.
 */

/**
 * Both scales are declared through an interface rather than inferred from the literal:
 * ArkTS rejects an untyped object literal (arkts-no-untyped-obj-literals), and every
 * literal in this codebase names its type the same way (see HealthMetrics.ets).
 */
export interface FontScale {
  footnote: number;
  caption: number;
  label: number;
  body: number;
  row: number;
  control: number;
  title: number;
  headline: number;
}

export interface IconScale {
  xs: number;
  sm: number;
  md: number;
  lg: number;
  xl: number;
  xxl: number;
}

/** Text the user reads, in the app chrome. Unscaled -- see the note above. */
export const FONT: FontScale = {
  /** Timestamps, counts, retirement dates, error detail. */
  footnote: 11,
  /** Hints, intros, notices, per-row secondary lines. */
  caption: 12,
  /** Section labels, status lines, empty/error states. */
  label: 13,
  /** List values, dialog buttons, drawer rows. */
  body: 14,
  /** The title line of a row: a model name, a metric, a session. */
  row: 15,
  /** Menu and settings items -- one tap target per line. */
  control: 16,
  /** Dialog titles. */
  title: 18,
  /** The app name, and a page title. */
  headline: 20
};

/** Standalone glyph affordances. Unscaled -- an icon that grows shifts the layout. */
export const ICON: IconScale = {
  /** Inline carets and the stop square inside the send button. */
  xs: 12,
  /** A glyph riding along inside a caption row. */
  sm: 14,
  /** Row actions: trash, paperclip, the glyph inside a circular button. */
  md: 18,
  /** Dialog close, send, new-chat. */
  lg: 20,
  /** Drawer back chevron. */
  xl: 22,
  /** The hamburger that opens everything. */
  xxl: 24
};
