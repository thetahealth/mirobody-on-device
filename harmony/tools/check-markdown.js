#!/usr/bin/env node
//
// Checks for the hand-written markdown renderer and the documents that depend on
// it. Run from anywhere:
//
//     node harmony/tools/check-markdown.js
//
// WHY THIS EXISTS: core/Markdown.ets is the only markdown parser in the repo that
// is ours (every other client delegates to a library -- see docs/markdown.md), and
// it is the one place where "what renders" is a decision rather than a dependency.
// It has already shipped three defects that a single assertion each would have
// caught: emphasis that swallowed the math inside it, a ```` fence that ate the
// rest of the message, and escapes that corrupted rather than passed through.
//
// It runs the REAL source, not a copy. Markdown.ets is pure logic -- no ArkUI
// decorators -- so it transpiles and imports directly. RenderHost.ets cannot
// (it imports ArkUI kits), so its two pure functions are lifted out by name; a
// rename there fails this script loudly rather than drifting away from it.
//
// Needs TypeScript, taken from a normal install or from DevEco's bundled copy, so
// a machine that can build the app can run this without installing anything.

'use strict';

const fs = require('fs');
const path = require('path');
const os = require('os');

const HARMONY = path.resolve(__dirname, '..');
const ETS = path.join(HARMONY, 'entry/src/main/ets');
const HELP = path.join(HARMONY, 'entry/src/main/resources/rawfile/help');

//------------------------------------------------------------------------------
// Loading the real source
//------------------------------------------------------------------------------

function typescript() {
  try {
    return require('typescript');
  } catch (e) { /* fall through to the SDK's copy */ }
  const home = process.env.DEVECO_HOME
    || path.join(process.env['ProgramFiles'] || 'C:/Program Files', 'Huawei/DevEco Studio');
  const bundled = path.join(home,
    'sdk/default/openharmony/ets/build-tools/ets-loader/node_modules/typescript');
  try {
    return require(bundled);
  } catch (e) {
    console.error('check-markdown: no TypeScript found.\n'
      + '  Install one (npm i -g typescript) or set DEVECO_HOME so the bundled copy resolves.\n'
      + '  Looked for: ' + bundled);
    process.exit(2);
  }
}

const ts = typescript();
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'mirobody-md-'));

/** Transpile TS source text and require it as a module. */
function load(name, source) {
  const js = ts.transpileModule(source, {
    compilerOptions: { target: ts.ScriptTarget.ES2018, module: ts.ModuleKind.CommonJS }
  }).outputText;
  const file = path.join(tmp, name + '.js');
  fs.writeFileSync(file, js);
  return require(file);
}

const md = load('Markdown', fs.readFileSync(path.join(ETS, 'core/Markdown.ets'), 'utf8'));

/**
 * RenderHost's SVG gatekeepers, lifted out by name. They are pure and they decide
 * whether a figure is drawn at all, which makes them worth testing; the file they
 * live in is not importable here because it pulls in ArkUI.
 */
const renderHost = fs.readFileSync(path.join(ETS, 'core/RenderHost.ets'), 'utf8');
const lifted = ['svgIsSafe', 'hasExternalRef', 'attrValue', 'plainPixels', 'svgSize'].map((fn) => {
  const m = new RegExp('^function ' + fn + '\\([\\s\\S]*?^\\}', 'm').exec(renderHost);
  if (!m) {
    console.error('check-markdown: RenderHost.ets no longer defines ' + fn
      + ' — update the lift list in this script.');
    process.exit(2);
  }
  return m[0];
});
const svg = load('svglib',
  'interface RenderedImage { ok: boolean; url: string; w: number; h: number; }\n'
  + lifted.join('\n\n') + '\nexport { svgIsSafe, svgSize };\n');

//------------------------------------------------------------------------------
// Reporting
//------------------------------------------------------------------------------

let failures = 0;
let checks = 0;

function eq(name, got, want) {
  checks++;
  if (got === want) return;
  failures++;
  console.log('FAIL  ' + name + '\n  want ' + want + '\n  got  ' + got);
}

function ok(name, cond) {
  checks++;
  if (cond) return;
  failures++;
  console.log('FAIL  ' + name);
}

//------------------------------------------------------------------------------
// Rendering the parse result as a compact, diffable string
//------------------------------------------------------------------------------

function spans(list) {
  return list.map((s) => {
    let f = '';
    if (s.bold) f += 'b';
    if (s.italic) f += 'i';
    if (s.strike) f += 's';
    if (s.href !== undefined) f += '->' + s.href;
    const kind = s.kind === 'text' ? '' : s.kind + ':';
    return kind + JSON.stringify(s.text) + (f ? '{' + f + '}' : '');
  }).join(' ');
}

function blocks(src) {
  return md.parseBlocks(src).map((b) => {
    if (b.kind === 'list') {
      return 'list[' + b.entries.map((e) =>
        e.marker + (e.task ? (e.checked ? '(done)' : '(todo)') : '')
        + '|' + e.depth + '|' + spans(e.spans)).join(' ; ') + ']';
    }
    if (b.kind === 'table') {
      return 'table(' + b.aligns.join(',') + ')['
        + b.rows.map((r) => r.cells.map(spans).join('|')).join(' ; ') + ']';
    }
    if (b.kind === 'code' || b.kind === 'math' || b.kind === 'chart' || b.kind === 'svg') {
      return b.kind + (b.language ? ':' + b.language : '') + (b.closed ? '' : '(OPEN)')
        + '{' + JSON.stringify(b.body) + '}';
    }
    if (b.kind === 'heading') return 'h' + b.level + '{' + spans(b.spans) + '}';
    if (b.kind === 'rule') return 'rule';
    return b.kind + '{' + spans(b.spans) + '}';
  }).join(' + ');
}

//------------------------------------------------------------------------------
// 1. Blocks
//------------------------------------------------------------------------------

console.log('— blocks');
[
  ['ATX heading',           '# One\n## Two\n###### Six', 'h1{"One"} + h2{"Two"} + h6{"Six"}'],
  ['ATX needs a space',     '#NoSpace',                  'paragraph{"#NoSpace"}'],
  ['setext h1',             'Title\n===',                'h1{"Title"}'],
  ['=== alone is text',     '===',                       'paragraph{"==="}'],
  ['thematic break',        'a\n\n---\n\nb',             'paragraph{"a"} + rule + paragraph{"b"}'],
  ['spaced break',          '- - -',                     'rule'],
  ['*** break',             '***',                       'rule'],
  ['**bold** is not one',   '**bold**',                  'paragraph{"bold"{b}}'],
  // The fence that ate the rest of the message: a longer opener needs a longer closer.
  ['4-tick fence closes',   '````\nhas ``` in it\n````\n\nafter',
                            'code{"has ``` in it"} + paragraph{"after"}'],
  ['~~~ fence',             '~~~js\nx=1\n~~~',           'code:js{"x=1"}'],
  ['~~~ not closed by ```', '~~~\nx\n```\n~~~',          'code{"x\\n```"}'],
  ['indented code',         'text\n\n    code();\n\nafter',
                            'paragraph{"text"} + code{"code();"} + paragraph{"after"}'],
  ['no code mid-paragraph', 'text\n    still text',      'paragraph{"text\\n    still text"}'],
  ['blockquote',            '> quoted',                  'quote{"quoted"}'],
  ['nested quote flattens', '> outer\n>> inner',         'quote{"outer\\ninner"}'],
  ['bullets - * +',         '- a\n* b\n+ c',             'list[•|0|"a" ; •|0|"b" ; •|0|"c"]'],
  ['ordered keeps start',   '5. a\n6. b',                'list[5.|0|"a" ; 6.|0|"b"]'],
  ['ordered 1)',            '1) a',                      'list[1.|0|"a"]'],
  ['nested list',           '- a\n  - b\n    - c',       'list[•|0|"a" ; •|1|"b" ; •|2|"c"]'],
  ['item continuation',     '- first\n  continued',      'list[•|0|"first\\ncontinued"]'],
  ['task list',             '- [ ] todo\n- [x] done',    'list[☐(todo)|0|"todo" ; ☑(done)|0|"done"]'],
  ['table + aligns',        '| a | b | c |\n| :-- | :-: | --: |\n| 1 | 2 | 3 |',
                            'table(left,center,right)["a"|"b"|"c" ; "1"|"2"|"3"]'],
  ['table, no outer pipes', 'a | b\n--- | ---\n1 | 2',   'table(left,left)["a"|"b" ; "1"|"2"]'],
  ['echarts fence',         '```echarts\n{}\n```',       'chart{"{}"}'],
  ['svg fence',             '```svg\n<svg/>\n```',       'svg{"<svg/>"}'],
  ['unknown info is code',  '```rust\nx\n```',           'code:rust{"x"}'],
  ['block math',            '$$\nE=mc^2\n$$',            'math{"E=mc^2"}'],
  ['block math one-liner',  '$$ E=mc^2 $$',              'math{"E=mc^2"}'],
  ['\\[ \\] math',          '\\[\nE=mc^2\n\\]',          'math{"E=mc^2"}'],
].forEach((c) => eq(c[0], blocks(c[1]), c[2]));

//------------------------------------------------------------------------------
// 2. Inline
//------------------------------------------------------------------------------

console.log('— inline');
[
  ['escape',                'not \\*italic\\* here',     'paragraph{"not *italic* here"}'],
  ['escape backtick',       'a \\`b\\` c',               'paragraph{"a `b` c"}'],
  ['non-escapable kept',    'C:\\path and \\n',          'paragraph{"C:\\\\path and \\\\n"}'],
  ['code span',             'a `x = 1` b',               'paragraph{"a " code:"x = 1" " b"}'],
  ['double-tick code span', 'a ``x ` y`` b',             'paragraph{"a " code:"x ` y" " b"}'],
  ['tick inside code',      'use `` ` `` here',          'paragraph{"use " code:"`" " here"}'],
  ['star emphasis',         '*it* and **b** and ***x***',
                            'paragraph{"it"{i} " and " "b"{b} " and " "x"{bi}}'],
  ['underscore emphasis',   '_it_ and __b__',            'paragraph{"it"{i} " and " "b"{b}}'],
  ['snake_case safe',       'a snake_case_name here',    'paragraph{"a snake_case_name here"}'],
  ['strikethrough',         '~~gone~~ ok',               'paragraph{"gone"{s} " ok"}'],
  ['link',                  'see [t](http://e.com)',     'paragraph{"see " "t"{->http://e.com}}'],
  ['link title dropped',    '[a](http://e.com "T")',     'paragraph{"a"{->http://e.com}}'],
  ['image',                 '![alt](http://e.com/i.png)','paragraph{image:"alt"{->http://e.com/i.png}}'],
  ['autolink',              '<http://e.com>',            'paragraph{"http://e.com"{->http://e.com}}'],
  ['bare url',              'go https://e.com now',      'paragraph{"go " "https://e.com"{->https://e.com} " now"}'],
  ['bare url, sentence end','see https://e.com.',        'paragraph{"see " "https://e.com"{->https://e.com} "."}'],
  ['www',                   'at www.e.com ok',           'paragraph{"at " "www.e.com"{->https://www.e.com} " ok"}'],
  ['entities',              'AT&amp;T &copy; &#8212;',   'paragraph{"AT&T © —"}'],
  ['unknown entity kept',   'a &nope; b',                'paragraph{"a &nope; b"}'],
  ['inline math $',         'ion $Na^+$ here',           'paragraph{"ion " math:"Na^+" " here"}'],
  ['inline math \\( \\)',   'ion \\(Na^+\\) here',       'paragraph{"ion " math:"Na^+" " here"}'],
  ['currency is not math',  'costs $100 and $200',       'paragraph{"costs $100 and $200"}'],
].forEach((c) => eq(c[0], blocks(c[1]), c[2]));

//------------------------------------------------------------------------------
// 3. Nesting — the styles have to compose, which is why they are flags
//------------------------------------------------------------------------------

console.log('— nesting');
[
  // The one that shipped broken: the closer for `*` must skip the inner `**` run.
  ['bold inside italic',    '*a **b** c*',               '"a "{i} "b"{bi} " c"{i}'],
  ['italic inside bold',    '**a *b* c**',               '"a "{b} "b"{bi} " c"{b}'],
  ['triple',                '***both***',                '"both"{bi}'],
  ['strike inside bold',    '**~~a~~ b**',               '"a"{bs} " b"{b}'],
  ['bold inside strike',    '~~**a** b~~',               '"a"{bs} " b"{s}'],
  // The one the whole rewrite started from: math inside bold.
  ['math inside bold',      '**sodium ($Na^+$)**',       '"sodium ("{b} math:"Na^+"{b} ")"{b}'],
  ['code inside link',      '[`x`](http://e.com)',       'code:"x"{->http://e.com}'],
  ['bold inside link',      '[**a** b](http://e.com)',   '"a"{b->http://e.com} " b"{->http://e.com}'],
  ['link inside bold',      '**see [a](http://e.com)**', '"see "{b} "a"{b->http://e.com}'],
].forEach((c) => eq(c[0], spans(md.parseInline(c[1])), c[2]));

// Streaming: an unterminated marker must stay literal rather than eat the tail.
console.log('— streaming');
eq('open bold stays literal', blocks('some **bo'), 'paragraph{"some **bo"}');
eq('open fence stays open',   blocks('```js\nlet a'), 'code:js(OPEN){"let a"}');
eq('open math not closed',    md.parseBlocks('$$\nE=mc')[0].closed, false);

//------------------------------------------------------------------------------
// 4. SVG gatekeepers
//------------------------------------------------------------------------------

console.log('— svg');
[
  '<svg viewBox="0 0 10 10"><rect width="10" height="10" fill="#1e3a6b"/></svg>',
  '<svg viewBox="0 0 10 10"><defs><linearGradient id="g"/></defs><rect fill="url(#g)"/></svg>',
].forEach((s) => ok('accept ' + s.slice(0, 40), svg.svgIsSafe(s)));

[
  ['script',        '<svg><script>fetch("http://e.com")</script></svg>'],
  ['remote image',  '<svg><image href="https://tracker.example/px.png"/></svg>'],
  ['handler attr',  '<svg onload="alert(1)"><rect/></svg>'],
  ['nested handler','<svg><rect onclick="x()"/></svg>'],
  ['entity bomb',   '<!DOCTYPE svg [<!ENTITY a "aaa">]><svg/>'],
  ['foreignObject', '<svg><foreignObject><body>hi</body></foreignObject></svg>'],
  ['external use',  '<svg><use href="http://e.com#x"/></svg>'],
].forEach((c) => ok('refuse ' + c[0], !svg.svgIsSafe(c[1])));

const dim = (s) => { const r = svg.svgSize(s); return r.w + 'x' + r.h; };
eq('viewBox',                dim('<svg viewBox="0 0 640 480">'), '640x480');
eq('viewBox beats width',    dim('<svg width="100%" height="100%" viewBox="0 0 16 9">'), '16x9');
eq('comma viewBox',          dim('<svg viewBox="0,0,40,20">'), '40x20');
eq('width/height fallback',  dim('<svg width="300" height="150">'), '300x150');
eq('px units',               dim('<svg width="300px" height="150px">'), '300x150');
eq('percent is unknown',     dim('<svg width="100%" height="100%">'), '0x0');
eq('nothing is unknown',     dim('<svg>'), '0x0');
eq('stroke-width is not a size',
   dim('<svg><rect stroke-width="2" height="9"/></svg>'), '0x0');

//------------------------------------------------------------------------------
// 5. The shipped help documents
//
// They are written to be BOTH the user's guide and a live sample of every
// construct, so a formatting regression shows up the first time anyone types
// /help. This asserts they still carry that sample.
//------------------------------------------------------------------------------

console.log('— help documents');
const helpFiles = fs.readdirSync(HELP).filter((f) => f.endsWith('.md'));
ok('both languages present', helpFiles.length === 2);

helpFiles.forEach((file) => {
  const src = fs.readFileSync(path.join(HELP, file), 'utf8');
  const bs = md.parseBlocks(src);
  const kinds = {};
  bs.forEach((b) => { kinds[b.kind] = (kinds[b.kind] || 0) + 1; });

  const all = [];
  bs.forEach((b) => {
    all.push.apply(all, b.spans);
    b.entries.forEach((e) => all.push.apply(all, e.spans));
    b.rows.forEach((r) => r.cells.forEach((c) => all.push.apply(all, c)));
  });
  const has = (fn) => all.some(fn);
  const tag = file + ': ';

  ok(tag + 'headings',      (kinds.heading || 0) >= 8);
  ok(tag + 'lists',         (kinds.list || 0) >= 3);
  ok(tag + 'tables',        (kinds.table || 0) >= 3);
  ok(tag + 'quotes',        (kinds.quote || 0) >= 2);
  ok(tag + 'rules',         (kinds.rule || 0) >= 5);
  ok(tag + 'code fence',    (kinds.code || 0) >= 1);
  ok(tag + 'chart',         (kinds.chart || 0) === 1);
  ok(tag + 'svg',           (kinds.svg || 0) === 1);
  ok(tag + 'display math',  (kinds.math || 0) === 1);
  ok(tag + 'bold',          has((s) => s.bold));
  ok(tag + 'italic',        has((s) => s.italic));
  ok(tag + 'strikethrough', has((s) => s.strike));
  ok(tag + 'inline code',   has((s) => s.kind === 'code'));
  ok(tag + 'inline math',   has((s) => s.kind === 'math'));
  ok(tag + 'link',          has((s) => s.href !== undefined));
  ok(tag + 'task done',     bs.some((b) => b.entries.some((e) => e.task && e.checked)));
  ok(tag + 'task todo',     bs.some((b) => b.entries.some((e) => e.task && !e.checked)));
  ok(tag + 'nested list',   bs.some((b) => b.entries.some((e) => e.depth > 0)));
  ok(tag + 'all 3 aligns',  ['left', 'center', 'right'].every((a) =>
                              bs.some((b) => b.aligns.indexOf(a) >= 0)));
  ok(tag + 'entity decoded', has((s) => s.text.indexOf('©') >= 0 || s.text.indexOf('°') >= 0));
  ok(tag + 'escape applied', has((s) => s.text.indexOf('*') >= 0 && s.text.indexOf('\\*') < 0));
  // A fence leaking into a span means a block boundary was misread.
  ok(tag + 'no stray fence', !all.some((s) => s.text.indexOf('```') >= 0));

  // The document must survive OUR OWN gates, or it demonstrates a figure that
  // renders as XML on the very screen it is demonstrating.
  const fig = bs.filter((b) => b.kind === 'svg')[0];
  if (fig) {
    ok(tag + 'svg accepted', svg.svgIsSafe(fig.body));
    ok(tag + 'svg has a size', svg.svgSize(fig.body).w > 0 && svg.svgSize(fig.body).h > 0);
  }
  const chart = bs.filter((b) => b.kind === 'chart')[0];
  if (chart) {
    let parses = true;
    try { JSON.parse(chart.body); } catch (e) { parses = false; }
    ok(tag + 'chart option is JSON', parses);
  }
});

//------------------------------------------------------------------------------
// 6. The command palette and the help table must agree
//
// The table in the guide is the easiest thing in this repo to leave stale: adding
// a command touches SlashCommand.ets, and nothing forces the document to follow.
//------------------------------------------------------------------------------

console.log('— commands');
const slash = fs.readFileSync(path.join(ETS, 'core/SlashCommand.ets'), 'utf8');
const consts = {};
slash.replace(/export const (SLASH_[A-Z]+) = '(\/[a-z]+)';/g, (_, k, v) => { consts[k] = v; return _; });
const menuBlock = /export const SLASH_MENU:[\s\S]*?\];/.exec(slash);
ok('SLASH_MENU found', !!menuBlock);
const offered = menuBlock
  ? (menuBlock[0].match(/name: (SLASH_[A-Z]+)/g) || []).map((m) => consts[m.slice(6)])
  : [];
ok('palette is not empty', offered.length > 0);

helpFiles.forEach((file) => {
  const doc = fs.readFileSync(path.join(HELP, file), 'utf8');
  offered.forEach((cmd) => ok(file + ': documents ' + cmd, doc.indexOf('`' + cmd + '`') >= 0));
  // /probe earns its usefulness by NOT being listed; documenting it makes it a
  // feature offer rather than a developer tool.
  ok(file + ': keeps /probe undocumented', doc.indexOf('/probe') < 0);
});

//------------------------------------------------------------------------------

fs.rmSync(tmp, { recursive: true, force: true });
console.log('\n' + (checks - failures) + '/' + checks + ' checks passed');
if (failures > 0) {
  console.log(failures + ' FAILED');
  process.exit(1);
}
