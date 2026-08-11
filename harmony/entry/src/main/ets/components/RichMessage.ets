/**
 * RichMessage — an assistant message body: optional collapsible thinking block
 * followed by the markdown-rendered answer.
 *
 * Consumes plain strings (not MessagePart[]) on purpose: strings diff cheaply
 * under @Prop, and phase 1's part order is fixed (thinking, then text). The
 * ForEach keys carry a content EXTENT, not just RichBlock.id (kind + start offset).
 * ArkUI skips a ForEach item whose key is unchanged, so an id that is stable across
 * streaming appends means a growing block never re-renders: a streaming reply showed
 * only the opening characters of each block and looked right only once the finished
 * message rebuilt from history. Adding the extent churns exactly the one item being
 * appended to and leaves every completed block untouched.
 *
 * Thinking UX kills the reasoning-model dead air: while the model is still
 * thinking (streaming && no answer text yet) the thought stream shows expanded,
 * live; once the answer starts it collapses to a one-line "深度思考" handle the
 * user can reopen. A manual toggle always wins over the automatic rule.
 *
 * math / chart blocks render as placeholders in this phase — but only CLOSED
 * ones show their raw source; a half-streamed formula/option shows a busy hint
 * instead (the `closed` contract from core/Markdown.ets).
 */
import { common } from '@kit.AbilityKit';
import { parseBlocks, RichBlock, InlineSpan, ListEntry, TableRow } from '../core/Markdown';
import { RenderHost, RenderedImage } from '../core/RenderHost';

@Component
export struct RichMessage {
  @Prop text: string = '';
  @Prop thinking: string = '';
  @Prop streaming: boolean = false;
  @Prop fontScale: number = 1;

  // Bumped by MathRenderer when a formula finishes. Read inside the math
  // helpers below, so exactly the messages holding formulas rebuild and pick
  // their SVG out of the cache.
  @StorageProp('renderEpoch') renderEpoch: number = 0;

  // Ink is baked into the rendered SVG/PNG, so the render calls below need the
  // color mode explicitly (EntryAbility publishes it). As @StorageProp it is also
  // the rebuild trigger: a theme flip re-runs these lookups under the other mode's
  // key, misses, and queues a re-render.
  @StorageProp('darkMode') darkMode: boolean = false;

  // Manual fold override: unset until the user taps, then it wins.
  @State private userToggled: boolean = false;
  @State private manualExpand: boolean = false;

  // Math cache accessors. All read mathEpoch so ArkUI tracks the dependency;
  // the first miss queues the render (see MathRenderer.lookup).
  private mathOf(tex: string, display: boolean): RenderedImage | undefined {
    const epoch = this.renderEpoch;   // dependency anchor; value unused
    return epoch >= 0 ? RenderHost.lookupMath(tex, display, this.darkMode) : undefined;
  }

  private chartOf(optionJson: string): RenderedImage | undefined {
    const epoch = this.renderEpoch;   // dependency anchor; value unused
    return epoch >= 0 ? RenderHost.lookupChart(optionJson, this.darkMode) : undefined;
  }

  private svgOf(source: string): RenderedImage | undefined {
    const epoch = this.renderEpoch;   // dependency anchor; value unused
    return epoch >= 0 ? RenderHost.lookupSvg(source) : undefined;
  }

  private svgReady(source: string): boolean {
    const s = this.svgOf(source);
    return s !== undefined && s.ok;
  }

  private svgUrl(source: string): string {
    const s = this.svgOf(source);
    return s !== undefined ? s.url : '';
  }

  /** 4:3 when the source declares no size — the shape a diagram usually is. */
  private svgAspect(source: string): number {
    const s = this.svgOf(source);
    return s !== undefined && s.w > 0 && s.h > 0 ? s.w / s.h : 4 / 3;
  }

  private chartReady(optionJson: string): boolean {
    const c = this.chartOf(optionJson);
    return c !== undefined && c.ok;
  }

  private chartUrl(optionJson: string): string {
    const c = this.chartOf(optionJson);
    return c !== undefined ? c.url : '';
  }

  private chartAspect(optionJson: string): number {
    const c = this.chartOf(optionJson);
    return c !== undefined && c.h > 0 ? c.w / c.h : 1.6;
  }

  private mathReady(tex: string, display: boolean): boolean {
    const m = this.mathOf(tex, display);
    return m !== undefined && m.ok;
  }

  private mathUrl(tex: string, display: boolean): string {
    const m = this.mathOf(tex, display);
    return m !== undefined ? m.url : '';
  }

  private mathW(tex: string, display: boolean): number {
    const m = this.mathOf(tex, display);
    return m !== undefined ? m.w * this.fontScale : 0;
  }

  private mathH(tex: string, display: boolean): number {
    const m = this.mathOf(tex, display);
    return m !== undefined ? m.h * this.fontScale : 0;
  }

  /** Any span the render host has to produce an image for. */
  private static hasMath(spans: InlineSpan[]): boolean {
    for (const s of spans) {
      if (s.kind === 'math') {
        return true;
      }
    }
    return false;
  }

  /**
   * Does anything in this block wait on RenderHost? Formula/chart BLOCKS obviously
   * do; a paragraph, list item or table cell does too once it holds inline math.
   */
  private static needsRender(b: RichBlock): boolean {
    if (b.kind === 'math' || b.kind === 'chart' || b.kind === 'svg') {
      return true;
    }
    if (RichMessage.hasMath(b.spans)) {
      return true;
    }
    for (const e of b.entries) {
      if (RichMessage.hasMath(e.spans)) {
        return true;
      }
    }
    for (const r of b.rows) {
      for (const cell of r.cells) {
        if (RichMessage.hasMath(cell)) {
          return true;
        }
      }
    }
    return false;
  }

  /**
   * Identity + how much content the block currently holds. The extent is what makes
   * the key change as text streams in; without it ArkUI treats the item as unchanged
   * and never rebuilds it (same reason the model picker keys on its group's ids).
   *
   * A block waiting on RenderHost also carries the epoch, and MUST: a finished render
   * changes no text at all, so identity + extent are byte-identical before and after.
   * ArkUI skips an item whose key is unchanged, which skipped the very `if
   * (this.mathReady(...))` the epoch bump exists to re-run — @StorageProp marked this
   * component dirty and the ForEach then threw the rebuild away, so a formula sat on
   * its fallback source forever and the SVG was rendered, cached and never drawn.
   * Only formula/chart-bearing blocks pay the churn, and it stops once their renders
   * land, so plain prose still diffs on content alone.
   */
  private blockKey(b: RichBlock): string {
    let n = b.body.length;
    for (const s of b.spans) {
      n += s.text.length;
    }
    for (const e of b.entries) {
      n += e.marker.length;
      for (const s of e.spans) {
        n += s.text.length;
      }
    }
    for (const r of b.rows) {
      for (const cell of r.cells) {
        for (const s of cell) {
          n += s.text.length;
        }
      }
    }
    return b.id + '#' + n + (RichMessage.needsRender(b) ? '@' + this.renderEpoch : '');
  }

  private thinkingExpanded(): boolean {
    if (this.userToggled) {
      return this.manualExpand;
    }
    return this.streaming && this.text.length === 0;
  }

  build() {
    Column({ space: 6 }) {
      // trim(): a model that declines to reason still emits the empty <think></think>
      // pair, whose whitespace body would otherwise render a "深度思考" handle with
      // nothing behind it.
      if (this.thinking.trim().length > 0) {
        this.thinkingBlock()
      }
      ForEach(parseBlocks(this.text), (b: RichBlock) => {
        this.block(b)
      }, (b: RichBlock) => this.blockKey(b))
      // Nothing has arrived yet: hold the answer's place with the waiting dots.
      // The send button turns into a stop control at the same moment, but it sits
      // at the bottom of the screen while the answer will appear here, and the
      // gap is seconds long whenever the model thinks or calls a tool first.
      // Kept below the thinking block so the two read in the order they happen.
      if (this.streaming && this.text.length === 0) {
        TypingDots({ fontScale: this.fontScale })
      }
    }
    .alignItems(HorizontalAlign.Start)
    // Definite width so the blocks' percentage sizes (and the code blocks'
    // horizontal Scroll clipping) resolve against the bubble, not against
    // whatever the widest text line happens to be.
    .width('100%')
  }

  //----------------------------------------------------------------------------
  // Thinking
  //----------------------------------------------------------------------------

  @Builder
  private thinkingBlock() {
    Column({ space: 4 }) {
      Row({ space: 6 }) {
        Text(this.streaming && this.text.length === 0 ? '🤔' : '💭')
          .fontSize(12)
        Text($r('app.string.chat_thinking_label'))
          .fontSize(12 * this.fontScale)
          .fontColor($r('app.color.text_tertiary'))
        Text(this.thinkingExpanded() ? '▾' : '▸')
          .fontSize(12)
          .fontColor($r('app.color.text_tertiary'))
      }
      .onClick(() => {
        this.manualExpand = !this.thinkingExpanded();
        this.userToggled = true;
      })

      if (this.thinkingExpanded()) {
        this.thinkingBody()
      }
    }
    .alignItems(HorizontalAlign.Start)
    .width('100%')
    .padding({ left: 8, right: 8, top: 6, bottom: 6 })
    .backgroundColor($r('app.color.bg_overlay'))
    .borderRadius(8)
  }

  /**
   * The reasoning body — markdown-rendered, but subordinate to the answer.
   *
   * This used to be one plain Text, so the markdown a reasoning model writes in its
   * scratch work (option lists, bold conclusions, small headings) showed as literal
   * source. It now goes through the same parseBlocks + inline() path as the answer,
   * at the thinking type size and color so the hierarchy still reads.
   *
   * Block math and charts deliberately render as their SOURCE instead of going to
   * the render host: an ECharts PNG or a MathJax image inside scratch work is not
   * worth a render, and the reader is skimming reasoning rather than studying a
   * figure. Nothing here runs while the block is folded — the caller's `if` leaves
   * the whole subtree unbuilt.
   */
  @Builder
  private thinkingBody() {
    Column({ space: 4 }) {
      ForEach(parseBlocks(this.thinking), (b: RichBlock) => {
        if (b.kind === 'list') {
          ForEach(b.entries, (e: ListEntry, idx: number) => {
            Row({ space: 6 }) {
              Text(e.marker)
                .fontSize(13 * this.fontScale)
                .fontColor($r('app.color.text_tertiary'))
                .constraintSize({ minWidth: 16 })
              Column() {
                this.inline(e.spans, 13 * this.fontScale, $r('app.color.text_tertiary'),
                            FontWeight.Normal)
              }
              .layoutWeight(1)
              .alignItems(HorizontalAlign.Start)
            }
            .width('100%')
            .alignItems(VerticalAlign.Top)
            .padding({ left: e.depth * 12 })
          }, (e: ListEntry, idx: number) => b.id + '-' + idx + '#' + e.spans.length)
        } else if (b.kind === 'table') {
          // Reuse the real table: a grid is the only readable form, and reasoning
          // does produce comparison tables.
          this.table(b)
        } else if (b.kind === 'rule') {
          // Reasoning does section its scratch work; a rule carries no text, so
          // without this branch it would fall through to an empty Text and vanish.
          this.rule()
        } else if (b.kind === 'code' || b.kind === 'math' || b.kind === 'chart'
                   || b.kind === 'svg') {
          Text(b.body)
            .fontFamily('monospace')
            .fontSize(12 * this.fontScale)
            .fontColor($r('app.color.text_tertiary'))
            .width('100%')
        } else if (b.spans.length === 0 && b.body.length > 0) {
          // Any future block kind that keeps its content outside `spans` would
          // otherwise render as nothing here; show the source rather than lose it.
          Text(b.body)
            .fontSize(13 * this.fontScale)
            .fontColor($r('app.color.text_tertiary'))
            .width('100%')
        } else {
          this.inline(b.spans,
                      (b.kind === 'heading' ? 14 : 13) * this.fontScale,
                      $r('app.color.text_tertiary'),
                      b.kind === 'heading' ? FontWeight.Bold : FontWeight.Normal)
        }
      }, (b: RichBlock) => this.blockKey(b))
    }
    .alignItems(HorizontalAlign.Start)
    .width('100%')
  }

  //----------------------------------------------------------------------------
  // Blocks
  //----------------------------------------------------------------------------

  @Builder
  private block(b: RichBlock) {
    if (b.kind === 'heading') {
      this.heading(b)
    } else if (b.kind === 'code') {
      this.codeBlock(b)
    } else if (b.kind === 'quote') {
      this.quote(b)
    } else if (b.kind === 'rule') {
      this.rule()
    } else if (b.kind === 'list') {
      this.list(b)
    } else if (b.kind === 'math') {
      this.mathBlock(b)
    } else if (b.kind === 'table') {
      this.table(b)
    } else if (b.kind === 'chart') {
      this.chartBlock(b)
    } else if (b.kind === 'svg') {
      this.svgBlock(b)
    } else {
      this.paragraph(b)
    }
  }

  /** Struck / not, since every branch below has to answer the same question. */
  private static line(s: InlineSpan): TextDecorationType {
    return s.strike === true ? TextDecorationType.LineThrough : TextDecorationType.None;
  }

  /**
   * Open a link. Untrusted text, so only http(s) and mailto are ever handed to the
   * system — a model that writes a `file://` or a custom scheme gets a shrug, not a
   * launched intent.
   */
  private openLink(href: string): void {
    if (!href.startsWith('http://') && !href.startsWith('https://')
        && !href.startsWith('mailto:')) {
      return;
    }
    try {
      (getContext(this) as common.UIAbilityContext).startAbility({
        action: 'ohos.want.action.viewData',
        entities: ['entity.system.browsable'],
        uri: href
      });
    } catch (e) {
      // No handler installed for the scheme. Nothing useful to say about it.
    }
  }

  @Builder
  private inline(spans: InlineSpan[], size: number, color: ResourceColor, weight: FontWeight) {
    Text() {
      ForEach(spans, (s: InlineSpan) => {
        if (s.kind === 'code') {
          Span(s.text)
            .fontFamily('monospace')
            .fontSize(size * 0.92)
            .fontColor($r('app.color.inline_code_fg'))
            .decoration({ type: RichMessage.line(s) })
            .textBackgroundStyle({ color: $r('app.color.bg_inline_code'), radius: 3 })
        } else if (s.kind === 'math') {
          if (this.mathReady(s.text, false)) {
            ImageSpan(this.mathUrl(s.text, false))
              .width(this.mathW(s.text, false))
              .height(this.mathH(s.text, false))
              .verticalAlign(ImageSpanAlignment.CENTER)
          } else {
            // Render still pending (or failed): readable TeX beats a hole.
            Span(s.text)
              .fontStyle(FontStyle.Italic)
              .fontColor($r('app.color.text_secondary'))
          }
        } else if (s.kind === 'image') {
          // The alt text, marked, NOT the picture. Fetching a URL a model wrote
          // would hand a third party a beacon that says when this user read this
          // reply — in a health app, from a message about their own results. Charts
          // have their own lane (```echarts), which renders locally.
          Span('🖼 ' + (s.text.length > 0 ? s.text : s.href ?? ''))
            .fontSize(size * 0.92)
            .fontColor($r('app.color.text_tertiary'))
        } else if (s.href !== undefined) {
          Span(s.text)
            .fontColor($r('app.color.brand_text'))
            .fontWeight(s.bold === true ? FontWeight.Bold : weight)
            .fontStyle(s.italic === true ? FontStyle.Italic : FontStyle.Normal)
            .decoration({
              type: s.strike === true ? TextDecorationType.LineThrough : TextDecorationType.Underline,
              color: $r('app.color.brand_text')
            })
            .onClick(() => {
              this.openLink(s.href ?? '');
            })
        } else {
          Span(s.text)
            .fontWeight(s.bold === true ? FontWeight.Bold : weight)
            .fontStyle(s.italic === true ? FontStyle.Italic : FontStyle.Normal)
            .decoration({ type: RichMessage.line(s) })
        }
      }, (s: InlineSpan, i: number) => i + ':' + s.kind + ':' + s.text.length)
    }
    .fontSize(size)
    .fontColor(color)
    .lineHeight(size * 1.5)
  }

  @Builder
  private paragraph(b: RichBlock) {
    this.inline(b.spans, 16 * this.fontScale, $r('app.color.text_primary'), FontWeight.Normal)
  }

  @Builder
  private heading(b: RichBlock) {
    Column() {
      this.inline(b.spans,
        (b.level <= 1 ? 21 : b.level === 2 ? 19 : b.level === 3 ? 17.5 : 16.5) * this.fontScale,
        $r('app.color.text_primary'), FontWeight.Bold)
    }
    .margin({ top: b.id.endsWith(':0') ? 0 : 4, bottom: 2 })
  }

  @Builder
  private codeBlock(b: RichBlock) {
    Column() {
      if (b.language.length > 0) {
        Text(b.language)
          .fontSize(11 * this.fontScale)
          .fontColor($r('app.color.text_tertiary'))
          .width('100%')
          .padding({ left: 10, top: 5 })
      }
      Scroll() {
        Text(b.body)
          .fontFamily('monospace')
          .fontSize(13.5 * this.fontScale)
          .fontColor($r('app.color.code_fg'))
          .lineHeight(20 * this.fontScale)
          .padding({ left: 10, right: 10, top: b.language.length > 0 ? 4 : 8, bottom: 8 })
      }
      .scrollable(ScrollDirection.Horizontal)
      .scrollBar(BarState.Off)
      .align(Alignment.TopStart)
      .width('100%')
    }
    .alignItems(HorizontalAlign.Start)
    .width('100%')
    .backgroundColor($r('app.color.bg_surface'))
    .border({ width: 1, color: $r('app.color.border') })
    .borderRadius(8)
  }

  /**
   * Thematic break. Its own margin on top of the column's spacing, because a rule
   * is there to SEPARATE — sitting at the same distance as the gap between two
   * paragraphs it would read as one more line of content.
   */
  @Builder
  private rule() {
    Divider()
      .strokeWidth(1)
      .color($r('app.color.divider'))
      .margin({ top: 6, bottom: 6 })
  }

  @Builder
  private quote(b: RichBlock) {
    Row({ space: 8 }) {
      Column()
        .width(3)
        .backgroundColor($r('app.color.quote_bar'))
        .borderRadius(2)
      Column() {
        this.inline(b.spans, 15 * this.fontScale, $r('app.color.text_secondary'), FontWeight.Normal)
      }
      .layoutWeight(1)
      .alignItems(HorizontalAlign.Start)
    }
    .alignItems(VerticalAlign.Top)
    .width('100%')
  }

  @Builder
  private list(b: RichBlock) {
    Column({ space: 3 }) {
      ForEach(b.entries, (e: ListEntry) => {
        Row({ space: 6 }) {
          Text(e.marker)
            .fontSize(15 * this.fontScale)
            // A ticked box earns the accent; everything else is furniture.
            .fontColor(e.checked === true ? $r('app.color.brand_text') : $r('app.color.text_secondary'))
            .constraintSize({ minWidth: 18 })
          Column() {
            this.inline(e.spans, 16 * this.fontScale, $r('app.color.text_primary'), FontWeight.Normal)
          }
          .layoutWeight(1)
          .alignItems(HorizontalAlign.Start)
        }
        .alignItems(VerticalAlign.Top)
        .padding({ left: e.depth * 14 })
        .width('100%')
      }, (e: ListEntry, idx: number) => {
        let n = e.marker.length;
        for (const s of e.spans) {
          n += s.text.length;
        }
        return b.id + '-' + idx + '#' + n;
      })
    }
    .alignItems(HorizontalAlign.Start)
    .width('100%')
  }

  /**
   * GFM table.
   *
   * Equal-weight columns that WRAP rather than a horizontally scrolling grid: on a
   * phone the bubble is narrow and every column matters, so wrapping keeps all the
   * content reachable, while a scroller would hide columns behind a gesture the user
   * has no reason to expect. The cost is that a very wide table gets cramped.
   *
   * A streaming table has its header (and delimiter) before any body row, so it
   * renders header-only and grows — which is why blockKey() counts row content.
   */
  @Builder
  private table(b: RichBlock) {
    Column() {
      ForEach(b.rows, (row: TableRow, rowIdx: number) => {
        Row() {
          ForEach(row.cells, (cell: InlineSpan[], colIdx: number) => {
            Column() {
              this.inline(cell, (rowIdx === 0 ? 14 : 14.5) * this.fontScale,
                          rowIdx === 0 ? $r('app.color.text_primary') : $r('app.color.text_secondary'),
                          rowIdx === 0 ? FontWeight.Bold : FontWeight.Normal)
            }
            .layoutWeight(1)
            .padding({ left: 8, right: 8, top: 6, bottom: 6 })
            .alignItems(colIdx < b.aligns.length && b.aligns[colIdx] === 'right'
              ? HorizontalAlign.End
              : colIdx < b.aligns.length && b.aligns[colIdx] === 'center'
                ? HorizontalAlign.Center
                : HorizontalAlign.Start)
          }, (cell: InlineSpan[], colIdx: number) => rowIdx + ':' + colIdx)
        }
        .width('100%')
        .alignItems(VerticalAlign.Top)
        // The header keeps a tint so it reads as a header even when a row wraps to
        // several lines and the alignment alone stops carrying that.
        .backgroundColor(rowIdx === 0 ? $r('app.color.bg_overlay') : Color.Transparent)
        .border({ width: { bottom: 0.5 }, color: $r('app.color.divider') })
      }, (row: TableRow, rowIdx: number) => rowIdx.toString())
    }
    .width('100%')
    .margin({ top: 2, bottom: 2 })
    .borderRadius(6)
    .clip(true)
    .border({ width: 0.5, color: $r('app.color.divider') })
  }

  /**
   * Display math. Only a CLOSED block goes to the renderer (half a formula is
   * garbage — the `closed` contract); while the SVG is pending (or MathJax
   * rejected the TeX) the source shows dimmed, so there is never a hole.
   */
  @Builder
  private mathBlock(b: RichBlock) {
    if (b.closed && this.mathReady(b.body, true)) {
      Scroll() {
        Image(this.mathUrl(b.body, true))
          .width(this.mathW(b.body, true))
          .height(this.mathH(b.body, true))
          .objectFit(ImageFit.Contain)
      }
      .scrollable(ScrollDirection.Horizontal)
      .scrollBar(BarState.Off)
      .align(Alignment.Start)
      .width('100%')
      .margin({ top: 2, bottom: 2 })
    } else {
      Column({ space: 2 }) {
        if (!b.closed) {
          Text('𝑓𝑥 ' + getContext(this).resourceManager.getStringSync($r('app.string.chat_block_generating').id))
            .fontSize(12 * this.fontScale)
            .fontColor($r('app.color.text_tertiary'))
        }
        Text(b.body)
          .fontFamily('monospace')
          .fontSize(12.5 * this.fontScale)
          .fontColor($r('app.color.text_secondary'))
          .maxLines(6)
          .textOverflow({ overflow: TextOverflow.Ellipsis })
      }
      .alignItems(HorizontalAlign.Start)
      .width('100%')
      .padding(8)
      .backgroundColor($r('app.color.bg_overlay'))
      .borderRadius(8)
    }
  }

  /**
   * Chart. Same contract as math: only a CLOSED block (complete option JSON)
   * goes to ECharts; pending/streaming/failed falls back to the placeholder.
   */
  @Builder
  private chartBlock(b: RichBlock) {
    if (b.closed && this.chartReady(b.body)) {
      Image(this.chartUrl(b.body))
        .width('100%')
        .aspectRatio(this.chartAspect(b.body))
        .objectFit(ImageFit.Contain)
        .borderRadius(8)
        .margin({ top: 2, bottom: 2 })
    } else {
      this.placeholder(b)
    }
  }

  /**
   * ```svg figure. Unlike math and charts there is nothing to RENDER — the source
   * is the image — so RenderHost only validates it and writes the file.
   *
   * Sized by the bubble's width and the source's own aspect ratio, not by the
   * figure's declared pixels: a model picks a canvas without knowing the screen, and
   * a diagram authored at 900px would otherwise overflow or be scaled by luck. When
   * the source declares no size at all, 4:3 is the shape a diagram usually is.
   *
   * onError is the second half of the guarantee: validation proves only that we are
   * willing to draw the source, never that the rasterizer can. A file that draws
   * blank demotes itself and the block falls back to its XML, which is what the
   * reader had before this feature existed.
   */
  @Builder
  private svgBlock(b: RichBlock) {
    if (b.closed && this.svgReady(b.body)) {
      Image(this.svgUrl(b.body))
        .width('100%')
        .aspectRatio(this.svgAspect(b.body))
        .objectFit(ImageFit.Contain)
        .margin({ top: 2, bottom: 2 })
        .onError(() => {
          RenderHost.markSvgFailed(b.body);
        })
    } else if (!b.closed) {
      Text('🖼 ' + getContext(this).resourceManager.getStringSync($r('app.string.chat_block_generating').id))
        .fontSize(12 * this.fontScale)
        .fontColor($r('app.color.text_tertiary'))
        .width('100%')
        .padding(8)
        .backgroundColor($r('app.color.bg_overlay'))
        .borderRadius(8)
    } else {
      // Refused, unwritable, or undrawable: the source, exactly as a code block —
      // the reader keeps everything the model produced.
      this.codeBlock(b)
    }
  }

  /** chart fallback: raw source once closed, a busy hint while still streaming. */
  @Builder
  private placeholder(b: RichBlock) {
    Column({ space: 2 }) {
      Text('📊 ' + this.hintFor(b))
        .fontSize(12 * this.fontScale)
        .fontColor($r('app.color.text_tertiary'))
      if (b.closed) {
        Text(b.body)
          .fontFamily('monospace')
          .fontSize(12.5 * this.fontScale)
          .fontColor($r('app.color.text_secondary'))
          .maxLines(6)
          .textOverflow({ overflow: TextOverflow.Ellipsis })
      }
    }
    .alignItems(HorizontalAlign.Start)
    .width('100%')
    .padding(8)
    .backgroundColor($r('app.color.bg_overlay'))
    .borderRadius(8)
  }

  private hintFor(b: RichBlock): string {
    if (!b.closed) {
      return getContext(this).resourceManager.getStringSync($r('app.string.chat_block_generating').id);
    }
    return getContext(this).resourceManager.getStringSync($r('app.string.chat_chart_placeholder').id);
  }
}

/**
 * TypingDots — the waiting indicator shown in the assistant's place from the
 * moment a turn is sent until its first token.
 *
 * Its own @Component rather than a @Builder on RichMessage, because the pulse is
 * a state CHANGE and only a change animates. A builder would have had to flip a
 * flag on the enclosing component, where it stays flipped: aboutToAppear runs
 * before the first render (so the property is already at its end value and
 * nothing animates), and a flag set on the first turn is still set on the
 * second, so the dots would sit there frozen. A fresh component per appearance
 * starts at `lit = false` every time, and onAppear -- which fires AFTER the
 * first frame -- moves it, which is what ArkUI animates.
 *
 * Each dot runs the same cycle offset by its index, so the pulse travels left to
 * right. Opacity only: nothing here changes layout, so the bubble cannot twitch.
 */
@Component
export struct TypingDots {
  @Prop fontScale: number = 1;
  @State private lit: boolean = false;

  build() {
    Row({ space: 4 }) {
      ForEach([0, 1, 2], (i: number) => {
        Circle()
          .width(6)
          .height(6)
          .fill($r('app.color.text_tertiary'))
          .opacity(this.lit ? 1 : 0.25)
          .animation({
            duration: 400,
            delay: i * 160,
            curve: Curve.EaseInOut,
            iterations: -1,
            playMode: PlayMode.Alternate
          })
      }, (i: number) => i.toString())
    }
    // About one line of body text, so the swap to real content does not jump.
    .height(22 * this.fontScale)
    .alignItems(VerticalAlign.Center)
    .onAppear(() => { this.lit = true; })
  }
}
