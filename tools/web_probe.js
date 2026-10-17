/* Front-end checks for colaimdation's web/assets/*.js.
 *
 * The editor front-end has no build step, so a syntax error or a bad refactor
 * only shows up inside WebView2. These checks run the real assets under Node
 * (see web_harness.js) and cover the two things that are easy to get wrong and
 * impossible to see from the C++ side: HTML/URL escaping, and the editing paths
 * that used to corrupt or drop text.
 *
 * Usage: node tools/web_probe.js
 */
'use strict';

const path = require('path');
const harness = require('./web_harness').load(path.resolve(__dirname, '..'));

const Render = harness.Render;

let failures = 0;
function check(label, ok, detail) {
  console.log((ok ? '  ok    ' : '  FAIL  ') + label + (ok || !detail ? '' : '  [' + detail + ']'));
  if (!ok) failures += 1;
}

console.log('escaping and URL handling');
check('plain text is escaped', Render.inlineHtml('<script>x</script>') === '&lt;script&gt;x&lt;/script&gt;');
check('javascript: link is not emitted as href',
  Render.inlineHtml('[x](javascript:alert(1))').indexOf('href') === -1,
  Render.inlineHtml('[x](javascript:alert(1))'));
check('javascript: image is not emitted as src',
  Render.inlineHtml('![x](javascript:alert(1))').indexOf('src') === -1);
check('vbscript: link is neutralised',
  Render.inlineHtml('[x](VBScript:msgbox(1))').indexOf('href') === -1);
check('obfuscated javascript: link is neutralised',
  Render.inlineHtml('[x](java\tscript:alert(1))').indexOf('href="javascript') === -1);
check('https link survives',
  Render.inlineHtml('[x](https://example.com/a)').indexOf('href="https://example.com/a"') >= 0);
check('mailto link survives',
  Render.inlineHtml('[x](mailto:a@b.c)').indexOf('href="mailto:a@b.c"') >= 0);
check('relative link survives',
  Render.inlineHtml('[x](./notes.md)').indexOf('href="./notes.md"') >= 0);
check('data:image image survives',
  Render.inlineHtml('![x](data:image/png;base64,AAA)').indexOf('src="data:image/png') >= 0);
check('link carries rel=noreferrer',
  Render.inlineHtml('[x](https://e.com)').indexOf('rel="noreferrer noopener"') >= 0);
check('a quote cannot break out of an attribute',
  Render.inlineHtml('[x](https://e.com/" onmouseover="alert(1))').indexOf('onmouseover="alert') === -1);

console.log('');
console.log('block parsing');
check('a continued paragraph does not throw',
  (function () {
    try { Render.parseBlocks('line one\nline two\n\nsecond para\n'); return true; }
    catch (error) { return false; }
  })());
{
  const blocks = Render.parseBlocks('# t\n\npara\n\n- a\n- b\n');
  check('block spans stay inside the document',
    blocks.every((b) => b.start <= b.end && b.end <= '# t\n\npara\n\n- a\n- b\n'.length));
  check('a heading, a paragraph and a list were recognised',
    blocks.length === 3 && blocks[0].kind === 'heading' && blocks[2].kind === 'list',
    blocks.map((b) => b.kind).join(','));
}

console.log('');
console.log('editing');
check('an empty document renders a placeholder',
  (function () {
    const { rootEl } = harness.createEditor('');
    return rootEl.childNodes.length === 1 && rootEl.childNodes[0].classList.contains('md-empty');
  })());
check('typing into an empty document reaches the model',
  (function () {
    const { editor, rootEl } = harness.createEditor('');
    const placeholder = rootEl.childNodes[0];
    placeholder.textContent = 'Hello';
    const node = harness.allTextNodes(placeholder, [])[0];
    harness.setCaret(node, 5);
    editor._handleInput();
    return editor.text === 'Hello';
  })(),
  'see web_typing.js for the full typing suite');
check('rendered blocks are not editable',
  (function () {
    const { rootEl } = harness.createEditor('one\n\ntwo\n');
    return rootEl.childNodes[1].getAttribute('contenteditable') === 'false';
  })());
check('the edited document never loses its tail',
  (function () {
    const { editor, rootEl } = harness.createEditor('Intro text.\n\n    code line\n');
    const first = rootEl.childNodes[0];
    first.textContent = '    Intro text.';
    const node = harness.allTextNodes(first, [])[0];
    harness.setCaret(node, 15);
    editor._handleInput();
    return editor.text.indexOf('code line') >= 0;
  })());

console.log('');
console.log('display formulas');
{
  // A "$$" block keeps the newline that terminates its closing delimiter line, and
  // the closing delimiter used to survive, so KaTeX was handed a stray "$$" and
  // drew an error instead of the formula. Checked in every shape a document can
  // have, because only the last block in a file used to work.
  const formula = [
    '$$',
    '\\begin{pmatrix} a & b \\\\ c & d \\end{pmatrix}',
    '\\begin{pmatrix} x \\\\ y \\end{pmatrix}',
    '=',
    '\\begin{pmatrix} ax + by \\\\ cx + dy \\end{pmatrix}',
    '$$',
  ].join('\n');
  const shapes = {
    'followed by a paragraph': 'Before.\n\n' + formula + '\n\nAfter.\n',
    'at the end of the file': 'Before.\n\n' + formula + '\n',
    'without a final newline': 'Before.\n\n' + formula,
    'with crlf endings': 'Before.\r\n\r\n' + formula.replace(/\n/g, '\r\n') + '\r\n\r\nAfter.\r\n',
  };
  for (const [label, doc] of Object.entries(shapes)) {
    const math = Render.parseBlocks(doc).filter((b) => b.kind === 'math_block');
    const rendered = math.length ? Render.blockHtml(math[0]) : '';
    check('a display formula is rendered by KaTeX (' + label + ')',
      math.length === 1 &&
      rendered.indexOf('$$') === -1 &&
      rendered.indexOf('\\begin{pmatrix}') >= 0 &&
      rendered.indexOf('\\\\') >= 0,
      rendered.slice(0, 120));
  }
  // The inline form must keep working: "$...$" inside a paragraph.
  check('inline math keeps its single delimiters',
    Render.inlineHtml('mass $E = mc^2$ here').indexOf('$$') === -1 &&
    Render.inlineHtml('mass $E = mc^2$ here').indexOf('E = mc^2') >= 0,
    Render.inlineHtml('mass $E = mc^2$ here'));
}

console.log('');
console.log(failures === 0 ? 'web probe: all checks passed' : 'web probe: ' + failures + ' check(s) FAILED');
process.exit(failures === 0 ? 0 : 1);
