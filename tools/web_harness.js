/* Minimal DOM harness for running colaimdation's web assets under Node.
 *
 * The front-end has no build step, so without this a broken render.js/editor.js
 * is only discovered inside WebView2. The stub models the parts the editor
 * actually depends on, and deliberately models two browser behaviours that are
 * easy to get wrong:
 *
 *   - the caret (Selection) is driven through removeAllRanges/addRange, so the
 *     editor's own caret restoration is exercised rather than assumed;
 *   - appendChild moves a fragment's children, like the real API.
 *
 * Usage:
 *   const { createEditor, allTextNodes, getCaret, latestSignature } = require('./web_harness');
 */
'use strict';

const fs = require('fs');
const path = require('path');
const vm = require('vm');

let idCounter = 0;

class Node {
  constructor(type) {
    this.nodeType = type;
    this.childNodes = [];
    this.parentNode = null;
    this._id = ++idCounter;
  }
  get firstChild() { return this.childNodes[0] || null; }
  get nextSibling() {
    if (!this.parentNode) return null;
    const list = this.parentNode.childNodes;
    return list[list.indexOf(this) + 1] || null;
  }
  get nodeValue() { return this._value || ''; }
  set nodeValue(v) { this._value = v; }
  appendChild(node) {
    if (node instanceof Fragment) {
      const kids = node.childNodes.slice();
      node.childNodes = [];
      for (const kid of kids) { kid.parentNode = null; this.appendChild(kid); }
      return node;
    }
    if (node.parentNode) node.parentNode.removeChild(node);
    node.parentNode = this;
    this.childNodes.push(node);
    return node;
  }
  insertBefore(node, reference) {
    if (node instanceof Fragment) {
      const kids = node.childNodes.slice();
      node.childNodes = [];
      for (const kid of kids) { kid.parentNode = null; this.insertBefore(kid, reference); }
      return node;
    }
    if (node.parentNode) node.parentNode.removeChild(node);
    node.parentNode = this;
    const at = reference ? this.childNodes.indexOf(reference) : -1;
    if (at < 0) this.childNodes.push(node);
    else this.childNodes.splice(at, 0, node);
    return node;
  }
  removeChild(node) {
    const i = this.childNodes.indexOf(node);
    if (i >= 0) this.childNodes.splice(i, 1);
    node.parentNode = null;
    return node;
  }
  get textContent() {
    if (this.nodeType === 3) return this._value || '';
    let out = '';
    for (const child of this.childNodes) out += child.textContent;
    return out;
  }
  set textContent(value) {
    this.childNodes = [];
    if (value !== '') this.appendChild(new TextNode(String(value)));
  }
  addEventListener() {}
  removeEventListener() {}
  dispatchEvent() { return true; }
}

class TextNode extends Node {
  constructor(value) { super(3); this._value = String(value); this.nodeName = '#text'; }
}

class Element extends Node {
  constructor(tag) {
    super(1);
    this.nodeName = String(tag).toUpperCase();
    this.attributes = {};
    this._classes = new Set();
    this.classList = {
      add: (...names) => names.forEach((n) => this._classes.add(n)),
      remove: (...names) => names.forEach((n) => this._classes.delete(n)),
      toggle: (name, on) => {
        if (on === undefined) on = !this._classes.has(name);
        if (on) this._classes.add(name); else this._classes.delete(name);
        return on;
      },
      contains: (name) => this._classes.has(name),
    };
    this.innerHTMLValue = '';
  }
  get className() { return Array.from(this._classes).join(' '); }
  set className(value) { this._classes = new Set(String(value).split(/\s+/).filter(Boolean)); }
  setAttribute(name, value) { this.attributes[name] = String(value); }
  getAttribute(name) {
    return Object.prototype.hasOwnProperty.call(this.attributes, name) ? this.attributes[name] : null;
  }
  removeAttribute(name) { delete this.attributes[name]; }
  hasAttribute(name) { return Object.prototype.hasOwnProperty.call(this.attributes, name); }
  get innerHTML() { return this.innerHTMLValue; }
  set innerHTML(value) { this.innerHTMLValue = String(value); this.childNodes = []; }
  get offsetTop() { return 0; }
  scrollIntoView() {}
  focus() {}
  getBoundingClientRect() { return { top: 0, bottom: 0, left: 0, right: 0 }; }
  querySelector(selector) {
    // Only the class selector the editor actually uses is supported; without it
    // the stub silently returned null and hid the empty-document typing path.
    const name = String(selector).replace(/^\./, '');
    let found = null;
    (function walk(node) {
      if (found) return;
      for (const child of node.childNodes) {
        if (child.nodeType !== 1) continue;
        if (child.classList.contains(name)) { found = child; return; }
        walk(child);
        if (found) return;
      }
    })(this);
    return found;
  }
  querySelectorAll() { return []; }
  contains(node) { let cur = node; while (cur) { if (cur === this) return true; cur = cur.parentNode; } return false; }
}

class Fragment extends Element {
  constructor() { super('#fragment'); this.nodeName = '#fragment'; }
}

function allTextNodes(node, out) {
  out = out || [];
  for (const child of node.childNodes) {
    if (child.nodeType === 3) out.push(child);
    else if (child.nodeType === 1) allTextNodes(child, out);
  }
  return out;
}

function load(root) {
  let caret = { node: null, offset: 0, endNode: null, endOffset: 0 };

  // The selection models a *range*: the toolbar rewrites the selected text and
  // asks for the selection offsets back, which a collapsed-only stub cannot
  // represent (it silently turned every "make this bold" into an empty wrap).
  const selection = {
    get anchorNode() { return caret.node; },
    get anchorOffset() { return caret.offset; },
    get focusNode() { return caret.endNode || caret.node; },
    get focusOffset() { return caret.endNode ? caret.endOffset : caret.offset; },
    // A real Selection reports both of these, and code that reads them (a Backspace at
    // a block boundary, for one) must see the same shape as in the browser.
    get rangeCount() { return caret.node ? 1 : 0; },
    get isCollapsed() {
      return (caret.endNode || caret.node) === caret.node &&
        (caret.endNode ? caret.endOffset : caret.offset) === caret.offset;
    },
    removeAllRanges() { caret = { node: null, offset: 0, endNode: null, endOffset: 0 }; },
    addRange(range) {
      if (!range._node) return;
      caret = {
        node: range._node,
        offset: range._offset,
        endNode: range._endNode || null,
        endOffset: range._endOffset === null || range._endOffset === undefined ? 0 : range._endOffset,
      };
    },
  };

  const documentStub = {
    readyState: 'complete',
    documentElement: { getAttribute: () => 'light', setAttribute: () => {}, style: { setProperty: () => {} } },
    body: { style: {} },
    createElement: (tag) => new Element(tag),
    createDocumentFragment: () => new Fragment(),
    createTextNode: (value) => new TextNode(value),
    createTreeWalker: (rootNode) => {
      const nodes = allTextNodes(rootNode, []);
      let i = -1;
      return {
        currentNode: null,
        nextNode() { i += 1; this.currentNode = nodes[i] || null; return !!this.currentNode; },
      };
    },
    createRange: () => ({
      _node: null,
      _offset: 0,
      _endNode: null,
      _endOffset: null,
      // A real Range exposes these; the editor reads startContainer/startOffset
      // when it re-selects a range it just rewrote (the toolbar does). Without
      // them the stub silently collapsed every selection to offset 0.
      get startContainer() { return this._node; },
      get startOffset() { return this._offset; },
      get endContainer() { return this._endNode || this._node; },
      get endOffset() { return this._endOffset === null ? this._offset : this._endOffset; },
      setStart(node, offset) { this._node = node; this._offset = offset; },
      setEnd(node, offset) { this._endNode = node; this._endOffset = offset; },
      selectNodeContents(node) {
        const texts = allTextNodes(node, []);
        this._node = texts[texts.length - 1] || node;
        this._offset = this._node.nodeValue ? this._node.nodeValue.length : 0;
      },
      collapse() {},
      toString() {
        if (!this._node) return '';
        if (this._node.nodeType === 3) return this._node.nodeValue.slice(0, this._offset);
        return '';
      },
    }),
    getElementById: () => null,
    querySelector: () => null,
    querySelectorAll: () => [],
    addEventListener: () => {},
    removeEventListener: () => {},
  };

  const windowStub = {    document: documentStub,
    getSelection: () => selection,
    NodeFilter: { SHOW_TEXT: 4, SHOW_ELEMENT: 1 },
    console,
    setTimeout,
    clearTimeout,
    setInterval: () => 0,
    clearInterval: () => {},
  };

  const sandbox = {
    window: windowStub,
    document: documentStub,
    NodeFilter: windowStub.NodeFilter,
    console,
    setTimeout,
    clearTimeout,
    setInterval: () => 0,
    clearInterval: () => {},
    navigator: {},
  };
  sandbox.globalThis = sandbox;
  vm.createContext(sandbox);
  for (const file of ['web/assets/i18n.js', 'web/assets/render.js', 'web/assets/editor.js']) {
    vm.runInContext(fs.readFileSync(path.join(root, file), 'utf8'), sandbox, { filename: file });
  }

  const Editor = sandbox.window.ColaiEditor;
  let rebuilds = 0;
  const originalRenderAll = Editor.prototype._renderAll;
  Editor.prototype._renderAll = function () { rebuilds += 1; return originalRenderAll.apply(this, arguments); };
  const originalCommit = Editor.prototype._commitAndRerender;
  Editor.prototype._commitAndRerender = function () { rebuilds += 1; return originalCommit.apply(this, arguments); };

  return {
    Render: sandbox.window.ColaiRender,
    Editor,
    allTextNodes,
    getCaret: () => caret,
    setCaret(node, offset) { caret = { node, offset, endNode: null, endOffset: 0 }; },
    rebuildCount: () => rebuilds,
    resetRebuilds() { rebuilds = 0; },
    createEditor(text) {
      const rootEl = new Element('div');
      const textarea = new Element('textarea');
      textarea.value = '';
      // A real text box has a selection, and the editor reads and writes it whenever
      // the plain-text (source) view is open. Without it every caret move there threw.
      textarea.selectionStart = 0;
      textarea.selectionEnd = 0;
      textarea.setSelectionRange = function (start, end) {
        const length = String(this.value || '').length;
        const clamp = (value) => {
          const number = typeof value === 'number' ? value : 0;
          return Math.max(0, Math.min(number, length));
        };
        this.selectionStart = clamp(start);
        this.selectionEnd = clamp(end === undefined ? start : end);
      };
      textarea.select = function () {
        this.setSelectionRange(0, String(this.value || '').length);
      };
      const editor = new Editor(rootEl, textarea);
      editor.setDocument(text, { revision: 'r1' });
      rebuilds = 0;
      return { editor, rootEl };
    },
  };
}

module.exports = { load, allTextNodes, Element, TextNode };
