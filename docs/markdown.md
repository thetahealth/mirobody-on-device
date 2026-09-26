# Markdown

The syntax set a mirobody client is expected to render, and where each client stands
against it. **This file is the target, not a report** — a client that renders less has a gap
to close, and a client that renders more has found something worth adding here first.

Deliberately not under any client directory: every client answers to it.

> **Clients in this repo (2026-09).** The web client (`htdoc/`), Qt, Electron and
> the WeChat Mini Program left this repo when it narrowed to the phone; they are
> preserved at the `v2-full-2026-08` tag. The phone apps are moving to a native
> shell around the [mirobody-web](https://github.com/thetahealth/mirobody-web)
> build in a WebView, so the web renderer becomes theirs too. Rows below for the
> removed clients are history.

Scope is set by what the models actually write. A construct earns a row here because chat
replies contain it, not because CommonMark lists it; §5 records what is left out and why, so
"unsupported" never has to be re-derived from a silence.

The HarmonyOS column is measured, not asserted — its parser is the only hand-rolled one, and
the tables below were produced by transpiling
[`core/Markdown.ets`](../harmony/entry/src/main/ets/core/Markdown.ets) and feeding it one
construct per case. **Source of truth is always the code**; change the code first, then this
file.

---

## 1. Renderers

| Client | Engine | File |
|---|---|---|
| HarmonyOS | **hand-rolled** — the only one where coverage is a decision rather than a dependency | [`core/Markdown.ets`](../harmony/entry/src/main/ets/core/Markdown.ets) → [`components/RichMessage.ets`](../harmony/entry/src/main/ets/components/RichMessage.ets) |
| Web | marked (GFM) + KaTeX, DOMPurify-sanitized before `innerHTML` — **plus** `$…$` / `$$…$$` math, ` ```svg ` and ` ```echarts ` fences and a link-scheme allowlist, all as marked extensions | [`htdoc/src/markdown.js`](https://github.com/thetahealth/mirobody-on-device/blob/v2-full-2026-08/htdoc/src/markdown.js), [`htdoc/src/charts.js`](https://github.com/thetahealth/mirobody-on-device/blob/v2-full-2026-08/htdoc/src/charts.js) |
| iOS | MarkdownUI (GFM); a message containing math is re-routed to an offline KaTeX WebView | [`MarkdownText.swift`](../ios/Mirobody/UI/Chat/MarkdownText.swift) |
| Android | Markwon (commonmark-java) + tables, strikethrough, HTML, linkify, JLatexMath — **plus hand-written** `$…$` math, SVG figures, fence splitting and a link-scheme allowlist | [`MarkdownText.kt`](../android/app/src/main/java/ai/thetahealth/mirobody/ui/chat/MarkdownText.kt), [`InlineMath.kt`](../android/app/src/main/java/ai/thetahealth/mirobody/ui/chat/InlineMath.kt), [`SvgFigure.kt`](../android/app/src/main/java/ai/thetahealth/mirobody/ui/chat/SvgFigure.kt), [`MarkdownSegments.kt`](../android/app/src/main/java/ai/thetahealth/mirobody/ui/chat/MarkdownSegments.kt) |
| Qt | `Text.MarkdownText` — Qt's built-in importer. No math. | [`MessageDelegate.qml`](https://github.com/thetahealth/mirobody-on-device/blob/v2-full-2026-08/qt/qml/MessageDelegate.qml) |
| Mini program | **none** — `{{item.content}}` in a plain `<view>`, so markdown shows as source | [`chat.wxml:60`](https://github.com/thetahealth/mirobody-on-device/blob/v2-full-2026-08/miniapp/pages/chat/chat.wxml) |

Legend for every table below: ✅ renders · ⚠️ renders differently, see the note · ❌ shows as
literal source · — not applicable to that client.

> **How much to trust a cell.** Measured: the whole **HarmonyOS** column (by running its
> parser), the **Android** math / image / SVG rows and everything Android contributes to §4
> (by reading Markwon's own sources out of the Gradle cache, and by
> `MarkdownRenderingTest`), and the rest of **§4** (by reading each client's extension
> registration and stream-event handling). Everything else in §2–§3 states the **documented
> dialect of the engine named in §1** rather than a construct-by-construct run of that
> client. Treat those as the claim to check when you close a gap, and correct the cell here
> when the device disagrees.

---

## 2. Blocks

| Construct | Harmony | Web | iOS | Android | Qt | Notes |
|---|:--:|:--:|:--:|:--:|:--:|---|
| ATX heading `#` … `######` | ✅ | ✅ | ✅ | ✅ | ✅ | A space after the hashes is required (`#NoSpace` is a paragraph, per CommonMark). Harmony does not strip a closing sequence: `## Two ##` shows `Two ##`. |
| Setext H1 (`===` underline) | ✅ | ✅ | ✅ | ✅ | ✅ | |
| Setext H2 (`---` underline) | ⚠️ | ✅ | ✅ | ✅ | ✅ | Harmony reads it as a thematic break — deliberate, §5. |
| Paragraph, blank line | ✅ | ✅ | ✅ | ✅ | ✅ | |
| Thematic break `---` `***` `___` | ✅ | ✅ | ✅ | ✅ | ✅ | Three or more marks alone on the line; `- - -` counts. |
| Fenced code ` ``` ` / `~~~` | ✅ | ✅ | ✅ | ✅ | ✅ | Fence length and character both matter: a ` ```` ` block closes only on four or more backticks. Info string becomes the label. |
| Indented code block (4 spaces) | ✅ | ✅ | ✅ | ✅ | ✅ | Only where CommonMark allows one — not interrupting a paragraph, and not inside a list item. |
| Block quote `>` | ⚠️ | ✅ | ✅ | ✅ | ✅ | Harmony flattens: every leading `>` is stripped and the body is **inline content only**, so a list or code block inside a quote renders as inline text. |
| Bullet list `-` `*` `+` | ✅ | ✅ | ✅ | ✅ | ✅ | Harmony draws all three as `•`. |
| Ordered list `1.` `1)` | ✅ | ✅ | ✅ | ✅ | ✅ | Harmony normalizes `)` to `.` and keeps the start number as written. |
| Nested lists | ✅ | ✅ | ✅ | ✅ | ✅ | Harmony: two leading spaces per level. |
| List item continuation line | ✅ | ✅ | ✅ | ✅ | ✅ | Harmony requires the continuation to be indented ≥ 2 — §5. |
| Task list `- [ ]` / `- [x]` | ✅ | ✅ | ✅ | ⚠️ | ✅ | Harmony draws `☐` / `☑` in place of the bullet. Markwon has no task-list extension wired in. |
| GFM pipe table | ✅ | ✅ | ✅ | ✅ | ✅ | `:--` / `:-:` / `--:` alignment; outer pipes optional; `\|` escapes a pipe in a cell; ragged rows are padded so the grid survives. |

## 3. Inline

| Construct | Harmony | Web | iOS | Android | Qt | Notes |
|---|:--:|:--:|:--:|:--:|:--:|---|
| Backslash escape `\*` | ✅ | ✅ | ✅ | ✅ | ✅ | ASCII punctuation only, so `C:\path` and `\n` keep their backslash. |
| Code span `` `x` `` | ✅ | ✅ | ✅ | ✅ | ✅ | The opening run's length is the closer, so ``` ``a ` b`` ``` holds a backtick. One leading and trailing space is stripped. |
| Emphasis `*a*` `**a**` `***a***` | ✅ | ✅ | ✅ | ✅ | ✅ | |
| Emphasis `_a_` `__a__` | ✅ | ✅ | ✅ | ✅ | ✅ | Underscore is intraword-blind, so `snake_case_name` stays prose. A star is not: `sn*ake*case` emphasizes. |
| Strikethrough `~~a~~` | ✅ | ✅ | ✅ | ✅ | ✅ | |
| Nested styles | ✅ | ✅ | ✅ | ✅ | ✅ | They compose: `**~~a~~**` is bold **and** struck, and `*a **b** c*` keeps the inner bold. |
| Link `[text](url)` | ✅ | ✅ | ✅ | ✅ | ✅ | Tappable. A `"title"` is parsed and dropped. Harmony opens only `http`, `https`, `mailto` — §5. |
| Autolink `<http://…>` | ✅ | ✅ | ✅ | ✅ | ✅ | `http`, `https`, `mailto`. |
| Bare URL | ✅ | ✅ | ✅ | ✅ | ⚠️ | `http://`, `https://`, `www.`. Trailing sentence punctuation is left out of the link. Bare email addresses are **not** linkified on Harmony. |
| Image `![alt](url)` | ⚠️ | ✅ | ✅ | ⚠️ | ✅ | Harmony shows `🖼 alt` and **never fetches the URL** — §5. Android renders the alt text as plain prose with no marker and no fetch: `CorePlugin` registers no `Image` span factory and `ImagesPlugin` is not used, so the visitor takes its "no span factory → ignore" branch. Same privacy outcome as Harmony, arrived at by omission rather than by decision. |
| HTML entity `&amp;` `&#8212;` | ✅ | ✅ | ✅ | ✅ | ✅ | Harmony decodes the two dozen named entities models actually write, plus every numeric form (`&#38;`, `&#x26;`). An unknown name stays literal. |

## 4. Ours, not Markdown

Three separate things, and they are not the same KIND of thing — which is the part that keeps
getting lost. Math is a genuine markdown-level extension: it lives in the message text and
every client parses it out. Charts mostly are not markdown at all. SVG is a fenced block on
two clients and nothing on the rest.

### 4.1 LaTeX math

In the message text, delimited. This is the one true syntax extension of the three.

| Syntax | | Harmony | Web | iOS | Android | Qt |
|---|---|:--:|:--:|:--:|:--:|:--:|
| `$…$` | inline | ✅ | ✅ | ✅ | ✅ | ❌ |
| `\(…\)` | inline | ✅ | ❌ | ❌ | ❌ | ❌ |
| `$$…$$` | display | ✅ | ✅ | ✅ | ✅ | ❌ |
| `\[…\]` | display | ✅ | ❌ | ❌ | ❌ | ❌ |

**Android's `$…$` is ours, and it displaces Markwon's rather than adding to it.** Markwon's
`JLatexMathInlineProcessor` is `Pattern.compile("(\\${2})([\\s\\S]+?)\\1")` — **two** dollar
signs, mandatory — so `inlinesEnabled(true)` turns on inline `$$…$$`, not `$…$`. That is only
the visible half. `InlineProcessor.match()` runs `matcher.find()` over a *region* instead of
anchoring at the current index, so on `costs $5, and $$x$$` their processor is invoked at the
`$` before `5`, matches the `$$x$$` further along, and advances past it — **silently dropping
", and " from the reply**. [`InlineMath.kt`](../android/app/src/main/java/ai/thetahealth/mirobody/ui/chat/InlineMath.kt)
is therefore a *total* handler for `$` that always returns a node (a literal `$` when nothing
matches), registered ahead of theirs on the factory builder so theirs is never reached.
`inlinesEnabled(true)` stays on for one reason: it is also what registers the visitor and span
for an inline math node.

`$…$` is never taken at face value anywhere, because a dollar sign is also money. Harmony's
rule (`looksLikeTex`) makes a TeX metacharacter (`\ ^ _ { }`) decisive; failing that the body
must be short and expression-shaped, and CJK inside means the pair spans prose rather than
delimiting a formula, so `costs $100 and $200` stays text. Android runs a **port of that same
function**, so the two hand-written clients agree construct for construct. Web and iOS use a
regex of equivalent intent: opener not followed by a space, closer not preceded by one and not
followed by a digit.

`\(…\)` and `\[…\]` are Harmony-only. They are unambiguous — no currency reading to guard
against — so a model prompted toward them needs no heuristic at all. Worth considering for the
other clients before extending the dollar guards further.

### 4.2 ECharts charts — **not markdown on most clients**

A chart normally reaches a client as a **stream event**, not as text. The model calls the
`render_chart` tool ([`res/mcp_tools/render_chart.cpp`](../res/mcp_tools/render_chart.cpp)),
the core's `ChartFilter` ([`src/chat/event/filter/chart.cpp`](../src/chat/event/filter/chart.cpp))
lifts the ECharts option out of the call, and the transport emits `{"type":"chart", …}` which
the client hands to `echarts.setOption()`. The message text never carries it, and no markdown
parser is involved.

Every client is on that event, over both transports:

| | Harmony | Web | iOS | Android | Qt |
|---|:--:|:--:|:--:|:--:|:--:|
| `chart` event → `setOption()` | ✅ | ✅ | ✅ | ⚠️ | ❌ |
| ` ```echarts ` fence, drawn where it stands | ✅ | ✅ | ❌ | ✅ | ❌ |

**The Android cell is the one to distrust.** Its first run on a real device drew nothing —
found via a ` ```echarts ` fence, but the event path is the same `EChartsView`, so treat both
as unconfirmed until a device says otherwise.

The device named the cause once the page was made to report its own failures:
`chart-theme.js did not load`, while `echarts.min.js` had. The asymmetry is the diagnosis —
**that file's only top-level statement is `})(globalThis)`, and `globalThis` is ES2020
(Chrome 71+)**, so on an older WebView it throws a ReferenceError and the file dies before
defining `mbChartTheme`, while echarts, transpiled to ES5, loads fine. Shimmed in the host
page (`if (typeof globalThis === 'undefined') window.globalThis = window`) rather than in the
script, which is vendored **byte-identical to four clients** — harmony/rawfile/render,
htdoc/static, ios/Resources and android/assets. **Any client hosting this file in an
older engine has the same latent break**, and closing it host-side is what keeps those four
copies identical.

Two sizing bugs were fixed alongside it: the container was `height: 100%`, which resolves to 0
while the WebView is still laying out (echarts sizes its canvas once at init and no resize
event follows, so a 0-tall canvas stays 0-tall), and `useWideViewPort` was unset, so the
viewport meta tag was ignored and one CSS pixel was not one dp.

The lesson worth keeping: the error hook has to be installed **before** the scripts it is
meant to catch. The first attempt put `window.onerror` in `<body>`, after both `<head>`
scripts had already failed, so it could only report *that* a global was missing, not why. And
`window.onerror` never fires for a subresource that could not be fetched — telling "could not
fetch" apart from "threw" needs a capture-phase `error` listener.

The event reaches a client two ways, and they now carry the same vocabulary:

- **HTTP/SSE** — `{"type":"chart","content":<title>,"chart":{…option…}}`, for Web, iOS and
  Android.
- **C ABI** — `mirobody_chat_handler("chart", <option JSON>, …)`, for a client that links the
  core in-process (Harmony). Flat `(type, content)`, so the option travels as the content
  string rather than nested.

Both come off the **same** `make_event_pipeline()`. The C ABI used to hand raw `llm::Event`s
to its callback, which made every filter server-only: an embedded client saw the raw
`render_chart` tool triple where an HTTP client saw one clean event, and had to re-implement
`ChartFilter` itself. Now a filter added in one place reaches both transports by construction.

**A chart never travels inside the answer text.** On Harmony the option is appended to the
message's *display* text as a ` ```echarts ` fence — that is the markdown parser's chart IR,
and it is what puts the figure back between the paragraph that introduces it and the one that
reads it. Android understands the same fence
([`MarkdownSegments.kt`](../android/app/src/main/java/ai/thetahealth/mirobody/ui/chat/MarkdownSegments.kt)),
though its `chart` events still render appended after the text rather than in place; the fence
is what lets a *written* figure sit where it was introduced. Web understands it as of the
slash-command round: the marked extension emits an empty `div.mb-chart` carrying the option in
a data attribute, and [`charts.js`](https://github.com/thetahealth/mirobody-on-device/blob/v2-full-2026-08/htdoc/src/charts.js) fills it once the sanitized HTML is
in the DOM — an ECharts option is a live object, so no renderer can produce it as markup.
Hydration is deliberately skipped on the throttled streaming re-render (a canvas cannot outlive
its container being replaced ten times a second) and the previous instances are disposed before
each render, since echarts keys its registry by the very nodes `innerHTML` drops. Web's `chart`
events still render in the visuals block under the reply, as Android's do. On Harmony,
`ChatMessage.content` is stripped of those fences before the message is stored.
`content` is replayed to the model on every later turn and is what a long press copies; an
ECharts option is hundreds of tokens of plumbing in both places. The `parts[]` text part keeps
the display form, so a reloaded conversation still draws its charts. Same separation
`thinking` already had, for the same reason.

Harmony's **cloud lane has no charts at all**, and structurally so rather than as a gap: it is
BYOK straight to the provider (OpenAI-shaped SSE, `choices[0].delta.content`), with no mirobody
event protocol and no tool pipeline for `render_chart` to run in. Charts need the native lane.

### 4.3 SVG figures

A ` ```svg ` fenced block whose body is drawn as an actual figure instead of escaped source.

| | Harmony | Web | iOS | Android | Qt |
|---|:--:|:--:|:--:|:--:|:--:|
| ` ```svg ` | ✅ | ✅ | ✅ | ✅ | ❌ |

Web and iOS register it as a marked block extension (`svgFence`, same tokenizer in
[`htdoc/src/markdown.js`](https://github.com/thetahealth/mirobody-on-device/blob/v2-full-2026-08/htdoc/src/markdown.js) and
[`ios/…/math/render.html`](../ios/Mirobody/Resources/math/render.html)) and sanitize with
DOMPurify before it reaches the DOM. On Qt the fence is an ordinary code block, so the reader
gets the XML.

Harmony needs no renderer for this one — the source *is* the file, so it skips the WebView
that math and charts go through and writes the body straight to disk for a native `Image`.
Android is the same shape with a different pipe: Coil's `SvgDecoder` is registered app-wide
already ([`MirobodyApp.kt`](../android/app/src/main/java/ai/thetahealth/mirobody/MirobodyApp.kt)),
so the body goes to it as a `ByteArray` and the fence costs no new dependency —
[`SvgFigure.kt`](../android/app/src/main/java/ai/thetahealth/mirobody/ui/chat/SvgFigure.kt)
is a port of the three rules below, not of a renderer.

Three rules make that safe and useful:

- **Validate and refuse, never rewrite.** Model-authored markup is untrusted, and stripping
  dangerous constructs out of untrusted markup by text surgery is whack-a-mole against
  whoever writes the input. A source carrying `<script>`, `<foreignObject>`, `<use>`, a
  DOCTYPE/ENTITY, an `on…=` handler, or any `href`/`src` that is not a same-document
  fragment is simply **not drawn** — the block shows its XML instead. A diagram needs none of
  those; the external-reference rule is the same read-receipt concern that markdown images
  are refused for (§5).
- **Sized by the bubble, shaped by the source.** Width is 100% of the bubble and the aspect
  ratio comes from `viewBox` (then `width`/`height`, but a percentage counts as unknown, and
  `stroke-width` is not a size). A model picks a canvas without knowing the screen, so its
  declared pixels are not honoured. No size at all → 4:3.
- **`onError` demotes.** Validation proves only that we are *willing* to draw the source, not
  that ArkUI's rasterizer can. A figure that draws blank marks itself failed and falls back to
  the source, so the worst case is what the reader had before the feature existed.

### How Harmony draws the rendered ones

Formula and chart both go through one hidden WebView
([`core/RenderHost.ets`](../harmony/entry/src/main/ets/core/RenderHost.ets)): MathJax renders
TeX to SVG, ECharts renders an option to PNG, each result is written to a file, and the message
list draws a native `Image` from it. The list itself never hosts a WebView — which is what
keeps it scrolling natively, and what makes the files a cache that survives the launch.

---

## 5. Left out, and why

Not a backlog. Each of these was looked at and declined; reopen one with a reason, not with a
spec citation.

**Raw HTML — refused, not missing.** Model output is untrusted. The web client can afford
HTML only because DOMPurify sanitizes it before `innerHTML`; Harmony has no sanitizer and no
HTML renderer, and building one to display text a language model produced is a large attack
surface bought for nothing a model needs. `<div>`, `<b>` and `<!-- -->` all render as literal
source on Harmony. **Android is the exception, and a narrow one**: Markwon's `HtmlPlugin` is
registered, but it maps a fixed tag list to spans rather than building a DOM — there is no
script engine, and `<img>` loads nothing because no `ImagesPlugin` is present. A tag outside
its list is dropped, not rendered. That is a different risk from `innerHTML`, not the same one
accepted.

**Images are shown, never fetched (Harmony).** Requesting a URL a model wrote hands a third
party a beacon that fires when this user reads this reply — in a health app, from a message
about their own results. The alt text renders with a `🖼` marker instead. Figures that are
genuinely ours go through ` ```echarts `, which renders locally.

**Link schemes are allowlisted (Harmony, Android, Web).** Only `http`, `https` and `mailto` are
handed to the system. A model that writes `file://` or a custom scheme gets nothing, rather
than a launched intent chosen by generated text. Android needed a `LinkResolver` of its own
for this: Markwon's `LinkResolverDef` fires `ACTION_VIEW` on whatever the link says, and
coerces a scheme-less link to `https` on the way — the coercion is the right reading of
`www.foo.com`, so ours keeps it and then applies the allowlist to the result. Web tests the
scheme of the **resolved** URL in a DOMPurify `afterSanitizeAttributes` hook, so an ordinary
relative link still works and only an explicit foreign scheme loses its `href` (the text
stays, the click does nothing). This is narrower than DOMPurify's default, which also passes
`ftp`, `tel`, `sms`, `callto`, `cid` and `xmpp`.

**Setext H2 (`---` under a paragraph) is a thematic break (Harmony).** CommonMark reads it as
an underline. The failure is asymmetric: a model that ends a section and opens the next with a
`---` separator routinely omits the blank line, and turning the paragraph above into a giant
heading is a louder failure than a rule where a heading was meant. Models reach for `##` when
they want a heading. Setext H1 (`===`) has no competing reading and **is** supported.

**Link reference definitions and footnotes.** `[text][id]` with `[id]: url` elsewhere, and
`[^1]`, both need a document-wide pre-pass to collect definitions before any inline parsing —
which fights the streaming design, where a message is re-parsed on every token and the
definition may not have arrived yet. Models write inline links. Both render as literal source.

**Lazy list continuation (Harmony).** CommonMark folds an *unindented* line after a list item
into that item. Harmony requires ≥ 2 spaces. After a list, an unindented line is far more often
the next paragraph than more of the last bullet, and getting that wrong swallows a paragraph.

**Loose vs tight lists (Harmony).** `- a` / blank / `- b` renders as two adjacent lists rather
than one list with looser spacing. The difference is a few pixels of leading.

---

## 6. Non-obvious behaviour worth knowing

**Every newline survives.** CommonMark folds a soft line break inside a paragraph into a
space; Harmony keeps it as a line break. Model prose is written to be read with its line
structure intact, and CJK has no inter-word space to fold into. A consequence is that "hard
break" markers (two trailing spaces, a trailing backslash) are unnecessary — and, on Harmony,
inert.

**Streaming shapes the parser.** A message is re-parsed on every token, which is why:

- an unterminated marker stays literal — `some **bo` renders as written rather than eating the
  rest of the message;
- fenced code and display math render half-open, because text reads fine incomplete;
- math and chart blocks are withheld from their renderer until the closing fence arrives —
  half a formula or half a JSON option is garbage, not a preview;
- block ids are `<kind>:<start offset>`, never array indexes, so an append cannot re-key every
  block after it.

**A table needs its delimiter row.** A header-looking line alone is a paragraph containing
pipes, so a streaming table only becomes a table once its second line lands.

---

## 7. Adding a construct (HarmonyOS)

Branch order in `parseBlocks` is load-bearing:

- **Fences first** — a `# heading` inside a code block is code.
- **Thematic break before lists** — `- - -` and `* * *` satisfy the list matcher (marker plus a
  space), so a rule written that way would become a list of empty items.
- **Indented code after lists** — inside a list item, four spaces is item content, not code.

In `parseInline` the order is escape → code → math → image/link → autolink → emphasis →
entity, and every construct from image down **recurses** into its body, folding its own style
into the spans that come back. That is what makes `**[a `b` c](url)**` work, and it means
anything added there nests inside everything else for free.

`InlineSpan` separates **content kind** (`text` / `code` / `math` / `image`, mutually
exclusive) from **style flags** (`bold`, `italic`, `strike`, `href`, independent and
composable). Add a style as a flag; add a content type as a kind. Do not grow a kind that
means a *combination* — that is what the old `'bolditalic'` was, and it does not scale past
two styles.

A new block kind needs three edits: the branch in `parseBlocks`, the `kind` comment on
`RichBlock`, and a builder wired into **both** `block()` and `thinkingBody()` in
`RichMessage.ets` — the reasoning block renders its own subset, and a kind missing there falls
through to an empty `Text` and disappears silently.

If the new kind needs an image from `RenderHost`, add it to `needsRender()` too, or the
finished render never reaches the screen: the `ForEach` key would not change, and ArkUI skips
an item whose key is unchanged.
