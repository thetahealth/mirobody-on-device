/**
 * Markdown -> RichBlock[] — the intermediate representation the chat renderer
 * consumes (components/RichMessage.ets draws one component per block).
 *
 * Built for STREAMING text, which drives three design rules:
 *
 *  1. Stable ids. A block's id is "<kind>:<start offset>" — appends never move
 *     earlier text, so every already-started block keeps its id across
 *     re-parses and ArkUI's ForEach diff leaves its component alone. Never use
 *     array indexes: one block growing would re-key everything after it.
 *
 *  2. `closed`. Fenced code / $$ math / ```echarts arrive with their opener
 *     long before their terminator. Text-ish blocks render fine half-open;
 *     heavy blocks (math, chart) must NOT be handed to their renderer until
 *     closed — half a JSON option or a dangling formula is garbage. The flag
 *     says which state the block is in; the renderer decides what that means.
 *
 *  3. Pass order: fences are recognized before anything else on a line-by-line
 *     scan (a `# heading` inside a code block is code, not a heading), and
 *     inline styling never crosses a fence. Inline code spans likewise protect
 *     their contents from bold/italic parsing.
 *
 * Coverage (phase 1): headings, paragraphs, ordered/unordered lists (with
 * indent nesting), block quotes, fenced code, ```echarts -> chart, $$ -> math,
 * inline `code` / **bold** / *italic* / ***both***, GFM pipe tables. Links still
 * render as plain text (phase 2).
 */

export interface InlineSpan {
  kind: string;   // 'text' | 'bold' | 'italic' | 'bolditalic' | 'code' | 'math'
  text: string;   // for 'math': the TeX source (delimiters stripped)
}

/** One table cell's inline content; alignment lives on the block, per column. */
export interface TableRow {
  cells: InlineSpan[][];
}

export interface ListEntry {
  marker: string;        // '•' or '1.' — already display-ready
  depth: number;         // 0-based indent level
  spans: InlineSpan[];
}

export interface RichBlock {
  id: string;            // stable across streaming appends: kind + start offset
  kind: string;          // 'paragraph' | 'heading' | 'list' | 'quote' | 'code' | 'math' | 'chart' | 'table'
  closed: boolean;
  level: number;         // heading depth 1..6 (0 otherwise)
  language: string;      // code fence info string ('' otherwise)
  body: string;          // code / math / chart source text
  spans: InlineSpan[];   // heading / paragraph / quote inline content
  entries: ListEntry[];  // list items
  rows: TableRow[];      // table rows; rows[0] is the header
  aligns: string[];      // per column: 'left' | 'center' | 'right'
}

function makeBlock(kind: string, start: number): RichBlock {
  return {
    id: kind + ':' + start,
    kind: kind,
    closed: true,
    level: 0,
    language: '',
    body: '',
    spans: [],
    entries: [],
    rows: [],
    aligns: []
  };
}

//------------------------------------------------------------------------------
// Inline parsing
//------------------------------------------------------------------------------

/**
 * Is the text between two single `$` inline math, or is the `$` a currency sign?
 *
 * Requiring a TeX metacharacter (the earlier rule) rejected the single most common
 * inline formula there is — a bare variable, `$a$` — so models writing
 * "sides $a$ and $b$" showed the dollars literally. The tests, in order:
 *
 *   1. a TeX metacharacter is decisive          -> math
 *   2. a body opening with a digit is a price   -> not math ("$100 and $200" has
 *                                                  body "100 and ")
 *   3. CJK inside means the pair spans prose    -> not math
 *   4. otherwise short and expression-shaped is math, but an interior space is only
 *      allowed alongside an operator, so "five or " stays prose while "x + y" is math
 */
function looksLikeTex(body: string): boolean {
  for (let i = 0; i < body.length; i++) {
    const c = body.charAt(i);
    if (c === '\\' || c === '^' || c === '_' || c === '{' || c === '}') {
      return true;
    }
  }

  const t = body.trim();
  if (t.length === 0 || t.length > 48) {
    return false;
  }
  // A price: "$100", "$1,200.50". Math almost never opens a bare `$…$` with a digit.
  const first = t.charCodeAt(0);
  if (first >= 0x30 && first <= 0x39) {
    return false;
  }

  let hasSpace = false;
  let hasOperator = false;
  for (let i = 0; i < t.length; i++) {
    const c = t.charAt(i);
    const code = t.charCodeAt(i);
    if (code > 0x2e7f) {
      return false;             // CJK (and friends): the $ pair spans prose
    }
    if (c === ' ') {
      hasSpace = true;
    } else if (c === '+' || c === '-' || c === '*' || c === '/' || c === '='
               || c === '<' || c === '>' || c === '(' || c === ')' || c === ','
               || c === '.' || c === '|' || c === '!') {
      hasOperator = true;
    } else if (!((code >= 0x30 && code <= 0x39) ||
                 (code >= 0x41 && code <= 0x5a) ||
                 (code >= 0x61 && code <= 0x7a))) {
      return false;             // anything else is not expression-shaped
    }
  }
  // Several words with no operator is prose that merely sits between two dollars.
  return !hasSpace || hasOperator;
}

/**
 * text -> spans. Single left-to-right scan; `code` binds tightest (its content
 * is opaque), then inline math ($...$ with a TeX-shaped body, or \(...\)),
 * then *** ** * emphasis. Unterminated markers fall back to literal text, so a
 * streaming tail like "some **bo" renders as-is instead of eating the marker.
 */
export function parseInline(text: string): InlineSpan[] {
  const spans: InlineSpan[] = [];
  let plain = '';
  let i = 0;

  const flushPlain = (): void => {
    if (plain.length > 0) {
      spans.push({ kind: 'text', text: plain });
      plain = '';
    }
  };

  while (i < text.length) {
    const c = text.charAt(i);

    if (c === '`') {
      const end = text.indexOf('`', i + 1);
      if (end > i) {
        flushPlain();
        spans.push({ kind: 'code', text: text.substring(i + 1, end) });
        i = end + 1;
        continue;
      }
      // No closing backtick (yet) — literal.
      plain += c;
      i++;
      continue;
    }

    // \( ... \) — unambiguous inline math.
    if (c === '\\' && i + 1 < text.length && text.charAt(i + 1) === '(') {
      const end = text.indexOf('\\)', i + 2);
      if (end > i + 2) {
        flushPlain();
        spans.push({ kind: 'math', text: text.substring(i + 2, end).trim() });
        i = end + 2;
        continue;
      }
      plain += c;
      i++;
      continue;
    }

    // $ ... $ — inline math only when the body is TeX-shaped (see looksLikeTex),
    // so prices ("$100 and $200") stay literal.
    if (c === '$' && (i + 1 >= text.length || text.charAt(i + 1) !== '$')) {
      const end = text.indexOf('$', i + 1);
      if (end > i + 1) {
        const body = text.substring(i + 1, end);
        if (looksLikeTex(body)) {
          flushPlain();
          spans.push({ kind: 'math', text: body.trim() });
          i = end + 1;
          continue;
        }
      }
      plain += c;
      i++;
      continue;
    }

    if (c === '*') {
      let stars = 1;
      while (i + stars < text.length && text.charAt(i + stars) === '*' && stars < 3) {
        stars++;
      }
      const mark = text.substring(i, i + stars);
      const end = text.indexOf(mark, i + stars);
      // Emphasis needs a non-empty body and a matching closer.
      if (end > i + stars) {
        flushPlain();
        const kind = stars === 3 ? 'bolditalic' : (stars === 2 ? 'bold' : 'italic');
        spans.push({ kind: kind, text: text.substring(i + stars, end) });
        i = end + stars;
        continue;
      }
      plain += mark;
      i += stars;
      continue;
    }

    plain += c;
    i++;
  }
  flushPlain();
  return spans;
}

//------------------------------------------------------------------------------
// Block parsing
//------------------------------------------------------------------------------

interface Line {
  text: string;    // without the terminator
  start: number;   // char offset of the line's first char in the input
}

function splitLines(text: string): Line[] {
  const lines: Line[] = [];
  let start = 0;
  for (let i = 0; i <= text.length; i++) {
    if (i === text.length || text.charAt(i) === '\n') {
      lines.push({ text: text.substring(start, i), start: start });
      start = i + 1;
    }
  }
  return lines;
}

/** Leading-space count with tabs as 2. */
function indentOf(s: string): number {
  let n = 0;
  for (let i = 0; i < s.length; i++) {
    const c = s.charAt(i);
    if (c === ' ') {
      n += 1;
    } else if (c === '\t') {
      n += 2;
    } else {
      break;
    }
  }
  return n;
}

interface ListMatch {
  matched: boolean;
  ordered: boolean;
  marker: string;
  rest: string;
}

/** "- x" / "* x" / "+ x" / "12. x" / "12) x" (after indent removal). */
function matchListItem(s: string): ListMatch {
  const none: ListMatch = { matched: false, ordered: false, marker: '', rest: '' };
  if (s.length < 2) {
    return none;
  }
  const c = s.charAt(0);
  if ((c === '-' || c === '*' || c === '+') && s.charAt(1) === ' ') {
    return { matched: true, ordered: false, marker: '•', rest: s.substring(2) };
  }
  let d = 0;
  while (d < s.length && d < 3 && s.charAt(d) >= '0' && s.charAt(d) <= '9') {
    d++;
  }
  if (d > 0 && d + 1 < s.length &&
      (s.charAt(d) === '.' || s.charAt(d) === ')') && s.charAt(d + 1) === ' ') {
    return { matched: true, ordered: true, marker: s.substring(0, d) + '.', rest: s.substring(d + 2) };
  }
  return none;
}

/**
 * Split one table line into raw cells on UNESCAPED pipes, dropping the optional
 * leading/trailing delimiters GFM allows. `\|` is a literal pipe in a cell.
 */
function splitCells(line: string): string[] {
  const out: string[] = [];
  let cur = '';
  for (let i = 0; i < line.length; i++) {
    const c = line.charAt(i);
    if (c === '\\' && i + 1 < line.length && line.charAt(i + 1) === '|') {
      cur += '|';
      i++;
    } else if (c === '|') {
      out.push(cur);
      cur = '';
    } else {
      cur += c;
    }
  }
  out.push(cur);
  // A row written as |a|b| yields an empty first and last cell; those are the
  // optional outer delimiters, not real columns.
  if (out.length > 0 && out[0].trim().length === 0) {
    out.shift();
  }
  if (out.length > 0 && out[out.length - 1].trim().length === 0) {
    out.pop();
  }
  return out;
}

/**
 * Alignments if `line` is a GFM delimiter row (`|---|:--:|---:|`), else [].
 *
 * The delimiter row is what MAKES a table: a header-looking line on its own is
 * just a paragraph that happens to contain pipes, which also means a table being
 * streamed only becomes one once its second line lands.
 */
function tableAligns(line: string): string[] {
  const cells = splitCells(line);
  if (cells.length === 0) {
    return [];
  }
  const out: string[] = [];
  for (const raw of cells) {
    const c = raw.trim();
    const left = c.startsWith(':');
    const right = c.endsWith(':');
    const bar = c.substring(left ? 1 : 0, c.length - (right ? 1 : 0));
    if (bar.length === 0) {
      return [];
    }
    for (let i = 0; i < bar.length; i++) {
      if (bar.charAt(i) !== '-') {
        return [];
      }
    }
    out.push(left && right ? 'center' : right ? 'right' : 'left');
  }
  return out;
}

function makeRow(line: string, columns: number): TableRow {
  const raw = splitCells(line);
  const cells: InlineSpan[][] = [];
  for (let i = 0; i < columns; i++) {
    // Pad short rows and ignore extra cells, so a ragged table still renders as a
    // grid instead of collapsing.
    cells.push(parseInline(i < raw.length ? raw[i].trim() : ''));
  }
  return { cells: cells };
}

export function parseBlocks(text: string): RichBlock[] {
  const blocks: RichBlock[] = [];
  const lines = splitLines(text);
  let i = 0;

  // Paragraph accumulator — consecutive plain lines joined with '\n'.
  let para: string[] = [];
  let paraStart = 0;
  const flushPara = (): void => {
    if (para.length > 0) {
      const b = makeBlock('paragraph', paraStart);
      b.spans = parseInline(para.join('\n'));
      blocks.push(b);
      para = [];
    }
  };

  while (i < lines.length) {
    const line = lines[i];
    const t = line.text;
    const trimmed = t.trim();

    // --- fenced code / chart -------------------------------------------------
    if (trimmed.startsWith('```')) {
      flushPara();
      const info = trimmed.substring(3).trim();
      const isChart = info === 'echarts';
      const b = makeBlock(isChart ? 'chart' : 'code', line.start);
      b.language = isChart ? '' : info;
      const body: string[] = [];
      let closed = false;
      i++;
      while (i < lines.length) {
        if (lines[i].text.trim() === '```') {
          closed = true;
          i++;
          break;
        }
        body.push(lines[i].text);
        i++;
      }
      b.body = body.join('\n');
      b.closed = closed;
      blocks.push(b);
      continue;
    }

    // --- display math: $$ ... $$ and \[ ... \] --------------------------------
    {
      const isDollar  = trimmed === '$$' || (trimmed.startsWith('$$') && trimmed.length > 2 && trimmed.endsWith('$$'));
      const isBracket = trimmed === '\\[' || (trimmed.startsWith('\\[') && trimmed.endsWith('\\]') && trimmed.length > 2);
      if (isDollar || isBracket) {
        flushPara();
        const b = makeBlock('math', line.start);
        const open = isDollar ? '$$' : '\\[';
        const close = isDollar ? '$$' : '\\]';
        if (trimmed.length > open.length && trimmed.endsWith(close)) {
          // One-liner: $$ e = mc^2 $$  /  \[ e = mc^2 \]
          b.body = trimmed.substring(open.length, trimmed.length - close.length).trim();
          b.closed = true;
          i++;
        } else {
          const body: string[] = [];
          let closed = false;
          i++;
          while (i < lines.length) {
            if (lines[i].text.trim() === close) {
              closed = true;
              i++;
              break;
            }
            body.push(lines[i].text);
            i++;
          }
          b.body = body.join('\n');
          b.closed = closed;
        }
        blocks.push(b);
        continue;
      }
    }

    // --- heading --------------------------------------------------------------
    if (trimmed.startsWith('#')) {
      let level = 0;
      while (level < trimmed.length && level < 6 && trimmed.charAt(level) === '#') {
        level++;
      }
      if (level < trimmed.length && trimmed.charAt(level) === ' ') {
        flushPara();
        const b = makeBlock('heading', line.start);
        b.level = level;
        b.spans = parseInline(trimmed.substring(level + 1).trim());
        blocks.push(b);
        i++;
        continue;
      }
    }

    // --- block quote (consecutive > lines merge) --------------------------------
    if (trimmed.startsWith('>')) {
      flushPara();
      const b = makeBlock('quote', line.start);
      const body: string[] = [];
      while (i < lines.length) {
        const q = lines[i].text.trim();
        if (!q.startsWith('>')) {
          break;
        }
        body.push(q.startsWith('> ') ? q.substring(2) : q.substring(1));
        i++;
      }
      b.spans = parseInline(body.join('\n'));
      blocks.push(b);
      continue;
    }

    // --- list (consecutive items merge; indent -> depth) -------------------------
    {
      const indent = indentOf(t);
      const m = matchListItem(t.substring(indent));
      if (m.matched) {
        flushPara();
        const b = makeBlock('list', line.start);
        while (i < lines.length) {
          const it = lines[i].text;
          const ind = indentOf(it);
          const im = matchListItem(it.substring(ind));
          if (!im.matched) {
            break;
          }
          b.entries.push({
            marker: im.marker,
            depth: Math.floor(ind / 2),
            spans: parseInline(im.rest)
          });
          i++;
        }
        blocks.push(b);
        continue;
      }
    }

    // --- GFM pipe table -----------------------------------------------------
    // Needs the NEXT line to be a delimiter row; that is what distinguishes a
    // table from a paragraph containing pipes.
    if (trimmed.indexOf('|') >= 0 && i + 1 < lines.length) {
      const aligns = tableAligns(lines[i + 1].text.trim());
      if (aligns.length > 0) {
        flushPara();
        const b = makeBlock('table', line.start);
        b.aligns = aligns;
        b.rows.push(makeRow(t, aligns.length));
        i += 2;                                  // header + delimiter consumed
        while (i < lines.length) {
          const rowText = lines[i].text.trim();
          if (rowText.length === 0 || rowText.indexOf('|') < 0) {
            break;                               // blank line or non-row ends it
          }
          b.rows.push(makeRow(lines[i].text, aligns.length));
          i++;
        }
        blocks.push(b);
        continue;
      }
    }

    // --- blank line = paragraph break -------------------------------------------
    if (trimmed.length === 0) {
      flushPara();
      i++;
      continue;
    }

    // --- plain paragraph line -----------------------------------------------------
    if (para.length === 0) {
      paraStart = line.start;
    }
    para.push(t);
    i++;
  }

  flushPara();
  return blocks;
}
