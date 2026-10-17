/* Colaimdation - the editor surface.
 *
 * Model: `text` is the whole document; `blocks` are its source spans (parsed by
 * render.js). The DOM holds one element per block:
 *
 *   - the block containing the caret is rendered as *raw Markdown source*
 *     (this is the "in-place WYSIWYG" behaviour: your line is the source, the
 *     rest of the document is the rendered result),
 *   - every other block is rendered HTML.
 *
 * Editing therefore happens exactly in the buffer the Agent sees, and a change
 * is reported to the host as (revision_before, full text, caret). There is no
 * lossy HTML -> Markdown guessing anywhere.
 */
(function (global) {
  'use strict';

  var Render = global.ColaiRender;

  function t(key, args) {
    return global.ColaiI18n ? global.ColaiI18n.t(key, args) : key;
  }

  /* ------------------------------------------------------------ code boxes */

  // What a Tab means inside a code box. Two spaces where that is the convention,
  // four anywhere else: the fence's language decides, so a JSON snippet indents
  // like JSON and a C++ one like C++.
  var kTwoSpaceLanguages = {
    javascript: 1, js: 1, jsx: 1, mjs: 1, cjs: 1, typescript: 1, ts: 1, tsx: 1,
    json: 1, jsonc: 1, yaml: 1, yml: 1, html: 1, xml: 1, svg: 1, css: 1, scss: 1,
    less: 1, vue: 1, svelte: 1, markdown: 1, md: 1, coffeescript: 1, haml: 1,
  };

  function codeFenceLanguage(block) {
    if (!block) return '';
    return String(block.language || block.info || '').trim();
  }

  function codeIndentUnit(language) {
    return kTwoSpaceLanguages[String(language || '').toLowerCase()] ? '  ' : '    ';
  }

  function leadingWhitespace(text) {
    var match = /^[ \t]*/.exec(text || '');
    return match ? match[0] : '';
  }

  // One level shallower: up to a unit of spaces, or a single tab.
  function removeIndentUnit(line, unit) {
    var spaces = /^ +/.exec(line);
    if (spaces) return line.slice(Math.min(spaces[0].length, unit.length));
    return line.charAt(0) === '\t' ? line.slice(1) : line;
  }

  // How much one level of outdenting takes off a run of leading whitespace: a tab
  // counts as a level of its own (a tab-indented block stays tab-indented), spaces
  // go a unit at a time, and a partial level left over from pasting is tidied up.
  function indentDrop(whitespace, unit) {
    if (!whitespace) return 0;
    if (whitespace.charAt(0) === '\t') return 1;
    var spaces = /^ +/.exec(whitespace);
    if (!spaces) return 0;
    var remainder = spaces[0].length % unit.length;
    return remainder === 0 ? Math.min(unit.length, spaces[0].length) : remainder;
  }

  // Where the text box's caret and selection are. The Node harness's text box has
  // no selection API, so a missing value means "the caret is at the end".
  function codeSelection(input, value) {
    var start = typeof input.selectionStart === 'number' ? input.selectionStart : value.length;
    var end = typeof input.selectionEnd === 'number' ? input.selectionEnd : start;
    if (start > value.length) start = value.length;
    if (end > value.length) end = value.length;
    if (start < 0) start = 0;
    if (end < start) end = start;
    return { start: start, end: end };
  }

  // A line that opens a block: after "{", "(" or "[" the next line is one level
  // deeper. A trailing ":" means the same in Python, YAML and C++ labels, but not
  // in prose - so a comment ending in a colon is left alone.
  function opensCodeBlock(body) {
    if (/[{\[(]$/.test(body)) return true;
    if (/^(#|\/\/|\*|<!--|--)/.test(body)) return false;
    return /:$/.test(body);
  }

  function Editor(root, textarea) {
    this.root = root;
    this.textarea = textarea;
    this.text = '';
    this.blocks = [];
    this.activeIndex = -1;
    this.sourceMode = false;
    this.revision = '';
    this.readOnly = false;
    this.dirty = false;
    this.blockElements = [];
    this.highlights = [];
    this.onChange = null;
    this.onCaret = null;
    this.onStatus = null;
    this._inputTimer = null;
    this._caretTimer = null;
    this._composing = false;
    this._suspendSelection = false;
    // Set while the writer asked to see a code block's Markdown instead of its
    // code box (the header click); it lasts until the caret leaves the block.
    this._codeSourceView = false;
    // Where the caret is, for decisions taken *while* rendering - the engine's own
    // selection is stale then, because the DOM is being replaced.
    this._caretHint = 0;
    this._lastReportedCaret = null;
    // True between mousedown and mouseup. A drag is a *selection* gesture: the
    // editor must not repaint the block under the caret while it is in progress.
    this._pointerDown = false;
    this._inputBound = this._handleInput.bind(this);
    this._selectionBound = this._handleSelectionChange.bind(this);
    this._compositionStartBound = this._handleCompositionStart.bind(this);
    this._compositionEndBound = this._handleCompositionEnd.bind(this);
    this._beforeInputBound = this._handleBeforeInput.bind(this);
    this._pointerDownBound = this._handlePointerDown.bind(this);
    this._pointerUpBound = this._handlePointerUp.bind(this);
    this._copyBound = this._handleCopy.bind(this);
    this._boundaryKeyDownBound = this._handleBoundaryKeyDown.bind(this);

    root.addEventListener('input', this._inputBound);
    root.addEventListener('beforeinput', this._beforeInputBound);
    root.addEventListener('keydown', this._boundaryKeyDownBound);
    root.addEventListener('compositionstart', this._compositionStartBound);
    root.addEventListener('compositionend', this._compositionEndBound);
    root.addEventListener('mousedown', this._pointerDownBound);
    // On the document: the button is often released outside the editor, and a
    // stuck "pointer is down" flag would stop click-to-edit for good.
    document.addEventListener('mouseup', this._pointerUpBound);
    // Copy is taken over on the document too: a selection that spans the blocks the
    // engine renders as non-editable content copies as *nothing* in Chrome (it looks
    // selected and the clipboard stays empty).
    document.addEventListener('copy', this._copyBound);
    textarea.addEventListener('input', this._inputBound);
    document.addEventListener('selectionchange', this._selectionBound);
  }

  Editor.prototype._handlePointerDown = function (event) {
    this._pointerDown = true;
    if (this.readOnly) return;
    // Clicking a *rendered* code block opens its code box: the block is not
    // editable, so there is nothing else the click could do.
    //
    // This is one delegated handler rather than a per-element one. A per-element
    // handler stayed attached when that same element was later repainted as an open
    // code box, so a mousedown inside the text box ran it: it cancelled the press
    // (preventDefault) and reset the caret to the start of the code, which is why
    // the mouse could neither place the caret nor select text inside a code box.
    var target = event.target;
    if (!target || !target.closest) return;
    if (target.closest('.md-code-input')) return;
    var element = target.closest('[data-md-block]');
    if (!element || element.classList.contains('md-code-box')) return;
    // A code block that is already showing its Markdown - the writer clicked the
    // header, or the caret is on the fence line - is editable text like any other
    // block: the browser has to place the caret and drag the selection. Taking the
    // press here put the caret at the start of the code *and* switched the block
    // back to a code box, which is why the source of a code block could not be
    // clicked into at all.
    if (element.classList.contains('md-active')) return;
    var index = this._nodeBlockIndex(element);
    var block = this.blocks[index];
    if (!block || block.kind !== 'code_block') return;
    event.preventDefault();
    this._caretHint = block.contentStart;
    this._activateBlock(index);
    this._setCaretOffset(block.contentStart, false);
  };

  Editor.prototype._handlePointerUp = function () {
    if (!this._pointerDown) return;
    this._pointerDown = false;
    // The click's selection has settled now: activate the block under the caret
    // (that is what shows its Markdown), or leave a drag selection alone.
    this._handleSelectionChange();
  };

  /* ------------------------------------------------------------------ model */

  Editor.prototype.getText = function () {
    return this.sourceMode ? this.textarea.value : this.text;
  };

  // Markdown has no block for a document that is only whitespace, but the editor
  // still has to hold one: without a block the caret has nowhere to live, the
  // empty-state element is rebuilt on every keystroke and the spaces the writer
  // typed are thrown away. Keep the text as a single paragraph instead.
  Editor.prototype._ensureBlocksFor = function (text) {
    var blocks = Render.parseBlocks(text);
    if (blocks.length || !text.length) return blocks;
    return [{
      kind: 'paragraph',
      start: 0,
      end: text.length,
      contentStart: 0,
      contentEnd: text.length,
      source: text,
      index: 0,
    }];
  };

  Editor.prototype.setDocument = function (text, options) {
    options = options || {};
    this.text = String(text == null ? '' : text).replace(/\r\n?/g, '\n');
    this.blocks = this._ensureBlocksFor(this.text);
    if (typeof options.revision === 'string') this.revision = options.revision;
    if (typeof options.readOnly === 'boolean') this.readOnly = options.readOnly;
    this.dirty = !!options.dirty;
    // Keep the caret where it was when the text is unchanged (selection-only
    // updates from the host must not disturb typing).
    var caret = options.caret;
    if (typeof caret !== 'number') caret = this.caretOffset();
    this._codeSourceView = false;
    this._caretHint = caret;
    this.activeIndex = -1;
    this._renderAll();
    this._setCaretOffset(typeof caret === 'number' ? caret : 0, true);
    this._syncMode();
    // The plain-text view is a view of the same document, so it has to follow every
    // change that comes from elsewhere too: switching tabs in source mode used to
    // keep showing the previous tab's text, and editing it then wrote that text into
    // the new tab.
    if (this.sourceMode) this.textarea.value = this.text;
    this._emitStatus();
  };

  Editor.prototype.setSourceMode = function (on) {
    this.sourceMode = !!on;
    this._syncMode();
    if (this.sourceMode) {
      this.textarea.value = this.text;
      this.textarea.focus();
    } else {
      // Whatever reached the text box is the document: re-parse before rendering, so
      // a text box changed by a route that never fired `input` still renders right.
      this.text = this.textarea.value.replace(/\r\n?/g, '\n');
      this.blocks = this._ensureBlocksFor(this.text);
      this.activeIndex = -1;
      this._renderAll();
      this._setCaretOffset(this._offsetFromTextarea(), true);
    }
    this._emitStatus();
  };

  Editor.prototype._syncMode = function () {
    this.root.hidden = this.sourceMode;
    this.textarea.hidden = !this.sourceMode;
    var badge = document.getElementById('status-mode');
    if (badge) {
      badge.textContent = this.sourceMode ? t('status.modeSource') : t('status.mode');
      badge.classList.toggle('raw', this.sourceMode);
    }
    var button = document.getElementById('btn-source');
    if (button) button.classList.toggle('on', this.sourceMode);
  };

  // Re-renders the strings this object produces itself (mode badge, empty-state
  // placeholder) after an interface-language change.
  Editor.prototype.refreshLanguage = function () {
    this._syncMode();
    if (!this.blocks.length && !this.sourceMode) this._renderAll();
    this._emitStatus();
  };

  /* --------------------------------------------------------------- rendering */

  Editor.prototype._renderAll = function () {
    var fragment = document.createDocumentFragment();
    var self = this;
    this.blockElements = [];
    if (!this.blocks.length) {
      // An empty document needs somewhere to type. This element is a real,
      // editable block-shaped target (it is the only child of the contenteditable
      // root), and its hint text is rendered by CSS from a data attribute rather
      // than being its text content - otherwise the hint would be read back as
      // the document and typed characters would be appended to it.
      var empty = document.createElement('div');
      empty.className = 'md-empty' + (this.readOnly ? '' : ' md-empty-editable');
      empty.setAttribute('data-empty-hint', t('editor.empty'));
      empty.setAttribute('contenteditable', this.readOnly ? 'false' : 'true');
      fragment.appendChild(empty);
      this.root.innerHTML = '';
      this.root.appendChild(fragment);
      return;
    }
    this.blocks.forEach(function (block, index) {
      var element = self._createBlockElement(block, index);
      self.blockElements.push(element);
      fragment.appendChild(element);
    });
    this.root.innerHTML = '';
    this.root.appendChild(fragment);
    Render.renderMermaid(this.root);
  };

  Editor.prototype._createBlockElement = function (block, index) {
    var element = document.createElement('div');
    element.className = 'md-block' + (block.kind === 'code_block' ? ' md-code' : '');
    element.setAttribute('data-md-block', String(index));
    element.setAttribute('data-md-kind', block.kind);
    this._fillBlockElement(element, block, index);
    return element;
  };

  Editor.prototype._fillBlockElement = function (element, block, index) {
    var self = this;
    if (this.activeIndex === index) {
      // A fenced code block is edited as a code box (see _fillCodeBox), not as
      // Markdown source: the writer wants to write code, not fences.
      if (this._isCodeBox(block)) {
        this._fillCodeBox(element, block, index);
        return;
      }
      element.classList.remove('md-code-box');
      element.classList.add('md-active', 'md-source');
      element.setAttribute('contenteditable', this.readOnly ? 'false' : 'true');
      this._fillSource(element, block);
    } else {
      element.classList.remove('md-active', 'md-source', 'md-code-box');
      // Inactive blocks must not be editable. The root is contenteditable, so
      // simply omitting the attribute left every rendered block editable: a
      // keystroke landing in one was read as "the block's text changed" and
      // replaced the block with its own rendered HTML as plain text.
      element.setAttribute('contenteditable', 'false');
      element.innerHTML = Render.blockHtml(block);
      // A click on a rendered code block opens its code box; that is handled by the
      // delegated mousedown in _handlePointerDown (a listener attached here would
      // outlive this rendering and fire again once the block *is* a code box).
    }
  };

  // Shows the block's source in the element the writer edits, with the newline
  // that terminates the block rendered as a line box of its own instead of a
  // trailing '\n' character.
  //
  // A caret at the end of a block does not type where it looks: Chromium inserts
  // a character typed there *before* the trailing line break, so pressing Enter
  // (the caret moves to the line the terminator opens) and then typing put the
  // character back at the end of the line above -
  //     "para one\n" + Enter + "X"  ->  "para oneX\n"
  // - and the new line stayed empty. Measured in the engine, the same is true of
  // a trailing <br>; only a block-level element that holds the line is typeable
  // from every position. So the terminator becomes
  //     <div data-md-term="1"><br></div>
  // which renders as the same empty line and which _rawTextOfElement reads back
  // as the single '\n' it stands for, so an untouched block still round-trips to
  // exactly its own source.
  Editor.prototype._fillSource = function (element, block) {
    var source = block.source || '';
    // Only the block's own terminator becomes the line box; the marker
    // characters ("# ", "- ", the fences) and any trailing spaces stay editable
    // text.
    var terminated = source.length > 0 && source.charAt(source.length - 1) === '\n';
    element.textContent = terminated ? source.slice(0, -1) : source;
    if (terminated) element.appendChild(this._createTerminatorLine());
  };

  Editor.prototype._createTerminatorLine = function () {
    var line = document.createElement('div');
    line.setAttribute('data-md-term', '1');
    line.appendChild(document.createElement('br'));
    return line;
  };

  /* -------------------------------------------------------------- code boxes */

  // A fenced code block is edited in a code box: a language header, the code in a
  // text box of its own, and the fences nowhere in sight. What the writer types
  // there is written back as ordinary Markdown (see _codeBoxSource), so the file,
  // the Agent and the rendered view all still see a plain fenced block.
  //
  // The box appears as soon as the opening fence line is complete - "```cpp" then
  // Enter - and not before: while the info string is still being typed, that line
  // is source text the writer is editing.
  Editor.prototype._isCodeBox = function (block) {
    if (!block || block.kind !== 'code_block' || block.indented) return false;
    if (this._codeSourceView) return false;
    if (block.contentStart <= block.start) return false;
    if (this.text.charAt(block.contentStart - 1) !== '\n') return false;
    // A fence line that happens to be followed by an existing newline is still a
    // line being typed: while the caret is on it (the language), it stays source.
    var caret = this._caretHint;
    return !(typeof caret === 'number' && caret >= block.start && caret < block.contentStart);
  };

  // What the text box holds: the code between the fences, without the newline
  // that ends its last line. A text box has no use for that newline, and the
  // closing fence needs it - see _codeBoxSource.
  Editor.prototype._codeBoxValue = function (block) {
    var code = this.text.slice(block.contentStart, block.contentEnd);
    return code.charAt(code.length - 1) === '\n' ? code.slice(0, -1) : code;
  };

  // The Markdown a code box writes back: the opening fence exactly as it was, the
  // code, and a closing fence.
  //
  // A block whose fence was never closed gets one, as long as the fence that
  // opened it - without it the code would swallow the rest of the document, and
  // the writer cannot add it by hand any more.
  Editor.prototype._codeBoxSource = function (block, value) {
    var open = this.text.slice(block.start, block.contentStart);
    var close;
    if (block.closed) {
      close = this.text.slice(block.contentEnd, block.end);
    } else {
      var fence = /^[ \t]*(`{3,}|~{3,})/.exec(open);
      close = (fence ? fence[1] : '```') + '\n';
    }
    var body = value;
    if (body !== '' && body.charAt(body.length - 1) !== '\n') body += '\n';
    return open + body + close;
  };

  // The Markdown a rendered code box stands for, read back out of the live text
  // box. The generic DOM -> text rules cannot do this: a text box keeps its value
  // in the DOM property, not in a text node.
  Editor.prototype._codeBoxSourceOf = function (element) {
    var index = parseInt(element.getAttribute('data-md-block'), 10);
    var block = this.blocks[index];
    var input = element.querySelector ? element.querySelector('.md-code-input') : null;
    if (!block || !input || typeof input.value !== 'string') return '';
    return this._codeBoxSource(block, input.value);
  };

  // The raw text of a code box element: the Markdown it stands for, plus the line
  // below it (the block's terminator line, which is an editable line of its own so
  // the writer can carry on writing after the block).
  Editor.prototype._codeBoxRaw = function (element) {
    var raw = this._codeBoxSourceOf(element);
    var line = element.lastChild;
    if (line && line.nodeType === 1 && line.getAttribute && line.getAttribute('data-md-term')) {
      if (raw.charAt(raw.length - 1) !== '\n') raw += '\n';
      raw += this._lineBoxText(line);
    }
    return raw;
  };

  // An empty line box is filled with a <br>, which is a placeholder rather than
  // content: set the text of an empty line box with the same rule here so the two
  // read back the same way.
  Editor.prototype._lineBoxText = function (line) {
    if (!line) return '';
    var child = line.firstChild;
    if (child && child.nodeType === 1 && child.nodeName === 'BR' && !child.nextSibling) return '';
    return this._rawTextOfElement(line);
  };

  Editor.prototype._codeInputOf = function (element) {
    return element && element.querySelector ? element.querySelector('.md-code-input') : null;
  };

  // Puts the editor into (and out of) "show the document" mode: every block is
  // rendered, including the one the caret is in, which normally shows its Markdown
  // source. Printing and exporting go through this, and the caret is put back
  // afterwards, so the writer finds the editor exactly as they left it.
  Editor.prototype.setExporting = function (on) {
    if (on) {
      if (this._exportState) return;
      this._exportState = {
        active: this.activeIndex,
        caret: this.caretOffset(),
        sourceView: this._codeSourceView,
      };
      this.activeIndex = -1;
      this._codeSourceView = false;
      this._renderAll();
      return;
    }
    var state = this._exportState;
    this._exportState = null;
    if (!state) return;
    this.activeIndex = -1;
    this._codeSourceView = false;
    this._renderAll();
    // Restoring the caret activates the block it is in again, which is what puts a code
    // box (or the Markdown of a code block) back on screen.
    this._codeSourceView = !!state.sourceView;
    this._setCaretOffset(state.caret, false);
  };

  // Grows the text box with its content, so a long function is not a scrollbar.
  Editor.prototype._autosizeCodeBox = function (input) {
    if (!input || !input.style) return;
    input.style.height = 'auto';
    if (typeof input.scrollHeight === 'number') input.style.height = input.scrollHeight + 'px';
  };

  Editor.prototype._fillCodeBox = function (element, block, index) {
    var self = this;
    element.classList.remove('md-source');
    element.classList.add('md-active', 'md-code-box');
    // The box itself is not editable content; the text box and the line below it
    // are the things the writer types into.
    element.setAttribute('contenteditable', 'false');
    element.innerHTML = '';

    // The header and the code sit inside a frame of their own, and the line the
    // writer moves on to sits *below* that frame: while the frame was the block
    // element itself, that line was drawn inside the box's border and background, so
    // starting the next paragraph looked like the caret was still in the code.
    var frame = document.createElement('div');
    frame.className = 'md-code-frame';
    frame.setAttribute('contenteditable', 'false');

    var head = document.createElement('div');
    head.className = 'md-code-head';
    head.setAttribute('contenteditable', 'false');
    head.title = t('toolbar.source');
    // Clicking the header shows this block's Markdown instead of the box, which is
    // how the fence (and with it the language) is changed.
    head.addEventListener('mousedown', function (event) {
      event.preventDefault();
      event.stopPropagation();
      self._codeSourceView = true;
      self._repaintBlock(index);
      self._setCaretOffset(self.blocks[index].start, false);
    });
    var label = document.createElement('span');
    label.className = 'md-code-lang';
    // The language as it was written in the fence; a bare fence has none.
    label.textContent = block.info || block.language || '';
    head.appendChild(label);
    frame.appendChild(head);

    // The syntax colours live in a <pre> that sits exactly under the text box, and
    // the text box draws its own text transparent: a decoration layer instead of
    // spans inside the thing being typed into, where every re-wrap of the HTML
    // would move the caret mid-keystroke.
    var field = document.createElement('div');
    field.className = 'md-code-field';
    field.setAttribute('contenteditable', 'false');
    var shade = document.createElement('pre');
    shade.className = 'md-code-highlight';
    shade.setAttribute('aria-hidden', 'true');
    field.appendChild(shade);

    var input = document.createElement('textarea');
    input.className = 'md-code-input';
    input.value = this._codeBoxValue(block);
    input.spellcheck = false;
    input.rows = 1;
    input.setAttribute('wrap', 'off');
    input.readOnly = this.readOnly;
    input.addEventListener('input', function () { self._onCodeInput(index); });
    input.addEventListener('keydown', function (event) { self._onCodeKeyDown(event, index); });
    input.addEventListener('keyup', function () { self._onCodeCaret(); });
    input.addEventListener('click', function () { self._onCodeCaret(); });
    input.addEventListener('select', function () { self._onCodeCaret(); });
    // The layer has to scroll with the text box (a long line scrolls sideways), and
    // it steps aside while an input method is composing: the composition string is
    // drawn by the text box itself and its text is transparent while it types.
    input.addEventListener('scroll', function () { self._syncCodeScroll(element); });
    input.addEventListener('compositionstart', function () {
      field.classList.add('md-code-composing');
    });
    input.addEventListener('compositionend', function () {
      field.classList.remove('md-code-composing');
      self._paintCodeHighlight(element);
    });
    field.appendChild(input);
    frame.appendChild(field);
    element.appendChild(frame);

    var line = this._createTerminatorLine();
    line.className = 'md-code-after';
    line.setAttribute('contenteditable', this.readOnly ? 'false' : 'true');
    line.addEventListener('keydown', function (event) { self._onCodeAfterKeyDown(event, index); });
    element.appendChild(line);

    this._autosizeCodeBox(input);
    this._paintCodeHighlight(element);
  }

  /* ------------------------------------------------- code colour + indentation */

  // Keys on the empty line the box leaves for the next paragraph. Up goes back into
  // the code, and Backspace must *not* hand the caret back to it: Chrome's own
  // "join with the line above" put the caret after the last code character, where the
  // writer then went on typing code instead of a paragraph. Joining that line with the
  // code would glue the next paragraph onto the closing fence, so the only useful
  // thing Backspace can do there is remove a box that no longer holds any code.
  Editor.prototype._onCodeAfterKeyDown = function (event, index) {
    var element = this.blockElements[index];
    var line = event.target;
    if (event.key === 'ArrowUp' && !event.shiftKey) {
      var box = this._codeInputOf(element);
      if (!box || !box.setSelectionRange) return;
      event.preventDefault();
      try { box.focus({ preventScroll: true }); } catch (error) { box.focus(); }
      box.setSelectionRange(box.value.length, box.value.length);
      this._onCodeCaret();
      return;
    }
    if (event.key !== 'Backspace' || event.isComposing) return;
    this._handleCodeAfterBackspace(index, line, event);
  };

  // Stops the edit on that line, whatever spelling of Backspace it is (Ctrl+Backspace
  // and Shift+Backspace included) and however the engine delivers it - a key press or
  // a beforeinput edit. Returns true when the press was handled.
  Editor.prototype._handleCodeAfterBackspace = function (index, line, event) {
    if (this.readOnly || this.sourceMode) return false;
    // Only from an empty line: with text on it there is something to delete.
    if (line && this._lineBoxText(line) !== '') return false;
    if (event && event.cancelable === false) return false;
    if (event) event.preventDefault();
    var element = this.blockElements[index];
    var input = this._codeInputOf(element);
    var code = input && typeof input.value === 'string' ? input.value
                                                       : this._codeBoxValue(this.blocks[index]);
    if (code !== '') return true;
    this._deleteCodeBlock(index);
    return true;
  };

  // Backspace at the very start of a block whose previous block is a code box. There is
  // nothing to join the two with - the line above is a code box, not text - and the
  // engine's own attempt at joining handed the caret back into the code, so the press
  // is stopped here too. An empty box goes with it, like everywhere else.
  Editor.prototype._handleBoundaryKeyDown = function (event) {
    if (event.key !== 'Backspace' || event.isComposing) return;
    if (this.readOnly || this.sourceMode) return;
    // Keys in a code box's text box belong to it (see _onCodeKeyDown): the DOM
    // selection can still point at the block the writer came from, and acting on that
    // would cancel what they are typing.
    var target = event.target;
    if (target && target.classList && target.classList.contains('md-code-input')) return;
    var selection = global.getSelection();
    if (!selection || !selection.rangeCount || !selection.isCollapsed || !selection.anchorNode) {
      return;
    }
    var index = this._nodeBlockIndex(selection.anchorNode);
    if (index <= 0 || index !== this.activeIndex) return;
    var block = this.blocks[index];
    var element = this.blockElements[index];
    var previous = this.blocks[index - 1];
    if (!block || !element || !previous) return;
    // A code box and its own line are handled by _onCodeAfterKeyDown.
    if (block.kind === 'code_block' || previous.kind !== 'code_block' || previous.indented) return;
    if (this._rawOffsetInElement(element, selection.anchorNode, selection.anchorOffset) !== 0) {
      return;
    }
    event.preventDefault();
    if (this._codeBoxValue(previous) === '') this._deleteCodeBlock(index - 1);
  };

  // Removes the whole fenced block - fences, code and the newline that ends it - which
  // is how an empty code box is got rid of. One blank-line separator goes with it: a
  // block between two paragraphs is surrounded by blank lines, and dropping the block
  // alone would leave a double blank line behind.
  Editor.prototype._deleteCodeBlock = function (index) {
    var block = this.blocks[index];
    if (!block) return false;
    var before = this.text.slice(0, block.start);
    var after = this.text.slice(block.end);
    if (after.charAt(0) === '\n' &&
        (before === '' || before.charAt(before.length - 1) === '\n')) {
      after = after.slice(1);
    }
    this.text = before + after;
    this._codeSourceView = false;
    this._caretHint = Math.min(block.start, this.text.length);
    this.activeIndex = -1;
    this._commitAndRerender(this._caretHint);
    this._notifyChange('typing');
    return true;
  };

  // The code box's colours: the same rules the rendered block uses, so the code
  // looks the same whether the writer is inside it or not.
  Editor.prototype._paintCodeHighlight = function (element) {
    if (!element || !element.querySelector) return;
    var shade = element.querySelector('.md-code-highlight');
    var input = this._codeInputOf(element);
    if (!shade || !input || typeof input.value !== 'string') return;
    var index = parseInt(element.getAttribute('data-md-block'), 10);
    var block = this.blocks[index];
    shade.innerHTML = Render.highlightCodeHtml(input.value, codeFenceLanguage(block));
  };

  // A long code line scrolls the text box sideways; the layer under it has to move
  // by the same amount or the colours drift away from the glyphs.
  Editor.prototype._syncCodeScroll = function (element) {
    if (!element || !element.querySelector) return;
    var shade = element.querySelector('.md-code-highlight');
    var input = this._codeInputOf(element);
    if (!shade || !input || !shade.style) return;
    shade.scrollLeft = input.scrollLeft || 0;
    shade.scrollTop = input.scrollTop || 0;
  };

  Editor.prototype._setCodeSelection = function (input, start, end) {
    var length = String(input.value || '').length;
    if (typeof start !== 'number') start = length;
    if (typeof end !== 'number') end = start;
    if (start < 0) start = 0;
    if (end < start) end = start;
    if (start > length) start = length;
    if (end > length) end = length;
    if (typeof input.setSelectionRange === 'function') {
      input.setSelectionRange(start, end);
      return;
    }
    input.selectionStart = start;
    input.selectionEnd = end;
  };

  // Applies one edit to a code box. insertText is used whenever the browser offers
  // it: assigning `value` throws away the text box's own undo history, and a writer
  // who presses Ctrl+Z after a Tab expects the indentation to come back.
  //
  // `caretStart`/`caretEnd` are offsets from the start of the replacement.
  Editor.prototype._applyCodeEdit = function (input, index, edit) {
    var applied = false;
    if (typeof document !== 'undefined' && typeof document.execCommand === 'function' &&
        typeof input.setSelectionRange === 'function') {
      input.setSelectionRange(edit.start, edit.end);
      try { applied = document.execCommand('insertText', false, edit.text); } catch (error) { applied = false; }
    }
    if (!applied) {
      var value = String(input.value || '');
      input.value = value.slice(0, edit.start) + edit.text + value.slice(edit.end);
      this._setCodeSelection(input, edit.start + edit.caretStart, edit.start + edit.caretEnd);
      this._onCodeInput(index);
      return;
    }
    // insertText fired `input`, so the model is already in step; only the caret has
    // to be put back where the writer expects it.
    this._setCodeSelection(input, edit.start + edit.caretStart, edit.start + edit.caretEnd);
  };

  // Enter: keep the line's indentation, and open one level deeper after a line that
  // starts a block. In front of a closing bracket the new line stays on the outer
  // level, so "}" lands where it belongs instead of being pushed inward.
  Editor.prototype._codeEnterEdit = function (input, unit) {
    var value = String(input.value || '');
    var span = codeSelection(input, value);
    var lineStart = value.lastIndexOf('\n', span.start - 1) + 1;
    var lineEnd = value.indexOf('\n', span.start);
    if (lineEnd < 0) lineEnd = value.length;
    var before = value.slice(lineStart, span.start);
    var indent = leadingWhitespace(before);
    var body = before.slice(indent.length).replace(/[ \t]+$/, '');
    var deeper = opensCodeBlock(body) && !/^[ \t]*[}\])]/.test(value.slice(span.start, lineEnd));
    var text = '\n' + indent + (deeper ? unit : '');
    return { start: span.start, end: span.end, text: text, caretStart: text.length, caretEnd: text.length };
  };

  // Tab indents (and never leaves the box); Shift+Tab takes a level back. With a
  // selection that spans lines, every line in it moves together - which is what
  // makes a pasted snippet possible to line up.
  Editor.prototype._codeTab = function (input, index, unit, outdent) {
    var value = String(input.value || '');
    var span = codeSelection(input, value);
    var firstStart = value.lastIndexOf('\n', span.start - 1) + 1;
    if (value.slice(span.start, span.end).indexOf('\n') === -1) {
      if (!outdent) {
        this._applyCodeEdit(input, index, {
          start: span.start, end: span.end, text: unit,
          caretStart: unit.length, caretEnd: unit.length,
        });
        return;
      }
      var lead = leadingWhitespace(value.slice(firstStart, value.length));
      if (!lead) return;
      var drop = indentDrop(lead, unit);
      if (!drop) return;
      var caret = Math.max(firstStart, span.start - drop) - firstStart;
      this._applyCodeEdit(input, index, {
        start: firstStart, end: firstStart + drop, text: '', caretStart: caret, caretEnd: caret,
      });
      return;
    }
    // A selection that ends exactly at the start of a line does not include it.
    var endPos = span.end;
    if (endPos > span.start && value.charAt(endPos - 1) === '\n') endPos -= 1;
    var lastLineEnd = value.indexOf('\n', endPos);
    if (lastLineEnd < 0) lastLineEnd = value.length;
    var region = value.slice(firstStart, lastLineEnd);
    var lines = region.split('\n');
    for (var i = 0; i < lines.length; i++) {
      lines[i] = outdent ? removeIndentUnit(lines[i], unit) : unit + lines[i];
    }
    var text = lines.join('\n');
    // The same lines stay selected, so Tab can be pressed again.
    this._applyCodeEdit(input, index, {
      start: firstStart, end: lastLineEnd, text: text, caretStart: 0, caretEnd: text.length,
    });
  };

  // A closing bracket typed on a line that is nothing but indentation steps out one
  // level first: the writer who opened a block with Enter and typed "}" gets the
  // bracket under the line that opened it.
  Editor.prototype._codeCloserEdit = function (input, unit, key) {
    var value = String(input.value || '');
    var span = codeSelection(input, value);
    if (span.start !== span.end) return null;
    var lineStart = value.lastIndexOf('\n', span.start - 1) + 1;
    var before = value.slice(lineStart, span.start);
    if (!/^[ \t]+$/.test(before)) return null;
    var drop = indentDrop(before, unit);
    if (!drop) return null;
    var text = before.slice(0, before.length - drop) + key;
    return { start: lineStart, end: span.start, text: text, caretStart: text.length, caretEnd: text.length };
  };;

  // The writer typed in the code box: write the code back into the block and keep
  // the model's block spans in step. The DOM is deliberately left alone - the text
  // box is what the caret lives in, and rebuilding it would throw the caret, the
  // selection and any composition away.
  Editor.prototype._onCodeInput = function (index) {
    var element = this.blockElements[index];
    var block = this.blocks[index];
    var input = this._codeInputOf(element);
    if (!element || !block || !input || this.readOnly) return;
    var source = this._codeBoxSource(block, input.value);
    this.text = this.text.slice(0, block.start) + source + this.text.slice(block.end);
    this.blocks = this._ensureBlocksFor(this.text);
    this._autosizeCodeBox(input);
    this._paintCodeHighlight(element);
    this.dirty = true;
    // The code can end the box: a line that closes the fence ("```") splits the
    // block, and then the box no longer describes the document. The check is
    // whether the block that came out of the re-parse is still exactly the Markdown
    // the text box stands for.
    var settled = this.blocks[index] && this._isCodeBox(this.blocks[index]) &&
      this.text.slice(this.blocks[index].start, this.blocks[index].end) === source;
    if (!settled) {
      var target = Math.min(this.text.length, block.start + source.length);
      this._commitAndRerender(target);
      return;
    }
    this._emitStatus();
    this._notifyChange('typing');
  };

  // Inside a code box: Tab indents (and does not move the focus out), Enter keeps
  // the indentation and opens a level deeper after "{" or a line ending in ":",
  // a closing bracket on a line of pure indentation steps back out, and the down
  // arrow on the last line steps out of the box onto the line below it.
  Editor.prototype._onCodeKeyDown = function (event, index) {
    // A read-only text box still receives keys, and typing into one would leave the
    // colours behind the text it draws - the model would not change at all.
    if (this.readOnly || this.sourceMode || event.isComposing) return;
    var input = event.target;
    var unit = codeIndentUnit(codeFenceLanguage(this.blocks[index]));
    if (event.key === 'Enter' && !event.shiftKey && !event.ctrlKey && !event.altKey &&
        !event.metaKey) {
      event.preventDefault();
      this._applyCodeEdit(input, index, this._codeEnterEdit(input, unit));
      return;
    }
    if (event.key === 'Tab') {
      event.preventDefault();
      this._codeTab(input, index, unit, event.shiftKey);
      return;
    }
    // An empty code box is removed with Backspace: there is nothing left to delete
    // inside it, and the box (fences and all) is what the writer is getting rid of.
    // So "delete the last line of code, then press Backspace again" works.
    if (event.key === 'Backspace' && !event.ctrlKey && !event.altKey && !event.metaKey &&
        String(input.value || '') === '') {
      event.preventDefault();
      this._deleteCodeBlock(index);
      return;
    }
    if ((event.key === '}' || event.key === ')' || event.key === ']') && !event.ctrlKey &&
        !event.altKey && !event.metaKey) {
      var closer = this._codeCloserEdit(input, unit, event.key);
      if (closer) {
        event.preventDefault();
        this._applyCodeEdit(input, index, closer);
        return;
      }
    }
    if (event.key === 'ArrowDown' && !event.shiftKey) {
      var input2 = event.target;
      var rest = input2.value.slice(input2.selectionStart);
      if (rest.indexOf('\n') === -1) {
        // Already on the last line: the line below the box is where writing
        // continues.
        event.preventDefault();
        this._setCaretOffset(this.blocks[index].end, false);
      }
    }
  };

  Editor.prototype._onCodeCaret = function () {
    this._emitStatus();
    this._emitCaret(true);
  };

  // The text box of the block being edited, when that is where the caret is.
  Editor.prototype._activeCodeInput = function () {
    if (this.sourceMode || this.activeIndex < 0) return null;
    var block = this.blocks[this.activeIndex];
    if (!this._isCodeBox(block)) return null;
    var element = this.blockElements[this.activeIndex];
    var input = this._codeInputOf(element);
    if (!input || !input.setSelectionRange) return null;
    if (!document.activeElement || document.activeElement !== input) return null;
    return input;
  };

  Editor.prototype._repaintBlock = function (index) {
    var element = this.blockElements[index];
    var block = this.blocks[index];
    if (!element || !block) return;
    this._fillBlockElement(element, block, index);
    if (this.activeIndex !== index) Render.renderMermaid(element);
  };

  /* --------------------------------------------------------------- selection */

  Editor.prototype._nodeBlockIndex = function (node) {
    var current = node;
    while (current && current !== this.root) {
      if (current.nodeType === 1 && current.hasAttribute && current.hasAttribute('data-md-block')) {
        return parseInt(current.getAttribute('data-md-block'), 10);
      }
      current = current.parentNode;
    }
    return -1;
  };

  // Offset of a DOM position in the *raw* text of an element - the text
  // _rawTextOfElement() produces, where a <br>, the terminator line and every
  // block boundary count as one '\n'. Used to read the caret out of the block
  // being edited and (for a block the browser split) out of one of the pieces.
  Editor.prototype._rawOffsetInElement = function (element, node, offset) {
    var blockLevel = /^(DIV|P|LI|UL|OL|BLOCKQUOTE|PRE|TABLE|TR|H[1-6]|SECTION|ARTICLE)$/;
    var out = '';
    var reached = false;
    function walk(current) {
      // A caret *on* an element sits between its children, so only the first
      // `offset` of them are in front of it.
      var limit = current === node ? offset : current.childNodes.length;
      for (var i = 0; i < limit && !reached; i++) {
        var child = current.childNodes[i];
        if (child === node) {
          reached = true;
          if (child.nodeType === 3) {
            out += child.nodeValue.slice(0, offset);
            return;
          }
          // The caret is on this element. On a terminator line the newline it
          // stands for comes first, and then the first `offset` of its children.
          if (child.getAttribute && child.getAttribute('data-md-term') &&
              out.charAt(out.length - 1) !== '\n') {
            out += '\n';
          }
          walk(child);
          return;
        }
        if (child.nodeType === 3) { out += child.nodeValue; continue; }
        if (child.nodeType !== 1) continue;
        if (child.nodeName === 'BR') {
          if (out.charAt(out.length - 1) !== '\n') out += '\n';
          continue;
        }
        // The active block's terminator line, exactly as _rawTextOfElement
        // counts it.
        if (child.getAttribute && child.getAttribute('data-md-term')) {
          if (out.charAt(out.length - 1) !== '\n') out += '\n';
          walk(child);
          continue;
        }
        if (!blockLevel.test(child.nodeName)) { walk(child); continue; }
        if (out.length > 0 && out.charAt(out.length - 1) !== '\n') out += '\n';
        var emptyBlock = child.textContent === '';
        walk(child);
        if (emptyBlock) {
          if (out.charAt(out.length - 1) !== '\n') out += '\n';
          out += '\n';
        }
      }
    }
    walk(element);
    return out.length;
  };

  // The inverse of _rawOffsetInElement: a collapsed range at raw offset `local`
  // inside a block element. The caret has to be placed with the same rules the
  // text is read back with, or the two drift apart by the newline the block's
  // terminator line stands for.
  Editor.prototype._rangeAtRawOffset = function (element, local) {
    // A code box: the code itself is a text box (placed by _setCaretOffset), so
    // only the line below the box has DOM positions to hand out.
    if (element.classList && element.classList.contains('md-code-box')) {
      var prefix = this._codeBoxSourceOf(element);
      var line = element.lastChild;
      var boxRange = document.createRange();
      if (!line) {
        boxRange.selectNodeContents(element);
        boxRange.collapse(false);
        return boxRange;
      }
      var base = prefix.length + (prefix.charAt(prefix.length - 1) === '\n' ? 0 : 1);
      var inside = Math.max(0, local - base);
      var text = line.firstChild;
      if (text && text.nodeType === 3) {
        boxRange.setStart(text, Math.min(inside, text.nodeValue.length));
      } else if (inside <= 0 && text && text.nodeType === 1 && text.nodeName === 'BR') {
        boxRange.setStart(line, 0);
      } else {
        boxRange.selectNodeContents(line);
        boxRange.collapse(false);
      }
      return boxRange;
    }
    var blockLevel = /^(DIV|P|LI|UL|OL|BLOCKQUOTE|PRE|TABLE|TR|H[1-6]|SECTION|ARTICLE)$/;
    var range = document.createRange();
    var out = '';
    var placed = false;
    function walk(current) {
      for (var child = current.firstChild; child && !placed; child = child.nextSibling) {
        if (child.nodeType === 3) {
          var length = child.nodeValue.length;
          if (local - out.length <= length) {
            range.setStart(child, Math.max(0, local - out.length));
            placed = true;
            return;
          }
          out += child.nodeValue;
          continue;
        }
        if (child.nodeType !== 1) continue;
        if (child.nodeName === 'BR') {
          if (out.charAt(out.length - 1) !== '\n') out += '\n';
          continue;
        }
        var terminator = child.getAttribute && child.getAttribute('data-md-term');
        if (terminator) {
          if (out.charAt(out.length - 1) !== '\n') out += '\n';
          // The line the terminator opens: its start is a caret position of its
          // own, one past the newline.
          if (local <= out.length) {
            range.setStart(child, 0);
            placed = true;
            return;
          }
          walk(child);
          continue;
        }
        if (!blockLevel.test(child.nodeName)) { walk(child); continue; }
        if (out.length > 0 && out.charAt(out.length - 1) !== '\n') out += '\n';
        var emptyBlock = child.textContent === '';
        walk(child);
        if (emptyBlock) {
          if (out.charAt(out.length - 1) !== '\n') out += '\n';
          out += '\n';
        }
      }
    }
    walk(element);
    if (!placed) {
      range.selectNodeContents(element);
      range.collapse(false);
    }
    return range;
  };

  Editor.prototype._domOffsetInBlock = function (blockIndex, node, offset) {
    var element = this.blockElements[blockIndex];
    if (!element) return 0;
    var block = this.blocks[blockIndex];
    if (!block) return 0;
    // In source view the block shows `block.source`, so the caret's position
    // inside the element *is* the offset in the source - measured with the same
    // rules that turn the DOM back into text, so the terminator line counts as
    // the '\n' it stands for.
    //
    // Nothing is clamped to the block's own length: the element is the text the
    // writer is looking at, and it is one keystroke ahead of `block.source` while
    // this is called. Clamping to the *old* length was what made the caret come
    // back one short, so the next character was inserted before the last one
    // ("```cpp" produced "``cpp`").
    if (this.activeIndex === blockIndex) {
      if (node !== element && (!node || !element.contains(node))) {
        // A selection the repaint invalidated, or a node that belongs to a copy
        // of this block the browser made when it split it: nothing better than
        // the position the engine reported.
        return Math.max(0, Math.min(offset, block.source.length));
      }
      if (element.classList && element.classList.contains('md-code-box')) {
        // Only the line below the box is contenteditable; the code lives in a text
        // box, and the caret's place there is read in caretOffset().
        var boxRaw = this._codeBoxSourceOf(element);
        var line = element.lastChild;
        if (!line || line !== node && !line.contains(node)) {
          return Math.max(0, this._rawOffsetInElement(element, node, offset));
        }
        var base = boxRaw.length + (boxRaw.charAt(boxRaw.length - 1) === '\n' ? 0 : 1);
        return base + this._rawOffsetInElement(line, node, offset);
      }
      return Math.max(0, this._rawOffsetInElement(element, node, offset));
    }
    // Rendered view: anchor on the clicked text, then locate it in the source.
    var range = document.createRange();
    try {
      range.setStart(element, 0);
      range.setEnd(node, offset);
    } catch (error) {
      return 0;
    }
    var text = range.toString();
    var trimmed = text.replace(/\s+/g, ' ').trim();
    if (!trimmed) return 0;
    var needle = trimmed.slice(-64);
    var source = block.source;
    var at = source.lastIndexOf(needle);
    if (at < 0) {
      // Fall back to the first character of the needle.
      at = source.indexOf(needle.charAt(0));
      if (at < 0) at = 0;
      return at;
    }
    return at + needle.length;
  };

  Editor.prototype.caretOffset = function () {
    if (this.sourceMode) return this._offsetFromTextarea();
    var input = this._activeCodeInput();
    if (input) {
      return this.blocks[this.activeIndex].contentStart +
        Math.min(input.selectionStart || 0, input.value.length);
    }
    var selection = global.getSelection();
    if (!selection || !selection.anchorNode) {
      return this.activeIndex >= 0 && this.blocks[this.activeIndex]
        ? this.blocks[this.activeIndex].start
        : 0;
    }
    var index = this._nodeBlockIndex(selection.anchorNode);
    // The selection can outlive the block list: emptying the document rebuilds
    // the DOM and destroys the node the selection points at. Guard the lookup -
    // this used to read this.blocks[index].start on a stale index and throw.
    if (index < 0 || !this.blocks[index]) {
      return this.blocks.length ? this.blocks[this.blocks.length - 1].end : 0;
    }
    return this.blocks[index].start + this._domOffsetInBlock(index, selection.anchorNode, selection.anchorOffset);
  };

  Editor.prototype.selectionOffsets = function () {
    if (this.sourceMode) {
      var value = this.textarea.value;
      return { anchor: this.textarea.selectionStart, head: this.textarea.selectionEnd };
    }
    var input = this._activeCodeInput();
    if (input) {
      var base = this.blocks[this.activeIndex].contentStart;
      var length = input.value.length;
      return {
        anchor: base + Math.min(input.selectionStart || 0, length),
        head: base + Math.min(input.selectionEnd || 0, length),
      };
    }
    var selection = global.getSelection();
    if (!selection || !selection.anchorNode) {
      var caret = this.caretOffset();
      return { anchor: caret, head: caret };
    }
    var anchorIndex = this._nodeBlockIndex(selection.anchorNode);
    var headIndex = this._nodeBlockIndex(selection.focusNode);
    if (anchorIndex < 0 || headIndex < 0 ||
        !this.blocks[anchorIndex] || !this.blocks[headIndex]) {
      // Stale selection: the node it points at belonged to a block list that has
      // since been rebuilt (emptying the document destroys the node). Fall back
      // to the plain caret instead of dereferencing a block that is gone - this
      // threw from the debounced caret timer, which runs *after* the edit.
      var only = this.caretOffset();
      return { anchor: only, head: only };
    }
    return {
      anchor: this.blocks[anchorIndex].start + this._domOffsetInBlock(anchorIndex, selection.anchorNode, selection.anchorOffset),
      head: this.blocks[headIndex].start + this._domOffsetInBlock(headIndex, selection.focusNode, selection.focusOffset),
    };
  };

  Editor.prototype._offsetFromTextarea = function () {
    return this.textarea.selectionStart || 0;
  };

  Editor.prototype._setCaretOffset = function (offset, alsoScroll) {
    offset = Math.max(0, Math.min(offset, this.text.length));
    this._caretHint = offset;
    if (this.sourceMode) {
      this.textarea.setSelectionRange(offset, offset);
      return;
    }
    var index = this._blockIndexForOffset(offset);
    if (index < 0) return;
    var element = this.blockElements[index];
    if (!element) return;
    if (this.activeIndex !== index) {
      this._activateBlock(index);
      element = this.blockElements[index];
    }
    // Moving the caret from the fence line into the code (or back) changes what
    // that block *is*: source text, or a code box.
    if (this._isCodeBox(this.blocks[index]) !==
        (element.classList && element.classList.contains('md-code-box'))) {
      this._repaintBlock(index);
      element = this.blockElements[index];
      if (!element) return;
    }
    // A code box: the caret lives either in its text box or on the line below it.
    if (element.classList && element.classList.contains('md-code-box')) {
      var codeBlock = this.blocks[index];
      var source = this._codeBoxSourceOf(element);
      if (offset - codeBlock.start < source.length) {
        var input = this._codeInputOf(element);
        if (input && input.setSelectionRange) {
          var inside = Math.max(0, Math.min(offset - codeBlock.contentStart, input.value.length));
          try { input.focus({ preventScroll: true }); } catch (error) { input.focus(); }
          input.setSelectionRange(inside, inside);
          if (alsoScroll && input.scrollIntoView) input.scrollIntoView({ block: 'nearest' });
          this._emitStatus();
          return;
        }
      }
    }
    // The block's raw length, not its textContent length: the newline that
    // terminates the block is a line box of its own, so the last source offsets
    // sit in that box rather than in a text node.
    var local = Math.max(0, Math.min(offset - this.blocks[index].start,
                                      this.blocks[index].source.length));
    var selection = global.getSelection();
    if (!selection) return;
    var range = this._rangeAtRawOffset(element, local);
    range.collapse(true);
    this._suspendSelection = true;
    selection.removeAllRanges();
    selection.addRange(range);
    this._suspendSelection = false;
    if (alsoScroll && element.scrollIntoView) {
      element.scrollIntoView({ block: 'nearest' });
    }
  };

  Editor.prototype._blockIndexForOffset = function (offset) {
    if (!this.blocks.length) return -1;
    if (offset <= this.blocks[0].start) return 0;
    // Nearest *preceding* block, not "the last block": an offset sitting in the
    // blank line between two blocks used to resolve to the end of the document,
    // which sent the caret to the wrong block and produced a negative in-block
    // offset for _setCaretOffset().
    var best = -1;
    for (var i = 0; i < this.blocks.length; i++) {
      if (this.blocks[i].start <= offset) best = i;
      else break;
    }
    return best >= 0 ? best : 0;
  };

  // Layout of a block: its kind plus its source with trailing whitespace
  // removed. Byte positions are deliberately excluded - typing shifts the span
  // of the edited block and of every block after it.
  Editor.prototype._layoutSignature = function (blocks) {
    var parts = [];
    for (var i = 0; i < blocks.length; i++) {
      parts.push(blocks[i].kind + '\u0001' + String(blocks[i].source || '').replace(/[ \t\r\n]+$/, ''));
    }
    return parts.join('\u0003');
  };

  // True when the new block list is the same shape as the old one and only the
  // edited block's own content changed - the case where the DOM can stay as it
  // is and the model is simply kept in sync.
  //
  // Repainting the whole document on every keystroke reset the caret, made the
  // view flicker and broke IME composition, so this is the fast path; keeping
  // stale elements while the spans have moved made the next keystroke splice
  // mismatched offsets and delete the document, so anything else rebuilds.
  Editor.prototype._canUpdateInPlace = function (previous, next, editedIndex) {
    if (previous.length !== next.length) return false;
    if (!next[editedIndex] || !previous[editedIndex]) return false;
    // The edited block may grow or shrink, but its start and kind must not move:
    // a newline inside it (Enter) or a marker that changes its kind ("#", "-",
    // "  ") reflows the document.
    if (next[editedIndex].start !== previous[editedIndex].start) return false;
    if (next[editedIndex].kind !== previous[editedIndex].kind) return false;
    for (var i = 0; i < next.length; i++) {
      if (i === editedIndex) continue;
      if (this._layoutSignature([previous[i]]) !== this._layoutSignature([next[i]])) return false;
    }
    return true;
  };

  // Caret position (in the *new* block text) for an edit that was just typed
  // into the DOM: simply where the caret is in the element, read with the same
  // rules the element's text is read with.
  //
  // This used to need a correction: the measurement was clamped to `block.source`
  // (the text from *before* the keystroke) while the element already held the
  // character the browser had just inserted, so a caret at the end came back one
  // short and the next character was inserted before the last one (typing
  // "```cpp" produced "``cpp`"). _domOffsetInBlock no longer clamps, so there is
  // nothing left to compensate.
  Editor.prototype._localCaretForEdit = function (blockIndex, node, offset) {
    return this._domOffsetInBlock(blockIndex, node, offset);
  };

  // Reads the raw text out of a block element, turning the DOM's line breaks back
  // into '\n'.
  //
  // This is the whole trick behind "the caret line shows source": the browser
  // does NOT store a newline as a '\n' text node. Pressing Enter in a
  // contenteditable produces <div> elements (Chrome: "AA<div>BB</div>", whose
  // textContent is "AABB"), an empty line becomes "<div><br></div>", and
  // Shift+Enter produces a bare <br>. Only understanding <br> meant every
  // newline the writer typed was silently dropped, so the model never changed:
  // that is why pasting a document rendered but typing a line break did not.
  Editor.prototype._rawTextOfElement = function (element) {
    if (!element) return '';
    // A code box keeps its code in a text box, not in text nodes.
    if (element.classList && element.classList.contains('md-code-box')) {
      return this._codeBoxRaw(element);
    }
    var out = '';
    var blockLevel = /^(DIV|P|LI|UL|OL|BLOCKQUOTE|PRE|TABLE|TR|H[1-6]|SECTION|ARTICLE)$/;
    (function walk(node) {
      for (var child = node.firstChild; child; child = child.nextSibling) {
        if (child.nodeType === 3) {
          out += child.nodeValue;
          continue;
        }
        if (child.nodeType !== 1) continue;
        if (child.nodeName === 'BR') {
          if (out.charAt(out.length - 1) !== '\n') out += '\n';
          continue;
        }
        // The line box the active block's own terminator is rendered as (see
        // _fillSource): exactly the one '\n' it stands for, plus whatever the
        // writer has typed on that line since.
        if (child.getAttribute && child.getAttribute('data-md-term')) {
          if (out.charAt(out.length - 1) !== '\n') out += '\n';
          walk(child);
          continue;
        }
        if (!blockLevel.test(child.nodeName)) {
          walk(child);
          continue;
        }
        // A block-level child begins a new line. One break is enough, even when
        // these nest: each opens a line and the parent's break closes it. A block
        // that held nothing ("<div><br></div>", a blank line) also contributes the
        // empty line itself.
        if (out.length > 0 && out.charAt(out.length - 1) !== '\n') out += '\n';
        var emptyBlock = child.textContent === '';
        walk(child);
        if (emptyBlock) {
          if (out.charAt(out.length - 1) !== '\n') out += '\n';
          out += '\n';
        }
      }
    })(element);
    // The text ends with a newline only when the DOM really ends with one (the
    // last block entry opens a line that nothing follows).
    return out;
  };

  // Puts the element of the block being edited back into the shape _fillSource()
  // builds, with the caret where the model says it is.
  //
  // Everything the writer does inside a block is read back as the element's whole
  // raw text and written over the block's span, and the caret has to sit in a
  // position the engine will really type at. Two things drift on their own: text
  // typed on the line the block's terminator opens lands *inside* that line box,
  // and a line break the engine inserts by itself (execCommand('insertLineBreak'),
  // a drop) arrives as a plain '\n' in the text. Left alone, the mismatch surfaced
  // on the next keystroke, which spliced the two texts together a newline short:
  // typing a second character on a new line ate the blank line that separated the
  // two paragraphs, merging them.
  Editor.prototype._canonicalizeBlockElement = function (index, caretOffset) {
    var element = this.blockElements[index];
    var block = this.blocks[index];
    if (!element || !block || this.activeIndex !== index) return;
    if (element.getAttribute('contenteditable') === 'false') return;
    var source = block.source || '';
    var terminated = source.length > 0 && source.charAt(source.length - 1) === '\n';
    var last = element.lastChild;
    var lineBox = !!last && last.nodeType === 1 && last.getAttribute &&
      last.getAttribute('data-md-term');
    if ((terminated ? lineBox : !lineBox) && this._rawTextOfElement(element) === source) return;
    this._fillSource(element, block);
    if (typeof caretOffset === 'number') this._setCaretOffset(caretOffset, false);
  };

  Editor.prototype._activateBlock = function (index) {
    if (this.activeIndex === index) return;
    // The "show this code block's source" choice is about the block the writer was
    // in, not about code blocks in general.
    this._codeSourceView = false;
    var previous = this.activeIndex;
    this.activeIndex = index;
    if (previous >= 0 && previous < this.blocks.length && previous !== index) {
      this._repaintBlock(previous);
    }
    var element = this.blockElements[index];
    if (!element) return;
    this._repaintBlock(index);
    if (!this.readOnly && !this.sourceMode) {
      try { element.focus({ preventScroll: true }); } catch (error) { element.focus(); }
    }
  };

  /* ------------------------------------------------------------- block edits */

  // Enter is handled here, not by the browser.
  //
  // The active block is an editable element *inside* the editable root, so
  // Chromium treats Enter as "insertParagraph" and - instead of putting the new
  // line inside the block being edited - it splits the block into sibling
  // elements and gives the new line a *copy* of the element, attribute
  // `data-md-block` included:
  //
  //     edited block being:  <div class md-block md-active md-source data-md-block=0>para one\n</div>
  //     after Enter:         <div ... data-md-block=0>para one\n</div>
  //                          <div ... data-md-block=0><br></div>      <- the caret moved here
  //
  // Only the first element is in `blockElements`, so the editor went on reading
  // the *original* text: `raw` still equalled `block.source`, the line break never
  // reached the document, and every character typed on the new line was read off
  // the copy and silently dropped - while it stayed visible on screen until the
  // next re-render rebuilt the root and threw the copy (and the writer's text)
  // away. A split in the middle of a paragraph was worse: the half that ended up
  // in the copy was lost and the paragraph break after the block was eaten.
  //
  // `beforeinput` is the point to stop it: it is cancelable, it is fired for both
  // the Enter key and Shift+Enter, and cancelling it stops the engine from
  // touching the DOM at all. The edit is then applied to the model, where it is
  // deterministic: exactly one '\n' lands at the caret of the buffer the Agent
  // reads, and re-parsing renders the new line (heading, list item, fence) at
  // once.
  Editor.prototype._handleBeforeInput = function (event) {
    if (this.readOnly || this.sourceMode || this._composing) return;
    // Events from a code box's text box are not contenteditable edits: the text
    // box handles them itself (see _onCodeInput).
    var target = event.target;
    if (target && target.classList && target.classList.contains('md-code-input')) return;
    var type = event.inputType;
    // Backspace-like deletions on the empty line a code box leaves for the next
    // paragraph are stopped here as well as in the keydown handler: on an engine that
    // delivers the edit only as beforeinput, the browser's own "join with the line
    // above" is what put the caret back inside the code.
    if (type && type.indexOf('delete') === 0 && target && target.classList &&
        target.classList.contains('md-code-after')) {
      if (this._handleCodeAfterBackspace(this._nodeBlockIndex(target), target, event)) return;
    }
    // An edit over a selection that spans more than one block cannot be performed by
    // the engine: every block except the active one is contenteditable=false, so the
    // browser would delete that rendered content instead of editing the document.
    // Apply it to the model instead - which is also what makes Ctrl+A followed by
    // typing (or pasting) safe.
    if (!this._selectionIsEditable()) {
      var replacement = null;
      if ((type === 'insertText' || type === 'insertReplacementText') &&
          typeof event.data === 'string') {
        replacement = event.data;
      } else if ((type === 'insertFromPaste' || type === 'insertFromDrop') && event.dataTransfer) {
        replacement = event.dataTransfer.getData('text/plain');
      } else if (type.indexOf('delete') === 0) {
        replacement = '';
      }
      if (replacement !== null) {
        if (event.cancelable) event.preventDefault();
        this._replaceSelection(replacement);
        return;
      }
    }
    if (type !== 'insertParagraph' && type !== 'insertLineBreak') return;
    // Not cancelable (an engine that does not let this edit be intercepted):
    // fall through to _handleInput, which folds the split back into the model.
    if (!event.cancelable) return;
    event.preventDefault();
    this.insertLineBreak();
  };

  // True when the pending edit's selection lives inside the block that is actually
  // editable. Anything else covers blocks rendered as non-editable content.
  Editor.prototype._selectionIsEditable = function () {
    var selection = global.getSelection();
    if (!selection || !selection.anchorNode) return true;
    var element = this.blockElements[this.activeIndex];
    if (!element) return false;
    var inside = function (node) {
      return !!node && (node === element || (element.contains && element.contains(node)));
    };
    return inside(selection.anchorNode) && inside(selection.focusNode);
  };

  // True when a selection's node belongs to this editor. `contains()` is false for
  // the node itself, and a whole-document selection is anchored *on* the root, so
  // checking only contains() silently ignored Ctrl+A.
  Editor.prototype._selectionInEditor = function (selection) {
    if (!selection || !selection.anchorNode) return false;
    var node = selection.anchorNode;
    return node === this.root || this.root.contains(node);
  };

  // Copies the selected range itself. Chrome's own copy produces an empty clipboard
  // when the selection reaches the blocks the engine renders as contenteditable=false
  // - the writer sees a highlight and pastes nothing.
  Editor.prototype._handleCopy = function (event) {
    if (!event || !event.clipboardData) return;
    var selection = global.getSelection();
    if (!selection || selection.isCollapsed || !selection.rangeCount) return;
    if (!this._selectionInEditor(selection)) return;
    var text = selection.getRangeAt(0).toString();
    if (!text) return;
    event.clipboardData.setData('text/plain', text);
    event.preventDefault();
  };

  // Applies an edit to the model directly, for selections the engine cannot edit
  // itself. Keeps the exact range, so nothing is lost to a DOM deletion.
  Editor.prototype._replaceSelection = function (text) {
    var offsets = this.selectionOffsets();
    var begin = Math.min(offsets.anchor, offsets.head);
    var end = Math.max(offsets.anchor, offsets.head);
    var inserted = text || '';
    this._applyFormatEdit(this.text.slice(0, begin) + inserted + this.text.slice(end),
                          begin + inserted.length);
  };

  // Selects the whole document. The browser's own Ctrl+A stops at the end of the
  // block being edited (every other block is contenteditable=false), which is why
  // "select everything and copy" did not work in WYSIWYG mode.
  Editor.prototype.selectAll = function () {
    if (this.sourceMode) {
      if (this.textarea.focus) this.textarea.focus();
      this.textarea.select();
      this._emitStatus();
      this._emitCaret(true);
      return;
    }
    // Inside a code box the writer means that box's code, not the whole document.
    var input = this._activeCodeInput();
    if (input) {
      if (input.focus) input.focus();
      if (input.select) input.select();
      this._onCodeCaret();
      return;
    }
    var selection = global.getSelection();
    if (!selection) return;
    // From the first text node to the last one, not "the root's contents": a range
    // anchored on the root itself is not an ordinary text selection, and Chrome
    // copies it as nothing (and reports an empty Selection.toString()).
    var walker = document.createTreeWalker(this.root, NodeFilter.SHOW_TEXT, null);
    var first = null;
    var last = null;
    var node = walker.nextNode();
    while (node) {
      if (!first) first = node;
      last = node;
      node = walker.nextNode();
    }
    if (!first) return;
    var range = document.createRange();
    range.setStart(first, 0);
    range.setEnd(last, last.nodeValue ? last.nodeValue.length : 0);
    selection.removeAllRanges();
    selection.addRange(range);
    this._emitStatus();
    this._emitCaret(true);
  };

  // Inserts one line break at the caret, in the model, and re-renders.
  Editor.prototype.insertLineBreak = function () {
    if (this.readOnly || this.sourceMode) return;
    // A click can leave the caret in a block that is still rendered; its DOM
    // offset is only an approximation of a source offset, so switch it to source
    // view first (that is what the next keystroke would do anyway).
    this._activateCaretBlock();
    var offset = this.caretOffset();
    // Enter at the end of a list item carries the list on (and Enter on an item
    // that has no text yet ends it) - otherwise a writer who never touches the
    // marker would have to hand-write the next "- ".
    var atLineEnd = offset >= this.text.length || this.text.charAt(offset) === '\n';
    var continuation = atLineEnd ? this._listContinuation(offset) : null;
    if (continuation && continuation.removeTo !== undefined) {
      this.text = this.text.slice(0, continuation.removeFrom) +
        this.text.slice(continuation.removeTo);
      this._commitAndRerender(continuation.removeFrom);
      this._notifyChange('typing');
      return;
    }
    this.text = this.text.slice(0, offset) + '\n' + this.text.slice(offset);
    var caret = offset + 1;
    // "```cpp" + Enter opens a code box: close the fence straight away. A fence
    // typed in the middle of a document otherwise swallows everything below it -
    // and the writer never sees the fence again to close it by hand.
    var next = this._ensureBlocksFor(this.text);
    var index = this._blockIndexForOffset(offset);
    var block = next[index];
    if (block && block.kind === 'code_block' && !block.indented && !block.closed &&
        caret === block.contentStart) {
      var open = this.text.slice(block.start, block.contentStart);
      var fence = /^[ \t]*(`{3,}|~{3,})/.exec(open);
      var close = (fence ? fence[1] : '```') + '\n';
      this.text = this.text.slice(0, offset + 1) + close + this.text.slice(offset + 1);
      caret = offset + 1;
    } else if (continuation) {
      this.text = this.text.slice(0, caret) + continuation.insert + this.text.slice(caret);
      caret += continuation.insert.length;
    }
    this._commitAndRerender(caret);
    this._notifyChange('typing');
  };

  // What Enter should do to a list item: continue it, end it, or nothing.
  Editor.prototype._listContinuation = function (offset) {
    var lineStart = this.text.lastIndexOf('\n', Math.max(0, offset - 1)) + 1;
    var line = this.text.slice(lineStart, offset);
    var item = /^([ \t]*)([-*+]|\d+\.)[ \t]+(\[[ xX]\][ \t]+)?(.*)$/.exec(line);
    if (!item || line.trim() === '') return null;
    if (item[4].trim() === '') {
      // An item with no text yet: the writer pressed Enter twice, so the list ends
      // and the marker goes away instead of starting another empty item.
      return { removeFrom: lineStart, removeTo: offset };
    }
    var bullet = item[2];
    var nextBullet = /^\d+\.$/.test(bullet) ? (parseInt(bullet, 10) + 1) + '.' : bullet;
    return { insert: item[1] + nextBullet + ' ' + (item[3] ? '[ ] ' : '') };
  };

  // Makes the block the caret actually sits in the active one, keeping the caret
  // where the writer put it.
  Editor.prototype._activateCaretBlock = function () {
    if (this.sourceMode) return;
    var selection = global.getSelection();
    if (!selection || !selection.anchorNode) return;
    if (!this.root.contains(selection.anchorNode)) return;
    var index = this._nodeBlockIndex(selection.anchorNode);
    if (index < 0 || index === this.activeIndex || !this.blocks[index]) return;
    // Measure before activating: repainting the element as source destroys the
    // DOM range behind the reported selection.
    var offset = this.caretOffset();
    this._activateBlock(index);
    this._setCaretOffset(offset, false);
  };

  /* --------------------------------------------------------- toolbar editing */

  // The Markdown a "block" button writes. Nobody using these buttons is expected
  // to know the syntax, so the markers live here and never in the UI.
  var BLOCK_MARKERS = {
    paragraph: '',
    heading1: '# ',
    heading2: '## ',
    heading3: '### ',
    bullet: '- ',
    number: '1. ',
    task: '- [ ] ',
    quote: '> ',
    divider: '---',
  };

  // Inline wrappers that just put a marker on both sides of the selection.
  var INLINE_MARKERS = { bold: '**', italic: '*', inlineCode: '`' };

  // Applies a model-level edit exactly like a keystroke: re-render, restore the
  // caret/selection, then report (revision, text) to the host after the usual
  // debounce, so the host records one ordinary user edit.
  Editor.prototype._applyFormatEdit = function (nextText, caretBegin, caretEnd) {
    this.text = nextText;
    if (this.sourceMode) {
      this.blocks = this._ensureBlocksFor(this.text);
      this.textarea.value = this.text;
      var end = typeof caretEnd === 'number' ? caretEnd : caretBegin;
      this.textarea.setSelectionRange(caretBegin, end);
    } else {
      this._commitAndRerender(caretBegin);
      if (typeof caretEnd === 'number' && caretEnd !== caretBegin) {
        this._selectOffsets(caretBegin, caretEnd);
      }
    }
    this._notifyChange('toolbar');
  };

  // Selects a range of the *model*: after a button wrapped the selected word, the
  // word stays selected instead of the caret jumping to the end of the document.
  Editor.prototype._selectOffsets = function (begin, end) {
    begin = Math.max(0, Math.min(begin, this.text.length));
    end = Math.max(begin, Math.min(end, this.text.length));
    if (this.sourceMode) {
      this.textarea.setSelectionRange(begin, end);
      return;
    }
    var index = this._blockIndexForOffset(begin);
    if (index < 0) return;
    this._setCaretOffset(begin, false);
    var input = this._activeCodeInput();
    if (input) {
      // Inside a code box the caret lives in the text box, not in a text node.
      var base = this.blocks[index] ? this.blocks[index].contentStart : 0;
      try {
        input.setSelectionRange(Math.max(0, begin - base), Math.max(0, end - base));
      } catch (error) {
        /* a box that vanished between the two steps: the caret is good enough */
      }
      return;
    }
    if (this._blockIndexForOffset(end) !== index) return; // spans blocks: caret only
    var element = this.blockElements[index];
    var block = this.blocks[index];
    if (!element || !block) return;
    var head = this._rangeAtRawOffset(element, begin - block.start);
    var tail = this._rangeAtRawOffset(element, end - block.start);
    var selection = global.getSelection();
    if (!selection) return;
    var range = document.createRange();
    range.setStart(head.startContainer, head.startOffset);
    range.setEnd(tail.startContainer, tail.startOffset);
    this._suspendSelection = true;
    selection.removeAllRanges();
    selection.addRange(range);
    this._suspendSelection = false;
  };

  // Inserts a block at the caret. An empty line becomes that block; a line with
  // text keeps its text and gets the new block on the line below - "press H2 and
  // type the heading" is what someone who does not write Markdown expects, and
  // nothing already written is ever rewritten.
  Editor.prototype.insertBlock = function (kind) {
    if (this.readOnly) return;
    this._activateCaretBlock();
    var offset = this.caretOffset();
    var lineStart = this.text.lastIndexOf('\n', Math.max(0, offset - 1)) + 1;
    var lineEnd = this.text.indexOf('\n', offset);
    if (lineEnd < 0) lineEnd = this.text.length;
    var blank = this.text.slice(lineStart, lineEnd).trim() === '';

    var at = blank ? lineStart : lineEnd;
    var lead = blank ? '' : '\n';
    var insert = '';
    var caret = 0;
    var caretEnd = null;

    if (kind === 'code') {
      // A fence that is closed right away: an unterminated one would swallow the
      // rest of the document.
      insert = lead + '```\n\n```\n';
      caret = at + lead.length + 4;
    } else if (kind === 'table') {
      var table = this._tableSkeleton();
      insert = lead + table;
      // Leave the first header cell selected so the first keystroke replaces the
      // placeholder instead of ending up in front of it.
      caret = at + lead.length + 2;
      caretEnd = caret + String(t('format.tableHeader', ['1'])).length;
    } else {
      var marker = BLOCK_MARKERS[kind];
      if (marker === undefined) return;
      insert = lead + marker;
      caret = at + insert.length;
    }
    this._applyFormatEdit(this.text.slice(0, at) + insert + this.text.slice(at), caret, caretEnd);
  };

  // Wraps the selection (or the word the caret is in) in an inline marker.
  Editor.prototype.wrapSelection = function (kind) {
    if (this.readOnly) return;
    var offsets = this.selectionOffsets();
    var begin = Math.min(offsets.anchor, offsets.head);
    var end = Math.max(offsets.anchor, offsets.head);
    var selected = this.text.slice(begin, end);
    var open = INLINE_MARKERS[kind];
    var close = open;
    var body = selected;
    var caretBegin = begin;
    var caretEnd = begin;

    if (kind === 'link' || kind === 'image') {
      var isLink = kind === 'link';
      open = isLink ? '[' : '![';
      close = '](' + (isLink ? t('format.linkUrl') : t('format.imagePath')) + ')';
      body = selected || (isLink ? t('format.linkText') : t('format.imageAlt'));
      caretBegin = begin + open.length;
      caretEnd = caretBegin + body.length;
      if (selected) caretBegin = caretEnd = begin + open.length + body.length + close.length;
    } else if (kind === 'formula') {
      open = '$';
      close = '$';
      body = selected || t('format.formulaText');
      caretBegin = begin + 1;
      caretEnd = caretBegin + body.length;
      if (selected) caretBegin = caretEnd = begin + 1 + body.length + 1;
    } else if (open !== undefined) {
      if (!selected) {
        // Nothing selected: drop the empty markers in and sit between them, so the
        // next thing typed is bold (or italic, or code).
        this._applyFormatEdit(this.text.slice(0, begin) + open + close + this.text.slice(end),
                              begin + open.length);
        return;
      }
      caretBegin = begin + open.length;
      caretEnd = caretBegin + body.length;
    } else {
      return;
    }
    this._applyFormatEdit(
      this.text.slice(0, begin) + open + body + close + this.text.slice(end), caretBegin, caretEnd);
  };

  // A fresh three-column table with readable placeholder headers.
  Editor.prototype._tableSkeleton = function () {
    var header = [];
    for (var i = 1; i <= 3; i++) header.push(t('format.tableHeader', [String(i)]));
    return '| ' + header.join(' | ') + ' |\n| --- | --- | --- |\n|  |  |  |\n';
  };

  // Splits a table line into its cells (the outer pipes are optional).
  Editor.prototype._tableCells = function (line) {
    var text = String(line).trim();
    if (text.charAt(0) === '|') text = text.slice(1);
    if (text.charAt(text.length - 1) === '|') text = text.slice(0, -1);
    return text.split('|');
  };

  // The table block the caret sits in, or null when it is not in one.
  Editor.prototype._tableAtCaret = function () {
    var index = this._blockIndexForOffset(this.caretOffset());
    var block = this.blocks[index];
    if (!block || block.kind !== 'table') return null;
    return { index: index, block: block };
  };

  // Rewrites the table block with `lines` and puts the caret at `caretOffset`.
  Editor.prototype._replaceTableLines = function (table, lines, caretOffset) {
    var hadNewline = table.block.source.charAt(table.block.source.length - 1) === '\n';
    var source = lines.join('\n') + (hadNewline ? '\n' : '');
    this._applyFormatEdit(this.text.slice(0, table.block.start) + source +
                            this.text.slice(table.block.end),
                          caretOffset);
  };

  Editor.prototype.tableAddRow = function () {
    if (this.readOnly) return;
    var table = this._tableAtCaret();
    if (!table) return;
    var lines = table.block.source.replace(/\n$/, '').split('\n');
    var caret = this.caretOffset();
    var caretLine = this.text.slice(table.block.start, caret).split('\n').length - 1;
    if (caretLine < 0) caretLine = 0;
    if (caretLine > lines.length - 1) caretLine = lines.length - 1;
    var cells = this._tableCells(lines[0]).length;
    var row = '|' + new Array(cells + 1).join('  |');
    lines.splice(caretLine + 1, 0, row);
    var rowStart = table.block.start + lines.slice(0, caretLine + 1).join('\n').length + 1;
    this._replaceTableLines(table, lines, rowStart + 2);
  };

  Editor.prototype.tableAddColumn = function () {
    if (this.readOnly) return;
    var table = this._tableAtCaret();
    if (!table) return;
    var lines = table.block.source.replace(/\n$/, '').split('\n');
    var self = this;
    lines = lines.map(function (line) {
      var cells = self._tableCells(line);
      var isDash = function (cell) { return /^\s*:?-+:?\s*$/.test(cell); };
      // Only the alignment row grows with dashes; an empty body row must get an
      // empty cell, not "---".
      var separator = cells.some(isDash) &&
        cells.every(function (cell) { return isDash(cell) || cell.trim() === ''; });
      return line.replace(/\s*$/, '') + (separator ? ' --- |' : '  |');
    });
    this._replaceTableLines(table, lines, this.caretOffset());
  };

  // The elements the browser split a block into, or null when the block still
  // lives in the one element the editor tracks. Chromium copies the split half
  // (attributes and all), so the pieces are recognised by `data-md-block`.
  Editor.prototype._splitBlockPieces = function (index) {
    var element = this.blockElements[index];
    if (!element) return null;
    var pieces = [element];
    for (var next = element.nextSibling; next; next = next.nextSibling) {
      if (next.nodeType !== 1 || !next.getAttribute) break;
      if (next.getAttribute('data-md-block') !== String(index)) break;
      pieces.push(next);
    }
    return pieces.length > 1 ? pieces : null;
  };

  // Re-parses and rebuilds, keeping the caret at `offset`.
  Editor.prototype._commitAndRerender = function (offset) {
    // _ensureBlocksFor, not Render.parseBlocks: a whitespace-only document has no
    // Markdown block but still needs one for the caret to live in.
    this.blocks = this._ensureBlocksFor(this.text);
    // Rendering needs to know where the caret will be (a code box depends on it),
    // and the engine's own selection is stale while the DOM is being replaced.
    if (typeof offset === 'number') this._caretHint = Math.max(0, Math.min(offset, this.text.length));
    var active = this.activeIndex;
    this.activeIndex = -1;
    var fragment = document.createDocumentFragment();
    var self = this;
    this.blockElements = [];
    if (!this.blocks.length) {
      this._renderAll();
      return;
    }
    this.blocks.forEach(function (block, index) {
      var element = self._createBlockElement(block, index);
      self.blockElements.push(element);
      fragment.appendChild(element);
    });
    this.root.innerHTML = '';
    this.root.appendChild(fragment);
    Render.renderMermaid(this.root);
    void active;
    this._setCaretOffset(offset, false);
  };

  // Markdown only recognises an ATX heading at the start of a block, and only
  // when the #s are followed by a space. Writers break both rules constantly:
  //
  //     previous line
  //     ###Title          -> one paragraph, no heading, no space after the #s
  //
  // Neither renders, which reads as "typing a heading does nothing". So when a
  // run of #s starts a line of the block being edited:
  //   * a blank line is inserted before it if it directly follows text;
  //   * a space is inserted after the #s when there is none.
  // The heading then has its own block and renders immediately, while the file
  // stays standard Markdown that every other tool parses the same way.
  //
  // This runs at most once per input event, on the edited block's text, and only
  // looks at the block's last line. Running it over the whole block on every
  // keystroke treated the block's trailing newline as a separate line, so each
  // typed character inserted another blank line, the block could no longer be
  // updated in place, and the caret interleaved new text with what followed it.
  // Markdown only recognises an ATX heading at the start of a block, and only
  // when the #s are followed by a space. Writers break both rules constantly:
  //
  //     previous line
  //     ###Title
  //
  // is a single paragraph, so the heading never renders - which reads as "typing
  // a heading does nothing". This repairs a freshly typed heading on the line
  // containing the caret:
  //   * a blank line is inserted before it when it directly follows text;
  //   * a space is inserted after the #s when something follows them.
  // The heading then has its own block and renders immediately, while the file
  // stays standard Markdown that every other tool parses the same way.
  //
  // `raw` is the edited block's new text and `previous` the text it replaced, so
  // a block that already began with a heading is left alone. Work is done on the
  // raw text (not on `this.text`) because a block's own trailing newline is its
  // terminator, not a line the writer typed.
  Editor.prototype._withHeadingFixes = function (raw, previous) {
    if (raw === null || raw.indexOf('#') === -1) return raw;
    var lineBreak = raw.lastIndexOf('\n');
    var line = lineBreak === -1 ? raw : raw.slice(lineBreak + 1);
    // Still typing the marker, or already correct: nothing to do.
    var marker = /^( {0,3}#{1,6})([^ \t#\s]?)/.exec(line);
    if (!marker) return raw;
    if (!marker[2] && line.length === marker[1].length) {
      // A bare "#": separate it from the paragraph, but leave the space alone
      // until there is content (adding it now would put the #s typed next
      // behind it: "### T" became "# ## T").
      if (lineBreak > 0 && raw.charAt(lineBreak - 1) !== '\n') {
        return raw.slice(0, lineBreak) + '\n\n' + line;
      }
      return raw;
    }

    var text = raw;
    var hashEnd = lineBreak + 1 + marker[1].length;
    if (marker[2] && text.charAt(hashEnd) !== ' ') {
      text = text.slice(0, hashEnd) + ' ' + text.slice(hashEnd);
      hashEnd += 1;
    }
    // The blank line, when this heading directly follows text. The single
    // newline in front of it is the line break the writer typed; a second one
    // means they already left a blank line.
    var lineStart = hashEnd - marker[1].length;
    if (lineStart > 0 && text.charAt(lineStart - 1) === '\n' &&
        (lineStart < 2 || text.charAt(lineStart - 2) !== '\n')) {
      text = text.slice(0, lineStart) + '\n' + text.slice(lineStart);
    }
    void previous;
    return text;
  };

  Editor.prototype._handleInput = function (event) {
    if (this.readOnly) return;
    if (this._composing) return; // wait for compositionend
    if (this.sourceMode) {
      this.text = this.textarea.value.replace(/\r\n?/g, '\n');
      // The rendered view is built from `blocks`, so they have to be re-parsed here:
      // leaving them describing the text as it was before the edit made the WYSIWYG
      // view show the old document the moment the writer switched back to it.
      this.blocks = this._ensureBlocksFor(this.text);
      this._notifyChange('typing');
      return;
    }
    // A code box's text box edits are applied by _onCodeInput, which has the text
    // box's value; this handler must not read the same edit a second time out of
    // the DOM.
    var target = event && event.target;
    if (target && target.classList && target.classList.contains('md-code-input')) return;
    var selection = global.getSelection();
    var index = selection && selection.anchorNode ? this._nodeBlockIndex(selection.anchorNode) : -1;
    // An index is only usable if the block list still has that entry. After the
    // document is rebuilt (emptying it, or a structural edit) the reported node
    // can belong to an element that is no longer part of the block list, and
    // this.blocks[index] is then undefined - reading .source off it threw a
    // TypeError mid-edit, which is what scrambled the caret and dropped input.
    if (index < 0 || !this.blocks[index]) {
      // No addressable block: the document is empty and the caret is in the
      // empty-state element, or the selection points at a node a rebuild has
      // already replaced. Take the text from the editor surface - but never
      // overwrite a non-empty model with an empty surface, which would delete a
      // document whose DOM had lost its blocks.
      var emptyEl = this.root.querySelector('.md-empty');
      var surface = emptyEl ? this._rawTextOfElement(emptyEl) : this.root.textContent;
      if (surface) {
        this.text = surface.replace(/\r\n?/g, '\n');
        this.blocks = this._ensureBlocksFor(this.text);
        // Put the caret back at the end of what was just adopted. Leaving it
        // where the browser had it let the browser keep inserting at its own
        // offset, so an untouched empty surface accumulated whitespace that was
        // then read back as part of the document.
        this._commitAndRerender(this.text.length);
      }
      this._notifyChange('typing');
      return;
    }

    // Read the caret and the edit *before* touching the DOM: activating a block
    // repaints it, which destroys the range the browser reported and used to
    // make the keystroke look like a no-op.
    var block = this.blocks[index];
    var element = this.blockElements[index];
    var anchorNode = selection.anchorNode;
    var anchorOffset = selection.anchorOffset;

    // Enter is applied in _handleKeyDown, but the browser can split the active
    // block into sibling copies by other routes (a synthesized input event, a
    // drag & drop, an engine that reports the break differently). Reading the
    // tracked element alone would then read a fragment of what the writer sees -
    // and drop the rest - so fold every piece back into the model first.
    var pieces = this._splitBlockPieces(index);
    if (pieces) {
      var joined = '';
      var joinedCaret = -1;
      for (var pieceIndex = 0; pieceIndex < pieces.length; pieceIndex++) {
        // One break between the pieces, and only when the piece before it does not
        // already end with one: each piece carries the block's own terminator, and
        // counting it twice would add a blank line every time the engine split.
        if (pieceIndex > 0 && joined.charAt(joined.length - 1) !== '\n') joined += '\n';
        if (joinedCaret < 0 &&
            (pieces[pieceIndex] === anchorNode || pieces[pieceIndex].contains(anchorNode))) {
          joinedCaret = joined.length + this._rawOffsetInElement(pieces[pieceIndex], anchorNode, anchorOffset);
        }
        joined += this._rawTextOfElement(pieces[pieceIndex]);
      }
      if (joinedCaret < 0) joinedCaret = joined.length;
      this.text = this.text.slice(0, block.start) + joined + this.text.slice(block.end);
      this._commitAndRerender(block.start + joinedCaret);
      this._notifyChange('typing');
      return;
    }

    var alreadyActive = this.activeIndex === index;
    var raw = alreadyActive ? this._rawTextOfElement(element) : null;
    var localCaret = this._localCaretForEdit(index, anchorNode, anchorOffset);

    if (!alreadyActive) {
      // The user typed into a block that was still rendered: take the edit from
      // the DOM rather than discarding it, then switch it to source view.
      raw = this._rawTextOfElement(element);
      localCaret = this._localCaretForEdit(index, anchorNode, anchorOffset);
      this._activateBlock(index);
    }

    if (raw !== null && raw !== block.source) {
      // A heading typed straight under a paragraph needs the blank line and the
      // space after the #s that Markdown requires, or it never renders.
      var adjusted = block.kind === 'paragraph'
        ? this._withHeadingFixes(raw, block.source)
        : raw;
      var forcedRebuild = false;
      if (adjusted !== raw) {
        localCaret += adjusted.length - raw.length;
        raw = adjusted;
        forcedRebuild = true;
      }
      this.text = this.text.slice(0, block.start) + raw + this.text.slice(block.end);
      var nextBlocks = this._ensureBlocksFor(this.text);
      // Rebuild the DOM only when the block *layout* moved. Repainting the whole
      // document on every keystroke reset the caret, made the view flicker and
      // broke IME composition; but keeping the old elements while the spans have
      // shifted made the next keystroke splice together mismatched offsets and
      // delete the rest of the document. Text this method rewrote always falls
      // through to a rebuild: the caret was measured against the old text and the
      // spans have moved under it.
      if (forcedRebuild || !this._canUpdateInPlace(this.blocks, nextBlocks, index)) {
        this.blocks = nextBlocks;
        this._commitAndRerender(block.start + localCaret);
      } else {
        this.blocks = nextBlocks;
        // The element kept the writer's keystroke. Put its structure back the way
        // _fillSource builds it, so the next keystroke reads exactly the block's
        // source back out of it.
        this._canonicalizeBlockElement(index, block.start + localCaret);
      }
    }
    this._notifyChange('typing');
  };

  Editor.prototype._handleCompositionStart = function () {
    this._composing = true;
  };

  Editor.prototype._handleCompositionEnd = function () {
    this._composing = false;
    this._handleInput();
  };

  Editor.prototype._handleSelectionChange = function () {
    if (this._suspendSelection) return;
    var self = this;
    if (this._caretTimer) return;
    this._caretTimer = setTimeout(function () {
      self._caretTimer = null;
      self._afterSelectionChange();
    }, 60);
  };

  // True while the selection belongs to a code box's text box: that selection is
  // the text box's own, not the document's, and activating a block would steal the
  // focus out of it (the caret then cannot be placed or dragged with the mouse).
  Editor.prototype._selectionInCodeInput = function () {
    var active = document.activeElement;
    return !!(active && active.classList && active.classList.contains('md-code-input'));
  };

  Editor.prototype._afterSelectionChange = function () {
    if (this.sourceMode) {
      this._emitStatus();
      this._emitCaret(true);
      return;
    }
    var selection = global.getSelection();
    if (!selection || !selection.anchorNode) return;
    if (!this._selectionInEditor(selection)) return;
    // A selection is never a request to edit. Activating the block under it repaints
    // that block and collapses the selection to a caret, and because this runs ~60 ms
    // after the selection appears it destroyed every drag: selecting text with the
    // mouse, Ctrl+A, and selecting inside a code box were all impossible. Activation
    // happens on mouseup (for a plain click) or on the next edit instead.
    if (this._pointerDown || !selection.isCollapsed || this._selectionInCodeInput()) {
      this._emitStatus();
      this._emitCaret(true);
      return;
    }
    var index = this._nodeBlockIndex(selection.anchorNode);
    if (index !== this.activeIndex && index >= 0) {
      // Capture the offset *before* activating: _activateBlock repaints the
      // element as raw source, which invalidates the DOM range behind the
      // reported selection. Reading it afterwards returned 0, so clicking in the
      // middle of a paragraph jumped the caret to the start of the block.
      var offset = this.caretOffset();
      this._activateBlock(index);
      this._setCaretOffset(offset, false);
    }
    this._emitStatus();
    this._emitCaret(true);
  };

  /* -------------------------------------------------------------- reporting */

  Editor.prototype._notifyChange = function (hint) {
    this.dirty = true;
    this._emitStatus();
    var self = this;
    if (this._inputTimer) clearTimeout(this._inputTimer);
    this._inputTimer = setTimeout(function () {
      self._inputTimer = null;
      self._emitCaret(false);
      if (self.onChange) self.onChange(self.text, self.revision, hint || 'typing');
    }, 220);
  };

  Editor.prototype._emitCaret = function (selectionOnly) {
    var offsets = this.selectionOffsets();
    if (this._lastReportedCaret &&
        this._lastReportedCaret.anchor === offsets.anchor &&
        this._lastReportedCaret.head === offsets.head &&
        this._lastReportedCaret.dirty === this.dirty) {
      return;
    }
    this._lastReportedCaret = { anchor: offsets.anchor, head: offsets.head, dirty: this.dirty };
    if (this.onCaret) this.onCaret(offsets.anchor, offsets.head, selectionOnly);
  };

  Editor.prototype._emitStatus = function () {
    if (!this.onStatus) return;
    var offsets = this.selectionOffsets();
    var head = Math.max(offsets.anchor, offsets.head);
    var before = this.text.slice(0, head);
    var lines = before.split('\n');
    var line = lines.length;
    var column = lines[lines.length - 1].length + 1;
    var words = (this.text.match(/[\p{L}\p{N}_]+/gu) || []).length;
    this.onStatus({
      caret: this.caretOffset(),
      line: line,
      column: column,
      selectionLength: Math.abs(offsets.head - offsets.anchor),
      characters: this.text.length,
      lines: this.text ? this.text.split('\n').length : 0,
      words: words,
      blocks: this.blocks.length,
      dirty: this.dirty,
    });
  };

  /* ------------------------------------------------------------- agent edits */

  // Replaces the model with text coming from the host (agent patch, rollback,
  // external reload) while preserving the caret when it still makes sense.
  Editor.prototype.applyHostText = function (text, options) {
    options = options || {};
    var wasDirty = this.dirty;
    var caret = this.caretOffset();
    var previousLength = this.text.length;
    this.setDocument(text, {
      revision: options.revision,
      readOnly: this.readOnly,
      dirty: typeof options.dirty === 'boolean' ? options.dirty : wasDirty,
      caret: options.keepCaret === false ? 0 : Math.min(caret, previousLength),
    });
    if (options.highlightChanges && options.highlightChanges.length) {
      this.highlightRanges(options.highlightChanges);
    }
    if (this.sourceMode) this.textarea.value = this.text;
  };

  // Briefly marks the blocks an agent just touched.
  Editor.prototype.highlightRanges = function (changes) {
    var self = this;
    var indexes = {};
    changes.forEach(function (change) {
      var index = self._blockIndexForOffset(change.byte_start);
      if (index >= 0) indexes[index] = true;
    });
    Object.keys(indexes).forEach(function (key) {
      var element = self.blockElements[parseInt(key, 10)];
      if (!element) return;
      element.classList.add('md-agent-changed');
      setTimeout(function () { element.classList.remove('md-agent-changed'); }, 1500);
    });
  };

  Editor.prototype.setReadOnly = function (readOnly) {
    this.readOnly = !!readOnly;
    this.root.setAttribute('contenteditable', this.readOnly ? 'false' : 'true');
    this.textarea.readOnly = this.readOnly;
    // An open code box has a text box of its own to lock as well.
    if (this.activeIndex >= 0) {
      var element = this.blockElements[this.activeIndex];
      var input = this._codeInputOf(element);
      if (input) input.readOnly = this.readOnly;
      if (element && element.lastChild && element.lastChild.className === 'md-code-after') {
        element.lastChild.setAttribute('contenteditable', this.readOnly ? 'false' : 'true');
      }
    }
  };

  Editor.prototype.focus = function () {
    if (this.sourceMode) this.textarea.focus();
    else this.root.focus();
  };

  Editor.prototype.resize = function () { /* layout is CSS-driven */ };

  Editor.prototype.shutdown = function () {
    this.root.removeEventListener('input', this._inputBound);
    this.root.removeEventListener('beforeinput', this._beforeInputBound);
    this.root.removeEventListener('keydown', this._boundaryKeyDownBound);
    this.root.removeEventListener('compositionstart', this._compositionStartBound);
    this.root.removeEventListener('compositionend', this._compositionEndBound);
    this.root.removeEventListener('mousedown', this._pointerDownBound);
    document.removeEventListener('mouseup', this._pointerUpBound);
    document.removeEventListener('copy', this._copyBound);
    this.textarea.removeEventListener('input', this._inputBound);
    document.removeEventListener('selectionchange', this._selectionBound);
  };

  // Runs the editing scenarios that used to be broken, using this page's own code
  // and the real engine it is running in, and returns a readable report.
  //
  // This exists because the editor front-end cannot be exercised from the C++
  // test suite: the paths that broke (how the browser represents a line break,
  // and where the caret lands after a re-render) only exist inside the browser.
  Editor.prototype.selfTest = function () {
    var editor = this;
    var report = [];
    var failures = 0;
    var saved = { text: editor.text, blocks: editor.blocks, active: editor.activeIndex };

    function record(label, ok, detail) {
      if (!ok) failures += 1;
      report.push((ok ? 'ok   ' : 'FAIL ') + label + (ok || !detail ? '' : '  -> ' + detail));
    }

    // What the engine does for a line break: this is the assumption the whole
    // editor rests on, so it is checked first.
    var probe = document.createElement('div');
    probe.innerHTML = 'AA<div>BB</div>';
    record('the engine reports a <div> line break as a newline',
      editor._rawTextOfElement(probe) === 'AA\nBB',
      JSON.stringify(editor._rawTextOfElement(probe)));
    probe.innerHTML = 'AA<div><br></div><div>BB</div>';
    record('the engine reports a blank line as two newlines',
      editor._rawTextOfElement(probe) === 'AA\n\nBB',
      JSON.stringify(editor._rawTextOfElement(probe)));

    // A heading typed after a paragraph must render immediately.
    try {
      editor.setDocument('some text\n', { revision: 'self-test' });
      editor._setCaretOffset(editor.text.length, false);
      var element = editor.blockElements[editor.activeIndex];
      element.focus();
      var range = document.createRange();
      range.selectNodeContents(element);
      range.collapse(false);
      var selection = global.getSelection();
      selection.removeAllRanges();
      selection.addRange(range);
      var line = document.createElement('div');
      line.appendChild(document.createTextNode('###Title'));
      element.appendChild(line);
      var caret = document.createRange();
      caret.setStart(line.firstChild, 8);
      caret.collapse(true);
      selection.removeAllRanges();
      selection.addRange(caret);
      element.dispatchEvent(new Event('input', { bubbles: true }));
      record('a heading typed on a new line becomes a heading block',
        editor.blocks.some(function (b) { return b.kind === 'heading'; }),
        editor.blocks.map(function (b) { return b.kind; }).join(',') +
        '  text=' + JSON.stringify(editor.text));
    } catch (error) {
      record('a heading typed on a new line becomes a heading block', false, String(error));
    }

    // Typing in order must land in order: place each character at the caret the
    // editor reports, which is what a keystroke does.
    try {
      editor.setDocument('', { revision: 'self-test' });
      var typed = '1234';
      for (var i = 0; i < typed.length; i++) {
        var ch = typed.charAt(i);
        var index = editor.activeIndex >= 0 ? editor.activeIndex : 0;
        var host = editor.blockElements[index];
        if (!host) host = editor.root.querySelector('.md-empty') || editor.root;
        host.focus();
        var offset = editor.activeIndex >= 0
          ? editor.caretOffset() - editor.blocks[index].start
          : 0;
        var walker = document.createTreeWalker(host, NodeFilter.SHOW_TEXT, null);
        var node = null;
        var local = offset;
        while (walker.nextNode()) {
          var candidate = walker.currentNode;
          if (local <= candidate.nodeValue.length) { node = candidate; break; }
          local -= candidate.nodeValue.length;
        }
        if (!node) {
          node = host.appendChild(document.createTextNode(''));
          local = 0;
        }
        node.nodeValue = node.nodeValue.slice(0, local) + ch + node.nodeValue.slice(local);
        var caretRange = document.createRange();
        caretRange.setStart(node, local + ch.length);
        caretRange.collapse(true);
        var caretSelection = global.getSelection();
        caretSelection.removeAllRanges();
        caretSelection.addRange(caretRange);
        host.dispatchEvent(new Event('input', { bubbles: true }));
      }
      record('typing 1234 produces 1234', editor.text === '1234', JSON.stringify(editor.text));
    } catch (error) {
      record('typing 1234 produces 1234', false, String(error));
    }

    // A fenced code block turns into a code box once its fence line is complete.
    try {
      editor.setDocument('```cpp', { revision: 'self-test' });
      editor._setCaretOffset(editor.text.length, false);
      var fenceStillSource = !editor._isCodeBox(editor.blocks[0]);
      editor.insertLineBreak();
      record('a completed fence line is edited as a code box',
        fenceStillSource && editor.text === '```cpp\n```\n' && editor._isCodeBox(editor.blocks[0]),
        JSON.stringify(editor.text) + ' source-first=' + fenceStillSource);
    } catch (error) {
      record('a completed fence line is edited as a code box', false, String(error));
    }

    // Typing code in a code box: Enter keeps the line's indentation (and opens a
    // level deeper after "{"), Tab indents instead of leaving the box, and the code
    // is coloured on a layer under the text box (see tools/web_browser.js).
    try {
      editor.setDocument('```cpp\nint main(){\nreturn 0;\n}\n```\n', { revision: 'self-test' });
      editor._setCaretOffset(editor.blocks[0].contentStart + 3, false);
      var codeBox = editor.blockElements[editor.activeIndex];
      var codeInput = codeBox.querySelector('.md-code-input');
      var codeShade = codeBox.querySelector('.md-code-highlight');
      var pressKey = function (key) {
        editor._onCodeKeyDown({ key: key, target: codeInput, isComposing: false,
                                preventDefault: function () {} }, editor.activeIndex);
      };
      codeInput.focus();
      codeInput.setSelectionRange('int main(){'.length, 'int main(){'.length);
      pressKey('Enter');
      record('Enter inside a code box indents the next line',
        codeInput.value.indexOf('int main(){\n    \n') === 0, JSON.stringify(codeInput.value));
      codeInput.setSelectionRange(0, 0);
      pressKey('Tab');
      record('Tab indents the code instead of leaving the box',
        codeInput.value.indexOf('    int main(){') === 0, JSON.stringify(codeInput.value));
      record('the code box colours its code on a layer of its own',
        !!codeShade && codeShade.textContent === codeInput.value &&
        codeShade.querySelectorAll('[class^="hljs-"]').length > 0,
        codeShade ? JSON.stringify(codeShade.textContent) + ' vs ' + JSON.stringify(codeInput.value)
                  : 'no colour layer');

      // Backspace with nothing left in the box removes the box, and on the line after
      // the box it must not hand the caret back to the code.
      codeInput.value = '';
      editor._onCodeInput(editor.activeIndex);
      pressKey('Backspace');
      record('Backspace in an empty code box removes the block',
        editor.text === '', JSON.stringify(editor.text));

      editor.setDocument('text\n\n```js\nlet x = 1;\n```\n', { revision: 'self-test' });
      editor._setCaretOffset(editor.blocks[1].contentStart + 3, false);
      var afterElement = editor.blockElements[editor.activeIndex];
      var afterNode = afterElement.childNodes[afterElement.childNodes.length - 1];
      var swallowed = false;
      editor._onCodeAfterKeyDown({
        key: 'Backspace', target: afterNode, isComposing: false,
        preventDefault: function () { swallowed = true; },
      }, editor.activeIndex);
      record('Backspace on the line after a code box leaves the code alone',
        swallowed && editor.text === 'text\n\n```js\nlet x = 1;\n```\n',
        'swallowed=' + swallowed + ' text=' + JSON.stringify(editor.text));

      // The same at the start of a *block* that follows a code box: the caret must stay
      // out of the code, and an empty box goes with the press.
      editor.setDocument('para\n\n```js\n```\n\nafter\n', { revision: 'self-test' });
      editor._setCaretOffset(editor.blocks[2].start, false);
      editor.blockElements[2].focus();
      editor._handleBoundaryKeyDown({
        key: 'Backspace', isComposing: false, target: editor.blockElements[2],
        preventDefault: function () {},
      });
      record('Backspace after a code box removes an empty box',
        editor.text === 'para\n\nafter\n', JSON.stringify(editor.text));

      editor.setDocument('para\n\n```js\nlet x = 1;\n```\n\nafter\n', { revision: 'self-test' });
      editor._setCaretOffset(editor.blocks[2].start, false);
      editor.blockElements[2].focus();
      var keptAfterBox = editor.text;
      editor._handleBoundaryKeyDown({
        key: 'Backspace', isComposing: false, target: editor.blockElements[2],
        preventDefault: function () {},
      });
      record('Backspace after a code box leaves the code alone and the caret outside',
        editor.text === keptAfterBox &&
        String(document.activeElement.className).indexOf('md-code-input') < 0,
        JSON.stringify(editor.text) + ' focus=' + document.activeElement.nodeName +
          '.' + document.activeElement.className);
    } catch (error) {
      record('a code box indents and colours the code', false, String(error));
    }

    // The formatting toolbar: these are the buttons people who do not write
    // Markdown depend on, so the checks run in the engine that is really
    // rendering (see tools/web_format.js for the full suite).
    try {
      editor.setDocument('', { revision: 'self-test' });
      editor.insertBlock('heading2');
      record('the H2 button writes a heading marker', editor.text === '## ',
        JSON.stringify(editor.text));

      editor.setDocument('word\n', { revision: 'self-test' });
      editor._selectOffsets(0, 4);
      editor.wrapSelection('bold');
      record('the bold button wraps the selection', editor.text === '**word**\n',
        JSON.stringify(editor.text));

      editor.setDocument('- item\n', { revision: 'self-test' });
      editor._setCaretOffset(6, false);
      editor.insertLineBreak();
      record('Enter continues a list item', editor.text === '- item\n- \n',
        JSON.stringify(editor.text));

      editor.setDocument('', { revision: 'self-test' });
      editor.insertBlock('table');
      // Checked by shape, not by the placeholder text: those headers are
      // translated ("| 列 1 | ... |" in Chinese).
      var tableLines = editor.text.split('\n');
      var bars = function (line) { return (line.match(/\|/g) || []).length; };
      record('the table button writes a table skeleton',
        tableLines.length >= 3 && bars(tableLines[0]) === 4 && bars(tableLines[2]) === 4 &&
        tableLines[1].indexOf('---') >= 0, JSON.stringify(editor.text));
    } catch (error) {
      record('the formatting buttons write Markdown', false, String(error));
    }

    // A display formula has to arrive at KaTeX without its "$$" delimiters. A stray
    // one does not throw (throwOnError is off) - it draws "katex-error" instead,
    // which is exactly what "the formula does not render" looks like.
    try {
      var mathText = '$$\n\\begin{pmatrix} a & b \\\\ c & d \\end{pmatrix}\n$$\n\ntext\n';
      var mathBlocks = this._ensureBlocksFor(mathText);
      var mathBlock = null;
      for (var m = 0; m < mathBlocks.length; m++) {
        if (mathBlocks[m].kind === 'math_block') { mathBlock = mathBlocks[m]; break; }
      }
      var mathHtmlOut = mathBlock ? Render.blockHtml(mathBlock) : '';
      var katexUsed = mathHtmlOut.indexOf('class="katex') >= 0;
      record('a display formula reaches KaTeX without its delimiters',
        !!mathBlock && katexUsed && mathHtmlOut.indexOf('katex-error') < 0 &&
        mathHtmlOut.indexOf('pmatrix') < 0 && mathHtmlOut.indexOf('$$') < 0,
        mathHtmlOut.slice(0, 120));
    } catch (error) {
      record('a display formula reaches KaTeX without its delimiters', false, String(error));
    }

    // Select-all has to reach every block, not just the one being edited: the
    // engine's own Ctrl+A stops at the end of the editable block, which is why
    // "select everything and copy" did not work in the rendered view.
    try {
      editor.setDocument('first\n\nsecond\n\n```js\ncode\n```\n', { revision: 'self-test', caret: 0 });
      editor.selectAll();
      var allSelection = global.getSelection();
      var allRange = allSelection && allSelection.rangeCount ? allSelection.getRangeAt(0) : null;
      var firstIndex = allRange ? this._nodeBlockIndex(allRange.startContainer) : -1;
      var lastIndex = allRange ? this._nodeBlockIndex(allRange.endContainer) : -1;
      record('select-all covers every block',
        !!allRange && firstIndex === 0 && lastIndex === 2 &&
        allRange.toString().indexOf('code') >= 0,
        'start=' + firstIndex + ' end=' + lastIndex + ' text=' +
          JSON.stringify(allRange ? allRange.toString() : ''));
    } catch (error) {
      record('select-all covers every block', false, String(error));
    }

    // Restore what the writer had.
    editor.setDocument(saved.text, { revision: 'self-test-restore' });
    if (saved.active >= 0) editor._setCaretOffset(saved.active, false);

    report.unshift(failures === 0 ? 'front-end self-test: all checks passed'
                                  : 'front-end self-test: ' + failures + ' FAILED');
    return report.join('\n');
  };

  global.ColaiEditor = Editor;
})(window);
