/**
 * RichMessage — an assistant message body: optional collapsible thinking block
 * followed by the markdown-rendered answer.
 *
 * Consumes plain strings (not MessagePart[]) on purpose: strings diff cheaply
 * under @Prop, and phase 1's part order is fixed (thinking, then text). The
 * ForEach over blocks keys on RichBlock.id — kind + start offset, stable across
 * streaming appends — so a block that merely grew updates in place and the
 * ones before it are untouched.
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
import { parseBlocks, RichBlock, InlineSpan, ListEntry } from '../core/Markdown';
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

  // Manual fold override: unset until the user taps, then it wins.
  @State private userToggled: boolean = false;
  @State private manualExpand: boolean = false;

  // Math cache accessors. All read mathEpoch so ArkUI tracks the dependency;
  // the first miss queues the render (see MathRenderer.lookup).
  private mathOf(tex: string, display: boolean): RenderedImage | undefined {
    const epoch = this.renderEpoch;   // dependency anchor; value unused
    return epoch >= 0 ? RenderHost.lookupMath(tex, display) : undefined;
  }

  private chartOf(optionJson: string): RenderedImage | undefined {
    const epoch = this.renderEpoch;   // dependency anchor; value unused
    return epoch >= 0 ? RenderHost.lookupChart(optionJson) : undefined;
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

  private thinkingExpanded(): boolean {
    if (this.userToggled) {
      return this.manualExpand;
    }
    return this.streaming && this.text.length === 0;
  }

  build() {
    Column({ space: 6 }) {
      if (this.thinking.length > 0) {
        this.thinkingBlock()
      }
      ForEach(parseBlocks(this.text), (b: RichBlock) => {
        this.block(b)
      }, (b: RichBlock) => b.id)
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
          .fontColor('#8A8F99')
        Text(this.thinkingExpanded() ? '▾' : '▸')
          .fontSize(12)
          .fontColor('#8A8F99')
      }
      .onClick(() => {
        this.manualExpand = !this.thinkingExpanded();
        this.userToggled = true;
      })

      if (this.thinkingExpanded()) {
        Text(this.thinking)
          .fontSize(13 * this.fontScale)
          .fontColor('#8A8F99')
          .lineHeight(19 * this.fontScale)
      }
    }
    .alignItems(HorizontalAlign.Start)
    .width('100%')
    .padding({ left: 8, right: 8, top: 6, bottom: 6 })
    .backgroundColor('#00000008')
    .borderRadius(8)
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
    } else if (b.kind === 'list') {
      this.list(b)
    } else if (b.kind === 'math') {
      this.mathBlock(b)
    } else if (b.kind === 'chart') {
      this.chartBlock(b)
    } else {
      this.paragraph(b)
    }
  }

  @Builder
  private inline(spans: InlineSpan[], size: number, color: string, weight: FontWeight) {
    Text() {
      ForEach(spans, (s: InlineSpan) => {
        if (s.kind === 'code') {
          Span(s.text)
            .fontFamily('monospace')
            .fontSize(size * 0.92)
            .fontColor('#C2415A')
            .textBackgroundStyle({ color: '#00000010', radius: 3 })
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
              .fontColor('#5A6068')
          }
        } else {
          Span(s.text)
            .fontWeight(s.kind === 'bold' || s.kind === 'bolditalic' ? FontWeight.Bold : weight)
            .fontStyle(s.kind === 'italic' || s.kind === 'bolditalic' ? FontStyle.Italic : FontStyle.Normal)
        }
      })
    }
    .fontSize(size)
    .fontColor(color)
    .lineHeight(size * 1.5)
  }

  @Builder
  private paragraph(b: RichBlock) {
    this.inline(b.spans, 16 * this.fontScale, '#1A1A1A', FontWeight.Normal)
  }

  @Builder
  private heading(b: RichBlock) {
    Column() {
      this.inline(b.spans,
        (b.level <= 1 ? 21 : b.level === 2 ? 19 : b.level === 3 ? 17.5 : 16.5) * this.fontScale,
        '#1A1A1A', FontWeight.Bold)
    }
    .margin({ top: b.id.endsWith(':0') ? 0 : 4, bottom: 2 })
  }

  @Builder
  private codeBlock(b: RichBlock) {
    Column() {
      if (b.language.length > 0) {
        Text(b.language)
          .fontSize(11 * this.fontScale)
          .fontColor('#8A8F99')
          .width('100%')
          .padding({ left: 10, top: 5 })
      }
      Scroll() {
        Text(b.body)
          .fontFamily('monospace')
          .fontSize(13.5 * this.fontScale)
          .fontColor('#2A2E36')
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
    .backgroundColor('#FFFFFF')
    .border({ width: 1, color: '#E5E5E5' })
    .borderRadius(8)
  }

  @Builder
  private quote(b: RichBlock) {
    Row({ space: 8 }) {
      Column()
        .width(3)
        .backgroundColor('#C9CDD4')
        .borderRadius(2)
      Column() {
        this.inline(b.spans, 15 * this.fontScale, '#5A6068', FontWeight.Normal)
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
            .fontColor('#5A6068')
            .constraintSize({ minWidth: 18 })
          Column() {
            this.inline(e.spans, 16 * this.fontScale, '#1A1A1A', FontWeight.Normal)
          }
          .layoutWeight(1)
          .alignItems(HorizontalAlign.Start)
        }
        .alignItems(VerticalAlign.Top)
        .padding({ left: e.depth * 14 })
        .width('100%')
      }, (e: ListEntry, idx: number) => b.id + '-' + idx)
    }
    .alignItems(HorizontalAlign.Start)
    .width('100%')
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
            .fontColor('#8A8F99')
        }
        Text(b.body)
          .fontFamily('monospace')
          .fontSize(12.5 * this.fontScale)
          .fontColor('#5A6068')
          .maxLines(6)
          .textOverflow({ overflow: TextOverflow.Ellipsis })
      }
      .alignItems(HorizontalAlign.Start)
      .width('100%')
      .padding(8)
      .backgroundColor('#00000006')
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

  /** chart fallback: raw source once closed, a busy hint while still streaming. */
  @Builder
  private placeholder(b: RichBlock) {
    Column({ space: 2 }) {
      Text('📊 ' + this.hintFor(b))
        .fontSize(12 * this.fontScale)
        .fontColor('#8A8F99')
      if (b.closed) {
        Text(b.body)
          .fontFamily('monospace')
          .fontSize(12.5 * this.fontScale)
          .fontColor('#5A6068')
          .maxLines(6)
          .textOverflow({ overflow: TextOverflow.Ellipsis })
      }
    }
    .alignItems(HorizontalAlign.Start)
    .width('100%')
    .padding(8)
    .backgroundColor('#00000006')
    .borderRadius(8)
  }

  private hintFor(b: RichBlock): string {
    if (!b.closed) {
      return getContext(this).resourceManager.getStringSync($r('app.string.chat_block_generating').id);
    }
    return getContext(this).resourceManager.getStringSync($r('app.string.chat_chart_placeholder').id);
  }
}
