/* Colaimdation - application glue.
 *
 * Owns the connection to the C++ host (WebView2 web messages), the editor and
 * the panels, and enforces the two rules the product is built on:
 *
 *   1. the agent never touches a file: it edits the buffer the host owns, and
 *   2. the human is always in control: staged changes wait for a diff review
 *      when the policy asks for it, and every change is audited.
 *
 * UI text always goes through ColaiI18n so a language switch is instant and
 * covers strings this file builds in code, not just the ones in index.html.
 */
(function (global) {
  'use strict';

  var I18n = global.ColaiI18n;

  function t(key, args) { return I18n ? I18n.t(key, args) : key; }

  var host = null;
  var editor = null;
  var bootstrapped = false;
  var status = {
    listening: false,
    port: 0,
    endpoint: '',
    tools: 0,
    requests: 0,
    tool_calls: 0,
    sessions: 0,
    discovery_path: '',
  };
  var languageMenu = null;
  var selectionTimer = null;
  var auditTimer = null;
  var lastDocumentInfo = null;
  var lastEditorStatus = null;
  var tabs = [];
  var activeTab = 0;
  var lastMcpSettings = null;

  /* ------------------------------------------------------------------- host */

  function post(message) {
    if (host) {
      host.postMessage(message);
      return true;
    }
    if (global.console && global.console.debug) {
      global.console.debug('[Colaimdation] no host, dropping', message);
    }
    return false;
  }

  function receive(event) {
    var payload = event.data;
    if (typeof payload === 'string') {
      try { payload = JSON.parse(payload); } catch (error) { return; }
    }
    if (!payload || typeof payload !== 'object') return;
    dispatch(payload);
  }

  function dispatch(message) {
    switch (message.type) {
      case 'status_reply':
        applicationReply(message);
        return;
      case 'audit_reply':
        applicationReply(message);
        return;
      case 'rollback_reply':
        if (message.status === 'error') {
          ColaiPanels.toast(t('audit.noAgentChanges'), message.error || '', 'error');
        }
        post({ type: 'status_query' });
        return;
      case 'command':
        handleCommand(message);
        break;
      case 'document_loaded':
        applyDocumentLoaded(message);
        break;
      case 'document_changed':
        applyDocumentChanged(message);
        break;
      case 'document_saved':
        onSaved(message);
        break;
      case 'tabs':
        applyTabs(message);
        break;
      case 'user_activity':
        break;
      case 'selection_set':
        applySelectionSet(message);
        break;
      case 'confirmation_required':
        ColaiPanels.showConfirmation(message.pending, editor.getText());
        break;
      case 'policy_changed':
        // The host wraps the new policy in a message; passing the wrapper meant
        // every checkbox was read off an object that has no policy fields.
        ColaiPanels.setPolicy(message.policy || message);
        if (editor && (message.policy || message).allow_write !== undefined) {
          editor.setReadOnly(!(message.policy || message).allow_write);
        }
        break;
      case 'language_changed':
        setLanguage(message.language, false);
        break;
      case 'activity':
        ColaiPanels.addActivity({
          time: (message.timestamp || '').slice(11, 19),
          key: message.key || null,
          args: message.args || null,
          message: message.message,
          error: message.level === 'error',
        });
        break;
      case 'tool_call':
        ColaiPanels.addActivity({
          time: (message.timestamp || '').slice(11, 19),
          tool: message.tool,
          message: (message.session ? message.session + ' ' : '') +
            (message.is_error ? t('activity.failed') : t('activity.ok')) +
            (message.client ? ' (' + message.client + ')' : ''),
          error: !!message.is_error,
        });
        break;
      default:
        break;
    }
  }

  /* ------------------------------------------------------------- documents */

  function applyDocumentLoaded(message) {
    var doc = message.document || {};
    // Restore the caret the *document* remembers, not offset 0: switching tabs
    // should put the writer back where they were working.
    var caret = 0;
    if (doc.selection && typeof doc.selection.head === 'number') caret = doc.selection.head;
    editor.setDocument(doc.content || '', {
      revision: doc.revision,
      dirty: !!doc.dirty,
      caret: caret,
    });
    updateStatusbar(doc);
    ColaiPanels.clearPending();
  }

  /* ------------------------------------------------------------- open tabs */

  function applyTabs(message) {
    tabs = (message && message.tabs) || [];
    activeTab = message && typeof message.active === 'number' ? message.active : 0;
    renderTabs();
  }

  function renderTabs() {
    var bar = document.getElementById('tabbar');
    if (!bar) return;
    // A single document needs no tab: the window title already names it, and the
    // row would only cost vertical space.
    bar.classList.toggle('tabbar-single', tabs.length < 2);
    if (tabs.length < 2) {
      bar.innerHTML = '';
      return;
    }
    bar.innerHTML = '';
    tabs.forEach(function (tab, index) {
      var element = document.createElement('div');
      element.className = 'tab' + (index === activeTab ? ' active' : '') + (tab.dirty ? ' dirty' : '');
      element.setAttribute('role', 'tab');
      element.setAttribute('data-index', String(index));
      element.setAttribute('aria-selected', index === activeTab ? 'true' : 'false');
      element.title = tab.path || tab.name || t('status.untitled');
      element.addEventListener('click', function (event) {
        if (event.target && event.target.classList.contains('tab-close')) return;
        if (index === activeTab) return;
        post({ type: 'activate_tab', index: index });
      });
      element.addEventListener('auxclick', function (event) {
        // Middle click closes, like every other tabbed editor.
        if (event.button === 1) post({ type: 'close_tab', index: index });
      });

      var name = document.createElement('span');
      name.className = 'tab-name';
      name.textContent = tab.name || t('status.untitled');
      element.appendChild(name);

      var dot = document.createElement('span');
      dot.className = 'tab-dirty';
      dot.title = t('status.unsaved');
      element.appendChild(dot);

      var close = document.createElement('button');
      close.type = 'button';
      close.className = 'tab-close';
      close.title = t('tab.close');
      close.setAttribute('aria-label', t('tab.close'));
      close.textContent = '\u00d7';
      close.addEventListener('click', function (event) {
        event.stopPropagation();
        post({ type: 'close_tab', index: index });
      });
      element.appendChild(close);
      bar.appendChild(element);
    });

    var add = document.createElement('button');
    add.type = 'button';
    add.className = 'tab-new';
    add.id = 'tab-new';
    add.title = t('tab.new');
    add.setAttribute('aria-label', t('tab.new'));
    add.textContent = '+';
    add.addEventListener('click', function () { post({ type: 'command', command: 'new' }); });
    bar.appendChild(add);

    // Keep the active tab visible when the bar scrolls.
    var active = bar.querySelector ? bar.querySelector('.tab.active') : null;
    if (active && active.scrollIntoView) active.scrollIntoView({ block: 'nearest', inline: 'nearest' });
  }

  // Keeps the reader's place when an agent edits the document underneath them.
  function applyDocumentChanged(message) {
    var scroll = document.getElementById('editor-scroll');
    var anchorBlock = editor.activeIndex;
    var anchorTop = null;
    if (anchorBlock >= 0 && editor.blockElements[anchorBlock]) {
      anchorTop = editor.blockElements[anchorBlock].offsetTop - (scroll ? scroll.scrollTop : 0);
    }

    if (typeof message.content === 'string') {
      editor.applyHostText(message.content, {
        revision: message.revision,
        dirty: !!message.dirty,
        highlightChanges: message.changes || [],
      });
    }

    // Re-anchor on the same block (by index, then by nearest text match).
    if (scroll && anchorBlock >= 0) {
      var target = editor.blockElements[Math.min(anchorBlock, editor.blockElements.length - 1)];
      if (target && anchorTop !== null) {
        scroll.scrollTop = Math.max(0, target.offsetTop - anchorTop);
      }
      if (target) {
        editor.activeIndex = -1;
        var newIndex = editor.blockElements.indexOf(target);
        if (newIndex >= 0) {
          editor.activeIndex = newIndex;
          editor._repaintBlock(newIndex);
        }
      }
    }

    updateStatusbar(message);
    if (message.reason === 'agent_patch') {
      ColaiPanels.addActivity({
        time: new Date().toTimeString().slice(0, 8),
        key: 'activity.documentUpdated',
      });
    }
    refreshAuditSoon();
  }

  function updateStatusbar(documentInfo) {
    // The parameter must not be named `document`: that shadowed the DOM object of
    // the same name, so every getElementById() below threw a TypeError and the
    // status bar kept showing "untitled.md / rev -" for every file that was
    // opened or saved. document_changed and audit_reply carry no path/name, so
    // merge into what we already know instead of resetting a real file.
    if (documentInfo) {
      lastDocumentInfo = lastDocumentInfo
        ? Object.assign({}, lastDocumentInfo, documentInfo)
        : documentInfo;
    }
    var info = lastDocumentInfo || {};
    var path = info.path || info.name || t('status.untitled');
    var pathNode = document.getElementById('status-path');
    if (pathNode) {
      pathNode.textContent = path;
      pathNode.title = info.path || t('status.notSaved');
    }
    var revisionNode = document.getElementById('status-revision');
    if (revisionNode && info.revision) {
      revisionNode.textContent = t('status.rev', [String(info.revision).slice(0, 8)]);
    }
    var dirtyNode = document.getElementById('status-dirty');
    if (dirtyNode && typeof info.dirty === 'boolean') dirtyNode.hidden = !info.dirty;
  }

  function onEditorStatus(info) {
    lastEditorStatus = info;
    var selectionNode = document.getElementById('status-selection');
    if (selectionNode) {
      selectionNode.textContent = info.selectionLength
        ? t('status.selection', [info.selectionLength])
        : t('status.caret', [info.line, info.column]);
    }
    var countsNode = document.getElementById('status-counts');
    if (countsNode) {
      countsNode.textContent = t('status.counts', [info.words, info.characters, info.blocks]);
    }
    var dirtyNode = document.getElementById('status-dirty');
    if (dirtyNode) dirtyNode.hidden = !info.dirty;
  }

  function onEditorChange(text, revision, hint) {
    post({
      type: 'edit',
      revision_before: revision,
      text: text,
      caret: editor.caretOffset(),
      hint: hint || 'typing',
    });
  }

  function onEditorCaret(anchor, head) {
    post({ type: 'selection', anchor: anchor, head: head });
    if (selectionTimer) clearTimeout(selectionTimer);
    selectionTimer = setTimeout(refreshAuditSoon, 2500);
  }

  function refreshAuditSoon() {
    if (auditTimer) return;
    auditTimer = setTimeout(function () {
      auditTimer = null;
      if (ColaiPanels.pending()) return;
      post({ type: 'audit_query', limit: 40, include_diff: false });
    }, 600);
  }

  /* --------------------------------------------------------------- language */

  var SHORT_LABEL = { en: 'EN', 'zh-Hans': '\u4e2d', ja: '\u65e5' };

  function languageLabel(tag) {
    return SHORT_LABEL[tag] || String(tag).slice(0, 2).toUpperCase();
  }

  function buildLanguageMenu() {
    var items = document.getElementById('language-menu');
    if (!items) return null;
    items.innerHTML = '';
    I18n.languages.forEach(function (language) {
      var button = document.createElement('button');
      button.type = 'button';
      button.className = 'lang-item';
      button.setAttribute('role', 'menuitemradio');
      button.setAttribute('data-lang', language.tag);
      button.setAttribute('aria-checked', I18n.language() === language.tag ? 'true' : 'false');
      var name = document.createElement('span');
      name.className = 'lang-native';
      name.textContent = language.nativeName;
      var english = document.createElement('span');
      english.className = 'lang-english';
      english.textContent = language.nativeName === language.englishName ? '' : language.englishName;
      button.appendChild(name);
      button.appendChild(english);
      button.addEventListener('click', function () {
        closeLanguageMenu();
        setLanguage(language.tag, true);
      });
      items.appendChild(button);
    });
    return items;
  }

  function openLanguageMenu() {
    var menu = document.getElementById('language-menu');
    var button = document.getElementById('btn-lang');
    if (!menu || !button) return;
    buildLanguageMenu();
    menu.hidden = false;
    button.setAttribute('aria-expanded', 'true');
    // Anchored to the button: the toolbar wraps on narrow windows, so a CSS
    // absolute position would drift.
    var box = button.getBoundingClientRect();
    menu.style.top = Math.round(box.bottom + 6) + 'px';
    menu.style.left = Math.round(Math.max(8, box.right - menu.offsetWidth)) + 'px';
  }

  function closeLanguageMenu() {
    var menu = document.getElementById('language-menu');
    var button = document.getElementById('btn-lang');
    if (menu) menu.hidden = true;
    if (button) button.setAttribute('aria-expanded', 'false');
  }

  function toggleLanguageMenu() {
    var menu = document.getElementById('language-menu');
    if (menu && menu.hidden) openLanguageMenu();
    else closeLanguageMenu();
  }

  /* -------------------------------------------------------------- appearance */

  // The writer's choice: 'light', 'dark' or 'auto' (follow the operating system).
  // 'auto' is a *preference*, not a colour: it is resolved here so the stylesheet
  // only ever sees concrete "light"/"dark" and both themes stay one token set.
  var themePreference = 'auto';
  var THEMES = ['light', 'dark', 'auto'];

  function systemPrefersDark() {
    return !!(global.matchMedia && global.matchMedia('(prefers-color-scheme: dark)').matches);
  }

  function resolvedTheme(preference) {
    if (preference === 'dark') return 'dark';
    if (preference === 'light') return 'light';
    return systemPrefersDark() ? 'dark' : 'light';
  }

  function themeLabel(preference) {
    return t('theme.' + (THEMES.indexOf(preference) >= 0 ? preference : 'auto'));
  }

  function applyTheme(preference, notifyHost) {
    if (THEMES.indexOf(preference) < 0) preference = 'auto';
    themePreference = preference;
    var theme = resolvedTheme(preference);
    document.documentElement.setAttribute('data-theme', theme);
    document.documentElement.setAttribute('data-theme-preference', preference);
    // Code highlighting ships two stylesheets; only one may be active.
    var light = document.getElementById('hljs-light');
    var dark = document.getElementById('hljs-dark');
    if (light) light.disabled = theme === 'dark';
    if (dark) dark.disabled = theme !== 'dark';
    if (global.ColaiRender) global.ColaiRender.refreshMermaidTheme();
    var label = document.getElementById('btn-theme-label');
    if (label) label.textContent = themeLabel(preference);
    buildThemeMenu();
    if (notifyHost) post({ type: 'theme_changed', theme: preference });
  }

  function buildThemeMenu() {
    var menu = document.getElementById('theme-menu');
    if (!menu) return null;
    menu.innerHTML = '';
    THEMES.forEach(function (preference) {
      var button = document.createElement('button');
      button.type = 'button';
      button.className = 'lang-item';
      button.setAttribute('role', 'menuitemradio');
      button.setAttribute('data-theme-option', preference);
      button.setAttribute('aria-checked', themePreference === preference ? 'true' : 'false');
      var name = document.createElement('span');
      name.className = 'lang-native';
      name.textContent = themeLabel(preference);
      button.appendChild(name);
      button.addEventListener('click', function () {
        closeThemeMenu();
        applyTheme(preference, true);
      });
      menu.appendChild(button);
    });
    return menu;
  }

  function openThemeMenu() {
    var menu = document.getElementById('theme-menu');
    var button = document.getElementById('btn-theme');
    if (!menu || !button) return;
    buildThemeMenu();
    menu.hidden = false;
    button.setAttribute('aria-expanded', 'true');
    var box = button.getBoundingClientRect();
    menu.style.top = Math.round(box.bottom + 6) + 'px';
    menu.style.left = Math.round(Math.max(8, box.right - menu.offsetWidth)) + 'px';
  }

  function closeThemeMenu() {
    var menu = document.getElementById('theme-menu');
    var button = document.getElementById('btn-theme');
    if (menu) menu.hidden = true;
    if (button) button.setAttribute('aria-expanded', 'false');
  }

  function toggleThemeMenu() {
    var menu = document.getElementById('theme-menu');
    if (menu && menu.hidden) openThemeMenu();
    else closeThemeMenu();
  }

  // The appearance switch lives in this file (not in editor.js), so it reports
  // into the same self-test report the shell shows.
  function appearanceSelfTest() {
    var lines = [];
    var failures = 0;
    function record(label, ok, detail) {
      if (!ok) failures += 1;
      lines.push((ok ? 'ok   ' : 'FAIL ') + label + (ok || !detail ? '' : '  -> ' + detail));
    }
    var before = themePreference;
    var lightSheet = document.getElementById('hljs-light');
    var darkSheet = document.getElementById('hljs-dark');
    try {
      applyTheme('dark', false);
      record('the appearance switch applies the dark theme',
        document.documentElement.getAttribute('data-theme') === 'dark' &&
        !!darkSheet && darkSheet.disabled === false && !!lightSheet && lightSheet.disabled === true,
        document.documentElement.getAttribute('data-theme'));

      applyTheme('light', false);
      record('the appearance switch applies the light theme',
        document.documentElement.getAttribute('data-theme') === 'light' &&
        !!darkSheet && darkSheet.disabled === true,
        document.documentElement.getAttribute('data-theme'));

      applyTheme('auto', false);
      var expected = systemPrefersDark() ? 'dark' : 'light';
      record('"follow system" resolves to the operating system theme',
        document.documentElement.getAttribute('data-theme') === expected &&
        document.documentElement.getAttribute('data-theme-preference') === 'auto', expected);

      var menu = document.getElementById('theme-menu');
      record('the appearance menu offers light, dark and follow-system',
        !!menu && menu.childNodes.length === 3,
        menu ? String(menu.childNodes.length) : 'no menu');
    } catch (error) {
      record('the appearance switch works', false, String(error));
    }
    applyTheme(before, false);
    lines.unshift(failures === 0 ? 'appearance: all checks passed'
                                 : 'appearance: ' + failures + ' FAILED');
    return lines.join('\n');
  }

  function setLanguage(tag, notifyHost) {
    if (!I18n) return;
    I18n.setLanguage(tag);
    var label = document.getElementById('btn-lang-label');
    if (label) label.textContent = languageLabel(I18n.language());
    buildLanguageMenu();

    // Re-render everything this file builds in code.
    if (lastDocumentInfo) updateStatusbar(lastDocumentInfo);
    if (lastEditorStatus) onEditorStatus(lastEditorStatus);
    if (status && status.endpoint) ColaiPanels.setEndpoint(status.endpoint, bridgePathText(status), status);
    ColaiPanels.renderActivity();
    // Strings the editor renders itself (the mode badge and the empty-document
    // placeholder) would otherwise keep the previous language.
    if (editor) editor.refreshLanguage();
    // The tab bar and the appearance menu build their text in code too.
    renderTabs();
    applyTheme(themePreference, false);
    // The endpoint hint is a translated sentence built in code.
    if (lastMcpSettings) ColaiPanels.setMcpSettings(lastMcpSettings);

    if (notifyHost) post({ type: 'language_changed', language: I18n.language() });
  }

  function bridgePathText(mcp) {
    var base = 'colaimdation_mcp_bridge.exe';
    if (mcp && mcp.port) return base + ' --port ' + mcp.port + ' --token <token>';
    return base;
  }

  /* --------------------------------------------------------------- commands */

  function handleCommand(message) {
    var command = message.command;
    if (command === 'bootstrap') {
      applyBootstrap(message);
      return;
    }
    switch (command) {
      case 'undo':
        post({ type: 'command', command: 'undo' });
        break;
      case 'redo':
        post({ type: 'command', command: 'redo' });
        break;
      case 'toggle_source_mode':
        toggleSourceMode();
        break;
      case 'toggle_agent_panel':
        ColaiPanels.togglePanel('agent');
        break;
      case 'toggle_audit_panel':
        ColaiPanels.togglePanel('audit');
        refreshAuditSoon();
        break;
      case 'devtools':
        post({ type: 'command', command: 'devtools' });
        break;
      case 'request_status':
        post({ type: 'status_query' });
        break;
      case 'self_test': {
        // Runs the editor's own checks inside the engine that is actually
        // rendering, and reports back so the shell can show the result.
        let report;
        try {
          report = editor.selfTest();
        } catch (error) {
          report = 'front-end self-test crashed: ' + (error && error.message ? error.message : error);
        }
        report += '\n' + appearanceSelfTest();
        post({ type: 'self_test_result', report: report });
        break;
      }
      case 'set_language':
        setLanguage(message.language, false);
        break;
      case 'policy_changed':
        ColaiPanels.setPolicy(message.policy || message);
        if (editor && (message.policy || message).allow_write !== undefined) {
          editor.setReadOnly(!(message.policy || message).allow_write);
        }
        break;
      default:
        break;
    }
  }

  function applyBootstrap(message) {
    bootstrapped = true;
    if (message.mcp_settings) {
      lastMcpSettings = message.mcp_settings;
      ColaiPanels.setMcpSettings(message.mcp_settings);
    }
    if (message.mcp) {
      status = message.mcp;
      ColaiPanels.setEndpoint(message.mcp.endpoint || '', bridgePathText(message.mcp), message.mcp);
    }
    if (message.policy) {
      ColaiPanels.setPolicy(message.policy);
      editor.setReadOnly(!message.policy.allow_write);
    }
    if (message.options) {
      if (message.options.theme) {
        // Passed as a *preference*: "auto" means "follow the operating system",
        // which is resolved here rather than in the stylesheet.
        applyTheme(message.options.theme, false);
      }
      if (message.options.font_size) {
        document.documentElement.style.setProperty('--editor-font-size', message.options.font_size + 'px');
      }
      if (message.options.language) {
        setLanguage(message.options.language, false);
      }
    }
    post({ type: 'status_query' });
    post({ type: 'audit_query', limit: 40, include_diff: false });
  }

  function toggleSourceMode() {
    editor.setSourceMode(!editor.sourceMode);
    ColaiPanels.toast(
      editor.sourceMode ? t('toast.sourceMode') : t('toast.wysiwygMode'),
      editor.sourceMode ? t('toast.sourceModeBody') : t('toast.wysiwygModeBody'),
      'ok', 2200);
  }

  function onSaved(message) {
    // The host has just cleared the document's dirty flag; the page has to follow
    // it, otherwise the status bar keeps claiming "unsaved" for up to 15 seconds
    // after a successful save (until the next status query happens to arrive).
    if (editor) editor.dirty = false;
    var info = { dirty: typeof message.dirty === 'boolean' ? message.dirty : false };
    if (message.path) info.path = message.path;
    if (message.revision) info.revision = message.revision;
    updateStatusbar(info);
    // This used to show the "Copied" toast with the clipboard body text, so
    // saving a file told the writer "the value is on your clipboard".
    ColaiPanels.toast(t('toast.saved'), (message.path || '') + ' \u00b7 ' + (message.bytes || 0) + ' bytes',
      'ok', 2000);
  }

  function applySelectionSet(message) {
    if (typeof message.anchor === 'number' && typeof message.head === 'number') {
      editor._setCaretOffset(message.head, true);
    }
  }

  /* ------------------------------------------------------------- host replies */

  function applicationReply(message) {
    if (message.tabs) {
      applyTabs({ tabs: message.tabs, active: message.active_tab });
    }
    if (message.document) {
      updateStatusbar(message.document);
      // The host reports undo/redo availability here; without this the toolbar
      // buttons stayed disabled forever.
      updateUndoRedo(message.document);
    }
    if (message.policy) {
      ColaiPanels.setPolicy(message.policy);
      // A policy change may toggle write permission: apply it to the editor too,
      // otherwise the document stayed read-only until the next restart.
      if (editor) editor.setReadOnly(!message.policy.allow_write);
    }
    if (message.pending_changes && message.pending_changes.length) {
      var pending = message.pending_changes[message.pending_changes.length - 1];
      ColaiPanels.showConfirmation(pending, editor.getText());
    }
    if (message.mcp) {
      status = message.mcp;
      ColaiPanels.setEndpoint(message.mcp.endpoint || '', bridgePathText(message.mcp), message.mcp);
    }
    if (message.mcp_settings) {
      // Remember the last state so a language switch can re-render the hint.
      lastMcpSettings = message.mcp_settings;
      ColaiPanels.setMcpSettings(message.mcp_settings);
    }
    if (message.entries) {
      ColaiPanels.setAudit(message.entries, message.total_recorded, message.log_path);
    }
  }

  /* ------------------------------------------------------------ confirmation */

  function onResolvePending(id, accepted) {
    post({ type: 'confirm_change', pending_id: id, accepted: !!accepted });
    ColaiPanels.toast(
      accepted ? t('toast.changeApplied') : t('toast.changeRejected'),
      accepted ? t('toast.changeAppliedBody') : t('toast.changeRejectedBody'),
      accepted ? 'ok' : '', 2600);
  }

  /* ---------------------------------------------------------------- shortcuts */

  function onKeyDown(event) {
    var ctrl = event.ctrlKey || event.metaKey;
    if (ctrl && !event.altKey) {
      var key = (event.key || '').toLowerCase();
      if (key === 'z' && !event.shiftKey) { event.preventDefault(); post({ type: 'command', command: 'undo' }); return; }
      if (key === 'y' || (key === 'z' && event.shiftKey)) { event.preventDefault(); post({ type: 'command', command: 'redo' }); return; }
      if (key === 's') { event.preventDefault(); post({ type: 'command', command: event.shiftKey ? 'save_as' : 'save' }); return; }
      if (key === 'o') { event.preventDefault(); post({ type: 'command', command: 'open' }); return; }
      if (key === 'n') { event.preventDefault(); post({ type: 'command', command: 'new' }); return; }
      if (key === '/') { event.preventDefault(); toggleSourceMode(); return; }
      if (key === 'a' && event.shiftKey) { event.preventDefault(); ColaiPanels.togglePanel('agent'); return; }
      if (key === 'a') { event.preventDefault(); editor.selectAll(); return; }
      if (key === 'l' && event.shiftKey) { event.preventDefault(); ColaiPanels.togglePanel('audit'); refreshAuditSoon(); return; }
      if (key === 'w') { event.preventDefault(); post({ type: 'command', command: 'close_tab' }); return; }
      if (event.key === 'Tab') {
        // Ctrl+Tab / Ctrl+Shift+Tab walk the open documents. The host answers the
        // same shortcut when it owns the keystroke; whichever sees it first wins,
        // because the host swallows the key it handles.
        event.preventDefault();
        post({ type: 'command', command: event.shiftKey ? 'prev_tab' : 'next_tab' });
        return;
      }
    }
    if (event.key === 'Escape') {
      if (languageMenu && !languageMenu.hidden) { closeLanguageMenu(); return; }
      var themeMenu = document.getElementById('theme-menu');
      if (themeMenu && !themeMenu.hidden) { closeThemeMenu(); return; }
      if (ColaiPanels.pending()) { ColaiPanels.clearPending(); }
    }
  }

  /* -------------------------------------------------------------- toolbar wire */

  // Buttons that wrap the selection instead of inserting a block.
  var INLINE_FORMATS = { bold: true, italic: true, inlineCode: true, link: true,
                         image: true, formula: true };

  function wireFormatbar() {
    var bar = document.getElementById('formatbar');
    if (!bar) return;
    bar.addEventListener('mousedown', function (event) {
      // Keep the selection: clicking a button would otherwise collapse it before
      // the click handler runs, and every "make this bold" would lose its text.
      if (event.target && event.target.closest && event.target.closest('[data-format]')) {
        event.preventDefault();
      }
    });
    bar.addEventListener('click', function (event) {
      var button = event.target && event.target.closest ? event.target.closest('[data-format]') : null;
      if (!button || !editor) return;
      var format = button.getAttribute('data-format');
      if (format === 'table-row') editor.tableAddRow();
      else if (format === 'table-column') editor.tableAddColumn();
      else if (INLINE_FORMATS[format]) editor.wrapSelection(format);
      else editor.insertBlock(format);
      editor.focus();
    });
  }

  function wireToolbar() {
    var bindings = {
      'btn-new': 'new',
      'btn-open': 'open',
      'btn-save': 'save',
      'btn-save-as': 'save_as',
      'btn-undo': 'undo',
      'btn-redo': 'redo',
      'btn-endpoint': 'copy_endpoint',
    };
    Object.keys(bindings).forEach(function (id) {
      var button = document.getElementById(id);
      if (!button) return;
      button.addEventListener('click', function () {
        post({ type: 'command', command: bindings[id] });
      });
    });
    var source = document.getElementById('btn-source');
    if (source) source.addEventListener('click', toggleSourceMode);
    var agent = document.getElementById('btn-agent');
    if (agent) agent.addEventListener('click', function () { ColaiPanels.togglePanel('agent'); });
    var audit = document.getElementById('btn-audit');
    if (audit) audit.addEventListener('click', function () {
      ColaiPanels.togglePanel('audit');
      refreshAuditSoon();
    });

    var languageButton = document.getElementById('btn-lang');
    if (languageButton) {
      languageButton.addEventListener('click', function (event) {
        event.stopPropagation();
        toggleLanguageMenu();
      });
    }
    var themeButton = document.getElementById('btn-theme');
    if (themeButton) {
      themeButton.addEventListener('click', function (event) {
        event.stopPropagation();
        toggleThemeMenu();
      });
    }
    document.addEventListener('click', function (event) {
      if (languageMenu && !languageMenu.hidden) {
        if (!languageMenu.contains(event.target) &&
            !(event.target && event.target.id === 'btn-lang')) {
          closeLanguageMenu();
        }
      }
      var themeMenu = document.getElementById('theme-menu');
      if (themeMenu && !themeMenu.hidden) {
        if (!themeMenu.contains(event.target) &&
            !(event.target && event.target.id === 'btn-theme')) {
          closeThemeMenu();
        }
      }
    });
  }

  function updateUndoRedo(message) {
    var undo = document.getElementById('btn-undo');
    var redo = document.getElementById('btn-redo');
    if (undo && typeof message.can_undo === 'boolean') undo.disabled = !message.can_undo;
    if (redo && typeof message.can_redo === 'boolean') redo.disabled = !message.can_redo;
  }

  /* ------------------------------------------------------------------- boot */

  function boot() {
    // The language and appearance menus are created here rather than in
    // index.html so the markup stays short and the items always match the packs
    // that actually exist.
    var toolbar = document.getElementById('toolbar');
    if (toolbar && !document.getElementById('language-menu')) {
      languageMenu = document.createElement('div');
      languageMenu.id = 'language-menu';
      languageMenu.className = 'lang-menu';
      languageMenu.setAttribute('role', 'menu');
      languageMenu.hidden = true;
      toolbar.appendChild(languageMenu);
    } else {
      languageMenu = document.getElementById('language-menu');
    }
    if (toolbar && !document.getElementById('theme-menu')) {
      var themeMenu = document.createElement('div');
      themeMenu.id = 'theme-menu';
      themeMenu.className = 'lang-menu';
      themeMenu.setAttribute('role', 'menu');
      themeMenu.hidden = true;
      toolbar.appendChild(themeMenu);
    }

    if (I18n) {
      I18n.applyToDom(document);
      setLanguage(I18n.language(), false);
    }

    editor = new ColaiEditor(document.getElementById('editor'), document.getElementById('source-view'));
    editor.onChange = onEditorChange;
    editor.onCaret = onEditorCaret;
    editor.onStatus = onEditorStatus;

    ColaiPanels.init({
      onPolicyChange: function (patch) {
        post({ type: 'set_policy', policy: patch });
      },
      onResolvePending: onResolvePending,
      onRefreshAudit: function () { post({ type: 'audit_query', limit: 60, include_diff: true }); },
      onRollbackLast: function () {
        ColaiPanels.toast(t('audit.rollingBack'), t('audit.rollingBackBody'), '', 2000);
        post({ type: 'rollback_last' });
      },
      // Endpoint pinning: the host stores the port/token so the URL an Agent was
      // configured with survives a restart.
      onMcpSettings: function (patch) {
        post({ type: 'mcp_settings', port: patch.port, keep_token: patch.keep_token,
               regenerate_token: patch.regenerate_token });
      },
    });

    wireToolbar();
    wireFormatbar();
    document.addEventListener('keydown', onKeyDown, true);

    if (global.chrome && global.chrome.webview) {
      host = global.chrome.webview;
      host.addEventListener('message', receive);
    } else {
      // Running in a plain browser (development): show a helpful placeholder.
      editor.setDocument(
        '# Colaimdation\n\n' +
        'This page is the editor front-end. It is meant to run inside the\n' +
        'Colaimdation desktop shell, which owns the document and the MCP server.\n\n' +
        'Start `colaimdation.exe` and the same interface will load with your file.\n',
        { revision: 'dev', dirty: false });
      ColaiPanels.toast(t('toast.noHost'), t('toast.noHostBody'), 'error');
    }

    // Behave sensibly before the host's bootstrap arrives: start from the saved
    // preference in the page (localStorage is not used - the shell owns settings)
    // by resolving "auto" against the operating system.
    applyTheme(themePreference, false);
    if (global.matchMedia) {
      var darkMedia = global.matchMedia('(prefers-color-scheme: dark)');
      var followSystem = function () {
        if (themePreference === 'auto') applyTheme('auto', false);
      };
      if (darkMedia.addEventListener) darkMedia.addEventListener('change', followSystem);
      else if (darkMedia.addListener) darkMedia.addListener(followSystem);
    }
    updateUndoRedo({ can_undo: false, can_redo: false });
    onEditorStatus({ caret: 0, line: 1, column: 1, selectionLength: 0, characters: 0, lines: 0, words: 0, blocks: 0, dirty: false });

    post({ type: 'ready' });
    post({ type: 'tabs_query' });
    setTimeout(function () { post({ type: 'status_query' }); }, 400);
    setInterval(function () { post({ type: 'status_query' }); }, 15000);
  }

  // ---------------------------------------------------------------- print / export
  //
  // Printing, the PDF export and the picture export all show the *document*: the
  // editor stops showing the caret's block as Markdown (the caret block is source
  // while writing), the chrome is hidden and the page is laid out on paper by the
  // stylesheet, so nothing is cut off at the fold and no UI ends up on the page.
  // Paper is light even when the interface is dark: a dark page prints as a black
  // sheet, and the syntax colours would be the dark theme's light palette on white.
  var exportTheme = null;

  function exportPrepare(mode) {
    if (!editor) return { width: 0, height: 0 };
    if (!exportTheme) exportTheme = themePreference;
    editor.setExporting(true);
    document.body.classList.add('exporting');
    document.body.classList.toggle('export-image', mode === 'image');
    document.documentElement.setAttribute('data-theme', 'light');
    var light = document.getElementById('hljs-light');
    var dark = document.getElementById('hljs-dark');
    if (light) light.disabled = false;
    if (dark) dark.disabled = true;
    if (global.ColaiRender) global.ColaiRender.refreshMermaidTheme();
    // The size the page needs now that the chrome is out of the way; the shell uses it
    // to size the window surface that a picture export captures. Measured on the
    // editor itself: the document is what is being exported, not the window.
    var editorElement = document.getElementById('editor');
    var height = editorElement ? Math.ceil(editorElement.getBoundingClientRect().height) : 0;
    return {
      width: Math.ceil(document.documentElement.scrollWidth),
      height: Math.max(height, 200),
    };
  }

  function exportFinish() {
    document.body.classList.remove('exporting', 'export-image');
    if (editor) editor.setExporting(false);
    if (exportTheme !== null) {
      var preference = exportTheme;
      exportTheme = null;
      applyTheme(preference, false);
    }
  }

  global.colaimdation = {
    resize: function () { if (editor) editor.resize(); },
    focus: function () { if (editor) editor.focus(); },
    shutdown: function () { if (editor) editor.shutdown(); },
    devtools: function () { post({ type: 'command', command: 'devtools' }); },
    editor: function () { return editor; },
    setLanguage: function (tag) { setLanguage(tag, true); },
    exportPrepare: exportPrepare,
    exportFinish: exportFinish,
    // The print dialog is the engine's own; window.print() blocks until it closes, so
    // the paper layout can be put back straight afterwards.
    printDocument: function () {
      var size = exportPrepare('print');
      try {
        if (global.print) global.print();
      } finally {
        exportFinish();
      }
      return size;
    },
    exporting: function () { return document.body.classList.contains('exporting'); },
  };

  // A print started any other way (a menu of the engine, Ctrl+P when the shell does
  // not intercept it) still needs the same preparation.
  if (global.addEventListener) {
    global.addEventListener('beforeprint', function () { exportPrepare('print'); });
    global.addEventListener('afterprint', function () { exportFinish(); });
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', boot);
  } else {
    boot();
  }
})(window);
