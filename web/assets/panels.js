/* Colaimdation - side panels: Agent controls, audit log, diff confirmation. */
(function (global) {
  'use strict';

  function t(key, args) {
    return global.ColaiI18n ? global.ColaiI18n.t(key, args) : key;
  }

  var state = {
    activeTab: 'agent',
    pending: null,
    pendingText: '',
    endpoint: '',
    bridgePath: 'colaimdation_mcp_bridge.exe',
    activity: [],
    onPolicyChange: null,
    onCopy: null,
    onRollbackLast: null,
    onRefreshAudit: null,
    onResolvePending: null,
  };

  function $(id) { return document.getElementById(id); }

  /* ------------------------------------------------------------------ toasts */
  function toast(title, body, kind, timeout) {
    var stack = $('toast-stack');
    if (!stack) return null;
    var node = document.createElement('div');
    node.className = 'toast' + (kind ? ' ' + kind : '');
    var titleNode = document.createElement('div');
    titleNode.className = 'toast-title';
    titleNode.textContent = title;
    node.appendChild(titleNode);
    if (body) {
      var bodyNode = document.createElement('div');
      bodyNode.className = 'toast-body';
      bodyNode.textContent = body;
      node.appendChild(bodyNode);
    }
    stack.appendChild(node);
    setTimeout(function () {
      if (node.parentNode) node.parentNode.removeChild(node);
    }, timeout || (kind === 'error' ? 9000 : 4500));
    return node;
  }

  /* -------------------------------------------------------------------- tabs */
  function showTab(name) {
    state.activeTab = name;
    ['agent', 'audit', 'preview'].forEach(function (tab) {
      var body = $('tab-' + tab);
      if (body) body.hidden = tab !== name;
    });
    Array.prototype.forEach.call(document.querySelectorAll('.panel-tab'), function (button) {
      button.classList.toggle('active', button.getAttribute('data-tab') === name);
    });
    var panel = $('side-panel');
    if (panel) panel.hidden = false;
    syncToolbar();
  }

  function closePanel() {
    var panel = $('side-panel');
    if (panel) panel.hidden = true;
    syncToolbar();
  }

  function togglePanel(name) {
    var panel = $('side-panel');
    if (panel && !panel.hidden && state.activeTab === name) {
      closePanel();
      return;
    }
    showTab(name);
  }

  function syncToolbar() {
    var panel = $('side-panel');
    var open = panel && !panel.hidden;
    var agentButton = $('btn-agent');
    var auditButton = $('btn-audit');
    if (agentButton) agentButton.classList.toggle('on', !!open && state.activeTab === 'agent');
    if (auditButton) auditButton.classList.toggle('on', !!open && state.activeTab === 'audit');
  }

  /* --------------------------------------------------------------- diff view */
  // Applies the staged patches so the panel can show a real diff. The host
  // validates and applies the change for real; this is display only.
  function simulatePatches(text, patches) {
    var out = String(text == null ? '' : text);
    (patches || []).forEach(function (patch) {
      var op = patch.op;
      var replacement = patch.text == null ? '' : String(patch.text);
      if (op === 'append_section') {
        var payload = '';
        if (out.length) {
          if (out.charAt(out.length - 1) !== '\n') payload += '\n';
          payload += '\n';
        }
        payload += replacement;
        if (!payload.length || payload.charAt(payload.length - 1) !== '\n') payload += '\n';
        out += payload;
      } else if (op === 'insert_text') {
        var at = Math.max(0, Math.min(Number(patch.byte_start) || 0, out.length));
        out = out.slice(0, at) + replacement + out.slice(at);
      } else if (op === 'replace_range') {
        var begin = Math.max(0, Math.min(Number(patch.byte_start) || 0, out.length));
        var end = Math.max(begin, Math.min(Number(patch.byte_end) || 0, out.length));
        out = out.slice(0, begin) + replacement + out.slice(end);
      } else if (op === 'delete_range') {
        var dbegin = Math.max(0, Math.min(Number(patch.byte_start) || 0, out.length));
        var dend = Math.max(dbegin, Math.min(Number(patch.byte_end) || 0, out.length));
        out = out.slice(0, dbegin) + out.slice(dend);
      } else if (op === 'set_text') {
        out = replacement;
      }
      // Structural ops (replace_block / insert_block) are applied by the host;
      // the diff below falls back to the host's unified diff for those.
    });
    return out;
  }

  function lineDiff(before, after) {
    var a = String(before).split('\n');
    var b = String(after).split('\n');
    var prefix = 0;
    while (prefix < a.length && prefix < b.length && a[prefix] === b[prefix]) prefix++;
    var suffix = 0;
    while (suffix < a.length - prefix && suffix < b.length - prefix &&
           a[a.length - 1 - suffix] === b[b.length - 1 - suffix]) suffix++;
    var lines = [];
    for (var i = 0; i < prefix; i++) lines.push({ op: ' ', text: a[i] });
    for (var d = prefix; d < a.length - suffix; d++) lines.push({ op: '-', text: a[d] });
    for (var n = prefix; n < b.length - suffix; n++) lines.push({ op: '+', text: b[n] });
    for (var s = a.length - suffix; s < a.length; s++) lines.push({ op: ' ', text: a[s] });
    return lines;
  }

  function renderDiff(container, lines) {
    container.innerHTML = '';
    if (!lines.length) {
      var empty = document.createElement('div');
      empty.className = 'diff-empty';
      empty.textContent = t('diff.noDifferences');
      container.appendChild(empty);
      return;
    }
    lines.forEach(function (line) {
      var row = document.createElement('div');
      row.className = 'diff-line' + (line.op === '+' ? ' add' : (line.op === '-' ? ' del' : ''));
      var mark = document.createElement('span');
      mark.className = 'dl-mark';
      mark.textContent = line.op;
      var text = document.createElement('span');
      text.textContent = line.text;
      row.appendChild(mark);
      row.appendChild(text);
      container.appendChild(row);
    });
  }

  function showConfirmation(pending, currentText) {
    state.pending = pending;
    // Remember the document text this preview was built from: the language
    // switch re-renders the diff, and without it the panel recomputed the
    // preview against an empty document - every offset clamped to 0 and the
    // reviewer was told the whole document is being replaced.
    state.pendingText = String(currentText == null ? '' : currentText);
    var simulated = simulatePatches(state.pendingText, pending.patches);
    var lines;
    if (simulated === state.pendingText && pending.diff) {
      // The preview could not be reproduced locally: show the host's unified diff.
      lines = String(pending.diff).split('\n').map(function (line) {
        var op = line.charAt(0) === '+' && line.slice(0, 3) !== '+++' ? '+'
          : (line.charAt(0) === '-' && line.slice(0, 3) !== '---' ? '-' : ' ');
        return { op: op, text: line.replace(/^[+\- ]/, '') };
      });
    } else {
      lines = lineDiff(state.pendingText, simulated);
    }

    var title = $('preview-title');
    if (title) title.textContent = t('diff.title');
    var meta = $('preview-meta');
    if (meta) {
      var parts = [];
      if (pending.session) parts.push(t('diff.session', [pending.session]));
      if (pending.tool) parts.push(t('diff.tool', [pending.tool]));
      if (pending.summary) parts.push(pending.summary);
      var adds = lines.filter(function (l) { return l.op === '+'; }).length;
      var dels = lines.filter(function (l) { return l.op === '-'; }).length;
      parts.push(t('diff.meta', [adds, dels]));
      meta.textContent = parts.join(' \u00b7 ');
    }
    var view = $('diff-view');
    if (view) renderDiff(view, lines);
    showTab('preview');
    var banner = $('pending-banner');
    var bannerText = $('pending-text');
    if (bannerText) {
      var summary = pending.summary || pending.tool || t('diff.pendingFallback');
      bannerText.textContent = t('diff.banner', [summary]);
    }
    if (banner) banner.hidden = false;
  }

  function clearPending() {
    state.pending = null;
    state.pendingText = '';
    var banner = $('pending-banner');
    if (banner) banner.hidden = true;
    var view = $('diff-view');
    if (view) view.innerHTML = '';
    if (state.activeTab === 'preview') showTab('agent');
  }

  function resolvePending(accept) {
    if (!state.pending) return;
    var id = state.pending.id;
    clearPending();
    if (state.onResolvePending) state.onResolvePending(id, !!accept);
  }

  /* ------------------------------------------------------------- activity log */
  function addActivity(entry) {
    state.activity.unshift(entry);
    if (state.activity.length > 200) state.activity.pop();
    renderActivity();
  }

  // Activity entries carry a key plus arguments rather than a rendered string so
  // a language switch re-renders the log instead of leaving stale text behind.
  function activityText(entry) {
    if (entry.key) return t(entry.key, entry.args || []);
    return entry.message || '';
  }

  function renderActivity() {
    var list = $('activity-list');
    if (!list) return;
    list.innerHTML = '';
    if (!state.activity.length) {
      var empty = document.createElement('li');
      empty.textContent = t('agent.noActivity');
      list.appendChild(empty);
      return;
    }
    state.activity.slice(0, 60).forEach(function (entry) {
      var item = document.createElement('li');
      if (entry.time) {
        var time = document.createElement('span');
        time.className = 'act-time';
        time.textContent = entry.time;
        item.appendChild(time);
      }
      if (entry.tool) {
        var tool = document.createElement('span');
        tool.className = 'act-tool';
        tool.textContent = entry.tool;
        item.appendChild(tool);
        item.appendChild(document.createTextNode(' '));
      }
      var message = document.createElement('span');
      message.className = entry.error ? 'act-err' : '';
      message.textContent = activityText(entry);
      item.appendChild(message);
      list.appendChild(item);
    });
  }

  /* -------------------------------------------------------------- audit view */
  function setAudit(entries, meta) {
    var list = $('audit-list');
    if (!list) return;
    list.innerHTML = '';
    if (!entries || !entries.length) {
      var empty = document.createElement('div');
      empty.className = 'diff-empty';
      empty.textContent = t('audit.empty');
      list.appendChild(empty);
      return;
    }
    entries.forEach(function (entry) {
      var node = document.createElement('div');
      node.className = 'audit-entry';

      var head = document.createElement('div');
      head.className = 'ae-head';
      var source = document.createElement('span');
      source.className = 'ae-source ' + (entry.source || 'system');
      source.textContent = entry.source || 'system';
      head.appendChild(source);
      var tool = document.createElement('span');
      tool.className = 'ae-tool';
      tool.textContent = entry.tool || '-';
      head.appendChild(tool);
      var time = document.createElement('span');
      time.className = 'ae-time';
      time.textContent = (entry.timestamp || '').replace('T', ' ').replace('Z', '').slice(0, 19);
      head.appendChild(time);
      node.appendChild(head);

      var summary = document.createElement('div');
      summary.className = 'ae-summary';
      summary.textContent = entry.summary || '';
      node.appendChild(summary);

      var status = document.createElement('div');
      status.className = 'ae-status ' + (entry.status || '');
      var delta = entry.byte_delta || 0;
      status.textContent = (entry.status || '') +
        (delta ? '  ' + (delta > 0 ? '+' : '') + delta + ' bytes' : '') +
        (entry.error ? '  ' + (entry.error.code || 'error') + ': ' + (entry.error.message || '') : '');
      node.appendChild(status);

      if (entry.diff) {
        var pre = document.createElement('pre');
        pre.className = 'diff';
        pre.style.marginTop = '8px';
        pre.textContent = entry.diff;
        node.appendChild(pre);
      }
      list.appendChild(node);
    });
    if (meta) {
      var footer = document.createElement('p');
      footer.className = 'hint';
      footer.textContent = meta;
      list.appendChild(footer);
    }
  }

  var lastAudit = null;

  /* ------------------------------------------------------------------ policy */
  var POLICY_FIELDS = [
    ['policy-write', 'allow_write'],
    ['policy-confirm', 'require_confirmation'],
    ['policy-frontmatter', 'protect_front_matter'],
    ['policy-save', 'allow_save'],
    ['policy-rollback', 'allow_rollback'],
    ['policy-caret', 'allow_selection_control'],
  ];

  function setPolicy(policy) {
    POLICY_FIELDS.forEach(function (pair) {
      var input = $(pair[0]);
      if (input) input.checked = !!policy[pair[1]];
    });
    var directory = $('policy-directory');
    if (directory) directory.value = policy.allowed_directory || '';
  }

  function wirePolicyInputs() {
    POLICY_FIELDS.forEach(function (pair) {
      var input = $(pair[0]);
      if (!input) return;
      input.addEventListener('change', function () {
        if (!state.onPolicyChange) return;
        var patch = {};
        patch[pair[1]] = input.checked;
        state.onPolicyChange(patch);
      });
    });
    var directory = $('policy-directory');
    if (directory) {
      directory.addEventListener('change', function () {
        if (state.onPolicyChange) state.onPolicyChange({ allowed_directory: directory.value.trim() });
      });
    }
  }

  /* --------------------------------------------------------------------- api */
  function init(options) {
    state.onPolicyChange = options.onPolicyChange || null;
    state.onResolvePending = options.onResolvePending || null;
    state.onCopy = options.onCopy || null;
    state.onRollbackLast = options.onRollbackLast || null;
    state.onRefreshAudit = options.onRefreshAudit || null;
    state.onMcpSettings = options.onMcpSettings || null;

    Array.prototype.forEach.call(document.querySelectorAll('.panel-tab'), function (button) {
      button.addEventListener('click', function () { showTab(button.getAttribute('data-tab')); });
    });
    var close = $('panel-close');
    if (close) close.addEventListener('click', closePanel);

    var accept = $('btn-accept-change');
    if (accept) accept.addEventListener('click', function () { resolvePending(true); });
    var reject = $('btn-reject-change');
    if (reject) reject.addEventListener('click', function () { resolvePending(false); });
    var review = $('btn-review');
    if (review) review.addEventListener('click', function () { showTab('preview'); });

    var copyEndpoint = $('btn-copy-endpoint');
    if (copyEndpoint) copyEndpoint.addEventListener('click', function () { copyField('endpoint-input'); });
    var copyBridge = $('btn-copy-bridge');
    if (copyBridge) copyBridge.addEventListener('click', function () { copyField('bridge-input'); });

    var refresh = $('btn-audit-refresh');
    if (refresh) refresh.addEventListener('click', function () { if (state.onRefreshAudit) state.onRefreshAudit(); });
    var rollback = $('btn-rollback-last');
    if (rollback) rollback.addEventListener('click', function () { if (state.onRollbackLast) state.onRollbackLast(); });

    // Endpoint controls: pin the port and/or the token so an Agent configured once
    // keeps working after a restart.
    var portSave = $('btn-mcp-port');
    if (portSave) {
      portSave.addEventListener('click', function () {
        var field = $('mcp-port-input');
        var port = field ? parseInt(field.value, 10) : 0;
        if (!isFinite(port) || port < 0 || port > 65535) {
          toast(t('agent.portInvalid'), t('agent.portInvalidBody'), 'error');
          return;
        }
        if (state.onMcpSettings) state.onMcpSettings({ port: port });
        toast(t('agent.portSaved'), t('agent.portSavedBody', [String(port)]), 'ok');
      });
    }
    var keepToken = $('mcp-keep-token');
    if (keepToken) {
      keepToken.addEventListener('change', function () {
        if (state.onMcpSettings) state.onMcpSettings({ keep_token: !!keepToken.checked });
      });
    }
    var newToken = $('btn-mcp-new-token');
    if (newToken) {
      newToken.addEventListener('click', function () {
        if (state.onMcpSettings) state.onMcpSettings({ regenerate_token: true });
      });
    }

    wirePolicyInputs();
    renderActivity();

    // Re-render everything that is built in code when the language changes.
    if (global.ColaiI18n) {
      global.ColaiI18n.onLanguageChange(function () {
        renderActivity();
        reRenderAuditFooter();
        if (state.pending) showConfirmation(state.pending, state.pendingText || '');
        syncToolbar();
      });
    }
  }

  function copyField(id) {
    var field = $(id);
    if (!field || !field.value) return;
    if (navigator.clipboard && navigator.clipboard.writeText) {
      navigator.clipboard.writeText(field.value).then(function () {
        toast(t('toast.copied'), t('toast.copiedBody'), 'ok');
      }, function () {
        field.select();
        toast(t('toast.copyManually'), t('toast.copyManuallyBody'), 'error');
      });
    } else {
      field.select();
      document.execCommand('copy');
      toast(t('toast.copied'), '', 'ok');
    }
  }

  function setEndpoint(endpoint, bridgePath, status) {
    state.endpoint = endpoint || '';
    var field = $('endpoint-input');
    if (field) field.value = state.endpoint;
    if (bridgePath) {
      state.bridgePath = bridgePath;
      var bridge = $('bridge-input');
      if (bridge) bridge.value = bridgePath;
    }
    var badge = $('mcp-badge');
    var badgeText = $('mcp-badge-text');
    var listening = status && status.listening;
    if (badge) {
      badge.classList.toggle('on', !!listening);
      badge.classList.toggle('off', !listening);
      badge.title = listening ? t('mcp.listening', [endpoint]) : t('mcp.notRunning');
    }
    if (badgeText) {
      badgeText.textContent = listening ? t('mcp.badge') + ' :' + status.port : t('mcp.off');
    }
    if (status) {
      var tools = $('stat-tools');
      if (tools) tools.textContent = status.tools != null ? status.tools : '-';
      var requests = $('stat-requests');
      if (requests) requests.textContent = status.requests != null ? status.requests : '-';
      var calls = $('stat-calls');
      if (calls) calls.textContent = status.tool_calls != null ? status.tool_calls : '-';
      var sessions = $('stat-sessions');
      if (sessions) sessions.textContent = status.sessions != null ? status.sessions : '-';
    }
  }

  // Reflects the saved/effective endpoint settings in the Agent panel: the port the
  // editor will use next time, whether the token is pinned, and which port is live
  // right now.
  function setMcpSettings(settings) {
    if (!settings) return;
    var field = $('mcp-port-input');
    if (field && document.activeElement !== field) {
      field.value = String(settings.port != null ? settings.port : 0);
    }
    var keep = $('mcp-keep-token');
    if (keep) keep.checked = !!settings.keep_token;
    var hint = $('agent-endpoint-hint');
    if (hint) {
      var text = settings.keep_token
        ? t('agent.endpointPinned', [String(settings.active_port || settings.port || 0)])
        : t('agent.endpointEphemeral', [String(settings.active_port || settings.port || 0)]);
      // A non-fatal note from the host (for example "that port was busy").
      if (settings.notice) text += ' ' + settings.notice;
      hint.textContent = text;
    }
  }

  // The audit footer is built from a translated template, so keep the raw
  // numbers around and format it on demand - storing the already-formatted
  // string kept the previous language after a switch.
  function setAuditWithMeta(entries, count, logPath) {
    lastAudit = { entries: entries, count: count, logPath: logPath || '-' };
    setAudit(entries, t('audit.footer', [count, lastAudit.logPath]));
  }

  function reRenderAuditFooter() {
    if (!lastAudit) return;
    setAudit(lastAudit.entries, t('audit.footer', [lastAudit.count, lastAudit.logPath]));
  }

  global.ColaiPanels = {
    init: init,
    showTab: showTab,
    closePanel: closePanel,
    togglePanel: togglePanel,
    showConfirmation: showConfirmation,
    clearPending: clearPending,
    pending: function () { return state.pending; },
    setAudit: setAuditWithMeta,
    setPolicy: setPolicy,
    setEndpoint: setEndpoint,
    setMcpSettings: setMcpSettings,
    addActivity: addActivity,
    renderActivity: renderActivity,
    toast: toast,
    simulatePatches: simulatePatches,
    lineDiff: lineDiff,
  };
})(window);
