/* Colaimdation - front-end translations.
 *
 * The language packs are embedded as JS objects rather than fetched: the page
 * runs under a strict CSP with `connect-src 'none'`, and a desktop editor should
 * not need a single network round trip to draw its own toolbar.
 *
 * Usage:
 *   ColaiI18n.setLanguage('zh-Hans');
 *   ColaiI18n.t('toast.copied')                  // -> "已复制"
 *   ColaiI18n.t('audit.footer', [12, 'a.jsonl'])  // -> "共 12 条记录 · 日志：a.jsonl"
 *   ColaiI18n.applyToDom(document)                // rewrites everything tagged
 *                                                 // with data-i18n / data-i18n-title
 */
(function (global) {
  'use strict';

  /* ====================================================================== */
  /* Language metadata                                                      */
  /* ====================================================================== */
  var LANGUAGES = [
    { tag: 'en', englishName: 'English', nativeName: 'English' },
    { tag: 'zh-Hans', englishName: 'Chinese (Simplified)', nativeName: '\u7b80\u4f53\u4e2d\u6587' },
    { tag: 'ja', englishName: 'Japanese', nativeName: '\u65e5\u672c\u8a9e' }
  ];
  var FALLBACK = 'en';

  /* ====================================================================== */
  /* Packs                                                                  */
  /* ====================================================================== */
  var PACKS = {
    en: {
      'brand.tagline': 'Local WYSIWYG Markdown with an embedded MCP server',

      'toolbar.new': 'New',
      'toolbar.open': 'Open',
      'toolbar.save': 'Save',
      'toolbar.saveAs': 'Save as',
      'toolbar.undo': 'Undo',
      'toolbar.redo': 'Redo',
      'toolbar.source': 'Source',
      'toolbar.agent': 'Agent',
      'toolbar.audit': 'Audit',
      'toolbar.copyEndpoint': 'Copy endpoint',
      'toolbar.language': 'Language',
      'toolbar.theme': 'Appearance (light / dark / follow the system)',

      'theme.light': 'Light',
      'theme.dark': 'Dark',
      'theme.auto': 'Follow system',

      'tab.list': 'Open documents',
      'tab.new': 'New tab (Ctrl+N)',
      'tab.close': 'Close tab (Ctrl+W)',

      'format.paragraph': 'Start a new paragraph',
      'format.paragraphShort': '\u00b6',
      'format.h1': 'Heading 1',
      'format.h2': 'Heading 2',
      'format.h3': 'Heading 3',
      'format.bullet': 'Bullet list',
      'format.number': 'Numbered list',
      'format.task': 'Task list',
      'format.quote': 'Quote',
      'format.code': 'Code block',
      'format.divider': 'Divider',
      'format.table': 'Insert a table',
      'format.row': 'Add a row to the table',
      'format.rowShort': 'Row +',
      'format.column': 'Add a column to the table',
      'format.columnShort': 'Col +',
      'format.bold': 'Bold',
      'format.italic': 'Italic',
      'format.inlineCode': 'Inline code',
      'format.link': 'Insert a link',
      'format.linkShort': 'Link',
      'format.image': 'Insert an image',
      'format.imageShort': 'Image',
      'format.formula': 'Insert a formula',
      'format.formulaShort': '\u2211',
      'format.linkText': 'link text',
      'format.linkUrl': 'url',
      'format.imageAlt': 'image',
      'format.imagePath': 'path/to/image.png',
      'format.formulaText': 'formula',
      'format.tableHeader': 'Column {0}',

      'title.new': 'New document (Ctrl+N)',
      'title.open': 'Open .md (Ctrl+O)',
      'title.save': 'Save (Ctrl+S)',
      'title.saveAs': 'Save as (Ctrl+Shift+S)',
      'title.undo': 'Undo (Ctrl+Z)',
      'title.redo': 'Redo (Ctrl+Y)',
      'title.source': 'Show raw Markdown for the whole document (Ctrl+/)',
      'title.agent': 'Agent panel (Ctrl+Shift+A)',
      'title.audit': 'Audit log (Ctrl+Shift+L)',
      'title.copyEndpoint': 'Copy the MCP endpoint',
      'title.mcpBadge': 'MCP server status',
      'title.editor': 'Markdown document',
      'title.sourceView': 'Markdown source',

      'mcp.badge': 'MCP',
      'mcp.off': 'MCP off',
      'mcp.listening': 'MCP server listening on {0}',
      'mcp.notRunning': 'MCP server is not running',

      'panel.agent': 'Agent',
      'panel.audit': 'Audit',
      'panel.diff': 'Diff',
      'panel.close': 'Close panel',
      'panel.tabs': 'Panel sections',

      'agent.connection': 'Connection',
      'agent.connectionHint': 'Any MCP-capable Agent (OpenClaw, DeepSeek Harness, WorkBuddy, ...) can connect to this endpoint. Nothing else on this machine can, unless it has the token.',
      'agent.endpointLabel': 'MCP endpoint (contains the local token)',
      'agent.bridgeLabel': 'stdio bridge command (for hosts that launch a process)',
      'agent.copy': 'Copy',
      'agent.statTools': 'Tools',
      'agent.statRequests': 'Requests',
      'agent.statCalls': 'Tool calls',
      'agent.statSessions': 'Sessions',
      'agent.permissions': 'Permissions',
      'agent.allowWrite': 'Allow agents to modify this document',
      'agent.requireConfirmation': 'Require diff confirmation for every agent change',
      'agent.protectFrontMatter': 'Protect YAML front matter',
      'agent.allowSave': 'Allow agents to save to disk',
      'agent.portLabel': 'MCP port',
      'agent.portTitle': '0 = pick a free port and remember it',
      'agent.portSave': 'Save',
      'agent.portInvalid': 'That is not a port',
      'agent.portInvalidBody': 'Use a number between 0 and 65535 (0 = automatic).',
      'agent.portSaved': 'Port saved',
      'agent.portSavedBody': 'The editor will use port {0} from the next start on.',
      'agent.keepToken': 'Keep the token across restarts (same endpoint every time)',
      'agent.newToken': 'New token',
      'agent.endpointPinned': 'Pinned: this endpoint stays valid after a restart (port {0}).',
      'agent.endpointEphemeral': 'Port {0} is remembered, but a new token is generated on every start.',
      'agent.allowRollback': 'Allow agents to roll back changes',
      'agent.allowCaret': 'Allow agents to move the caret / selection',
      'agent.allowedDirectory': 'Only allow saving inside this directory (empty = unset)',
      'agent.activity': 'Activity',
      'agent.noActivity': 'No agent activity yet.',

      'audit.refresh': 'Refresh',
      'audit.rollbackLast': 'Rollback last agent change',
      'audit.hint': 'Every change is recorded with who made it, which tool, and what changed. Rolling back creates a new audited edit, so nothing is ever lost.',
      'audit.empty': 'No audit entries yet.',
      'audit.footer': '{0} entries recorded \u00b7 log: {1}',
      'audit.noAgentChanges': 'No agent changes to roll back',
      'audit.rollingBack': 'Rolling back',
      'audit.rollingBackBody': 'Reverting the last agent change...',

      'diff.title': 'Agent proposes a change',
      'diff.noDifferences': 'No line-level differences.',
      'diff.meta': '+{0} / -{1} lines',
      'diff.session': 'session {0}',
      'diff.tool': 'tool {0}',
      'diff.apply': 'Apply change',
      'diff.reject': 'Reject',
      'diff.banner': 'An Agent is waiting for your confirmation: {0}',
      'diff.review': 'Review diff',
      'diff.pendingFallback': 'proposed change',

      'toast.copied': 'Copied',
      'toast.copiedBody': 'The value is on your clipboard.',
      'toast.copyManually': 'Copy manually',
      'toast.copyManuallyBody': 'Select the field and press Ctrl+C.',
      'toast.sourceMode': 'Source mode',
      'toast.sourceModeBody': 'The whole document is shown as raw Markdown.',
      'toast.wysiwygMode': 'WYSIWYG mode',
      'toast.wysiwygModeBody': 'Your caret line shows the source, the rest renders.',
      'toast.changeApplied': 'Change applied',
      'toast.changeAppliedBody': 'The agent proposal is now part of the document.',
      'toast.changeRejected': 'Change rejected',
      'toast.changeRejectedBody': 'The agent proposal was discarded.',
      'toast.noHost': 'No host bridge',
      'toast.noHostBody': 'Running without the desktop shell: editing is local to this page.',
      'toast.toolFailed': 'Tool call failed',
      'toast.saved': 'Saved',
      'toast.languageChanged': 'Language changed',
      'toast.languageChangedBody': 'The interface is now in {0}.',

      'status.untitled': 'untitled.md',
      'status.notSaved': 'not saved yet',
      'status.rev': 'rev {0}',
      'status.unsaved': 'unsaved',
      'status.mode': 'WYSIWYG',
      'status.modeSource': 'SOURCE',
      'status.caret': 'ln {0}, col {1}',
      'status.selection': 'sel {0} ch',
      'status.counts': '{0} words \u00b7 {1} chars \u00b7 {2} blocks',
      'status.documentPath': 'Document path',
      'status.revision': 'Document revision',
      'status.caretTitle': 'Caret line and column',
      'status.countsTitle': 'Word, character and block counts',
      'status.modeTitle': 'Current mode',

      'editor.empty': 'Start writing, or open a Markdown file.',
      'editor.mermaidError': 'Mermaid: {0}',

      'activity.agentRead': 'agent {0} read the document',
      'activity.documentUpdated': 'document updated by an agent',
      'activity.unknown': 'unknown',
      'activity.ok': 'ok',
      'activity.failed': 'failed'
    },

    'zh-Hans': {
      'brand.tagline': '\u672c\u5730\u539f\u5730\u5b9e\u65f6\u6e32\u67d3 Markdown\uff0c\u5185\u7f6e MCP \u670d\u52a1',

      'toolbar.new': '\u65b0\u5efa',
      'toolbar.open': '\u6253\u5f00',
      'toolbar.save': '\u4fdd\u5b58',
      'toolbar.saveAs': '\u53e6\u5b58\u4e3a',
      'toolbar.undo': '\u64a4\u9500',
      'toolbar.redo': '\u91cd\u505a',
      'toolbar.source': '\u6e90\u7801',
      'toolbar.agent': 'Agent',
      'toolbar.audit': '\u5ba1\u8ba1',
      'toolbar.copyEndpoint': '\u590d\u5236\u7aef\u70b9',
      'toolbar.language': '\u8bed\u8a00',
      'toolbar.theme': '\u5916\u89c2\uff08\u6d45\u8272 / \u6df1\u8272 / \u8ddf\u968f\u7cfb\u7edf\uff09',

      'theme.light': '浅色',
      'theme.dark': '深色',
      'theme.auto': '跟随系统',

      'tab.list': '\u6253\u5f00\u7684\u6587\u6863',
      'tab.new': '\u65b0\u5efa\u6807\u7b7e\u9875 (Ctrl+N)',
      'tab.close': '\u5173\u95ed\u6807\u7b7e\u9875 (Ctrl+W)',

      'format.paragraph': '新起一段',
      'format.paragraphShort': '¶',
      'format.h1': '一级标题',
      'format.h2': '二级标题',
      'format.h3': '三级标题',
      'format.bullet': '无序列表',
      'format.number': '编号列表',
      'format.task': '任务清单',
      'format.quote': '引用',
      'format.code': '代码块',
      'format.divider': '分隔线',
      'format.table': '插入表格',
      'format.row': '在表格中加一行',
      'format.rowShort': '行 +',
      'format.column': '在表格中加一列',
      'format.columnShort': '列 +',
      'format.bold': '粗体',
      'format.italic': '斜体',
      'format.inlineCode': '行内代码',
      'format.link': '插入链接',
      'format.linkShort': '链接',
      'format.image': '插入图片',
      'format.imageShort': '图片',
      'format.formula': '插入公式',
      'format.formulaShort': '公式',
      'format.linkText': '链接文字',
      'format.linkUrl': '链接地址',
      'format.imageAlt': '图片说明',
      'format.imagePath': '图片路径',
      'format.formulaText': '公式',
      'format.tableHeader': '列 {0}',

      'title.new': '\u65b0\u5efa\u6587\u6863 (Ctrl+N)',
      'title.open': '\u6253\u5f00 .md \u6587\u4ef6 (Ctrl+O)',
      'title.save': '\u4fdd\u5b58 (Ctrl+S)',
      'title.saveAs': '\u53e6\u5b58\u4e3a (Ctrl+Shift+S)',
      'title.undo': '\u64a4\u9500 (Ctrl+Z)',
      'title.redo': '\u91cd\u505a (Ctrl+Y)',
      'title.source': '\u6574\u7bc7\u663e\u793a Markdown \u6e90\u7801 (Ctrl+/)',
      'title.agent': 'Agent \u9762\u677f (Ctrl+Shift+A)',
      'title.audit': '\u5ba1\u8ba1\u65e5\u5fd7 (Ctrl+Shift+L)',
      'title.copyEndpoint': '\u590d\u5236 MCP \u7aef\u70b9',
      'title.mcpBadge': 'MCP \u670d\u52a1\u72b6\u6001',
      'title.editor': 'Markdown \u6587\u6863',
      'title.sourceView': 'Markdown \u6e90\u7801',

      'mcp.badge': 'MCP',
      'mcp.off': 'MCP \u672a\u8fd0\u884c',
      'mcp.listening': 'MCP \u670d\u52a1\u6b63\u5728\u76d1\u542c {0}',
      'mcp.notRunning': 'MCP \u670d\u52a1\u672a\u8fd0\u884c',

      'panel.agent': 'Agent',
      'panel.audit': '\u5ba1\u8ba1',
      'panel.diff': '\u5dee\u5f02',
      'panel.close': '\u5173\u95ed\u9762\u677f',
      'panel.tabs': '\u9762\u677f\u5206\u533a',

      'agent.connection': '\u8fde\u63a5',
      'agent.connectionHint': '\u4efb\u4f55\u652f\u6301 MCP \u7684 Agent\uff08OpenClaw\u3001DeepSeek Harness\u3001WorkBuddy \u7b49\uff09\u90fd\u53ef\u4ee5\u8fde\u63a5\u8fd9\u4e2a\u7aef\u70b9\u3002\u672c\u673a\u5176\u4ed6\u7a0b\u5e8f\u9664\u975e\u62ff\u5230 Token\uff0c\u5426\u5219\u8fde\u4e0d\u4e0a\u3002',
      'agent.endpointLabel': 'MCP \u7aef\u70b9\uff08\u5305\u542b\u672c\u673a Token\uff09',
      'agent.bridgeLabel': 'stdio \u6865\u547d\u4ee4\uff08\u9002\u7528\u4e8e\u53ea\u80fd\u542f\u52a8\u8fdb\u7a0b\u7684\u5bbf\u4e3b\uff09',
      'agent.copy': '\u590d\u5236',
      'agent.statTools': '\u5de5\u5177',
      'agent.statRequests': '\u8bf7\u6c42',
      'agent.statCalls': '\u5de5\u5177\u8c03\u7528',
      'agent.statSessions': '\u4f1a\u8bdd',
      'agent.permissions': '\u6743\u9650',
      'agent.allowWrite': '\u5141\u8bb8 Agent \u4fee\u6539\u8fd9\u7bc7\u6587\u6863',
      'agent.requireConfirmation': 'Agent \u7684\u6bcf\u6b21\u6539\u52a8\u90fd\u8981\u6c42 Diff \u786e\u8ba4',
      'agent.protectFrontMatter': '\u4fdd\u62a4 YAML front matter',
      'agent.allowSave': '\u5141\u8bb8 Agent \u4fdd\u5b58\u5230\u78c1\u76d8',
      'agent.portLabel': 'MCP 端口',
      'agent.portTitle': '0 = 自动挑一个空闲端口并记住它',
      'agent.portSave': '保存',
      'agent.portInvalid': '端口不对',
      'agent.portInvalidBody': '请填 0 到 65535 之间的数字（0 = 自动）。',
      'agent.portSaved': '端口已保存',
      'agent.portSavedBody': '下次启动起编辑器会使用端口 {0}。',
      'agent.keepToken': '重启后仍使用同一个 token（端点每次都一样）',
      'agent.newToken': '重新生成 token',
      'agent.endpointPinned': '已固定：重启后这个端点依然可用（端口 {0}）。',
      'agent.endpointEphemeral': '端口 {0} 会被记住，但每次启动都会生成新的 token。',
      'agent.allowRollback': '\u5141\u8bb8 Agent \u56de\u6eda\u6539\u52a8',
      'agent.allowCaret': '\u5141\u8bb8 Agent \u79fb\u52a8\u5149\u6807 / \u9009\u533a',
      'agent.allowedDirectory': '\u53ea\u5141\u8bb8\u4fdd\u5b58\u5230\u8be5\u76ee\u5f55\u5185\uff08\u7559\u7a7a\u8868\u793a\u4e0d\u9650\u5236\uff09',
      'agent.activity': '\u52a8\u6001',
      'agent.noActivity': '\u6682\u65e0 Agent \u6d3b\u52a8\u3002',

      'audit.refresh': '\u5237\u65b0',
      'audit.rollbackLast': '\u56de\u6eda\u6700\u540e\u4e00\u6b21 Agent \u6539\u52a8',
      'audit.hint': '\u6bcf\u6b21\u6539\u52a8\u90fd\u4f1a\u8bb0\u5f55\uff1a\u8c01\u6539\u7684\u3001\u7528\u54ea\u4e2a\u5de5\u5177\u3001\u6539\u4e86\u4ec0\u4e48\u3002\u56de\u6eda\u4f1a\u751f\u6210\u4e00\u6761\u65b0\u7684\u5ba1\u8ba1\u8bb0\u5f55\uff0c\u56e0\u6b64\u4efb\u4f55\u5185\u5bb9\u90fd\u4e0d\u4f1a\u771f\u6b63\u4e22\u5931\u3002',
      'audit.empty': '\u6682\u65e0\u5ba1\u8ba1\u8bb0\u5f55\u3002',
      'audit.footer': '\u5171 {0} \u6761\u8bb0\u5f55 \u00b7 \u65e5\u5fd7\uff1a{1}',
      'audit.noAgentChanges': '\u6ca1\u6709\u53ef\u56de\u6eda\u7684 Agent \u6539\u52a8',
      'audit.rollingBack': '\u6b63\u5728\u56de\u6eda',
      'audit.rollingBackBody': '\u6b63\u5728\u64a4\u9500\u6700\u540e\u4e00\u6b21 Agent \u6539\u52a8\u2026\u2026',

      'diff.title': 'Agent \u8bf7\u6c42\u4fee\u6539\u6587\u6863',
      'diff.noDifferences': '\u6ca1\u6709\u884c\u7ea7\u5dee\u5f02\u3002',
      'diff.meta': '+{0} / -{1} \u884c',
      'diff.session': '\u4f1a\u8bdd {0}',
      'diff.tool': '\u5de5\u5177 {0}',
      'diff.apply': '\u5e94\u7528\u6539\u52a8',
      'diff.reject': '\u62d2\u7edd',
      'diff.banner': '\u6709 Agent \u6b63\u5728\u7b49\u5f85\u4f60\u786e\u8ba4\uff1a{0}',
      'diff.review': '\u67e5\u770b\u5dee\u5f02',
      'diff.pendingFallback': '\u5f85\u786e\u8ba4\u7684\u6539\u52a8',

      'toast.copied': '\u5df2\u590d\u5236',
      'toast.copiedBody': '\u5185\u5bb9\u5df2\u653e\u5165\u526a\u8d34\u677f\u3002',
      'toast.copyManually': '\u8bf7\u624b\u52a8\u590d\u5236',
      'toast.copyManuallyBody': '\u9009\u4e2d\u8be5\u8f93\u5165\u6846\u540e\u6309 Ctrl+C\u3002',
      'toast.sourceMode': '\u6e90\u7801\u6a21\u5f0f',
      'toast.sourceModeBody': '\u6574\u7bc7\u6587\u6863\u4ee5 Markdown \u6e90\u7801\u663e\u793a\u3002',
      'toast.wysiwygMode': '\u6240\u89c1\u5373\u6240\u5f97\u6a21\u5f0f',
      'toast.wysiwygModeBody': '\u5149\u6807\u6240\u5728\u884c\u663e\u793a\u6e90\u7801\uff0c\u5176\u4f59\u5185\u5bb9\u5b9e\u65f6\u6e32\u67d3\u3002',
      'toast.changeApplied': '\u6539\u52a8\u5df2\u5e94\u7528',
      'toast.changeAppliedBody': 'Agent \u7684\u4fee\u6539\u5df2\u5408\u5e76\u8fdb\u6587\u6863\u3002',
      'toast.changeRejected': '\u5df2\u62d2\u7edd\u6539\u52a8',
      'toast.changeRejectedBody': 'Agent \u7684\u4fee\u6539\u5df2\u88ab\u4e22\u5f03\u3002',
      'toast.noHost': '\u672a\u8fde\u63a5\u5bbf\u4e3b',
      'toast.noHostBody': '\u5f53\u524d\u4e0d\u5728\u684c\u9762\u5916\u58f3\u4e2d\u8fd0\u884c\uff1a\u7f16\u8f91\u53ea\u4f5c\u7528\u4e8e\u672c\u9875\u9762\u3002',
      'toast.toolFailed': '\u5de5\u5177\u8c03\u7528\u5931\u8d25',
      'toast.saved': '\u5df2\u4fdd\u5b58',
      'toast.languageChanged': '\u8bed\u8a00\u5df2\u5207\u6362',
      'toast.languageChangedBody': '\u754c\u9762\u73b0\u5728\u4f7f\u7528{0}\u3002',

      'status.untitled': '\u672a\u547d\u540d.md',
      'status.notSaved': '\u5c1a\u672a\u4fdd\u5b58',
      'status.rev': '\u7248\u672c {0}',
      'status.unsaved': '\u672a\u4fdd\u5b58',
      'status.mode': '\u5b9e\u65f6\u6e32\u67d3',
      'status.modeSource': '\u6e90\u7801',
      'status.caret': '\u7b2c {0} \u884c, \u7b2c {1} \u5217',
      'status.selection': '\u9009\u4e2d {0} \u5b57\u7b26',
      'status.counts': '{0} \u8bcd \u00b7 {1} \u5b57\u7b26 \u00b7 {2} \u5757',
      'status.documentPath': '\u6587\u6863\u8def\u5f84',
      'status.revision': '\u6587\u6863\u7248\u672c',
      'status.caretTitle': '\u5149\u6807\u6240\u5728\u884c\u5217',
      'status.countsTitle': '\u8bcd\u6570\u3001\u5b57\u7b26\u6570\u4e0e\u5757\u6570',
      'status.modeTitle': '\u5f53\u524d\u6a21\u5f0f',

      'editor.empty': '\u5f00\u59cb\u5199\u4f5c\uff0c\u6216\u6253\u5f00\u4e00\u4e2a Markdown \u6587\u4ef6\u3002',
      'editor.mermaidError': 'Mermaid\uff1a{0}',

      'activity.agentRead': 'Agent {0} \u8bfb\u53d6\u4e86\u6587\u6863',
      'activity.documentUpdated': '\u6587\u6863\u5df2\u88ab Agent \u66f4\u65b0',
      'activity.unknown': '\u672a\u77e5',
      'activity.ok': '\u6210\u529f',
      'activity.failed': '\u5931\u8d25'
    },

    ja: {
      'brand.tagline': '\u30ed\u30fc\u30ab\u30eb\u3067\u305d\u306e\u5834\u30ec\u30f3\u30c0\u30ea\u30f3\u30b0\u3059\u308b Markdown\u3001MCP \u30b5\u30fc\u30d0\u30fc\u5185\u8535',

      'toolbar.new': '\u65b0\u898f',
      'toolbar.open': '\u958b\u304f',
      'toolbar.save': '\u4fdd\u5b58',
      'toolbar.saveAs': '\u540d\u524d\u3092\u4ed8\u3051\u3066\u4fdd\u5b58',
      'toolbar.undo': '\u5143\u306b\u623b\u3059',
      'toolbar.redo': '\u3084\u308a\u76f4\u3059',
      'toolbar.source': '\u30bd\u30fc\u30b9',
      'toolbar.agent': '\u30a8\u30fc\u30b8\u30a7\u30f3\u30c8',
      'toolbar.audit': '\u76e3\u67fb',
      'toolbar.copyEndpoint': '\u30a8\u30f3\u30c9\u30dd\u30a4\u30f3\u30c8\u3092\u30b3\u30d4\u30fc',
      'toolbar.language': '\u8a00\u8a9e',
      'toolbar.theme': '\u5916\u89b3\uff08\u30e9\u30a4\u30c8 / \u30c0\u30fc\u30af / \u30b7\u30b9\u30c6\u30e0\u306b\u5f93\u3046\uff09',

      'theme.light': 'ライト',
      'theme.dark': 'ダーク',
      'theme.auto': 'システムに従う',

      'tab.list': '\u958b\u3044\u3066\u3044\u308b\u6587\u66f8',
      'tab.new': '\u65b0\u3057\u3044\u30bf\u30d6 (Ctrl+N)',
      'tab.close': '\u30bf\u30d6\u3092\u9589\u3058\u308b (Ctrl+W)',

      'format.paragraph': '新しい段落を始める',
      'format.paragraphShort': '¶',
      'format.h1': '見出し 1',
      'format.h2': '見出し 2',
      'format.h3': '見出し 3',
      'format.bullet': '箇条書き',
      'format.number': '番号付きリスト',
      'format.task': 'タスクリスト',
      'format.quote': '引用',
      'format.code': 'コードブロック',
      'format.divider': '区切り線',
      'format.table': '表を挿入',
      'format.row': '表に行を追加',
      'format.rowShort': '行 +',
      'format.column': '表に列を追加',
      'format.columnShort': '列 +',
      'format.bold': '太字',
      'format.italic': '斜体',
      'format.inlineCode': 'インラインコード',
      'format.link': 'リンクを挿入',
      'format.linkShort': 'リンク',
      'format.image': '画像を挿入',
      'format.imageShort': '画像',
      'format.formula': '数式を挿入',
      'format.formulaShort': '数式',
      'format.linkText': 'リンクテキスト',
      'format.linkUrl': 'URL',
      'format.imageAlt': '画像の説明',
      'format.imagePath': '画像パス',
      'format.formulaText': '数式',
      'format.tableHeader': '列 {0}',

      'title.new': '\u65b0\u898f\u6587\u66f8 (Ctrl+N)',
      'title.open': '.md \u3092\u958b\u304f (Ctrl+O)',
      'title.save': '\u4fdd\u5b58 (Ctrl+S)',
      'title.saveAs': '\u540d\u524d\u3092\u4ed8\u3051\u3066\u4fdd\u5b58 (Ctrl+Shift+S)',
      'title.undo': '\u5143\u306b\u623b\u3059 (Ctrl+Z)',
      'title.redo': '\u3084\u308a\u76f4\u3059 (Ctrl+Y)',
      'title.source': '\u6587\u66f8\u5168\u4f53\u3092 Markdown \u30bd\u30fc\u30b9\u3067\u8868\u793a (Ctrl+/)',
      'title.agent': '\u30a8\u30fc\u30b8\u30a7\u30f3\u30c8\u30d1\u30cd\u30eb (Ctrl+Shift+A)',
      'title.audit': '\u76e3\u67fb\u30ed\u30b0 (Ctrl+Shift+L)',
      'title.copyEndpoint': 'MCP \u30a8\u30f3\u30c9\u30dd\u30a4\u30f3\u30c8\u3092\u30b3\u30d4\u30fc',
      'title.mcpBadge': 'MCP \u30b5\u30fc\u30d0\u30fc\u306e\u72b6\u614b',
      'title.editor': 'Markdown \u6587\u66f8',
      'title.sourceView': 'Markdown \u30bd\u30fc\u30b9',

      'mcp.badge': 'MCP',
      'mcp.off': 'MCP \u505c\u6b62\u4e2d',
      'mcp.listening': 'MCP \u30b5\u30fc\u30d0\u30fc\u306f {0} \u3067\u5f85\u3061\u53d7\u3051\u4e2d',
      'mcp.notRunning': 'MCP \u30b5\u30fc\u30d0\u30fc\u304c\u52d5\u4f5c\u3057\u3066\u3044\u307e\u305b\u3093',

      'panel.agent': '\u30a8\u30fc\u30b8\u30a7\u30f3\u30c8',
      'panel.audit': '\u76e3\u67fb',
      'panel.diff': '\u5dee\u5206',
      'panel.close': '\u30d1\u30cd\u30eb\u3092\u9589\u3058\u308b',
      'panel.tabs': '\u30d1\u30cd\u30eb\u306e\u30bb\u30af\u30b7\u30e7\u30f3',

      'agent.connection': '\u63a5\u7d9a',
      'agent.connectionHint': 'MCP \u5bfe\u5fdc\u306e\u30a8\u30fc\u30b8\u30a7\u30f3\u30c8\uff08OpenClaw\u3001DeepSeek Harness\u3001WorkBuddy \u306a\u3069\uff09\u306f\u3053\u306e\u30a8\u30f3\u30c9\u30dd\u30a4\u30f3\u30c8\u306b\u63a5\u7d9a\u3067\u304d\u307e\u3059\u3002\u30c8\u30fc\u30af\u30f3\u3092\u6301\u305f\u306a\u3044\u4ed6\u306e\u30d7\u30ed\u30b0\u30e9\u30e0\u304b\u3089\u306f\u63a5\u7d9a\u3067\u304d\u307e\u305b\u3093\u3002',
      'agent.endpointLabel': 'MCP \u30a8\u30f3\u30c9\u30dd\u30a4\u30f3\u30c8\uff08\u30ed\u30fc\u30ab\u30eb\u30c8\u30fc\u30af\u30f3\u3092\u542b\u3080\uff09',
      'agent.bridgeLabel': 'stdio \u30d6\u30ea\u30c3\u30b8\u306e\u30b3\u30de\u30f3\u30c9\uff08\u30d7\u30ed\u30bb\u30b9\u3092\u8d77\u52d5\u3059\u308b\u30db\u30b9\u30c8\u5411\u3051\uff09',
      'agent.copy': '\u30b3\u30d4\u30fc',
      'agent.statTools': '\u30c4\u30fc\u30eb',
      'agent.statRequests': '\u30ea\u30af\u30a8\u30b9\u30c8',
      'agent.statCalls': '\u30c4\u30fc\u30eb\u547c\u3073\u51fa\u3057',
      'agent.statSessions': '\u30bb\u30c3\u30b7\u30e7\u30f3',
      'agent.permissions': '\u6a29\u9650',
      'agent.allowWrite': '\u30a8\u30fc\u30b8\u30a7\u30f3\u30c8\u306b\u3088\u308b\u3053\u306e\u6587\u66f8\u306e\u5909\u66f4\u3092\u8a31\u53ef',
      'agent.requireConfirmation': '\u30a8\u30fc\u30b8\u30a7\u30f3\u30c8\u306e\u5909\u66f4\u3054\u3068\u306b\u5dee\u5206\u78ba\u8a8d\u3092\u5fc5\u9808\u306b\u3059\u308b',
      'agent.protectFrontMatter': 'YAML front matter \u3092\u4fdd\u8b77\u3059\u308b',
      'agent.allowSave': '\u30a8\u30fc\u30b8\u30a7\u30f3\u30c8\u306b\u3088\u308b\u30c7\u30a3\u30b9\u30af\u3078\u306e\u4fdd\u5b58\u3092\u8a31\u53ef',
      'agent.portLabel': 'MCP \u30dd\u30fc\u30c8',
      'agent.portTitle': '0 = \u7a7a\u3044\u305f\u30dd\u30fc\u30c8\u3092\u9078\u3093\u3067\u8a18\u61b6\u3057\u307e\u3059',
      'agent.portSave': '\u4fdd\u5b58',
      'agent.portInvalid': '\u30dd\u30fc\u30c8\u304c\u4e0d\u6b63\u3067\u3059',
      'agent.portInvalidBody': '0〜65535 \u306e\u6570\u5b57\u3092\u5165\u529b\u3057\u3066\u304f\u3060\u3055\u3044\uff080 = \u81ea\u52d5\uff09\u3002',
      'agent.portSaved': '\u30dd\u30fc\u30c8\u3092\u4fdd\u5b58\u3057\u307e\u3057\u305f',
      'agent.portSavedBody': '\u6b21\u56de\u306e\u8d77\u52d5\u304b\u3089\u30dd\u30fc\u30c8 {0} \u3092\u4f7f\u3044\u307e\u3059\u3002',
      'agent.keepToken': '\u518d\u8d77\u52d5\u5f8c\u3082\u540c\u3058\u30c8\u30fc\u30af\u30f3\u3092\u4f7f\u3046\uff08\u30a8\u30f3\u30c9\u30dd\u30a4\u30f3\u30c8\u304c\u5909\u308f\u308a\u307e\u305b\u3093\uff09',
      'agent.newToken': '\u30c8\u30fc\u30af\u30f3\u3092\u518d\u751f\u6210',
      'agent.endpointPinned': '\u56fa\u5b9a\u6e08\u307f\uff1a\u518d\u8d77\u52d5\u5f8c\u3082\u3053\u306e\u30a8\u30f3\u30c9\u30dd\u30a4\u30f3\u30c8\u304c\u4f7f\u3048\u307e\u3059\uff08\u30dd\u30fc\u30c8 {0}\uff09\u3002',
      'agent.endpointEphemeral': '\u30dd\u30fc\u30c8 {0} \u306f\u8a18\u61b6\u3055\u308c\u307e\u3059\u304c\u3001\u30c8\u30fc\u30af\u30f3\u306f\u6bce\u56de\u65b0\u3057\u304f\u306a\u308a\u307e\u3059\u3002',
      'agent.allowRollback': '\u30a8\u30fc\u30b8\u30a7\u30f3\u30c8\u306b\u3088\u308b\u5909\u66f4\u306e\u53d6\u308a\u6d88\u3057\u3092\u8a31\u53ef',
      'agent.allowCaret': '\u30a8\u30fc\u30b8\u30a7\u30f3\u30c8\u306b\u3088\u308b\u30ab\u30fc\u30bd\u30eb\u30fb\u9078\u629e\u7bc4\u56f2\u306e\u79fb\u52d5\u3092\u8a31\u53ef',
      'agent.allowedDirectory': '\u3053\u306e\u30c7\u30a3\u30ec\u30af\u30c8\u30ea\u5185\u306e\u307f\u4fdd\u5b58\u3092\u8a31\u53ef\uff08\u7a7a\u6b04\u3067\u5236\u9650\u306a\u3057\uff09',
      'agent.activity': '\u30a2\u30af\u30c6\u30a3\u30d3\u30c6\u30a3',
      'agent.noActivity': '\u30a8\u30fc\u30b8\u30a7\u30f3\u30c8\u306e\u64cd\u4f5c\u306f\u307e\u3060\u3042\u308a\u307e\u305b\u3093\u3002',

      'audit.refresh': '\u66f4\u65b0',
      'audit.rollbackLast': '\u76f4\u524d\u306e\u30a8\u30fc\u30b8\u30a7\u30f3\u30c8\u5909\u66f4\u3092\u5143\u306b\u623b\u3059',
      'audit.hint': '\u3059\u3079\u3066\u306e\u5909\u66f4\u306f\u300c\u8ab0\u304c\u30fb\u3069\u306e\u30c4\u30fc\u30eb\u3067\u30fb\u4f55\u3092\u5909\u3048\u305f\u304b\u300d\u3068\u3068\u3082\u306b\u8a18\u9332\u3055\u308c\u307e\u3059\u3002\u53d6\u308a\u6d88\u3057\u3082\u65b0\u3057\u3044\u76e3\u67fb\u8a18\u9332\u3068\u3057\u3066\u6b8b\u308b\u305f\u3081\u3001\u5185\u5bb9\u304c\u5931\u308f\u308c\u308b\u3053\u3068\u306f\u3042\u308a\u307e\u305b\u3093\u3002',
      'audit.empty': '\u76e3\u67fb\u8a18\u9332\u306f\u307e\u3060\u3042\u308a\u307e\u305b\u3093\u3002',
      'audit.footer': '{0} \u4ef6\u306e\u8a18\u9332 \u00b7 \u30ed\u30b0: {1}',
      'audit.noAgentChanges': '\u5143\u306b\u623b\u305b\u308b\u30a8\u30fc\u30b8\u30a7\u30f3\u30c8\u5909\u66f4\u304c\u3042\u308a\u307e\u305b\u3093',
      'audit.rollingBack': '\u5143\u306b\u623b\u3057\u3066\u3044\u307e\u3059',
      'audit.rollingBackBody': '\u76f4\u524d\u306e\u30a8\u30fc\u30b8\u30a7\u30f3\u30c8\u5909\u66f4\u3092\u53d6\u308a\u6d88\u3057\u3066\u3044\u307e\u3059\u2026',

      'diff.title': '\u30a8\u30fc\u30b8\u30a7\u30f3\u30c8\u304c\u5909\u66f4\u3092\u63d0\u6848\u3057\u3066\u3044\u307e\u3059',
      'diff.noDifferences': '\u884c\u5358\u4f4d\u306e\u5dee\u5206\u306f\u3042\u308a\u307e\u305b\u3093\u3002',
      'diff.meta': '+{0} / -{1} \u884c',
      'diff.session': '\u30bb\u30c3\u30b7\u30e7\u30f3 {0}',
      'diff.tool': '\u30c4\u30fc\u30eb {0}',
      'diff.apply': '\u5909\u66f4\u3092\u9069\u7528',
      'diff.reject': '\u62d2\u5426',
      'diff.banner': '\u30a8\u30fc\u30b8\u30a7\u30f3\u30c8\u304c\u78ba\u8a8d\u3092\u5f85\u3063\u3066\u3044\u307e\u3059: {0}',
      'diff.review': '\u5dee\u5206\u3092\u78ba\u8a8d',
      'diff.pendingFallback': '\u63d0\u6848\u3055\u308c\u305f\u5909\u66f4',

      'toast.copied': '\u30b3\u30d4\u30fc\u3057\u307e\u3057\u305f',
      'toast.copiedBody': '\u30af\u30ea\u30c3\u30d7\u30dc\u30fc\u30c9\u306b\u5165\u308a\u307e\u3057\u305f\u3002',
      'toast.copyManually': '\u624b\u52d5\u3067\u30b3\u30d4\u30fc',
      'toast.copyManuallyBody': '\u5165\u529b\u6b04\u3092\u9078\u629e\u3057\u3066 Ctrl+C \u3092\u62bc\u3057\u3066\u304f\u3060\u3055\u3044\u3002',
      'toast.sourceMode': '\u30bd\u30fc\u30b9\u30e2\u30fc\u30c9',
      'toast.sourceModeBody': '\u6587\u66f8\u5168\u4f53\u3092 Markdown \u30bd\u30fc\u30b9\u3067\u8868\u793a\u3057\u3066\u3044\u307e\u3059\u3002',
      'toast.wysiwygMode': 'WYSIWYG \u30e2\u30fc\u30c9',
      'toast.wysiwygModeBody': '\u30ab\u30fc\u30bd\u30eb\u884c\u306f\u30bd\u30fc\u30b9\u3001\u305d\u308c\u4ee5\u5916\u306f\u305d\u306e\u5834\u3067\u30ec\u30f3\u30c0\u30ea\u30f3\u30b0\u3055\u308c\u307e\u3059\u3002',
      'toast.changeApplied': '\u5909\u66f4\u3092\u9069\u7528\u3057\u307e\u3057\u305f',
      'toast.changeAppliedBody': '\u30a8\u30fc\u30b8\u30a7\u30f3\u30c8\u306e\u63d0\u6848\u306f\u6587\u66f8\u306b\u53cd\u6620\u3055\u308c\u307e\u3057\u305f\u3002',
      'toast.changeRejected': '\u5909\u66f4\u3092\u62d2\u5426\u3057\u307e\u3057\u305f',
      'toast.changeRejectedBody': '\u30a8\u30fc\u30b8\u30a7\u30f3\u30c8\u306e\u63d0\u6848\u306f\u7834\u68c4\u3055\u308c\u307e\u3057\u305f\u3002',
      'toast.noHost': '\u30db\u30b9\u30c8\u672a\u63a5\u7d9a',
      'toast.noHostBody': '\u30c7\u30b9\u30af\u30c8\u30c3\u30d7\u30b7\u30a7\u30eb\u5916\u3067\u52d5\u4f5c\u4e2d\u3067\u3059\u3002\u7de8\u96c6\u306f\u3053\u306e\u30da\u30fc\u30b8\u5185\u3060\u3051\u306b\u7559\u307e\u308a\u307e\u3059\u3002',
      'toast.toolFailed': '\u30c4\u30fc\u30eb\u547c\u3073\u51fa\u3057\u306b\u5931\u6557\u3057\u307e\u3057\u305f',
      'toast.saved': '\u4fdd\u5b58\u3057\u307e\u3057\u305f',
      'toast.languageChanged': '\u8868\u793a\u8a00\u8a9e\u3092\u5909\u66f4\u3057\u307e\u3057\u305f',
      'toast.languageChangedBody': '\u30a4\u30f3\u30bf\u30fc\u30d5\u30a7\u30a4\u30b9\u306f {0} \u306b\u306a\u308a\u307e\u3057\u305f\u3002',

      'status.untitled': '\u7121\u984c.md',
      'status.notSaved': '\u672a\u4fdd\u5b58',
      'status.rev': '\u30ea\u30d3\u30b8\u30e7\u30f3 {0}',
      'status.unsaved': '\u672a\u4fdd\u5b58',
      'status.mode': 'WYSIWYG',
      'status.modeSource': '\u30bd\u30fc\u30b9',
      'status.caret': '{0} \u884c {1} \u5217',
      'status.selection': '{0} \u6587\u5b57\u3092\u9078\u629e',
      'status.counts': '{0} \u8a9e \u00b7 {1} \u6587\u5b57 \u00b7 {2} \u30d6\u30ed\u30c3\u30af',
      'status.documentPath': '\u6587\u66f8\u306e\u30d1\u30b9',
      'status.revision': '\u6587\u66f8\u306e\u30ea\u30d3\u30b8\u30e7\u30f3',
      'status.caretTitle': '\u30ab\u30fc\u30bd\u30eb\u306e\u884c\u3068\u5217',
      'status.countsTitle': '\u8a9e\u6570\u30fb\u6587\u5b57\u6570\u30fb\u30d6\u30ed\u30c3\u30af\u6570',
      'status.modeTitle': '\u73fe\u5728\u306e\u30e2\u30fc\u30c9',

      'editor.empty': '\u66f8\u304d\u59cb\u3081\u308b\u304b\u3001Markdown \u30d5\u30a1\u30a4\u30eb\u3092\u958b\u3044\u3066\u304f\u3060\u3055\u3044\u3002',
      'editor.mermaidError': 'Mermaid: {0}',

      'activity.agentRead': '\u30a8\u30fc\u30b8\u30a7\u30f3\u30c8 {0} \u304c\u6587\u66f8\u3092\u8aad\u307f\u53d6\u308a\u307e\u3057\u305f',
      'activity.documentUpdated': '\u6587\u66f8\u304c\u30a8\u30fc\u30b8\u30a7\u30f3\u30c8\u306b\u3088\u3063\u3066\u66f4\u65b0\u3055\u308c\u307e\u3057\u305f',
      'activity.unknown': '\u4e0d\u660e',
      'activity.ok': '\u6210\u529f',
      'activity.failed': '\u5931\u6557'
    }
  };

  /* ====================================================================== */
  /* State                                                                  */
  /* ====================================================================== */
  var current = FALLBACK;
  var listeners = [];

  function normalize(tag) {
    if (!tag) return FALLBACK;
    var lower = String(tag).toLowerCase();
    if (lower.indexOf('ja') === 0) return 'ja';
    if (lower.indexOf('zh') === 0) return 'zh-Hans';
    if (lower.indexOf('en') === 0) return FALLBACK;
    return FALLBACK;
  }

  function isSupported(tag) {
    return Object.prototype.hasOwnProperty.call(PACKS, normalize(tag));
  }

  /* ====================================================================== */
  /* Lookup                                                                 */
  /* ====================================================================== */
  function format(text, args) {
    if (!args || !args.length) return text;
    return text.replace(/\{(\d+)\}/g, function (match, index) {
      var position = parseInt(index, 10);
      return position < args.length && args[position] != null ? String(args[position]) : match;
    });
  }

  function t(key, args) {
    var pack = PACKS[current] || PACKS[FALLBACK];
    var value = pack[key];
    if (value == null) value = PACKS[FALLBACK][key];
    if (value == null) return key; // a missing key is better visibly wrong than blank
    return format(value, args);
  }

  function tIn(tag, key, args) {
    var pack = PACKS[normalize(tag)] || PACKS[FALLBACK];
    var value = pack[key];
    if (value == null) value = PACKS[FALLBACK][key];
    return value == null ? key : format(value, args);
  }

  /* ====================================================================== */
  /* DOM application                                                        */
  /* ====================================================================== */
  function applyToDom(root) {
    var scope = root || document;

    // Text content.
    var nodes = scope.querySelectorAll('[data-i18n]');
    Array.prototype.forEach.call(nodes, function (node) {
      var key = node.getAttribute('data-i18n');
      if (!key) return;
      var args = node.getAttribute('data-i18n-args');
      node.textContent = t(key, args ? args.split('\u0001') : null);
    });

    // Attributes: title / aria-label / placeholder.
    [['data-i18n-title', 'title'],
     ['data-i18n-aria-label', 'aria-label'],
     ['data-i18n-placeholder', 'placeholder']].forEach(function (pair) {
      Array.prototype.forEach.call(scope.querySelectorAll('[' + pair[0] + ']'), function (node) {
        var key = node.getAttribute(pair[0]);
        if (key) node.setAttribute(pair[1], t(key));
      });
    });
  }

  /* ====================================================================== */
  /* Public API                                                             */
  /* ====================================================================== */
  function setLanguage(tag, options) {
    options = options || {};
    var next = normalize(tag);
    var changed = next !== current;
    current = next;
    document.documentElement.setAttribute('lang', current);
    document.documentElement.setAttribute('data-lang', current);
    applyToDom(document);
    for (var i = 0; i < listeners.length; i++) {
      try { listeners[i](current, changed); } catch (error) { /* one bad listener */ }
    }
    // Application code listens for this and forwards the choice to the shell.
    try {
      document.dispatchEvent(new CustomEvent('colaimdation:language', {
        detail: { language: current, changed: changed }
      }));
    } catch (error) {
      // CustomEvent is unavailable in very old engines: the toolbar still works.
    }
    return current;
  }

  function onLanguageChange(listener) {
    listeners.push(listener);
    return function () {
      var index = listeners.indexOf(listener);
      if (index >= 0) listeners.splice(index, 1);
    };
  }

  global.ColaiI18n = {
    languages: LANGUAGES,
    normalize: normalize,
    isSupported: isSupported,
    language: function () { return current; },
    setLanguage: setLanguage,
    t: t,
    tIn: tIn,
    applyToDom: applyToDom,
    onLanguageChange: onLanguageChange,
    // Exposed so tests (and the host) can validate packs.
    packs: function () { return PACKS; },
    keys: function () { return Object.keys(PACKS[FALLBACK]); }
  };
})(window);
