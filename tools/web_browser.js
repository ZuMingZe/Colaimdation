/* Drive the real editor front-end in headless Chrome over the DevTools protocol.
 *
 * The DOM stub used by the other checks cannot model what a browser does with a
 * selection when contenteditable nodes are replaced, which is exactly where the
 * caret bugs lived. This harness loads the actual assets, types through real
 * key events and inspects the real DOM.
 *
 * Usage: node tools/web_browser.js
 */
'use strict';

const fs = require('fs');
const path = require('path');
const os = require('os');
const { spawn } = require('child_process');

const root = path.resolve(__dirname, '..');

function findBrowser() {
  const candidates = [
    process.env.CHROME_PATH,
    'C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe',
    'C:\\Program Files (x86)\\Google\\Chrome\\Application\\chrome.exe',
    'C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe',
    'C:\\Program Files\\Microsoft\\Edge\\Application\\msedge.exe',
  ].filter(Boolean);
  for (const candidate of candidates) if (fs.existsSync(candidate)) return candidate;
  return null;
}

class Session {
  constructor(ws) {
    this.ws = ws;
    this.nextId = 1;
    this.pending = new Map();
    ws.addEventListener('message', (event) => {
      let message;
      try { message = JSON.parse(event.data); } catch (error) { return; }
      if (message.id && this.pending.has(message.id)) {
        const { resolve, reject } = this.pending.get(message.id);
        this.pending.delete(message.id);
        if (message.error) reject(new Error(JSON.stringify(message.error)));
        else resolve(message.result);
      }
    });
  }
  send(method, params) {
    const id = this.nextId++;
    return new Promise((resolve, reject) => {
      this.pending.set(id, { resolve, reject });
      this.ws.send(JSON.stringify({ id, method, params: params || {} }));
      setTimeout(() => {
        if (this.pending.has(id)) {
          this.pending.delete(id);
          reject(new Error(method + ' timed out'));
        }
      }, 20000);
    });
  }
  async eval(expression) {
    const result = await this.send('Runtime.evaluate', {
      expression,
      returnByValue: true,
      awaitPromise: true,
    });
    if (result.exceptionDetails) {
      throw new Error('page error: ' + JSON.stringify(result.exceptionDetails.exception || result.exceptionDetails));
    }
    return result.result.value;
  }
}

async function main() {
  const browser = findBrowser();
  if (!browser) {
    console.log('no Chrome/Edge found: skipping the browser checks');
    process.exit(0);
  }

  const userDataDir = fs.mkdtempSync(path.join(os.tmpdir(), 'colai-cdp-'));
  // The page is served from a copy under an ASCII path: Chrome on Windows fails
  // to load file:// URLs whose path contains non-ASCII characters (the project
  // lives in one), which silently produces an empty document.
  const siteDir = fs.mkdtempSync(path.join(os.tmpdir(), 'colai-site-'));
  fs.cpSync(path.join(root, 'web'), siteDir, { recursive: true });
  fs.cpSync(path.join(root, 'third_party', 'vendor'), path.join(siteDir, 'vendor'), { recursive: true });

  const port = 9333;
  const child = spawn(browser, [
    '--headless=new',
    '--disable-gpu',
    '--no-first-run',
    '--no-default-browser-check',
    '--remote-debugging-port=' + port,
    '--user-data-dir=' + userDataDir,
    '--allow-file-access-from-files',
    'about:blank',
  ], { stdio: 'ignore' });

  const cleanup = () => {
    try { child.kill(); } catch (error) { /* ignore */ }
    try { fs.rmSync(userDataDir, { recursive: true, force: true }); } catch (error) { /* ignore */ }
    try { fs.rmSync(siteDir, { recursive: true, force: true }); } catch (error) { /* ignore */ }
  };

  try {
    // Wait for the DevTools endpoint, then open a page target explicitly:
    // /json/list also contains extension background pages, and connecting to one
    // of those silently inspects the wrong document.
    let target = null;
    for (let attempt = 0; attempt < 60 && !target; attempt++) {
      await new Promise((r) => setTimeout(r, 250));
      try {
        const created = await fetch(`http://127.0.0.1:${port}/json/new?about:blank`, { method: 'PUT' });
        if (created.ok) target = await created.json();
      } catch (error) { /* not up yet */ }
    }
    if (!target || !target.webSocketDebuggerUrl) {
      const list = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
      target = list.find((t) => t.type === 'page' && t.webSocketDebuggerUrl);
    }
    if (!target || !target.webSocketDebuggerUrl) throw new Error('the browser did not expose a page target');

    const ws = new WebSocket(target.webSocketDebuggerUrl);
    await new Promise((resolve, reject) => {
      ws.addEventListener('open', resolve);
      ws.addEventListener('error', () => reject(new Error('cannot connect to DevTools')));
    });
    const session = new Session(ws);
    session.onConsole = null;
    await session.send('Runtime.enable');
    await session.send('Log.enable');
    await session.send('Page.enable');
    // Real key events only reach the page when the engine considers it focused.
    await session.send('Emulation.setFocusEmulationEnabled', { enabled: true });
    await session.send('Page.bringToFront');
    ws.addEventListener('message', (event) => {
      let message;
      try { message = JSON.parse(event.data); } catch (error) { return; }
      if (message.method === 'Runtime.exceptionThrown') {
        const details = message.params.exceptionDetails;
        console.log('  [page error] ' + (details.exception && details.exception.description || details.text));
      }
      if (message.method === 'Runtime.consoleAPICalled' && message.params.type === 'error') {
        console.log('  [console.error] ' + message.params.args.map((a) => a.value || a.description).join(' '));
      }
    });

    // Load the real page with a stub host, exactly as the editor sees it inside
    // WebView2 except that messages go through window.colaimdationHost.
    const pageUrl = 'file:///' + path.join(siteDir, 'index.html').replace(/\\/g, '/');
    console.log('  loading ' + pageUrl);
    await session.send('Page.navigate', { url: pageUrl });
    await new Promise((r) => setTimeout(r, 2000));

    console.log('  document.readyState=' + await session.eval('document.readyState') +
      ' location=' + JSON.stringify(await session.eval('location.href')) +
      ' title=' + JSON.stringify(await session.eval('document.title')));
    console.log('  scripts=' + await session.eval('document.scripts.length') +
      ' editor div=' + await session.eval('!!document.getElementById("editor")'));
    console.log('  ColaiEditor=' + await session.eval('typeof window.ColaiEditor') +
      ' ColaiRender=' + await session.eval('typeof window.ColaiRender'));

    const ready = await session.eval('!!window.ColaiEditor && !!window.colaimdation && !!window.colaimdation.editor()');
    if (!ready) {
      const html = await session.eval('document.body ? document.body.innerHTML.slice(0, 400) : "(no body)"');
      throw new Error('the page did not initialise the editor; body was: ' + html);
    }

    // Enter in a contenteditable produces DOM structure, not '\n' text:
    // Chrome wraps the following line in a <div>, and an empty line becomes
    // <div><br></div>. Build exactly that and let the editor's own input handler
    // run, which is what a real key press does.
    await session.eval(`
      window.__enter = function () {
        var editor = window.colaimdation.editor();
        var element = editor.activeIndex >= 0 ? editor.blockElements[editor.activeIndex] : null;
        if (!element) return editor.text;
        var selection = window.getSelection();
        var node = selection && selection.anchorNode;
        var offset = selection ? selection.anchorOffset : 0;
        if (!node || !element.contains(node)) {
          node = element.firstChild || element.appendChild(document.createTextNode(''));
          offset = node.nodeValue.length;
        }
        var line = document.createElement('div');
        if (node.nodeType === 3) {
          var rest = node.nodeValue.slice(offset);
          node.nodeValue = node.nodeValue.slice(0, offset);
          if (rest) line.appendChild(document.createTextNode(rest));
        }
        if (!line.firstChild) line.appendChild(document.createElement('br'));
        element.appendChild(line);
        var range = document.createRange();
        if (line.firstChild.nodeType === 3) { range.setStart(line.firstChild, 0); }
        else { range.setStart(line, 0); }
        range.collapse(true);
        selection.removeAllRanges();
        selection.addRange(range);
        element.dispatchEvent(new Event('input', { bubbles: true }));
        return editor.text;
      };
    `);
    await session.eval(`
      // Places the DOM selection wherever the editor believes its caret is, so
      // insertText lands exactly where the model expects it.
      window.__syncCaret = function () {
        var editor = window.colaimdation.editor();
        var offset = editor.caretOffset();
        var index = editor._blockIndexForOffset(offset);
        var element = index >= 0 ? editor.blockElements[index] : null;
        if (!element) return;
        element.focus();
        var local = Math.max(0, offset - editor.blocks[index].start);
        var walker = document.createTreeWalker(element, NodeFilter.SHOW_TEXT, null);
        var range = document.createRange();
        var placed = false;
        while (walker.nextNode()) {
          var node = walker.currentNode;
          var length = node.nodeValue.length;
          if (local <= length) {
            range.setStart(node, local);
            placed = true;
            break;
          }
          local -= length;
        }
        if (!placed) {
          range.selectNodeContents(element);
          range.collapse(false);
        } else {
          range.collapse(true);
        }
        var selection = window.getSelection();
        selection.removeAllRanges();
        selection.addRange(range);
      };
      window.__focusEditor = function () {
        var editor = window.colaimdation.editor();
        if (editor.activeIndex >= 0 && editor.blockElements[editor.activeIndex]) {
          window.__syncCaret();
          return;
        }
        var emptyEl = editor.root.querySelector('.md-empty') || editor.root;
        emptyEl.focus();
        var emptyRange = document.createRange();
        emptyRange.selectNodeContents(emptyEl);
        emptyRange.collapse(false);
        var emptySelection = window.getSelection();
        emptySelection.removeAllRanges();
        emptySelection.addRange(emptyRange);
      };
      window.__type = function (text) {
        var editor = window.colaimdation.editor();
        window.__focusEditor();
        document.execCommand('insertText', false, text);
        // execCommand fires 'input' itself; the editor's handler is bound to it.
        return editor.text;
      };
      window.__state = function () {
        var editor = window.colaimdation.editor();
        return {
          text: editor.text,
          blocks: editor.blocks.map(function (b) { return b.kind + '[' + b.start + ',' + b.end + ')'; }),
          caret: editor.caretOffset(),
          rendered: Array.prototype.map.call(editor.root.children, function (el) {
            return el.className + '|' + (el.textContent || '').slice(0, 40);
          }),
        };
      };
    `);

    let failures = 0;
    const check = (label, ok, detail) => {
      console.log((ok ? '  ok    ' : '  FAIL  ') + label + (ok || !detail ? '' : '  [' + detail + ']'));
      if (!ok) failures += 1;
    };

    // The reported workflow, driven through the editor's own input handler with
    // the exact DOM structure a browser produces for Enter.
    await session.eval(`
      window.__enterAt = function (editor) {
        // Re-resolve the element every time: the editor replaces its block
        // elements when it re-renders, so a captured reference goes stale and the
        // DOM surgery would land outside the live tree.
        var element = editor.activeIndex >= 0 ? editor.blockElements[editor.activeIndex] : null;
        if (!element) element = editor.root.querySelector('.md-empty') || editor.root;
        var selection = window.getSelection();
        var node = selection && selection.anchorNode;
        var offset = selection ? selection.anchorOffset : 0;
        if (!node || !element.contains(node)) {
          // Derive the position from the editor's own caret.
          var walker = document.createTreeWalker(element, NodeFilter.SHOW_TEXT, null);
          var local = editor.activeIndex >= 0
            ? editor.caretOffset() - editor.blocks[editor.activeIndex].start
            : 0;
          node = null;
          while (walker.nextNode()) {
            var candidate = walker.currentNode;
            if (local <= candidate.nodeValue.length) { node = candidate; offset = local; break; }
            local -= candidate.nodeValue.length;
          }
          if (!node) { node = element.appendChild(document.createTextNode('')); offset = 0; }
        }
        var line = document.createElement('div');
        var rest = '';
        if (node.nodeType === 3) {
          rest = node.nodeValue.slice(offset);
          node.nodeValue = node.nodeValue.slice(0, offset);
        }
        if (rest) line.appendChild(document.createTextNode(rest));
        else line.appendChild(document.createElement('br'));
        // Insert directly after the line the caret is on, which is where the
        // browser puts it.
        var anchor = node.nodeType === 3 ? node.parentNode : node;
        if (anchor === element) element.appendChild(line);
        else if (anchor.nextSibling) anchor.parentNode.insertBefore(line, anchor.nextSibling);
        else anchor.parentNode.appendChild(line);
        var range = document.createRange();
        range.setStart(line.firstChild && line.firstChild.nodeType === 3 ? line.firstChild : line, 0);
        range.collapse(true);
        selection.removeAllRanges();
        selection.addRange(range);
        element.dispatchEvent(new Event('input', { bubbles: true }));
        return editor.text;
      };
      // Typing one character, with the caret placed where the editor believes it
      // is, then an input event - i.e. what the browser does for a keystroke.
      window.__key = function (editor, ch) {
        var element = editor.activeIndex >= 0 ? editor.blockElements[editor.activeIndex] : null;
        if (!element) element = editor.root.querySelector('.md-empty') || editor.root;
        var offset = editor.activeIndex >= 0 ? editor.caretOffset() : 0;
        var target = null;
        var local = offset - (editor.activeIndex >= 0 ? editor.blocks[editor.activeIndex].start : 0);
        var walker = document.createTreeWalker(element, NodeFilter.SHOW_TEXT, null);
        while (walker.nextNode()) {
          var candidate = walker.currentNode;
          if (local <= candidate.nodeValue.length) { target = candidate; break; }
          local -= candidate.nodeValue.length;
        }
        if (!target) {
          // No text node at the caret (an empty line holds only a <br>): the
          // browser replaces that placeholder with a text node, it does not
          // append one at the end.
          var placeholder = element.querySelector('br');
          target = document.createTextNode('');
          if (placeholder && placeholder.parentNode) placeholder.parentNode.replaceChild(target, placeholder);
          else element.appendChild(target);
          local = 0;
        }
        target.nodeValue = target.nodeValue.slice(0, local) + ch + target.nodeValue.slice(local);
        var range = document.createRange();
        range.setStart(target, local + ch.length);
        range.collapse(true);
        var selection = window.getSelection();
        selection.removeAllRanges();
        selection.addRange(range);
        element.dispatchEvent(new Event('input', { bubbles: true }));
        return editor.text;
      };
    `);

    // Real key presses. The engine's own editing code runs for these, which is the
    // only way to see what a writer's Enter actually does: it is handled as
    // "insertParagraph", and Chromium answers it by *splitting* the editable block
    // element and giving the new line a copy of it (same data-md-block), not by
    // putting the line inside the block. Anything driven through a hand-built DOM
    // or through execCommand misses that shape.
    const ENTER_KEY = { key: 'Enter', code: 'Enter', windowsVirtualKeyCode: 13, nativeVirtualKeyCode: 13 };
    async function realEnter() {
      await session.send('Input.dispatchKeyEvent', { type: 'keyDown', ...ENTER_KEY });
      await session.send('Input.dispatchKeyEvent', { type: 'char', ...ENTER_KEY, text: '\r', unmodifiedText: '\r' });
      await session.send('Input.dispatchKeyEvent', { type: 'keyUp', ...ENTER_KEY });
      await new Promise((r) => setTimeout(r, 200));
    }
    async function realChar(ch) {
      const code = ch.charCodeAt(0);
      const common = { key: ch, windowsVirtualKeyCode: code, nativeVirtualKeyCode: code };
      await session.send('Input.dispatchKeyEvent', { type: 'keyDown', ...common });
      await session.send('Input.dispatchKeyEvent', { type: 'char', ...common, text: ch, unmodifiedText: ch });
      await session.send('Input.dispatchKeyEvent', { type: 'keyUp', ...common });
      await new Promise((r) => setTimeout(r, 150));
    }

    // A key the editor handles itself: in a real session preventDefault stops the
    // browser from inserting anything, so only the keydown (and its keyup) may be
    // dispatched. Sending the "char" event as well would type the text twice.
    async function realKeyOnly(key, code, virtualKey, modifiers) {
      const common = {
        key,
        code,
        windowsVirtualKeyCode: virtualKey,
        nativeVirtualKeyCode: virtualKey,
        modifiers: modifiers || 0,
      };
      await session.send('Input.dispatchKeyEvent', { type: 'keyDown', ...common });
      await session.send('Input.dispatchKeyEvent', { type: 'keyUp', ...common });
      await new Promise((r) => setTimeout(r, 150));
    }

    await session.eval(`
      // Records what the engine asked the editor to do, so a check can tell a
      // keystroke the editor handled from one the engine performed itself.
      window.__inputTypes = [];
      window.__keyLog = [];
      window.colaimdation.editor().root.addEventListener('input', function (e) {
        window.__inputTypes.push(e.inputType || 'input');
      }, true);
      window.colaimdation.editor().root.addEventListener('keydown', function (e) {
        window.__keyLog.push('keydown:' + e.key);
      }, true);
      window.colaimdation.editor().root.addEventListener('beforeinput', function (e) {
        window.__keyLog.push('beforeinput:' + e.inputType);
      }, true);
      window.colaimdation.editor().root.addEventListener('compositionstart', function () {
        window.__keyLog.push('compositionstart');
      }, true);
      window.colaimdation.editor().root.addEventListener('compositionend', function () {
        window.__keyLog.push('compositionend');
      }, true);
      window.__resetInputs = function () { window.__inputTypes = []; window.__keyLog = []; };
      window.__caretInBlock = function (index, local) {
        // Through the editor's own caret placement: it activates the block, so the
        // caret ends up in the source view the writer types into. Setting a range
        // by hand can land in a rendered (non-editable) block, where the engine has
        // nothing to edit and a key press does nothing at all.
        var editor = window.colaimdation.editor();
        editor._setCaretOffset(editor.blocks[index].start + local, false);
        editor.blockElements[index].focus();
        return editor.caretOffset();
      };
      // What the checks care about: the document the model holds, the caret, and
      // whether the DOM still has exactly the blocks the editor tracks.
      window.__enterState = function (label) {
        var editor = window.colaimdation.editor();
        var element = editor.activeIndex >= 0 ? editor.blockElements[editor.activeIndex] : null;
        var input = element && element.querySelector ? element.querySelector('.md-code-input') : null;
        var lang = element && element.querySelector ? element.querySelector('.md-code-lang') : null;
        return {
          label: label,
          text: editor.text,
          caret: editor.caretOffset(),
          blocks: editor.blocks.map(function (b) { return b.kind + '[' + b.start + ',' + b.end + ')'; }),
          tracked: editor.blockElements.length,
          children: editor.root.children.length,
          inputs: window.__inputTypes.slice(),
          keys: window.__keyLog.slice(),
          composing: editor._composing,
          box: !!element && element.classList.contains('md-code-box'),
          boxValue: input ? input.value : null,
          boxLang: lang ? lang.textContent : null,
          rendered: editor.blocks.map(function (b) { return window.ColaiRender.blockHtml(b); }),
          activeElement: document.activeElement
            ? document.activeElement.nodeName + '.' + String(document.activeElement.className) : null,
          selection: (function () {
            var sel = window.getSelection();
            if (!sel || !sel.anchorNode) return null;
            var path = [];
            var cur = sel.anchorNode;
            while (cur && cur !== editor.root) {
              path.push(cur.nodeName + (cur.className ? '.' + String(cur.className).split(' ').join('.') : ''));
              cur = cur.parentNode;
            }
            return path.join(' < ') + ' @' + sel.anchorOffset + ' rangeCount=' + sel.rangeCount;
          })(),
        };
      };
    `);

    // Multi-line typing through a hand-built DOM insertion (the path a keystroke
    // takes when the engine does the edit itself): every character has to survive,
    // in order. Fenced code is not typed this way any more - it has a text box of
    // its own (see the code-box checks below) - so this exercises paragraphs.
    const workflow = await session.eval(`
      (function () {
        var editor = window.colaimdation.editor();
        editor.setDocument('', { revision: 'w1' });
        var steps = [];
        ['o','n','e','\\n','t','w','o','\\n','t','h','r','e','e'].forEach(function (ch) {
          window.__key(editor, ch);
          steps.push(editor.text);
        });
        return { steps: steps, text: editor.text,
          blocks: editor.blocks.map(function (b) { return b.kind + '[' + b.start + ',' + b.end + ')'; }) };
      })()
    `);
    for (const step of workflow.steps) console.log('    -> ' + JSON.stringify(step));
    console.log('  real browser: multi-line workflow -> ' + JSON.stringify(workflow.text) +
      '  blocks=' + workflow.blocks.join(','));
    check('the multi-line workflow kept every character',
      workflow.text === 'one\ntwo\nthree', workflow.text);

    const enterTwice = await session.eval(`
      (function () {
        var editor = window.colaimdation.editor();
        editor.setDocument('line one\\n', { revision: 'w2' });
        editor._setCaretOffset(editor.text.length, false);
        var before = editor.text;
        window.__enterAt(editor);
        return { before: before, afterEnter: editor.text,
          blocks: editor.blocks.map(function (b) { return b.kind + '[' + b.start + ',' + b.end + ')'; }) };
      })()
    `);
    console.log('  real browser: "' + JSON.stringify(enterTwice.before) + '" + Enter -> ' +
      JSON.stringify(enterTwice.afterEnter) + '  blocks=' + enterTwice.blocks.join(','));
    check('Enter adds exactly one line break',
      enterTwice.afterEnter === 'line one\n\n', JSON.stringify(enterTwice.afterEnter));

    // The multi-line workflow driven with Chrome's own editing commands rather
    // than with key events: execCommand('insertText') for a character and
    // execCommand('insertLineBreak') for a line break. This is NOT what a real
    // Enter does - execCommand fires no beforeinput and inserts a plain '\n' in the
    // text where the key produces a block split - so it checks the other side of
    // the editor: the line break has to be read back out of the DOM correctly.
    // tools/web_browser.js's real-key checks above cover the key itself.
    await session.eval(`
      window.__focusBlock = function (editor) {
        var element = editor.activeIndex >= 0 ? editor.blockElements[editor.activeIndex] : null;
        if (!element) element = editor.root.querySelector('.md-empty') || editor.root;
        element.focus();
        return element;
      };
      window.__realType = function (editor, text) {
        window.__focusBlock(editor);
        document.execCommand('insertText', false, text);
        return editor.text;
      };
      window.__realEnter = function (editor) {
        window.__focusBlock(editor);
        document.execCommand('insertLineBreak');
        return editor.text;
      };
    `);

    const typedDoc = await session.eval(`
      (function () {
        var editor = window.colaimdation.editor();
        editor.setDocument('', { revision: 'd1' });
        var steps = [];
        function line(text) {
          window.__realType(editor, text);
          window.__realEnter(editor);
          steps.push(editor.text);
        }
        line('$dcamda$');
        line('#daewamda');
        line('\`\`\`cpp');
        line('int main(){');
        line('return 0;');
        line('}');
        line('\`\`\`');
        return { text: editor.text, steps: steps,
          blocks: editor.blocks.map(function (b) { return b.kind; }) };
      })()
    `);
    for (const step of typedDoc.steps) console.log('    -> ' + JSON.stringify(step));
    console.log('  real browser: typed document blocks -> ' + typedDoc.blocks.join(', '));
    check('typing line by line produces several blocks', typedDoc.blocks.length > 1,
      typedDoc.blocks.join(','));
    check('the heading line became a heading',
      typedDoc.blocks.indexOf('heading') >= 0, typedDoc.blocks.join(','));
    check('the fence became a code block',
      typedDoc.blocks.indexOf('code_block') >= 0, typedDoc.blocks.join(','));
    check('nothing was lost', typedDoc.text.indexOf('int main') >= 0, typedDoc.text);

    // The same text arriving as a paste, already written the way the editor
    // corrects typing to, must produce exactly the same structure. (A paste of
    // the *uncorrected* "#daewamda" stays a paragraph, because pasted text is
    // taken as it is - only typing gets the heading fix.)
    const pastedDoc = await session.eval(`
      (function () {
        var editor = window.colaimdation.editor();
        var text = ['$dcamda$', '# daewamda', '\`\`\`cpp', 'int main(){', 'return 0;', '}', '\`\`\`', ''].join('\\n');
        editor.setDocument(text, { revision: 'd2' });
        return { text: editor.text, blocks: editor.blocks.map(function (b) { return b.kind; }) };
      })()
    `);
    console.log('  real browser: same document pasted -> blocks: ' + pastedDoc.blocks.join(', '));
    check('typed and pasted documents agree', pastedDoc.blocks.join(',') === typedDoc.blocks.join(','),
      'pasted=' + pastedDoc.blocks.join(',') + ' typed=' + typedDoc.blocks.join(','));
    check('the heading survives both ways',
      typedDoc.text.indexOf('# daewamda') >= 0 && pastedDoc.text.indexOf('# daewamda') >= 0,
      'typed=' + JSON.stringify(typedDoc.text) + ' pasted=' + JSON.stringify(pastedDoc.text));

    // Authoritative check of the function that turns DOM line breaks back into
    // '\n'. Each case is the exact DOM the browser produces.
    const extraction = await session.eval(`
      (function () {
        var editor = window.colaimdation.editor();
        var cases = {
          'plain text': 'AA',
          'enter (Chrome div)': 'AA<div>BB</div>',
          'enter twice': 'AA<div><br></div><div>BB</div>',
          'shift+enter br': 'AA<br>BB',
          'nested div': 'AA<div>BB<div>CC</div></div>'
        };
        var out = {};
        for (var name in cases) {
          var el = document.createElement('div');
          el.innerHTML = cases[name];
          out[name] = { html: cases[name], raw: editor._rawTextOfElement(el) };
        }
        return out;
      })()
    `);
    const expectations = {
      'plain text': 'AA',
      'enter (Chrome div)': 'AA\nBB',
      'enter twice': 'AA\n\nBB',
      'shift+enter br': 'AA\nBB',
      'nested div': 'AA\nBB\nCC',
    };
    for (const name of Object.keys(extraction)) {
      const got = extraction[name].raw;
      const want = expectations[name];
      check('raw text from ' + name + ' -> ' + JSON.stringify(got), got === want,
        'want ' + JSON.stringify(want) + ' for ' + JSON.stringify(extraction[name].html));
    }

    // A real Enter, driven by the engine's own key handling rather than by a
    // hand-built DOM. This is the check that was missing: the engine answers Enter
    // by splitting the block element into sibling copies, and the editor used to
    // go on reading the original one - so the line break never reached the model
    // and every character typed on the new line was silently dropped, while it
    // stayed visible on screen until the next re-render threw it away.
    await session.eval(`window.colaimdation.editor().setDocument('para one\\n\\npara two\\n', { revision: 'k1' })`);
    await session.eval('window.__caretInBlock(0, 8)');
    await session.eval('window.__resetInputs()');
    await realEnter();
    const enterAtEnd = await session.eval("window.__enterState('real Enter at the end of a paragraph')");
    console.log('  real browser: real Enter -> ' + JSON.stringify(enterAtEnd.text) +
      '  blocks=' + enterAtEnd.blocks.join(',') + '  inputs=' + enterAtEnd.inputs.join(',') +
      '  keys=' + enterAtEnd.keys.join(','));
    check('a real Enter adds exactly one newline', enterAtEnd.text === 'para one\n\n\npara two\n',
      JSON.stringify(enterAtEnd.text));
    check('the engine did not perform the edit itself', enterAtEnd.inputs.length === 0,
      enterAtEnd.inputs.join(','));
    check('the editor intercepted the engine\'s paragraph insertion',
      enterAtEnd.keys.indexOf('beforeinput:insertParagraph') >= 0, enterAtEnd.keys.join(','));
    check('the block was not split into a copy the editor cannot see',
      enterAtEnd.children === enterAtEnd.tracked,
      'children=' + enterAtEnd.children + ' tracked=' + enterAtEnd.tracked);

    await realChar('X');
    const typedAfterEnter = await session.eval("window.__enterState('typing on the new line')");
    check('the character typed after Enter goes on the new line',
      typedAfterEnter.text === 'para one\nX\n\npara two\n', JSON.stringify(typedAfterEnter.text));
    await realChar('Y');
    const typedSecond = await session.eval("window.__enterState('second character on the new line')");
    check('the second character stays there and the blank line survives',
      typedSecond.text === 'para one\nXY\n\npara two\n', JSON.stringify(typedSecond.text));

    // Enter in the middle of a paragraph must keep both halves (the second one
    // used to end up in the engine's copy and was lost).
    await session.eval(`window.colaimdation.editor().setDocument('alpha beta\\n\\nsecond\\n', { revision: 'k2' })`);
    await session.eval('window.__caretInBlock(0, 5)');
    await realEnter();
    const midEnter = await session.eval("window.__enterState('real Enter inside a paragraph')");
    console.log('  real browser: mid-paragraph Enter -> ' + JSON.stringify(midEnter.text));
    check('a real Enter inside a paragraph keeps the text after the caret',
      midEnter.text === 'alpha\n beta\n\nsecond\n', JSON.stringify(midEnter.text));
    await realChar('Z');
    const midTyped = await session.eval("window.__enterState('typing on the new line')");
    check('the character typed there lands on the new line',
      midTyped.text === 'alpha\nZ beta\n\nsecond\n', JSON.stringify(midTyped.text));

    // Enter at the very end of the document must also leave a line that types.
    await session.eval(`window.colaimdation.editor().setDocument('tail\\n', { revision: 'k3' })`);
    await session.eval('window.__caretInBlock(0, 4)');
    await realEnter();
    await realChar('E');
    const endOfDocument = await session.eval("window.__enterState('typing after Enter at the end')");
    console.log('  real browser: end of document -> ' + JSON.stringify(endOfDocument.text));
    check('the character typed after Enter at the end of the document lands on the new line',
      endOfDocument.text === 'tail\nE\n', JSON.stringify(endOfDocument.text));

    // The safety net for a block the engine split by some other route (a drag &
    // drop, a synthesized input event): the copy carries the same data-md-block,
    // so the editor must fold the pieces back into the model instead of reading
    // one of them and dropping the rest.
    const folded = await session.eval(`
      (function () {
        var editor = window.colaimdation.editor();
        editor.setDocument('alpha beta\\n\\nsecond\\n', { revision: 'k4' });
        editor._setCaretOffset(5, false);
        var element = editor.blockElements[0];
        // Exactly the shape a split leaves: the text after the caret in a copy of
        // the element, carrying the same data-md-block.
        var copy = element.cloneNode(true);
        copy.textContent = ' beta';
        copy.appendChild(editor._createTerminatorLine());
        element.textContent = 'alpha';
        element.appendChild(editor._createTerminatorLine());
        element.parentNode.insertBefore(copy, element.nextSibling);
        var range = document.createRange();
        range.setStart(copy.firstChild, 5);
        range.collapse(true);
        var selection = window.getSelection();
        selection.removeAllRanges();
        selection.addRange(range);
        element.dispatchEvent(new Event('input', { bubbles: true }));
        return { text: editor.text,
                 blocks: editor.blocks.map(function (b) { return b.kind + '[' + b.start + ',' + b.end + ')'; }),
                 tracked: editor.blockElements.length, children: editor.root.children.length };
      })()
    `);
    console.log('  real browser: split block folded -> ' + JSON.stringify(folded.text));
    check('a block the engine split is folded back into the model',
      folded.text === 'alpha\n beta\n\nsecond\n', JSON.stringify(folded.text));
    check('the copies are gone afterwards', folded.children === folded.tracked,
      'children=' + folded.children + ' tracked=' + folded.tracked);

    // A fenced code block is edited as a code box: "```cpp" then Enter gives a code
    // text box with the fence already closed, and the writer types code - never
    // fences - from then on.
    await session.eval(`window.colaimdation.editor().setDocument('', { revision: 'cb1' })`);
    await session.eval("window.colaimdation.editor().root.querySelector('.md-empty').focus()");
    for (const ch of '```cpp') await realChar(ch);
    const fenceTyped = await session.eval("window.__enterState('fence typed')");
    console.log('  real browser: "```cpp" -> ' + JSON.stringify(fenceTyped.text) +
      '  box=' + fenceTyped.box);
    check('the fence line stays source text while the language is typed',
      fenceTyped.text === '```cpp' && !fenceTyped.box, JSON.stringify(fenceTyped.text));

    await realEnter();
    const boxOpened = await session.eval("window.__enterState('code box')");
    console.log('  real browser: fence + Enter -> ' + JSON.stringify(boxOpened.text) +
      '  box=' + boxOpened.box + ' lang=' + boxOpened.boxLang +
      ' focus=' + boxOpened.activeElement);
    check('Enter closes the fence and opens a code box',
      boxOpened.text === '```cpp\n```\n' && boxOpened.box, JSON.stringify(boxOpened.text));
    check('the box is a text box with the language in its header',
      boxOpened.boxValue === '' && boxOpened.boxLang === 'cpp',
      'value=' + JSON.stringify(boxOpened.boxValue) + ' lang=' + boxOpened.boxLang);
    check('the caret is in the text box',
      boxOpened.activeElement === 'TEXTAREA.md-code-input', boxOpened.activeElement);

    for (const ch of 'int main(){') await realChar(ch);
    // The code box handles Enter itself (it keeps the line's indentation), and that
    // handler prevents the browser's own line break - so only the keydown is sent.
    await realKeyOnly('Enter', 'Enter', 13);
    for (const ch of 'return 0;') await realChar(ch);
    const codeTyped = await session.eval("window.__enterState('code typed')");
    console.log('  real browser: code typed -> ' + JSON.stringify(codeTyped.text));
    check('the code is written into the fenced block, in order',
      codeTyped.text === '```cpp\nint main(){\n    return 0;\n```\n', JSON.stringify(codeTyped.text));
    check('Enter inside the code keeps the indentation the line opened',
      codeTyped.boxValue === 'int main(){\n    return 0;',
      'value=' + JSON.stringify(codeTyped.boxValue));
    check('the text box keeps the code and the focus',
      codeTyped.boxValue === 'int main(){\n    return 0;' &&
      codeTyped.activeElement === 'TEXTAREA.md-code-input',
      'value=' + JSON.stringify(codeTyped.boxValue) + ' focus=' + codeTyped.activeElement);
    check('the caret is reported inside the code',
      codeTyped.caret === codeTyped.text.indexOf('return 0;') + 'return 0;'.length,
      'caret=' + codeTyped.caret);

    // Leaving the box: the down arrow on the last code line steps onto the line
    // below it, and writing there starts a paragraph after the block.
    await session.send('Input.dispatchKeyEvent', {
      type: 'keyDown', key: 'ArrowDown', code: 'ArrowDown',
      windowsVirtualKeyCode: 40, nativeVirtualKeyCode: 40,
    });
    await session.send('Input.dispatchKeyEvent', {
      type: 'keyUp', key: 'ArrowDown', code: 'ArrowDown',
      windowsVirtualKeyCode: 40, nativeVirtualKeyCode: 40,
    });
    await new Promise((r) => setTimeout(r, 150));
    const belowBox = await session.eval("window.__enterState('below the box')");
    check('the down arrow on the last code line steps out of the box',
      belowBox.activeElement === 'DIV.md-code-after' && belowBox.caret === belowBox.text.length,
      'focus=' + belowBox.activeElement + ' caret=' + belowBox.caret);
    for (const ch of 'done') await realChar(ch);
    const afterBox = await session.eval("window.__enterState('paragraph after the box')");
    console.log('  real browser: after the box -> ' + JSON.stringify(afterBox.text));
    check('writing below the box starts a paragraph after it',
      afterBox.text === '```cpp\nint main(){\n    return 0;\n```\ndone', JSON.stringify(afterBox.text));
    check('the code block renders as code, and never shows its fence',
      afterBox.rendered[0].indexOf('\`\`\`') < 0 && afterBox.rendered[0].indexOf('main') >= 0 &&
      afterBox.rendered[0].indexOf('return') >= 0 && afterBox.rendered[1].indexOf('done') >= 0,
      afterBox.rendered.join(' | '));

    // Auto-indentation and live colour while the code is edited: Enter opens a level
    // deeper after a line that ends in ":", Tab indents instead of moving the focus
    // out of the box, and the code is coloured on a layer that covers the text box
    // exactly (same box, same font) while the text box draws its text transparent.
    await session.eval(
      "window.colaimdation.editor().setDocument('```python\\ndef go(n):\\nreturn n\\n```\\n', { revision: 'cb3' })");
    const pythonBox = await session.eval(`
      (function () {
        var editor = window.colaimdation.editor();
        var block = editor.blocks[0];
        editor._setCaretOffset(block.contentStart + 3, false);
        var input = document.querySelector('.md-code-input');
        if (!input) return { box: false };
        input.focus();
        input.setSelectionRange('def go(n):'.length, 'def go(n):'.length);
        return { box: true, value: input.value, caret: input.selectionStart };
      })()
    `);
    check('a python block opens as a code box', pythonBox.box, JSON.stringify(pythonBox));
    await realKeyOnly('Enter', 'Enter', 13);
    const pythonIndented = await session.eval(`
      (function () {
        var input = document.querySelector('.md-code-input');
        return { value: input.value, text: window.colaimdation.editor().text,
                 caret: input.selectionStart };
      })()
    `);
    console.log('  real browser: python Enter -> ' + JSON.stringify(pythonIndented.value));
    check('Enter after a line ending in ":" indents the next line',
      pythonIndented.value === 'def go(n):\n    \nreturn n', JSON.stringify(pythonIndented.value));
    check('the indentation reaches the document, and the caret follows it',
      pythonIndented.text === '```python\ndef go(n):\n    \nreturn n\n```\n' &&
      pythonIndented.caret === 'def go(n):\n    '.length,
      JSON.stringify(pythonIndented.text) + ' caret=' + pythonIndented.caret);

    // Tab goes through the text box's own insert path (execCommand insertText), so
    // this also checks that the native undo history survives a Tab.
    await realKeyOnly('Tab', 'Tab', 9);
    const pythonTabbed = await session.eval(`
      (function () {
        var input = document.querySelector('.md-code-input');
        return { value: input.value, caret: input.selectionStart };
      })()
    `);
    check('Tab indents inside the code box instead of leaving it',
      pythonTabbed.value === 'def go(n):\n        \nreturn n' &&
      pythonTabbed.caret === 'def go(n):\n        '.length,
      JSON.stringify(pythonTabbed.value) + ' caret=' + pythonTabbed.caret);
    await realKeyOnly('Tab', 'Tab', 9, 8);
    const pythonOutdented = await session.eval(
      "(function () { return document.querySelector('.md-code-input').value; })()");
    check('Shift+Tab takes the level back',
      pythonOutdented === 'def go(n):\n    \nreturn n', JSON.stringify(pythonOutdented));

    const colours = await session.eval(`
      (function () {
        var input = document.querySelector('.md-code-input');
        var shade = document.querySelector('.md-code-highlight');
        var field = document.querySelector('.md-code-field');
        var inputStyle = getComputedStyle(input);
        var shadeStyle = getComputedStyle(shade);
        var inputRect = input.getBoundingClientRect();
        var shadeRect = shade.getBoundingClientRect();
        function round(rect) {
          return [rect.left, rect.top, rect.width, rect.height].map(Math.round).join(',');
        }
        return {
          sameField: !!field && shade.parentNode === field && input.parentNode === field,
          tokens: shade.querySelectorAll('[class^="hljs-"]').length,
          keywords: shade.querySelectorAll('.hljs-keyword').length,
          shadeText: shade.textContent,
          value: input.value,
          metrics: ['fontFamily', 'fontSize', 'lineHeight', 'paddingLeft', 'paddingTop',
                    'paddingRight', 'whiteSpace', 'tabSize']
            .map(function (name) { return name + ':' + inputStyle[name] + '/' + shadeStyle[name]; }),
          inputRect: round(inputRect),
          shadeRect: round(shadeRect),
          textColour: inputStyle.color,
          caretColour: inputStyle.caretColor,
          selectionColour: getComputedStyle(input, '::selection').backgroundColor,
          hiddenFromReader: shade.getAttribute('aria-hidden'),
        };
      })()
    `);
    console.log('  real browser: code colours -> ' + JSON.stringify(colours).slice(0, 320));
    const mismatched = colours.metrics.filter((pair) => {
      const parts = pair.split('/');
      return parts[0].slice(parts[0].indexOf(':') + 1) !== parts[1].slice(parts[1].indexOf(':') + 1);
    });
    check('the colour layer sits in the same field as the text box',
      colours.sameField, String(colours.sameField));
    check('the layer holds exactly the code in the text box',
      colours.shadeText === colours.value,
      JSON.stringify(colours.shadeText) + ' vs ' + JSON.stringify(colours.value));
    check('the code is coloured as code, not as plain text',
      colours.tokens > 0 && colours.keywords > 0,
      'tokens=' + colours.tokens + ' keywords=' + colours.keywords);
    check('the layer is laid out exactly like the text box',
      mismatched.length === 0 && colours.inputRect === colours.shadeRect,
      'metrics=' + JSON.stringify(mismatched.length ? mismatched : colours.metrics) +
        ' rects=' + colours.inputRect + ' / ' + colours.shadeRect);
    check('the text box draws its text transparent but keeps a visible caret',
      colours.textColour === 'rgba(0, 0, 0, 0)' &&
      colours.caretColour !== 'rgba(0, 0, 0, 0)',
      'text=' + colours.textColour + ' caret=' + colours.caretColour);
    check('selecting code keeps the colours readable (a translucent highlight)',
      /^rgba\(/.test(colours.selectionColour) &&
      !/,\s*0\)$/.test(colours.selectionColour),
      colours.selectionColour);
    check('the layer is hidden from assistive technology',
      colours.hiddenFromReader === 'true', String(colours.hiddenFromReader));

    // A fence typed in the middle of a document must not swallow what is below it.
    await session.eval(`window.colaimdation.editor().setDocument('before\\n\\nafter\\n', { revision: 'cb2' })`);
    await session.eval('window.__caretInBlock(0, 6)');
    await realEnter();
    for (const ch of '```js') await realChar(ch);
    const midFence = await session.eval("window.__enterState('mid-document fence')");
    check('a fence typed mid-document keeps the info string editable',
      midFence.text === 'before\n```js\n\nafter\n' && !midFence.box, JSON.stringify(midFence.text));
    await realEnter();
    const midBox = await session.eval("window.__enterState('mid-document box')");
    console.log('  real browser: mid-document box -> ' + JSON.stringify(midBox.text));
    check('Enter closes that fence and keeps the rest of the document',
      midBox.box && midBox.boxLang === 'js' && midBox.text === 'before\n```js\n```\n\n\nafter\n',
      JSON.stringify(midBox.text));

    // The header is the way back to the Markdown (to change the language).
    const header = await session.eval(`
      (function () {
        var editor = window.colaimdation.editor();
        var element = editor.blockElements[editor.activeIndex];
        var head = element.querySelector('.md-code-head');
        head.dispatchEvent(new MouseEvent('mousedown', { bubbles: true, cancelable: true }));
        var current = editor.blockElements[editor.activeIndex];
        return { source: current.classList.contains('md-source'),
                 kind: editor.blocks[editor.activeIndex].kind,
                 html: current.outerHTML.slice(0, 120) };
      })()
    `);
    console.log('  real browser: header click -> ' + JSON.stringify(header));
    check('clicking the code box header shows that block as Markdown',
      header.source && header.kind === 'code_block', JSON.stringify(header));

    // That Markdown is editable text like any other block, so a press inside it has to
    // place the caret where it was clicked. It used to be swallowed: the caret jumped
    // to the start of the code and the block turned back into a code box, which is why
    // the source of a code block could not be clicked into at all.
    const sourcePoint = await session.eval(`
      (function () {
        var editor = window.colaimdation.editor();
        var element = editor.blockElements[editor.activeIndex];
        var block = editor.blocks[editor.activeIndex];
        var rect = element.getBoundingClientRect();
        var style = getComputedStyle(element);
        var point = { x: Math.round(rect.left + 90),
                      y: Math.round(rect.top + (parseFloat(style.lineHeight) || 24) * 1.6) };
        // More precise when the source really is one text node: aim at the third
        // character of the second line (the code line, past the fence).
        var node = element.firstChild;
        var local = 5 + 1 + 2;
        if (node && node.nodeType === 3 && node.nodeValue.length > local) {
          var range = document.createRange();
          range.setStart(node, local);
          range.setEnd(node, local + 1);
          var box = range.getBoundingClientRect();
          point = { x: Math.round(box.left + 2), y: Math.round(box.top + box.height / 2) };
        }
        return { x: point.x, y: point.y, contentStart: block.contentStart,
                 caret: editor.caretOffset(),
                 classes: element.className };
      })()
    `);
    await session.send('Input.dispatchMouseEvent', {
      type: 'mousePressed', button: 'left', clickCount: 1, x: sourcePoint.x, y: sourcePoint.y,
    });
    await session.send('Input.dispatchMouseEvent', {
      type: 'mouseReleased', button: 'left', clickCount: 1, x: sourcePoint.x, y: sourcePoint.y,
    });
    await new Promise((r) => setTimeout(r, 400));
    const sourceClicked = await session.eval(`
      (function () {
        var editor = window.colaimdation.editor();
        var element = editor.blockElements[editor.activeIndex];
        return { caret: editor.caretOffset(), classes: element.className,
                 box: element.classList.contains('md-code-box'),
                 source: element.classList.contains('md-source') };
      })()
    `);
    console.log('  real browser: press in the code block source -> ' + JSON.stringify(sourceClicked));
    check('the block stays in the source view when its text is pressed',
      sourceClicked.source && !sourceClicked.box, sourceClicked.classes);
    check('the caret goes where the press landed, not to the start of the code',
      sourceClicked.caret > sourcePoint.contentStart, 'caret=' + sourceClicked.caret +
        ' contentStart=' + sourcePoint.contentStart + ' was=' + sourcePoint.caret);

    // Dragging there selects its Markdown (the same guard: the press belongs to the
    // browser), which is what makes the source of a code block copyable. The document
    // is set up here so the selection has a known text to cover.
    const sourceDrag = await session.eval(`
      (function () {
        var editor = window.colaimdation.editor();
        editor.setDocument('text\\n\\n\`\`\`js\\nlet x = 1;\\n\`\`\`\\n', { revision: 'src4' });
        editor._setCaretOffset(12, false);
        var element = editor.blockElements[editor.activeIndex];
        var head = element.querySelector('.md-code-head');
        head.dispatchEvent(new MouseEvent('mousedown', { bubbles: true, cancelable: true }));
        element = editor.blockElements[editor.activeIndex];
        var node = element.firstChild;
        if (!node || node.nodeType !== 3 || node.nodeValue.length < 8) return null;
        function point(local) {
          var range = document.createRange();
          range.setStart(node, local);
          range.setEnd(node, Math.min(local + 1, node.nodeValue.length));
          var box = range.getBoundingClientRect();
          return { x: Math.round(box.left + 1), y: Math.round(box.top + box.height / 2) };
        }
        // From the first code character to the last one, so the selection has to
        // contain the code itself.
        return { from: point(6), to: point(node.nodeValue.length - 4),
                 source: element.classList.contains('md-source') };
      })()
    `);
    if (!sourceDrag) {
      check('dragging in the source of a code block selects its Markdown', false,
        'the source is not a single text node');
    } else {
      await session.send('Input.dispatchMouseEvent', {
        type: 'mousePressed', button: 'left', clickCount: 1, x: sourceDrag.from.x, y: sourceDrag.from.y,
      });
      for (let step = 1; step <= 6; step++) {
        await session.send('Input.dispatchMouseEvent', {
          type: 'mouseMoved', button: 'left', buttons: 1,
          x: Math.round(sourceDrag.from.x + (sourceDrag.to.x - sourceDrag.from.x) * step / 6),
          y: Math.round(sourceDrag.from.y + (sourceDrag.to.y - sourceDrag.from.y) * step / 6),
        });
        await new Promise((r) => setTimeout(r, 25));
      }
      await session.send('Input.dispatchMouseEvent', {
        type: 'mouseReleased', button: 'left', clickCount: 1, x: sourceDrag.to.x, y: sourceDrag.to.y,
      });
      await new Promise((r) => setTimeout(r, 500));
      const draggedSource = await session.eval(`
        (function () {
          var editor = window.colaimdation.editor();
          var element = editor.blockElements[editor.activeIndex];
          return { text: String(window.getSelection()), classes: element.className,
                   box: element.classList.contains('md-code-box') };
        })()
      `);
      console.log('  real browser: drag in a code block source -> ' +
        JSON.stringify(draggedSource.text));
      check('dragging in the source of a code block selects its Markdown',
        sourceDrag.source && !draggedSource.box && draggedSource.text.indexOf('let x = 1;') >= 0,
        JSON.stringify(draggedSource));
    }

    // The plain-text (source) view is the same document: an edit there has to reach
    // the model - and therefore the rendered view - and a document from elsewhere has
    // to replace the text that is open in it.
    await session.eval("window.colaimdation.editor().setDocument('one\\n', { revision: 'src1' })");
    const sourceEntered = await session.eval(`
      (function () {
        document.getElementById('btn-source').click();
        var editor = window.colaimdation.editor();
        var area = document.getElementById('source-view');
        return { mode: editor.sourceMode, value: area.value, hidden: area.hidden,
                 rootHidden: editor.root.hidden };
      })()
    `);
    check('the Source button shows the plain-text view of the document',
      sourceEntered.mode && !sourceEntered.hidden && sourceEntered.rootHidden &&
      sourceEntered.value === 'one\n', JSON.stringify(sourceEntered));

    await session.eval(`
      (function () {
        var area = document.getElementById('source-view');
        area.value = 'one\\n\\n## two\\n';
        area.dispatchEvent(new Event('input', { bubbles: true }));
      })()
    `);
    const sourceTyped = await session.eval(`
      (function () {
        var editor = window.colaimdation.editor();
        return { text: editor.text,
                 kinds: editor.blocks.map(function (b) { return b.kind; }) };
      })()
    `);
    console.log('  real browser: plain-text edit -> ' + JSON.stringify(sourceTyped));
    check('editing the plain text updates the document and its blocks',
      sourceTyped.text === 'one\n\n## two\n' && sourceTyped.kinds.join(',') === 'paragraph,heading',
      JSON.stringify(sourceTyped));

    const sourceBack = await session.eval(`
      (function () {
        document.getElementById('btn-source').click();
        var editor = window.colaimdation.editor();
        // Leaving the plain-text view restores the caret, and the block the caret is
        // in shows its Markdown: put it in the paragraph so the heading is rendered.
        editor._setCaretOffset(0, false);
        return { mode: editor.sourceMode, text: editor.text,
                 rendered: editor.blocks.map(function (b) { return window.ColaiRender.blockHtml(b); }),
                 dom: Array.prototype.map.call(editor.root.children, function (element) {
                   return element.innerHTML; }).join('') };
      })()
    `);
    console.log('  real browser: back in the rendered view -> ' + JSON.stringify(sourceBack.text));
    check('switching back renders what the plain text holds',
      !sourceBack.mode && sourceBack.text === 'one\n\n## two\n' &&
      sourceBack.rendered[1].indexOf('<h2') === 0 && sourceBack.dom.indexOf('<h2') >= 0,
      JSON.stringify(sourceBack).slice(0, 240));

    // A document from the host (a tab switch, an agent edit, a reload) replaces the
    // text that is open there; otherwise the next keystroke writes the old document
    // back into the new one.
    await session.eval("document.getElementById('btn-source').click()");
    await session.eval("window.colaimdation.editor().setDocument('third\\n', { revision: 'src2' })");
    const sourceReplaced = await session.eval(
      "(function () { return document.getElementById('source-view').value; })()");
    check('a document from the host replaces the open plain text',
      sourceReplaced === 'third\n', JSON.stringify(sourceReplaced));
    await session.eval("document.getElementById('btn-source').click()");
    const afterSource = await session.eval(`(function () {
      var editor = window.colaimdation.editor();
      return { mode: editor.sourceMode, text: editor.text };
    })()`);
    check('leaving the plain-text view keeps the document the host sent',
      !afterSource.mode && afterSource.text === 'third\n', JSON.stringify(afterSource));

    // The empty line the writer moves on to after the code has to be *outside* the
    // box's frame: while it sat inside the border and background, starting the next
    // paragraph looked like the caret was still in the code box.
    const lineGeometry = await session.eval(`
      (function () {
        var editor = window.colaimdation.editor();
        editor.setDocument('text\\n\\n\`\`\`js\\nlet x = 1;\\n\`\`\`\\n', { revision: 'cb4' });
        editor._setCaretOffset(12, false);
        var element = editor.blockElements[editor.activeIndex];
        var frame = element.querySelector('.md-code-frame');
        var line = element.querySelector('.md-code-after');
        if (!frame || !line) return null;
        var frameRect = frame.getBoundingClientRect();
        var lineRect = line.getBoundingClientRect();
        return {
          insideFrame: frame.contains(line),
          frameHoldsBox: !!frame.querySelector('.md-code-input') && !!frame.querySelector('.md-code-head'),
          frameBottom: Math.round(frameRect.bottom),
          frameWidth: Math.round(frameRect.width),
          lineTop: Math.round(lineRect.top),
          lineLeft: Math.round(lineRect.left),
          blockLeft: Math.round(element.getBoundingClientRect().left),
        };
      })()
    `);
    console.log('  real browser: line after the code box -> ' + JSON.stringify(lineGeometry));
    check('the box frame holds the header and the code text box',
      !!lineGeometry && lineGeometry.frameHoldsBox,
      JSON.stringify(lineGeometry));
    check('the line after a code box is outside the box frame',
      !!lineGeometry && !lineGeometry.insideFrame &&
      lineGeometry.lineTop >= lineGeometry.frameBottom - 1 &&
      lineGeometry.lineLeft === lineGeometry.blockLeft,
      JSON.stringify(lineGeometry));

    // Backspace at the boundary between the box and the line after it:
    //  - on a box that holds no code, inside or below, it removes the box (fences and
    //    all) rather than leaving the writer stuck with an empty box;
    //  - on the line after a box that *does* hold code it must not hand the caret back
    //    to the code: Chrome's own "join with the line above" put the caret after the
    //    last code character, and the next word typed became code.
    const openBoxAt = (offset) => `
      (function () {
        var editor = window.colaimdation.editor();
        editor._setCaretOffset(${offset}, false);
        var input = document.querySelector('.md-code-input');
        if (input) {
          input.focus();
          input.setSelectionRange(input.value.length, input.value.length);
        }
        return { box: !!input, value: input ? input.value : null, text: editor.text };
      })()
    `;

    await session.eval("window.colaimdation.editor().setDocument('text\\n\\n```js\\nlet x = 1;\\n```\\n\\nmore\\n', { revision: 'bs1' })");
    const codeBoxOpen = await session.eval(openBoxAt(15));
    check('a box with code is open for the Backspace checks',
      codeBoxOpen.box && codeBoxOpen.value === 'let x = 1;', JSON.stringify(codeBoxOpen));

    await realKeyOnly('ArrowDown', 'ArrowDown', 40);
    const steppedOut = await session.eval("window.__enterState('stepped out of the box')");
    check('the caret steps onto the line after the box',
      steppedOut.activeElement === 'DIV.md-code-after' && steppedOut.box,
      steppedOut.activeElement);
    await realKeyOnly('Backspace', 'Backspace', 8);
    const afterBoundary = await session.eval("window.__enterState('backspace after the box')");
    console.log('  real browser: Backspace after a code box -> focus=' +
      afterBoundary.activeElement + ' value=' + JSON.stringify(afterBoundary.boxValue) +
      ' text=' + JSON.stringify(afterBoundary.text));
    check('Backspace on the line after a non-empty box leaves the code alone',
      afterBoundary.text === 'text\n\n```js\nlet x = 1;\n```\n\nmore\n' &&
      afterBoundary.activeElement === 'DIV.md-code-after' &&
      afterBoundary.boxValue === 'let x = 1;',
      JSON.stringify({ text: afterBoundary.text, focus: afterBoundary.activeElement,
                       value: afterBoundary.boxValue }));

    // An empty box: Backspace inside it removes the block.
    await session.eval("window.colaimdation.editor().setDocument('text\\n\\n```js\\n```\\n\\nmore\\n', { revision: 'bs2' })");
    const emptyBoxOpen = await session.eval(openBoxAt(12));
    check('an empty fenced block is an empty code box',
      emptyBoxOpen.box && emptyBoxOpen.value === '', JSON.stringify(emptyBoxOpen));
    await realKeyOnly('Backspace', 'Backspace', 8);
    const boxRemoved = await session.eval("window.__enterState('empty box removed')");
    console.log('  real browser: Backspace in an empty box -> ' +
      JSON.stringify(boxRemoved.text) + ' box=' + boxRemoved.box);
    check('Backspace in an empty code box removes the block',
      boxRemoved.text === 'text\n\nmore\n' && !boxRemoved.box,
      JSON.stringify({ text: boxRemoved.text, box: boxRemoved.box }));

    // An empty box: Backspace on the line after it removes the block too.
    await session.eval("window.colaimdation.editor().setDocument('text\\n\\n```js\\n```\\n\\nmore\\n', { revision: 'bs3' })");
    await session.eval(openBoxAt(12));
    await realKeyOnly('ArrowDown', 'ArrowDown', 40);
    const steppedOutEmpty = await session.eval("window.__enterState('out of the empty box')");
    check('the caret steps onto the line after the empty box',
      steppedOutEmpty.activeElement === 'DIV.md-code-after', steppedOutEmpty.activeElement);
    await realKeyOnly('Backspace', 'Backspace', 8);
    const removedFromBelow = await session.eval("window.__enterState('empty box removed from below')");
    check('Backspace on the line after an empty box removes the block',
      removedFromBelow.text === 'text\n\nmore\n', JSON.stringify(removedFromBelow.text));

    // With code in the box, Backspace still deletes one character and the box stays.
    await session.eval("window.colaimdation.editor().setDocument('text\\n\\n```js\\nlet x = 1;\\n```\\n\\nmore\\n', { revision: 'bs4' })");
    const beforeCharDelete = await session.eval(openBoxAt(15));
    check('the box is open for the character delete', beforeCharDelete.box, JSON.stringify(beforeCharDelete));
    await realKeyOnly('Backspace', 'Backspace', 8);
    const charDeleted = await session.eval("window.__enterState('character deleted')");
    check('Backspace with code in the box deletes one character and keeps the box',
      charDeleted.box && charDeleted.boxValue === 'let x = 1' &&
      charDeleted.text === 'text\n\n```js\nlet x = 1\n```\n\nmore\n',
      JSON.stringify({ value: charDeleted.boxValue, text: charDeleted.text }));

    // DIAGNOSTIC: Backspace at the very start of a block that follows another block.
    const startOfLastBlock = `
      (function () {
        var editor = window.colaimdation.editor();
        var index = editor.blocks.length - 1;
        editor._setCaretOffset(editor.blocks[index].start, false);
        editor.blockElements[index].focus();
        return { index: index, caret: editor.caretOffset(),
                 kinds: editor.blocks.map(function (b) { return b.kind; }) };
      })()
    `;

    // A block that follows a code box: Backspace at its very start has nothing to join
    // with, and the engine's own attempt at joining is what handed the caret back into
    // the code. An empty box in front of it goes with the press.
    await session.eval("window.colaimdation.editor().setDocument('para\\n\\n```js\\n```\\n\\nafter\\n', { revision: 'bs6' })");
    const boundaryStart = await session.eval(startOfLastBlock);
    await realKeyOnly('Backspace', 'Backspace', 8);
    const boundaryRemoved = await session.eval("window.__enterState('backspace at a block after an empty box')");
    console.log('  real browser: Backspace at a block after an empty box -> before=' +
      JSON.stringify(boundaryStart) + ' text=' + JSON.stringify(boundaryRemoved.text) +
      ' focus=' + boundaryRemoved.activeElement);
    check('Backspace at the start of a block after an empty code box removes the box',
      boundaryRemoved.text === 'para\n\nafter\n',
      JSON.stringify({ text: boundaryRemoved.text, focus: boundaryRemoved.activeElement }));

    await session.eval("window.colaimdation.editor().setDocument('para\\n\\n```js\\nlet x = 1;\\n```\\n\\nafter\\n', { revision: 'bs7' })");
    await session.eval(startOfLastBlock);
    await realKeyOnly('Backspace', 'Backspace', 8);
    const boundaryKept = await session.eval("window.__enterState('backspace at a block after a box with code')");
    console.log('  real browser: Backspace there with code in the box -> focus=' +
      boundaryKept.activeElement + ' caret=' + boundaryKept.caret + ' text=' +
      JSON.stringify(boundaryKept.text));
    check('it leaves a box that still holds code alone, and the caret where it was',
      boundaryKept.text === 'para\n\n```js\nlet x = 1;\n```\n\nafter\n' &&
      boundaryKept.activeElement.indexOf('DIV') === 0,
      JSON.stringify({ text: boundaryKept.text, focus: boundaryKept.activeElement,
                       caret: boundaryKept.caret }));

    // Clicking a rendered code block puts the caret in its code box. The block has to
    // be genuinely rendered for that: with the caret inside it, it is showing its
    // Markdown, and a press there belongs to the text (see the source checks above).
    await session.eval(`(function () {
      var editor = window.colaimdation.editor();
      editor.setDocument('text\\n\\n\`\`\`js\\nlet x = 1;\\n\`\`\`\\n', { revision: 'cb3' });
      editor._setCaretOffset(2, false);
    })()`);
    const codeBoxPoint = await session.eval(`(function () {
      var editor = window.colaimdation.editor();
      var rect = editor.blockElements[1].getBoundingClientRect();
      return { x: Math.round(rect.left + 20), y: Math.round(rect.top + 12) };
    })()`);
    await session.send('Input.dispatchMouseEvent', {
      type: 'mousePressed', button: 'left', clickCount: 1, x: codeBoxPoint.x, y: codeBoxPoint.y,
    });
    await session.send('Input.dispatchMouseEvent', {
      type: 'mouseReleased', button: 'left', clickCount: 1, x: codeBoxPoint.x, y: codeBoxPoint.y,
    });
    await new Promise((r) => setTimeout(r, 400));
    const clicked = await session.eval("window.__enterState('clicked the rendered code')");
    console.log('  real browser: click on a rendered code block -> box=' + clicked.box +
      ' value=' + JSON.stringify(clicked.boxValue) + ' focus=' + clicked.activeElement);
    check('clicking a rendered code block opens its code box',
      clicked.box && clicked.boxValue === 'let x = 1;' &&
      clicked.activeElement === 'TEXTAREA.md-code-input',
      JSON.stringify(clicked.boxValue) + ' focus=' + clicked.activeElement);

    // Mouse selection. Dragging has to select text, in the document *and* inside a
    // code box's text box: the selection handler used to activate (and repaint) the
    // block under the caret as soon as a selection settled, which collapsed the
    // selection that was being dragged and moved focus out of the text box.
    await session.eval(`window.colaimdation.editor().setDocument(
      'First paragraph here.\\n\\n\`\`\`js\\nlet x = 1;\\n\`\`\`\\n\\nLast paragraph.\\n',
      { revision: 'sel1', caret: 0 })`);
    await new Promise((r) => setTimeout(r, 250));

    const drag = await session.eval(`(function () {
      var rect = window.colaimdation.editor().blockElements[0].getBoundingClientRect();
      return { x1: Math.round(rect.left + 1), x2: Math.round(rect.left + rect.width - 10),
               y: Math.round(rect.top + rect.height / 2) };
    })()`);
    await session.send('Input.dispatchMouseEvent', {
      type: 'mousePressed', button: 'left', clickCount: 1, x: drag.x1, y: drag.y });
    for (let step = 1; step <= 6; step++) {
      await session.send('Input.dispatchMouseEvent', {
        type: 'mouseMoved', button: 'left', buttons: 1,
        x: Math.round(drag.x1 + (drag.x2 - drag.x1) * step / 6), y: drag.y });
      await new Promise((r) => setTimeout(r, 25));
    }
    await session.send('Input.dispatchMouseEvent', {
      type: 'mouseReleased', button: 'left', clickCount: 1, x: drag.x2, y: drag.y });
    await new Promise((r) => setTimeout(r, 500));
    const dragged = await session.eval('String(window.getSelection())');
    console.log('  real browser: drag across a paragraph -> ' + JSON.stringify(dragged));
    check('dragging across a paragraph selects its text',
      dragged.indexOf('First paragraph here.') >= 0, JSON.stringify(dragged));

    // Real Ctrl+C (the page does not intercept it) and a real clipboard read: this
    // is what "select and copy" has to do for the writer.
    const pressCtrl = async (letter, code) => {
      const vk = letter.toUpperCase().charCodeAt(0);
      await session.send('Input.dispatchKeyEvent', {
        type: 'keyDown', modifiers: 2, key: letter, code: code,
        windowsVirtualKeyCode: vk, nativeVirtualKeyCode: vk });
      await session.send('Input.dispatchKeyEvent', {
        type: 'keyUp', modifiers: 2, key: letter, code: code,
        windowsVirtualKeyCode: vk, nativeVirtualKeyCode: vk });
    };
    let clipboardGranted = true;
    try {
      await session.send('Browser.grantPermissions',
        { permissions: ['clipboardReadWrite', 'clipboardSanitizedWrite'] });
    } catch (error) {
      clipboardGranted = false;
      console.log('  real browser: clipboard permissions unavailable: ' + error.message);
    }
    const readClipboard = async () => {
      if (!clipboardGranted) return '';
      return String(await session.eval('navigator.clipboard.readText()'));
    };

    // The drag above selected the first paragraph: copying it must put that text on
    // the clipboard (Chrome's own copy is silent for these selections).
    await pressCtrl('c', 'KeyC');
    await new Promise((r) => setTimeout(r, 300));
    const copiedParagraph = await readClipboard();
    console.log('  real browser: clipboard after copying the drag -> ' +
      JSON.stringify(copiedParagraph.slice(0, 40)));
    check('a dragged selection can be copied',
      copiedParagraph.indexOf('First paragraph here.') >= 0, JSON.stringify(copiedParagraph));

    // Ctrl+A covers the whole document, and copying it has to give the whole text
    // (including the blocks that are rendered, not editable).
    await pressCtrl('a', 'KeyA');
    await new Promise((r) => setTimeout(r, 300));
    await pressCtrl('c', 'KeyC');
    await new Promise((r) => setTimeout(r, 300));
    const copiedAll = await readClipboard();
    console.log('  real browser: clipboard after Ctrl+A -> ' + JSON.stringify(copiedAll.slice(0, 70)));
    check('the whole document can be selected and copied',
      copiedAll.indexOf('First paragraph here.') >= 0 &&
      copiedAll.indexOf('let x = 1;') >= 0 &&
      copiedAll.indexOf('Last paragraph.') >= 0, JSON.stringify(copiedAll.slice(0, 90)));

    // Now the code box: open it, click into the middle of the text box, and drag
    // inside it. It is a real <textarea>, so both must behave like one.
    await session.eval(`window.colaimdation.editor().setDocument(
      'text\\n\\n\`\`\`js\\nlet value = 123;\\n\`\`\`\\n', { revision: 'sel2' })`);
    await new Promise((r) => setTimeout(r, 200));
    const openBox = await session.eval(`(function () {
      var rect = window.colaimdation.editor().blockElements[1].getBoundingClientRect();
      return { x: Math.round(rect.left + 20), y: Math.round(rect.top + 10) };
    })()`);
    await session.send('Input.dispatchMouseEvent', {
      type: 'mousePressed', button: 'left', clickCount: 1, x: openBox.x, y: openBox.y });
    await session.send('Input.dispatchMouseEvent', {
      type: 'mouseReleased', button: 'left', clickCount: 1, x: openBox.x, y: openBox.y });
    await new Promise((r) => setTimeout(r, 400));

    const boxDrag = await session.eval(`(function () {
      var input = document.querySelector('.md-code-input');
      if (!input) return null;
      var rect = input.getBoundingClientRect();
      return { x1: Math.round(rect.left + 6), x2: Math.round(rect.left + 70),
               y: Math.round(rect.top + rect.height / 2) };
    })()`);
    if (!boxDrag) {
      check('a code box is available to test mouse selection', false, 'no .md-code-input');
    } else {
      await session.send('Input.dispatchMouseEvent', {
        type: 'mousePressed', button: 'left', clickCount: 1, x: boxDrag.x1, y: boxDrag.y });
      await session.send('Input.dispatchMouseEvent', {
        type: 'mouseMoved', button: 'left', buttons: 1, x: boxDrag.x2, y: boxDrag.y });
      await new Promise((r) => setTimeout(r, 60));
      await session.send('Input.dispatchMouseEvent', {
        type: 'mouseReleased', button: 'left', clickCount: 1, x: boxDrag.x2, y: boxDrag.y });
      await new Promise((r) => setTimeout(r, 500));
      const box = await session.eval(`(function () {
        var active = document.activeElement;
        return { tag: active ? active.tagName : '', cls: active ? active.className : '',
                 start: active && typeof active.selectionStart === 'number' ? active.selectionStart : -1,
                 end: active && typeof active.selectionEnd === 'number' ? active.selectionEnd : -1,
                 value: active && typeof active.value === 'string' ? active.value : '' };
      })()`);
      console.log('  real browser: drag inside the code box -> ' + JSON.stringify(box));
      check('the code box keeps the mouse focus', box.cls.indexOf('md-code-input') >= 0,
        JSON.stringify(box.cls));
      check('dragging inside the code box selects its text',
        box.end > box.start && box.end > 0, JSON.stringify(box));
      check('the code box still holds its code', box.value === 'let value = 123;', JSON.stringify(box.value));

      // Ctrl+A inside a code box means that box's code, not the whole document.
      await session.send('Input.dispatchKeyEvent', {
        type: 'keyDown', modifiers: 2, key: 'a', code: 'KeyA',
        windowsVirtualKeyCode: 65, nativeVirtualKeyCode: 65 });
      await session.send('Input.dispatchKeyEvent', {
        type: 'keyUp', modifiers: 2, key: 'a', code: 'KeyA',
        windowsVirtualKeyCode: 65, nativeVirtualKeyCode: 65 });
      await new Promise((r) => setTimeout(r, 300));
      const boxSelectAll = await session.eval(`(function () {
        var a = document.activeElement;
        return { cls: a ? a.className : '', start: a.selectionStart, end: a.selectionEnd,
                 length: a.value.length };
      })()`);
      console.log('  real browser: Ctrl+A inside the code box -> ' + JSON.stringify(boxSelectAll));
      check('Ctrl+A inside a code box selects the code of that box',
        boxSelectAll.start === 0 && boxSelectAll.end === boxSelectAll.length &&
        boxSelectAll.length > 0, JSON.stringify(boxSelectAll));
    }

    // Printing and exporting show the document: the chrome steps aside, the editor
    // column stops clipping (the engine used to print one screenful and cut the rest
    // off), and the caret's block is rendered instead of showing its Markdown. The
    // print media query is what the print engine and PrintToPdf both use.
    await session.eval("window.colaimdation.editor().setDocument('para one\\n\\npara two\\n', { revision: 'paper1' })");
    await session.send('Emulation.setEmulatedMedia', { media: 'print' });
    await new Promise((r) => setTimeout(r, 200));
    const paper = await session.eval(`
      (function () {
        var hidden = ['#toolbar', '#formatbar', '#tabbar', '#statusbar'];
        var styles = hidden.map(function (selector) {
          var element = document.querySelector(selector);
          return selector + '=' + (element ? getComputedStyle(element).display : 'missing');
        });
        var scroll = document.getElementById('editor-scroll');
        return {
          hidden: styles.join(' '),
          bodyHeight: Math.round(document.body.getBoundingClientRect().height),
          htmlOverflow: getComputedStyle(document.documentElement).overflow,
          scrollOverflow: scroll ? getComputedStyle(scroll).overflowY : 'missing',
          pageHeight: Math.round(document.documentElement.scrollHeight),
          paperBackground: getComputedStyle(document.body).backgroundColor,
        };
      })()
    `);
    console.log('  real browser: print media -> ' + JSON.stringify(paper));
    check('printing hides the editor chrome',
      paper.hidden.indexOf('=none') >= 0 && paper.hidden.indexOf('=flex') < 0 &&
      paper.hidden.indexOf('=missing') < 0, paper.hidden);
    check('printing does not clip the document to the window',
      paper.htmlOverflow === 'visible' && paper.scrollOverflow === 'visible',
      'overflow=' + paper.htmlOverflow + '/' + paper.scrollOverflow +
        ' page=' + paper.pageHeight + ' body=' + paper.bodyHeight);
    check('printing uses a light page even in a dark theme',
      paper.paperBackground === 'rgb(255, 255, 255)', paper.paperBackground);
    await session.send('Emulation.setEmulatedMedia', { media: '' });
    await new Promise((r) => setTimeout(r, 150));

    // The export hooks the shell drives: prepare renders every block (the caret block
    // shows Markdown while writing) and reports the size, finish puts the editor back.
    const prepared = await session.eval(`
      (function () {
        var editor = window.colaimdation.editor();
        editor._setCaretOffset(8, false);
        var caret = editor.caretOffset();
        var before = document.querySelectorAll('#editor .md-source').length;
        var size = window.colaimdation.exportPrepare('image');
        var state = {
          caret: caret,
          beforeRaw: before,
          width: size.width,
          height: size.height,
          hidden: getComputedStyle(document.getElementById('toolbar')).display,
          imageMode: document.body.classList.contains('export-image'),
          rawWhileExporting: document.querySelectorAll('#editor .md-source').length,
          editorMaxWidth: getComputedStyle(document.getElementById('editor')).maxWidth,
        };
        window.colaimdation.exportFinish();
        state.rawAfter = document.querySelectorAll('#editor .md-source').length;
        state.caretAfter = editor.caretOffset();
        state.imageModeAfter = document.body.classList.contains('export-image');
        state.exportingAfter = window.colaimdation.exporting();
        return state;
      })()
    `);
    console.log('  real browser: export prepare -> ' + JSON.stringify(prepared));
    check('preparing an export hides the chrome and renders every block',
      prepared.beforeRaw >= 1 && prepared.imageMode && prepared.hidden === 'none' &&
      prepared.rawWhileExporting === 0 && prepared.height >= 200 && prepared.width > 200,
      JSON.stringify(prepared));
    check('finishing an export restores the editing surface and the caret',
      !prepared.imageModeAfter && !prepared.exportingAfter && prepared.rawAfter >= 1 &&
      prepared.caretAfter === prepared.caret,
      JSON.stringify(prepared));

    // The editor's own front-end self-check, the one the View menu runs inside the
    // engine that is really rendering.
    const selfTest = await session.eval('window.colaimdation.editor().selfTest()');
    console.log('  real browser: self-test -> ' + JSON.stringify(selfTest.split('\n')[0]));
    check('the front-end self-test passes',
      selfTest.indexOf('all checks passed') >= 0 && selfTest.indexOf('FAIL') < 0,
      selfTest.replace(/\n/g, ' | '));

    console.log('');
    console.log(failures === 0 ? 'browser: all checks passed' : 'browser: ' + failures + ' FAILED');
    ws.close();
    cleanup();
    process.exit(failures === 0 ? 0 : 1);
  } catch (error) {
    console.error('browser harness error: ' + error.message);
    cleanup();
    process.exit(2);
  }
}

main();
