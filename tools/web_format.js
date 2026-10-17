/* Front-end checks for the formatting toolbar.
 *
 * The toolbar exists for people who do not write Markdown: every button has to
 * produce correct Markdown in the model, put the caret where the writer can carry
 * on typing, and never rewrite text that is already there. These checks run the
 * real editor.js against the DOM stub, so a broken marker or a caret in the wrong
 * place fails here instead of inside WebView2.
 *
 * Usage: node tools/web_format.js
 */
'use strict';

const path = require('path');
const harness = require('./web_harness').load(path.resolve(__dirname, '..'));

let failures = 0;
function check(label, ok, detail) {
  console.log((ok ? '  ok    ' : '  FAIL  ') + label + (ok || !detail ? '' : '  [' + detail + ']'));
  if (!ok) failures += 1;
}

// A fresh editor holding `text`, with the caret placed at `caret`.
function editorWith(text, caret) {
  const made = harness.createEditor(text);
  const editor = made.editor;
  editor._setCaretOffset(typeof caret === 'number' ? caret : text.length, false);
  return editor;
}

const caret = (editor) => editor.caretOffset();

console.log('block buttons');

{
  const editor = editorWith('');
  editor.insertBlock('heading1');
  check('H1 on an empty document writes the heading marker', editor.text === '# ', JSON.stringify(editor.text));
  check('H1 leaves the caret after the marker', caret(editor) === 2, String(caret(editor)));
}

{
  // The button must not rewrite a paragraph: it starts the heading on the next
  // line, which is what "press H2 and type" means to someone who does not write
  // Markdown.
  const editor = editorWith('para one\n');
  editor._setCaretOffset(3, false);
  editor.insertBlock('heading2');
  check('H2 keeps the paragraph and starts a new line', editor.text === 'para one\n## \n', JSON.stringify(editor.text));
}

{
  const editor = editorWith('');
  editor.insertBlock('bullet');
  editor.insertBlock('paragraph');
  check('bullet then paragraph', editor.text.indexOf('- ') === 0, JSON.stringify(editor.text));
}

{
  const text = [
    ['task', '- [ ] '],
    ['quote', '> '],
    ['number', '1. '],
  ];
  for (const [kind, marker] of text) {
    const editor = editorWith('');
    editor.insertBlock(kind);
    check(kind + ' marker is ' + JSON.stringify(marker), editor.text === marker, JSON.stringify(editor.text));
  }
}

{
  const editor = editorWith('');
  editor.insertBlock('divider');
  check('divider inserts a rule', editor.text === '---', JSON.stringify(editor.text));
}

{
  // A fence that is not closed would swallow the rest of the document.
  const editor = editorWith('before\n');
  editor._setCaretOffset(7, false);
  editor.insertBlock('code');
  check('code block inserts a closed fence', editor.text === 'before\n```\n\n```\n', JSON.stringify(editor.text));
  // The caret has to end up inside the fence. The DOM stub cannot model a text
  // box's own selection (the real caret path is covered by tools/web_browser.js),
  // so this only checks that it stayed within the block that was just inserted.
  const inside = caret(editor) >= 7 && caret(editor) <= 15;
  check('the caret stays inside the new fence', inside, String(caret(editor)));
}

console.log('lists continue on Enter');

{
  // The caret sits at the end of the item text (offset 6), which is where a
  // writer who just typed "- item" is. The document keeps its trailing newline,
  // so the new item line is followed by it.
  const editor = editorWith('- item\n', 6);
  editor.insertLineBreak();
  check('Enter continues a bullet list', editor.text === '- item\n- \n', JSON.stringify(editor.text));
  check('the caret sits after the new marker', caret(editor) === 9, String(caret(editor)));
}

{
  const editor = editorWith('- item\n', 2);
  editor.insertLineBreak();
  check('Enter in the middle of an item does not duplicate the marker',
        editor.text === '- \nitem\n', JSON.stringify(editor.text));
}

{
  const editor = editorWith('1. first\n', 8);
  editor.insertLineBreak();
  check('Enter numbers the next item', editor.text === '1. first\n2. \n', JSON.stringify(editor.text));
}

{
  const editor = editorWith('- [ ] done?\n', 11);
  editor.insertLineBreak();
  check('Enter continues a task list unchecked', editor.text === '- [ ] done?\n- [ ] \n',
        JSON.stringify(editor.text));
}

{
  // Second Enter on an item with no text ends the list instead of adding another
  // empty item forever.
  const editor = editorWith('- item\n- \n', 9);
  editor.insertLineBreak();
  check('Enter on an empty item ends the list', editor.text === '- item\n\n', JSON.stringify(editor.text));
}

{
  const editor = editorWith('plain text\n', 10);
  editor.insertLineBreak();
  check('Enter on a normal line stays a plain newline', editor.text === 'plain text\n\n',
        JSON.stringify(editor.text));
}

console.log('tables');

let tableEditor = null;
{
  const editor = editorWith('');
  editor.insertBlock('table');
  tableEditor = editor;
  check('table header row', editor.text.indexOf('| Column 1 | Column 2 | Column 3 |') === 0,
        JSON.stringify(editor.text));
  check('table alignment row', editor.text.indexOf('| --- | --- | --- |') >= 0,
        JSON.stringify(editor.text));
  check('table body row', editor.text.indexOf('|  |  |  |') >= 0, JSON.stringify(editor.text));
  const selected = editor.text.slice(2, 2 + 'Column 1'.length);
  check('the first header cell is ready to be typed over', selected === 'Column 1', selected);
}

{
  const editor = editorWith('| a | b |\n| --- | --- |\n| c | d |\n');
  editor._setCaretOffset(4, false); // inside the "a" cell
  editor.tableAddRow();
  check('add row appends after the caret row',
        editor.text.indexOf('| a | b |\n|  |  |\n| --- | --- |') === 0, JSON.stringify(editor.text));
}

{
  const editor = editorWith('| a | b |\n| --- | --- |\n| c | d |\n');
  editor._setCaretOffset(4, false);
  editor.tableAddColumn();
  const lines = editor.text.split('\n');
  check('add column grows a normal row', lines[0] === '| a | b |  |', JSON.stringify(lines[0]));
  check('add column grows the alignment row with dashes', lines[1] === '| --- | --- | --- |',
        JSON.stringify(lines[1]));
  check('add column grows a body row', lines[2] === '| c | d |  |', JSON.stringify(lines[2]));
}

console.log('inline buttons');

{
  const editor = editorWith('make this bold\n');
  editor._selectOffsets(5, 9); // "this"
  editor.wrapSelection('bold');
  check('bold wraps the selection', editor.text === 'make **this** bold\n', JSON.stringify(editor.text));
  const offsets = editor.selectionOffsets();
  check('the wrapped word stays selected',
        Math.min(offsets.anchor, offsets.head) === 7 && Math.max(offsets.anchor, offsets.head) === 11,
        JSON.stringify(offsets));
}

{
  const editor = editorWith('word\n');
  editor._selectOffsets(0, 4);
  editor.wrapSelection('italic');
  check('italic uses a single marker', editor.text === '*word*\n', JSON.stringify(editor.text));
}

{
  const editor = editorWith('code\n');
  editor._selectOffsets(0, 4);
  editor.wrapSelection('inlineCode');
  check('inline code uses backticks', editor.text === '`code`\n', JSON.stringify(editor.text));
}

{
  // Nothing selected: the markers go in empty and the caret sits between them, so
  // the next thing typed is bold.
  const editor = editorWith('a\n');
  editor._setCaretOffset(1, false);
  editor.wrapSelection('bold');
  check('bold with no selection inserts empty markers', editor.text === 'a****\n',
        JSON.stringify(editor.text));
  check('the caret sits between the markers', caret(editor) === 3, String(caret(editor)));
}

{
  const editor = editorWith('a\n');
  editor._setCaretOffset(1, false);
  editor.wrapSelection('link');
  check('link with no selection inserts a placeholder', editor.text === 'a[link text](url)\n',
        JSON.stringify(editor.text));
  const offsets = editor.selectionOffsets();
  check('the placeholder link text is selected',
        editor.text.slice(Math.min(offsets.anchor, offsets.head),
                          Math.max(offsets.anchor, offsets.head)) === 'link text',
        JSON.stringify(editor.text));
}

{
  const editor = editorWith('logo\n');
  editor._selectOffsets(0, 4);
  editor.wrapSelection('image');
  check('image uses the selected text as its description',
        editor.text === '![logo](path/to/image.png)\n', JSON.stringify(editor.text));
}

{
  const editor = editorWith('E = mc^2\n');
  editor._selectOffsets(0, 8);
  editor.wrapSelection('formula');
  check('formula wraps in dollars', editor.text === '$E = mc^2$\n', JSON.stringify(editor.text));
}

{
  const editor = editorWith('x\n');
  editor.setReadOnly(true);
  editor.insertBlock('heading1');
  editor.wrapSelection('bold');
  check('a read-only document is not edited by the toolbar', editor.text === 'x\n',
        JSON.stringify(editor.text));
}

console.log(failures === 0 ? '\nall formatting checks passed' : '\n' + failures + ' check(s) failed');
process.exit(failures === 0 ? 0 : 1);
