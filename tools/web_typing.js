/* Front-end typing checks.
 *
 * These guard the editing paths that the DOM tests cannot reach from C++, and
 * they encode the properties that were broken before:
 *
 *   - an inline keystroke must NOT rebuild the whole document (it resets the
 *     caret, flickers the view and breaks IME composition);
 *   - a keystroke that changes the block layout (Enter, a marker, indentation)
 *     MUST rebuild, or the next keystroke splices mismatched offsets and
 *     deletes the rest of the document;
 *   - characters must land in the order they were typed;
 *   - Enter must actually reach the model, which it did not while the code only
 *     understood <br> line breaks (Chrome wraps the next line in a <div>).
 *
 * Usage: node tools/web_typing.js
 */
'use strict';

const path = require('path');
const harness = require('./web_harness').load(path.resolve(__dirname, '..'));
const { TextNode, Element } = require('./web_harness');

let failures = 0;
function check(label, ok, detail) {
  console.log((ok ? '  ok    ' : '  FAIL  ') + label + (ok || !detail ? '' : '  [' + detail + ']'));
  if (!ok) failures += 1;
}

/* Type one character the way a browser does.
 *
 * The crucial detail: Enter in a contenteditable does NOT insert a '\n' text
 * node. Chrome wraps the following text in a <div> ("AA<div>BB</div>", whose
 * textContent is "AABB"). Modelling that faithfully is what makes this suite
 * catch newline bugs; an earlier version inserted a '\n' and therefore passed
 * while the editor silently lost every line break the writer typed.
 *
 * After the editor rebuilds its DOM the old caret node is gone, so the caret is
 * re-resolved through the editor - which is what a browser's selection does. */
function type(harness, editor, rootEl, ch) {
  const caret = harness.getCaret();
  let node = caret.node;
  let at = caret.offset;

  let attached = false;
  if (node) {
    let cur = node;
    while (cur) { if (cur === rootEl) { attached = true; break; } cur = cur.parentNode; }
  }
  if (!attached) {
    node = null;
    const index = editor.activeIndex;
    const element = index >= 0 ? editor.blockElements[index] : null;
    if (element) {
      node = harness.allTextNodes(element, [])[0];
      at = node ? node.nodeValue.length : 0;
    }
  }
  if (!node) {
    const empty = rootEl.querySelector('.md-empty') || rootEl;
    node = harness.allTextNodes(empty, [])[0];
    if (!node) { node = new TextNode(''); empty.appendChild(node); }
    at = node.nodeValue.length;
  }

  if (ch === '\n') {
    // Exactly what Chrome does for Enter at the end of a line: the text after the
    // caret moves into a new <div>. (Keys after this one are placed by the
    // caller's caret bookkeeping, so the harness keeps this to the common case of
    // Enter at the end of the line.)
    const after = node.nodeValue.slice(at);
    node.nodeValue = node.nodeValue.slice(0, at);
    const host = node.parentNode;
    const line = new Element('div');
    let tail = null;
    if (after) {
      tail = new TextNode(after);
      line.appendChild(tail);
    } else {
      line.appendChild(new Element('br'));
    }
    host.appendChild(line);
    harness.setCaret(tail || line, 0);
    editor._handleInput();
    return;
  }

  if (node.nodeType === 1) {
    // The caret is on an element rather than in text: an empty line (the line box
    // a block's terminator is rendered as, or an empty document). The engine types
    // into a text node it puts at the caret, replacing the empty line's <br>
    // placeholder; it does not append text at the end of the element.
    const created = new TextNode('');
    const placeholder = node.childNodes[at] || null;
    if (placeholder) node.insertBefore(created, placeholder);
    else node.appendChild(created);
    if (placeholder && placeholder.nodeType === 1 && placeholder.nodeName === 'BR') {
      node.removeChild(placeholder);
    }
    node = created;
    at = 0;
  }

  node.nodeValue = node.nodeValue.slice(0, at) + ch + node.nodeValue.slice(at);
  harness.setCaret(node, at + ch.length);
  editor._handleInput();
}

function typed(harness, text, keys) {
  const { editor, rootEl } = harness.createEditor(text);
  harness.resetRebuilds();
  for (const ch of keys) type(harness, editor, rootEl, ch);
  return { editor, rootEl };
}

// Undoes the correction the editor is allowed to apply to a heading: the blank
// line it inserts before the marker. (It only inserts the space after the #s
// once something follows them, so nothing else changes what was typed.)
const withoutHeadingFixes = (text) =>
  text.split('\n')
    .filter((line, index, all) =>
      !(line === '' && index > 0 && all[index - 1] !== '' &&
        all[index + 1] !== undefined && /^ {0,3}#{1,6}/.test(all[index + 1])))
    .join('\n');

console.log('A. typing into an empty document keeps the typed order');
{
  const { editor } = typed(harness, '', '```cpp');
  check('"```cpp" produces "```cpp"', editor.text === '```cpp', JSON.stringify(editor.text));
  check('the caret is at the end', editor.caretOffset() === editor.text.length,
    'caret=' + editor.caretOffset() + ' len=' + editor.text.length);
}
{
  const quotes = "'''cpp";
  const { editor } = typed(harness, '', quotes);
  check('three single quotes then cpp keep their order', editor.text === quotes,
    JSON.stringify(editor.text));
}
{
  const { editor } = typed(harness, '', 'hello world');
  check('plain text lands in order', editor.text === 'hello world', JSON.stringify(editor.text));
}

console.log('');
console.log('B. markers typed as the first thing in an empty document');
{
  // Multi-line typing is verified in tools/web_browser.js against the real DOM;
  // here only the single-line shape is asserted.
  const { editor } = typed(harness, '', '```cpp');
  check('a fence marker stays intact', editor.text === '```cpp', JSON.stringify(editor.text));
  const { editor: e2 } = typed(harness, '', '# 标题');
  check('a heading at the start of a document is a heading block',
    e2.blocks[0].kind === 'heading', e2.blocks.map((b) => b.kind).join(','));
}

console.log('');
console.log('C. inline typing keeps the DOM stable');
{
  const { editor } = harness.createEditor('# Title\n\nsome paragraph\n\nlast line\n');
  const start = editor.blocks[1].start;
  editor._setCaretOffset(start + 4, false);
  harness.resetRebuilds();
  const rootEl = editor.root;
  for (const ch of 'XYZ') type(harness, editor, rootEl, ch);
  check('3 inline characters rebuild the DOM 0 times', harness.rebuildCount() === 0,
    'rebuilds=' + harness.rebuildCount());
  check('text is correct', editor.text === '# Title\n\nsomeXYZ paragraph\n\nlast line\n',
    JSON.stringify(editor.text));
  check('caret follows the inserted text', editor.caretOffset() === start + 7,
    'caret=' + editor.caretOffset());
  check('the active block is still shown as raw source',
    editor.blockElements[editor.activeIndex].classList.contains('md-source'));
  check('the rendered blocks stayed rendered', editor.blockElements[0].innerHTML.length > 0);
}

console.log('');
console.log('D. Enter actually reaches the model (the browser uses <div>)');
{
  // Enter at the end of a line ends the block: the model must gain a line break.
  const { editor, rootEl } = harness.createEditor('alpha\n');
  editor._setCaretOffset(5, false);
  type(harness, editor, rootEl, '\n');
  check('Enter ended the line', editor.text === 'alpha\n\n', JSON.stringify(editor.text));
}

console.log('');
console.log('E. a layout-changing keystroke rebuilds');
{
  const { editor } = typed(harness, 'alpha\n\nbeta\n', '# ');
  check('typing "# " rebuilds (paragraph becomes a heading)', harness.rebuildCount() >= 1,
    'rebuilds=' + harness.rebuildCount());
  check('the block became a heading', editor.blocks[0].kind === 'heading',
    'kind=' + editor.blocks[0].kind);
  check('the document tail survived', editor.text.indexOf('beta') >= 0, JSON.stringify(editor.text));

  const { editor: e2 } = typed(harness, 'alpha\n\nbeta\n', '    ');
  check('four leading spaces rebuild (indented code)', harness.rebuildCount() >= 1,
    'rebuilds=' + harness.rebuildCount());
  check('the block became a code block', e2.blocks[0].kind === 'code_block',
    'kind=' + e2.blocks[0].kind);
  check('the document tail survived', e2.text.indexOf('beta') >= 0, JSON.stringify(e2.text));
}

console.log('');
console.log('F. the model and the block spans stay consistent while typing');
{
  const { editor, rootEl } = harness.createEditor('one\n\ntwo\n');
  editor._setCaretOffset(0, false);
  let consistent = true;
  for (const ch of 'typing away') {
    type(harness, editor, rootEl, ch);
    const active = editor.blocks[editor.activeIndex];
    if (!active || editor.caretOffset() < active.start || editor.caretOffset() > active.end) {
      consistent = false;
      break;
    }
    const last = editor.blocks[editor.blocks.length - 1];
    if (last.end > editor.text.length) { consistent = false; break; }
  }
  check('caret stayed inside the active block for every keystroke', consistent);
  check('the document tail is intact', editor.text.indexOf('two') >= 0, JSON.stringify(editor.text));
  check('no rebuild for pure inline text', harness.rebuildCount() === 0,
    'rebuilds=' + harness.rebuildCount());
}

console.log('');
console.log('G. randomised typing keeps every character, in order');
{
  // The strongest invariant available: typing a string one character at a time
  // into an empty document must leave the document equal to that string, apart
  // from the heading corrections (see H). A fixed seed keeps it reproducible.
  //
  // The alphabet deliberately excludes '\n': the stub cannot re-place the caret
  // after a rebuild the way a browser does, so multi-line typing is covered by
  // tools/web_browser.js, which drives the real DOM in headless Chrome.
  const alphabet = ['a', '$', '#', '-', '`', '~', '|', '>', ' ', '1', '*', '_', '\u4e2d'];
  let seed = 12345;
  const rnd = (n) => { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed % n; };

  let mismatches = 0;
  let firstFailure = null;
  for (let trial = 0; trial < 3000; trial++) {
    const length = 1 + rnd(8);
    let input = '';
    for (let i = 0; i < length; i++) input += alphabet[rnd(alphabet.length)];
    const { editor, rootEl } = harness.createEditor('');
    for (const ch of input) type(harness, editor, rootEl, ch);
    if (withoutHeadingFixes(editor.text) !== input) {
      mismatches += 1;
      if (!firstFailure) firstFailure = { input, got: editor.text };
    }
  }
  check('3000 random typing sessions produce exactly what was typed', mismatches === 0,
    mismatches + ' mismatch(es), first: ' + JSON.stringify(firstFailure));
}

console.log('');
console.log('H. a heading typed under a paragraph renders immediately');
{
  // Markdown needs a blank line before an ATX heading and a space after the #s;
  // without them the heading stays part of the paragraph and never renders.
  const { editor, rootEl } = harness.createEditor('some text\n');
  editor._setCaretOffset(10, false);
  for (const ch of '### Title') type(harness, editor, rootEl, ch);
  check('a blank line was inserted before the heading',
    editor.text === 'some text\n\n### Title', JSON.stringify(editor.text));
  check('the heading is its own block',
    editor.blocks.some((b) => b.kind === 'heading'),
    editor.blocks.map((b) => b.kind).join(','));

  // The reported case: no space after the #s.
  const { editor: e3, rootEl: r3 } = harness.createEditor('some text\n');
  e3._setCaretOffset(10, false);
  for (const ch of '###\u6ca1\u6709\u6e32\u67d3') type(harness, e3, r3, ch);
  check('"###\u6ca1\u6709\u6e32\u67d3" under a paragraph becomes a heading',
    e3.blocks.some((b) => b.kind === 'heading'), e3.blocks.map((b) => b.kind).join(','));
  check('the file stays standard Markdown (space added after the #s)',
    e3.text === 'some text\n\n### \u6ca1\u6709\u6e32\u67d3', JSON.stringify(e3.text));

  // Already separated: nothing is added. (The model keeps blocks newline
  // terminated, hence the trailing newline.)
  const { editor: e2, rootEl: r2 } = harness.createEditor('some text\n\n');
  e2._setCaretOffset(11, false);
  for (const ch of '### T') type(harness, e2, r2, ch);
  check('an existing blank line is not duplicated', e2.text === 'some text\n\n### T\n',
    JSON.stringify(e2.text));

  // The fixer leaves text without a line break alone: there is no "previous
  // line" for a heading to be separated from.
  const { editor: e4 } = harness.createEditor('x\n');
  check('no line break means nothing to fix',
    e4._withHeadingFixes('###Title', '').indexOf('\n') === -1,
    JSON.stringify(e4._withHeadingFixes('###Title', '')));
  check('a bare marker is separated but not given a space',
    e4._withHeadingFixes('text\n#', 'text\n') === 'text\n\n#',
    JSON.stringify(e4._withHeadingFixes('text\n#', 'text\n')));
}

console.log('');
console.log('I. a fenced code block is edited as a code box, not as source');
{
  // While the info string is being typed the fence line is still source text...
  const { editor } = harness.createEditor('```cpp');
  check('an unfinished fence line is not a code box',
    !editor._isCodeBox(editor.blocks[0]), JSON.stringify(editor.text));

  // ...and the Enter that completes it opens the box with the fence already closed.
  editor._setCaretOffset(editor.text.length, false);
  editor.insertLineBreak();
  check('Enter closes the fence and opens a code box',
    editor.text === '```cpp\n```\n' && editor._isCodeBox(editor.blocks[0]),
    JSON.stringify(editor.text));
  const element = editor.blockElements[editor.activeIndex];
  check('the block renders as a code box',
    element.classList.contains('md-code-box'), element.className);
  check('the block itself is not editable content',
    element.getAttribute('contenteditable') === 'false',
    String(element.getAttribute('contenteditable')));

  const input = element.querySelector('.md-code-input');
  check('the box holds a text box with the code in it', !!input && input.value === '',
    input ? JSON.stringify(input.value) : 'no text box');

  // Typing in the text box is written back as a fenced block.
  input.value = 'int main(){\n  return 0;\n}';
  editor._onCodeInput(editor.activeIndex);
  check('the code is written back inside the fences',
    editor.text === '```cpp\nint main(){\n  return 0;\n}\n```\n', JSON.stringify(editor.text));
  check('the block stays a code box while the writer types',
    editor.blockElements[editor.activeIndex].classList.contains('md-code-box'));

  // A line that closes the fence belongs to the document, not to the box.
  input.value = 'int main(){}';
  editor._onCodeInput(editor.activeIndex);
  check('rewriting the code keeps it in the block',
    editor.text === '```cpp\nint main(){}\n```\n', JSON.stringify(editor.text));

  // A fence with no language still works, and the language stays in the header.
  const { editor: e2 } = harness.createEditor('```\nx\n```\n');
  e2._setCaretOffset(6, false);
  const box2 = e2.blockElements[e2.activeIndex];
  check('a closed fence typed by hand is edited in a box too',
    box2.classList.contains('md-code-box'), box2.className);
  const input2 = box2.querySelector('.md-code-input');
  check('the text box starts from the code already there',
    !!input2 && input2.value === 'x', input2 ? JSON.stringify(input2.value) : 'no text box');
  input2.value = 'x\ny';
  e2._onCodeInput(e2.activeIndex);
  check('a second line is written back too',
    e2.text === '```\nx\ny\n```\n', JSON.stringify(e2.text));
}

console.log('');
console.log('J. a code box indents like code, and colours what it holds');
{
  // Opens a code box on a block that is already in the document, the way a click
  // inside the fence does.
  function openBox(text, at) {
    const { editor } = harness.createEditor(text);
    editor._setCaretOffset(at, false);
    const element = editor.blockElements[editor.activeIndex];
    const input = element.querySelector('.md-code-input');
    return { editor, element, input };
  }

  function codeKey(editor, input, key, extra) {
    const event = Object.assign({ key, target: input, preventDefault() {} }, extra || {});
    editor._onCodeKeyDown(event, editor.activeIndex);
    return event;
  }

  function putCaret(input, start, end) {
    input.selectionStart = start;
    input.selectionEnd = typeof end === 'number' ? end : start;
  }

  const cpp = openBox('```cpp\nint main(){\nreturn 0;\n}\n```\n', 12);
  check('the cpp block opens as a code box',
    !!cpp.input && cpp.element.classList.contains('md-code-box'),
    cpp.element.className);

  // The frame holds the header and the text box; the line the writer moves on to has
  // to sit outside it, or starting the next paragraph looks like it is still code.
  const cppFrame = cpp.element.querySelector('.md-code-frame');
  check('the code box frame holds the header and the text box',
    !!cppFrame && !!cppFrame.querySelector('.md-code-head') &&
    !!cppFrame.querySelector('.md-code-input'),
    cppFrame ? cppFrame.className : 'no frame');
  const cppChildren = cpp.element.childNodes;
  const cppAfter = cppChildren[cppChildren.length - 1];
  check('the line after the code sits outside that frame',
    !!cppFrame && !!cppAfter && cppAfter.classList.contains('md-code-after') &&
    !cppFrame.contains(cppAfter),
    cppAfter ? cppAfter.className : 'no line');

  // Enter after a line that opens a block starts one level deeper.
  putCaret(cpp.input, 'int main(){'.length);
  codeKey(cpp.editor, cpp.input, 'Enter');
  check('Enter after "{" indents the new line',
    cpp.input.value === 'int main(){\n    \nreturn 0;\n}',
    JSON.stringify(cpp.input.value));
  check('the indentation is in the document, not just the text box',
    cpp.editor.text === '```cpp\nint main(){\n    \nreturn 0;\n}\n```\n',
    JSON.stringify(cpp.editor.text));

  // Enter again keeps that indentation, and a closing bracket steps back out.
  codeKey(cpp.editor, cpp.input, 'Enter');
  check('Enter keeps the indentation of the line above',
    cpp.input.value === 'int main(){\n    \n    \nreturn 0;\n}',
    JSON.stringify(cpp.input.value));
  codeKey(cpp.editor, cpp.input, '}');
  check('a closing bracket typed on an indented line steps out one level',
    cpp.input.value === 'int main(){\n    \n}\nreturn 0;\n}',
    JSON.stringify(cpp.input.value));

  // Tab: four spaces for C++, and it never leaves the box.
  const cppTyped = openBox('```cpp\nx\n```\n', 8);
  putCaret(cppTyped.input, 0);
  codeKey(cppTyped.editor, cppTyped.input, 'Tab');
  check('Tab indents a cpp line by four spaces',
    cppTyped.input.value === '    x', JSON.stringify(cppTyped.input.value));
  check('the caret follows the indentation',
    cppTyped.input.selectionStart === 4 && cppTyped.input.selectionEnd === 4,
    cppTyped.input.selectionStart + ',' + cppTyped.input.selectionEnd);
  codeKey(cppTyped.editor, cppTyped.input, 'Tab', { shiftKey: true });
  check('Shift+Tab takes one level back',
    cppTyped.input.value === 'x', JSON.stringify(cppTyped.input.value));

  // Two spaces where that is the convention: the fence decides.
  const json = openBox('```json\n{\n}\n```\n', 8);
  putCaret(json.input, 0);
  codeKey(json.editor, json.input, 'Tab');
  check('a json block indents by two spaces',
    json.input.value === '  {\n}', JSON.stringify(json.input.value));

  // A tab-indented line outdents by one tab, not by a unit of spaces.
  const tabbed = openBox('```\n\t\tx\n```\n', 6);
  putCaret(tabbed.input, 2);
  codeKey(tabbed.editor, tabbed.input, 'Tab', { shiftKey: true });
  check('Shift+Tab takes one tab off a tab-indented line',
    tabbed.input.value === '\tx', JSON.stringify(tabbed.input.value));
  check('Enter keeps a tab indentation as well', (function () {
    putCaret(tabbed.input, 1);
    codeKey(tabbed.editor, tabbed.input, 'Enter');
    return tabbed.input.value === '\t\n\tx';
  })(), JSON.stringify(tabbed.input.value));

  // A selection that spans lines moves together.
  const many = openBox('```\na\nb\n```\n', 5);
  putCaret(many.input, 0, 3);
  codeKey(many.editor, many.input, 'Tab');
  check('Tab indents every line of the selection',
    many.input.value === '    a\n    b', JSON.stringify(many.input.value));
  check('the same lines stay selected',
    many.input.selectionStart === 0 && many.input.selectionEnd === many.input.value.length,
    many.input.selectionStart + ',' + many.input.selectionEnd);

  // The colours are painted on a layer of their own, under the text box.
  const shade = many.element.querySelector('.md-code-highlight');
  check('the code box has a colour layer', !!shade,
    shade ? shade.className : 'no .md-code-highlight');
  check('the layer holds the code itself',
    !!shade && shade.innerHTML === '    a\n    b', shade ? JSON.stringify(shade.innerHTML) : '-');
  check('the layer is not in the tab order of the text box',
    !!shade && shade.getAttribute('aria-hidden') === 'true',
    shade ? String(shade.getAttribute('aria-hidden')) : '-');

  // Markup in the code is escaped rather than rendered, and the layer keeps up
  // while the writer types.
  const html = openBox('```html\n```\n', 9);
  html.input.value = 'a < b & c';
  html.editor._onCodeInput(html.editor.activeIndex);
  const htmlShade = html.element.querySelector('.md-code-highlight');
  check('markup in the code is escaped on the layer',
    !!htmlShade && htmlShade.innerHTML === 'a &lt; b &amp; c',
    htmlShade ? JSON.stringify(htmlShade.innerHTML) : '-');

  // A read-only document takes no indentation: the keys reach the text box, but the
  // document must not change.
  const locked = openBox('```cpp\nint main(){\n}\n```\n', 8);
  locked.editor.setReadOnly(true);
  putCaret(locked.input, 0);
  codeKey(locked.editor, locked.input, 'Enter');
  codeKey(locked.editor, locked.input, 'Tab');
  check('a read-only code box ignores Enter and Tab',
    locked.input.value === 'int main(){\n}' &&
    locked.editor.text === '```cpp\nint main(){\n}\n```\n',
    JSON.stringify(locked.input.value) + ' / ' + JSON.stringify(locked.editor.text));

  // Backspace in a box that holds no code removes the box itself (fences and all),
  // together with one of the blank lines that surrounded it.
  const removable = openBox('text\n\n```js\n```\n\nmore\n', 12);
  check('an empty fenced block opens as an empty code box',
    !!removable.input && removable.input.value === '',
    removable.input ? JSON.stringify(removable.input.value) : 'no box');
  codeKey(removable.editor, removable.input, 'Backspace');
  check('Backspace in an empty code box removes the block and one blank line',
    removable.editor.text === 'text\n\nmore\n', JSON.stringify(removable.editor.text));

  // With code in the box there is something to delete, so the block stays.
  const keepCode = openBox('```cpp\nint x = 1;\n```\n', 12);
  codeKey(keepCode.editor, keepCode.input, 'Backspace');
  check('Backspace with code in the box leaves the block alone',
    keepCode.editor.text === '```cpp\nint x = 1;\n```\n' &&
    keepCode.element.classList.contains('md-code-box'),
    JSON.stringify(keepCode.editor.text));

  // The line the box leaves for the next paragraph: Backspace there must not hand the
  // caret back to the code (Chrome joined it with the last code line, so the writer
  // went on typing code), and on an empty box it removes the box.
  function afterLineOf(editor) {
    const element = editor.blockElements[editor.activeIndex];
    return element.childNodes[element.childNodes.length - 1];
  }

  const afterKeep = openBox('text\n\n```js\nlet x = 1;\n```\n', 12);
  const afterKeepLine = afterLineOf(afterKeep.editor);
  let afterPrevented = false;
  afterKeep.editor._onCodeAfterKeyDown({
    key: 'Backspace',
    target: afterKeepLine,
    preventDefault: () => { afterPrevented = true; },
  }, afterKeep.editor.activeIndex);
  check('Backspace on the line after a non-empty box is swallowed',
    afterPrevented && afterKeep.editor.text === 'text\n\n```js\nlet x = 1;\n```\n',
    'prevented=' + afterPrevented + ' text=' + JSON.stringify(afterKeep.editor.text));

  const afterEmpty = openBox('text\n\n```js\n```\n\nmore\n', 12);
  const afterEmptyLine = afterLineOf(afterEmpty.editor);
  afterEmpty.editor._onCodeAfterKeyDown({
    key: 'Backspace',
    target: afterEmptyLine,
    preventDefault: () => {},
  }, afterEmpty.editor.activeIndex);
  check('Backspace on the line after an empty box removes the box',
    afterEmpty.editor.text === 'text\n\nmore\n', JSON.stringify(afterEmpty.editor.text));

  // Every spelling of Backspace is stopped there, not just the plain one.
  const afterCtrl = openBox('text\n\n```js\nlet x = 1;\n```\n', 12);
  const afterCtrlLine = afterLineOf(afterCtrl.editor);
  let ctrlPrevented = false;
  afterCtrl.editor._onCodeAfterKeyDown({
    key: 'Backspace',
    ctrlKey: true,
    target: afterCtrlLine,
    preventDefault: () => { ctrlPrevented = true; },
  }, afterCtrl.editor.activeIndex);
  check('Ctrl+Backspace on that line is stopped too',
    ctrlPrevented && afterCtrl.editor.text === 'text\n\n```js\nlet x = 1;\n```\n',
    'prevented=' + ctrlPrevented + ' text=' + JSON.stringify(afterCtrl.editor.text));

  // A block that follows a code box: Backspace at its very start has nothing to join
  // with, and the engine's own join is what handed the caret back into the code.
  function boundaryKeyDown(editor, event) {
    editor._handleBoundaryKeyDown(Object.assign({
      key: 'Backspace',
      preventDefault() {},
    }, event));
  }

  const boundary = openBox('para\n\n```js\nlet x = 1;\n```\n\nafter\n', 18);
  const boundaryIndex = boundary.editor.blocks.length - 1;
  boundary.editor._setCaretOffset(boundary.editor.blocks[boundaryIndex].start, false);
  let boundaryPrevented = false;
  const boundaryTarget = boundary.editor.blockElements[boundaryIndex];
  boundaryKeyDown(boundary.editor, {
    target: boundaryTarget,
    preventDefault: () => { boundaryPrevented = true; },
  });
  check('Backspace at the start of a block after a code box is stopped',
    boundaryPrevented && boundary.editor.text === 'para\n\n```js\nlet x = 1;\n```\n\nafter\n',
    'prevented=' + boundaryPrevented + ' text=' + JSON.stringify(boundary.editor.text));

  const boundaryEmpty = openBox('para\n\n```js\n```\n\nafter\n', 12);
  const boundaryEmptyIndex = boundaryEmpty.editor.blocks.length - 1;
  const boundaryEmptyCaret = boundaryEmpty.editor.blocks[boundaryEmptyIndex].start;
  boundaryEmpty.editor._setCaretOffset(boundaryEmptyCaret, false);
  boundaryKeyDown(boundaryEmpty.editor, {
    target: boundaryEmpty.editor.blockElements[boundaryEmptyIndex],
    preventDefault: () => {},
  });
  check('and an empty box in front of it goes with the press',
    boundaryEmpty.editor.text === 'para\n\nafter\n',
    JSON.stringify(boundaryEmpty.editor.text));

  // Keys in the code box itself must never be touched by that guard.
  let typedPrevented = false;
  const boxIndex = boundary.editor.activeIndex;
  boundary.editor._setCaretOffset(boundary.editor.blocks[boxIndex].contentStart + 3, false);
  boundary.editor._handleBoundaryKeyDown({
    key: 'Backspace',
    target: { classList: { contains: (name) => name === 'md-code-input' } },
    preventDefault: () => { typedPrevented = true; },
  });
  check('a Backspace in the code text box is left to the text box',
    !typedPrevented, 'prevented=' + typedPrevented);
}

console.log('');
console.log('K. the plain-text view and the rendered view stay in step');
{
  const { editor } = harness.createEditor('one\n');
  editor.setSourceMode(true);
  check('the plain-text view starts from the document',
    editor.textarea.value === 'one\n', JSON.stringify(editor.textarea.value));

  // What is typed there is the document: the rendered view is built from the
  // re-parsed blocks, so they have to follow the edit and not keep describing the
  // text as it was.
  editor.textarea.value = 'one\n\n## two\n';
  editor._handleInput({ target: editor.textarea });
  check('editing the plain text updates the document',
    editor.text === 'one\n\n## two\n', JSON.stringify(editor.text));
  check('the block list follows the plain-text edit',
    editor.blocks.length === 2 && editor.blocks[1].kind === 'heading',
    editor.blocks.map((b) => b.kind).join(','));

  editor.setSourceMode(false);
  check('switching back renders the edited document',
    editor.blockElements.length === 2 &&
    String(editor.blockElements[1].innerHTML).indexOf('<h2') === 0,
    editor.blockElements[1] ? JSON.stringify(editor.blockElements[1].innerHTML) : 'no block');

  // A document that arrives from elsewhere while the plain text is open must replace
  // it: otherwise the next keystroke writes the previous document back.
  editor.setSourceMode(true);
  editor.setDocument('third\n', { revision: 'r2' });
  check('a document from the host replaces the open plain text',
    editor.textarea.value === 'third\n', JSON.stringify(editor.textarea.value));
  editor.setSourceMode(false);
  check('and that is what the rendered view shows',
    editor.text === 'third\n' && editor.blockElements.length === 1,
    JSON.stringify(editor.text));

  // The Markdown of a code block (shown by clicking its header) is editable text: a
  // press inside it belongs to the browser, so the caret goes where it was clicked
  // and the block stays in the source view.
  const source = harness.createEditor('```cpp\nint x = 1;\n```\n');
  source.editor._setCaretOffset(12, false);
  source.editor._codeSourceView = true;
  source.editor._repaintBlock(source.editor.activeIndex);
  const sourceBlock = source.editor.blockElements[source.editor.activeIndex];
  check('clicking the header shows that block as Markdown source',
    sourceBlock.classList.contains('md-source'), sourceBlock.className);

  let prevented = false;
  const caretBefore = source.editor.caretOffset();
  source.editor._handlePointerDown({
    target: { closest: (selector) => (selector === '[data-md-block]' ? sourceBlock : null) },
    preventDefault: () => { prevented = true; },
  });
  check('a press inside the source of a code block is left to the browser',
    !prevented && sourceBlock.classList.contains('md-source') &&
    !sourceBlock.classList.contains('md-code-box') &&
    source.editor.caretOffset() === caretBefore,
    'prevented=' + prevented + ' class=' + sourceBlock.className +
    ' caret=' + source.editor.caretOffset() + ' was=' + caretBefore);

  // A *rendered* code block (one the caret is not in) still opens its code box on a
  // press - that behaviour must survive the fix above.
  const rendered = harness.createEditor('text\n\n```cpp\nint x = 1;\n```\n');
  const renderedBlock = rendered.editor.blockElements[1];
  let opened = false;
  rendered.editor._handlePointerDown({
    target: { closest: (selector) => (selector === '[data-md-block]' ? renderedBlock : null) },
    preventDefault: () => { opened = true; },
  });
  check('a press on a rendered code block still opens its code box',
    opened && rendered.editor.blockElements[1].classList.contains('md-code-box'),
    'prevented=' + opened + ' class=' + rendered.editor.blockElements[1].className);
}

console.log('');
console.log(failures === 0 ? 'typing: all checks passed' : 'typing: ' + failures + ' FAILED');
process.exit(failures === 0 ? 0 : 1);
