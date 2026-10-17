#include "core/i18n.h"

#include "core/utf8.h"

#include <algorithm>
#include <cstddef>
#include <map>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace colai::i18n {
namespace {

// ---------------------------------------------------------------------------
// Catalog
//
// One row per string, one column per language. Keeping the languages in a single
// table makes omissions obvious and lets a test assert that every row defines
// every shipped language.
// ---------------------------------------------------------------------------
struct Row {
    const char* key;
    const char* en;
    const char* zh;
    const char* ja;
};

const Row kRows[] = {
    // --- window titles ------------------------------------------------------
    {"app.name", "Colaimdation", "Colaimdation", "Colaimdation"},
    {"app.title", "Colaimdation", "Colaimdation", "Colaimdation"},
    {"app.title.webview_missing", "Colaimdation - WebView2 runtime missing",
     "Colaimdation - 缺少 WebView2 运行时", "Colaimdation - WebView2 ランタイムが見つかりません"},
    {"app.untitled", "untitled.md", "未命名.md", "無題.md"},

    // --- File menu ----------------------------------------------------------
    {"menu.file", "&File", "文件(&F)", "ファイル(&F)"},
    {"menu.file.new", "&New\tCtrl+N", "新建(&N)\tCtrl+N", "新規(&N)\tCtrl+N"},
    {"menu.file.open", "&Open...\tCtrl+O", "打开(&O)...\tCtrl+O", "開く(&O)...\tCtrl+O"},
    {"menu.file.save", "&Save\tCtrl+S", "保存(&S)\tCtrl+S", "保存(&S)\tCtrl+S"},
    {"menu.file.save_as", "Save &As...\tCtrl+Shift+S", "另存为(&A)...\tCtrl+Shift+S",
     "名前を付けて保存(&A)...\tCtrl+Shift+S"},
    {"menu.file.print", "&Print...\tCtrl+P", "打印(&P)...\tCtrl+P", "印刷(&P)...\tCtrl+P"},
    {"menu.file.export_pdf", "Export as &PDF...", "导出为 PDF(&P)...", "PDF として書き出し(&P)..."},
    {"menu.file.export_word", "Export as &Word...", "导出为 Word(&W)...",
     "Word として書き出し(&W)..."},
    {"menu.file.export_image", "Export as &image...", "导出为图片(&I)...",
     "画像として書き出し(&I)..."},
    {"menu.file.close_tab", "&Close tab\tCtrl+W", "关闭标签页(&C)\tCtrl+W",
     "タブを閉じる(&C)\tCtrl+W"},
    {"menu.file.reload", "&Reload from disk", "从磁盘重新载入(&R)", "ディスクから再読み込み(&R)"},
    {"menu.file.exit", "E&xit\tAlt+F4", "退出(&X)\tAlt+F4", "終了(&X)\tAlt+F4"},

    // --- Edit menu ----------------------------------------------------------
    {"menu.edit", "&Edit", "编辑(&E)", "編集(&E)"},
    {"menu.edit.undo", "&Undo\tCtrl+Z", "撤销(&U)\tCtrl+Z", "元に戻す(&U)\tCtrl+Z"},
    {"menu.edit.redo", "&Redo\tCtrl+Y", "重做(&R)\tCtrl+Y", "やり直す(&R)\tCtrl+Y"},
    {"menu.edit.copy_mcp", "Copy &MCP endpoint", "复制 MCP 端点(&M)", "MCP エンドポイントをコピー(&M)"},

    // --- Agent menu ---------------------------------------------------------
    {"menu.agent", "&Agent", "Agent(&A)", "エージェント(&A)"},
    {"menu.agent.panel", "&Agent panel\tCtrl+Shift+A", "Agent 面板(&A)\tCtrl+Shift+A",
     "エージェントパネル(&A)\tCtrl+Shift+A"},
    {"menu.agent.audit", "&Audit log\tCtrl+Shift+L", "审计日志(&A)\tCtrl+Shift+L",
     "監査ログ(&A)\tCtrl+Shift+L"},
    {"menu.agent.allow_writes", "Allow agent &writes", "允许 Agent 写入(&W)",
     "エージェントの書き込みを許可(&W)"},
    {"menu.agent.preview", "Require &diff confirmation", "要求 Diff 确认(&D)",
     "差分確認を必須にする(&D)"},
    {"menu.agent.rollback", "Rollback last agent change\tCtrl+Shift+Z",
     "回滚最后一次 Agent 改动\tCtrl+Shift+Z", "直前のエージェント変更を元に戻す\tCtrl+Shift+Z"},
    {"menu.agent.rollback_all", "Rollback all agent changes", "回滚全部 Agent 改动",
     "すべてのエージェント変更を元に戻す"},
    {"menu.agent.endpoint", "Copy endpoint and token", "复制端点与 Token",
     "エンドポイントとトークンをコピー"},

    // --- View menu ----------------------------------------------------------
    {"menu.view", "&View", "视图(&V)", "表示(&V)"},
    {"menu.view.source", "Toggle &source mode\tCtrl+/", "切换源码模式(&S)\tCtrl+/",
     "ソースモード切替(&S)\tCtrl+/"},
    {"menu.view.reload", "Reload front-&end\tF5", "重载前端(&E)\tF5", "フロントエンドを再読み込み(&E)\tF5"},
    {"menu.view.devtools", "Developer &tools\tF12", "开发者工具(&T)\tF12", "開発者ツール(&T)\tF12"},
    {"menu.view.next_tab", "&Next document\tCtrl+Tab", "下一个文档(&N)\tCtrl+Tab",
     "次の文書(&N)\tCtrl+Tab"},
    {"menu.view.prev_tab", "&Previous document\tCtrl+Shift+Tab", "上一个文档(&P)\tCtrl+Shift+Tab",
     "前の文書(&P)\tCtrl+Shift+Tab"},
    {"menu.view.selftest", "Check the &editor (report)", "编辑器自检(&E)", "エディタのセルフチェック(&E)"},
    {"menu.view.language", "&Language", "语言(&L)", "言語(&L)"},

    // --- Help menu ----------------------------------------------------------
    {"menu.help", "&Help", "帮助(&H)", "ヘルプ(&H)"},
    {"menu.help.mcp", "&How to connect an Agent", "如何接入 Agent(&H)",
     "エージェントの接続方法(&H)"},
    {"menu.help.about", "&About Colaimdation", "关于 Colaimdation(&A)",
     "Colaimdation について(&A)"},

    // --- common -------------------------------------------------------------
    {"common.ok", "OK", "确定", "OK"},

    // --- file command dialogs -----------------------------------------------
    {"dlg.unsaved.text", "This document has unsaved changes.\n"
     "\n"
     "Yes - save them first\n"
     "No  - discard them and continue\n"
     "Cancel - do nothing",
"这篇文档还有未保存的改动。\n"
     "\n"
     "是 —— 先保存\n"
     "否 —— 丢弃改动并继续\n"
     "取消 —— 什么都不做",
"この文書には未保存の変更があります。\n"
     "\n"
     "はい — 先に保存します\n"
     "いいえ — 変更を破棄して続行します\n"
     "キャンセル — 何もしません"},
    {"dlg.save_failed.text", "Cannot save\n\n{0}", "无法保存\n\n{0}", "保存できません\n\n{0}"},
    {"dlg.mcp_port_busy", "Port {0} was busy, so the MCP endpoint is on another port this time. Change the port in the Agent panel if your Agent needs a fixed one.",
"端口 {0} 已被占用，这次的 MCP 端点换用了其他端口。如果 Agent 需要一个固定端口，请在 Agent 面板里改。",
"ポート {0} は使用中だったため、今回の MCP エンドポイントは別のポートになりました。固定したい場合は Agent パネルで変更してください。"},
    {"dlg.selftest.title", "Editor self-check", "编辑器自检", "エディタのセルフチェック"},
    {"dlg.selftest.empty", "The editor returned no report.", "编辑器没有返回报告。",
     "エディタからレポートが返りませんでした。"},
    {"dlg.open_failed.text", "Cannot open the file\n\n{0}", "无法打开文件\n\n{0}",
     "ファイルを開けません\n\n{0}"},
    {"dlg.reload_failed.text", "Cannot reload\n\n{0}", "无法重新载入\n\n{0}",
     "再読み込みできません\n\n{0}"},
    {"dlg.not_saved_yet.text", "This document has not been saved yet.",
     "这篇文档尚未保存到磁盘。", "この文書はまだ保存されていません。"},
    {"dlg.discard_reload.text", "Discard unsaved changes and reload the file from disk?",
     "丢弃未保存的改动并从磁盘重新载入？",
     "未保存の変更を破棄してディスクから再読み込みしますか？"},
    {"dlg.nothing_to_rollback.text", "Nothing to roll back: {0}", "没有可回滚的改动：{0}",
     "元に戻せる変更がありません: {0}"},
    {"dlg.no_agent_changes", "no agent changes", "没有 Agent 改动",
     "エージェントによる変更はありません"},
    {"dlg.rollback_all.text", "Roll back every change made by Agents and keep your own edits?\n"
     "\n"
     "This cannot be undone in one step, but each rollback is recorded in the audit log.",
"回滚所有 Agent 做出的改动，并保留你自己的编辑？\n"
     "\n"
     "此操作无法一步撤回，但每一次回滚都会写入审计日志。",
"エージェントによるすべての変更を元に戻し、あなた自身の編集は保持しますか？\n"
     "\n"
     "この操作は一括では取り消せませんが、各ロールバックは監査ログに記録されます。"},
    {"dlg.rolled_back.text", "Reverted {0} agent change(s).", "已回滚 {0} 处 Agent 改动。",
     "エージェントの変更を {0} 件元に戻しました。"},
    {"dlg.mcp_not_running.text", "The MCP server is not running.\n\n{0}",
     "MCP 服务未运行。\n\n{0}", "MCP サーバーが動作していません。\n\n{0}"},
    {"dlg.endpoint_copied.text", "MCP endpoint copied to the clipboard:\n"
     "\n"
     "{0}\n"
     "\n"
     "Treat this URL like a password: it contains the local access token.",
"MCP 端点已复制到剪贴板：\n"
     "\n"
     "{0}\n"
     "\n"
     "请把这个 URL 当作密码对待：它包含本机访问 Token。",
"MCP エンドポイントをクリップボードにコピーしました:\n"
     "\n"
     "{0}\n"
     "\n"
     "この URL はパスワード同様に扱ってください。ローカルアクセストークンが含まれています。"},

    // --- help dialogs -------------------------------------------------------
    {"help.about.text", "Colaimdation {0}\n"
     "\n"
     "A local WYSIWYG Markdown editor with an embedded MCP server.\n"
     "Agents edit the in-memory document through MCP instead of touching files,\n"
     "so your unsaved work is never lost and every change is auditable.\n"
     "\n"
     "Endpoint: {1}",
"Colaimdation {0}\n"
     "\n"
     "本地原地实时渲染的 Markdown 编辑器，内置 MCP 服务。\n"
     "Agent 通过 MCP 修改内存中的文档，而不是直接读写文件，\n"
     "因此你未保存的工作不会丢失，每一次改动都可审计。\n"
     "\n"
     "端点：{1}",
"Colaimdation {0}\n"
     "\n"
     "MCP サーバーを内蔵したローカルの WYSIWYG Markdown エディタです。\n"
     "エージェントはファイルではなくメモリ上の文書を MCP 経由で編集するため、\n"
     "未保存の作業が失われず、すべての変更を監査できます。\n"
     "\n"
     "エンドポイント: {1}"},
    {"help.mcp.title", "Connect an Agent", "接入 Agent", "エージェントの接続"},
    {"help.mcp.text", "Point any MCP-capable Agent at this editor.\n"
     "\n"
     "HTTP endpoint (loopback only):\n"
     "  {0}\n"
     "\n"
     "stdio bridge (for hosts that launch a process):\n"
     "  {1}\n"
     "\n"
     "The endpoint, token and tool list are also written to:\n"
     "  {2}\n"
     "\n"
     "Use \"Copy MCP endpoint\" to put the URL on the clipboard.",
"让任何支持 MCP 的 Agent 接入这个编辑器。\n"
     "\n"
     "HTTP 端点（仅回环地址）：\n"
     "  {0}\n"
     "\n"
     "stdio 桥（适用于只能启动进程的宿主）：\n"
     "  {1}\n"
     "\n"
     "端点、Token 与工具清单也会写入：\n"
     "  {2}\n"
     "\n"
     "用「复制 MCP 端点」可以把 URL 放进剪贴板。",
"MCP 対応のエージェントをこのエディタに向けてください。\n"
     "\n"
     "HTTP エンドポイント（ループバックのみ）:\n"
     "  {0}\n"
     "\n"
     "stdio ブリッジ（プロセスを起動するホスト向け）:\n"
     "  {1}\n"
     "\n"
     "エンドポイント・トークン・ツール一覧は次にも書き出されます:\n"
     "  {2}\n"
     "\n"
     "「MCP エンドポイントをコピー」で URL をクリップボードに置けます。"},

    // --- language switching -------------------------------------------------
    {"dlg.language.changed", "The editor interface switched to {0}.\n"
     "\n"
     "The language is remembered for the next start and can be changed again from View -> Language.",
"编辑器界面已切换为 {0}。\n"
     "\n"
     "该选择会被记住，下次启动仍然生效；也可以通过「视图 → 语言」再次切换。",
"エディタの表示言語を {0} に切り替えました。\n"
     "\n"
     "この設定は次回起動時にも引き継がれ、「表示 → 言語」からいつでも変更できます。"},

    // --- native file dialogs ------------------------------------------------
    {"fd.open.title", "Open Markdown document", "打开 Markdown 文档", "Markdown 文書を開く"},
    {"fd.save.title", "Save Markdown document", "保存 Markdown 文档", "Markdown 文書を保存"},
    {"fd.filter.markdown", "Markdown (*.md;*.markdown;*.mdown)", "Markdown (*.md;*.markdown;*.mdown)",
     "Markdown (*.md;*.markdown;*.mdown)"},
    {"fd.filter.text", "Text (*.txt)", "文本 (*.txt)", "テキスト (*.txt)"},
    {"fd.filter.all", "All files (*.*)", "所有文件 (*.*)", "すべてのファイル (*.*)"},

    // --- printing and exporting ---------------------------------------------
    {"export.title", "Print and export", "打印与导出", "印刷と書き出し"},
    {"export.untitled", "untitled", "未命名", "無題"},
    {"export.pdf.title", "Export as PDF", "导出为 PDF", "PDF として書き出し"},
    {"export.word.title", "Export as Word", "导出为 Word", "Word として書き出し"},
    {"export.image.title", "Export as image", "导出为图片", "画像として書き出し"},
    {"export.pdf.done", "Saved the PDF to:\n{0}", "已导出 PDF：\n{0}",
     "PDF を書き出しました:\n{0}"},
    {"export.word.done", "Saved the Word document to:\n{0}", "已导出 Word 文档：\n{0}",
     "Word 文書を書き出しました:\n{0}"},
    {"export.image.done", "Saved the picture to:\n{0}", "已导出图片：\n{0}",
     "画像を書き出しました:\n{0}"},
    {"export.failed", "Could not write the file:\n{0}", "无法写入文件：\n{0}",
     "ファイルを書き出せませんでした:\n{0}"},
    {"export.unsupported",
     "This WebView2 runtime cannot print to PDF (it needs 1.0.1108 or newer).",
     "当前 WebView2 运行时无法导出 PDF（需要 1.0.1108 或更新版本）。",
     "この WebView2 ランタイムは PDF 出力に対応していません（1.0.1108 以降が必要）。"},

    // --- command line -------------------------------------------------------
    {"cli.usage", "Colaimdation - local WYSIWYG Markdown editor with an embedded MCP server\n"
     "\n"
     "usage: colaimdation [options] [document.md]\n"
     "\n"
     "options:\n"
     "  --file <path>            open this document (same as passing it positionally)\n"
     "  --port <n>               fix the MCP port (default: an ephemeral port)\n"
     "  --token <value>          use this MCP access token instead of generating one\n"
     "  --config <path>          read security policy overrides from a JSON file\n"
     "  --read-only              open the document read-only (no agent writes)\n"
     "  --confirm                require diff confirmation for every agent change\n"
     "  --allow-dir <path>       restrict file access to this directory tree\n"
     "  --demo                   open the built-in welcome document (default)\n"
     "  --no-demo                start with an empty document instead\n"
     "  --theme <auto|light|dark>  editor colour theme\n"
     "  --lang <tag>             interface language, one of: {0}\n"
     "  --font-size <points>     editor font size, 8..48\n"
     "  --no-webview             do not initialise the WebView2 front-end (diagnostics)\n"
     "  --log                    write the audit log next to the executable\n"
     "  --help                   show this message\n"
     "\n"
     "The editor listens on 127.0.0.1 only. The endpoint URL and local token are written to\n"
     "%LOCALAPPDATA%\\colaimdation\\mcp.json and shown in the Agent panel.\n"
     "",
"Colaimdation - 本地原地实时渲染的 Markdown 编辑器，内置 MCP 服务\n"
     "\n"
     "用法: colaimdation [选项] [文档.md]\n"
     "\n"
     "选项:\n"
     "  --file <路径>            打开指定文档（等同于直接传入路径）\n"
     "  --port <端口>            固定 MCP 端口（默认由系统分配）\n"
     "  --token <值>             使用指定的 MCP 访问 Token（默认随机生成）\n"
     "  --config <路径>          从 JSON 文件读取安全策略覆盖项\n"
     "  --read-only              以只读方式打开（禁止 Agent 写入）\n"
     "  --confirm                每次 Agent 改动都要求 Diff 确认\n"
     "  --allow-dir <路径>       限制文件访问到该目录树\n"
     "  --demo                   打开内置欢迎文档（默认）\n"
     "  --no-demo                改为启动空白文档\n"
     "  --theme <auto|light|dark>  编辑器配色主题\n"
     "  --lang <语言>            界面语言，可选: {0}\n"
     "  --font-size <磅值>       编辑器字号，8..48\n"
     "  --no-webview             不初始化 WebView2 前端（诊断用）\n"
     "  --log                    把审计日志写到程序所在目录\n"
     "  --help                   显示本帮助\n"
     "\n"
     "编辑器只监听 127.0.0.1。端点 URL 与本地 Token 会写入\n"
     "%LOCALAPPDATA%\\colaimdation\\mcp.json，并显示在 Agent 面板中。\n"
     "",
"Colaimdation - MCP サーバー内蔵のローカル WYSIWYG Markdown エディタ\n"
     "\n"
     "使い方: colaimdation [オプション] [文書.md]\n"
     "\n"
     "オプション:\n"
     "  --file <パス>            この文書を開く（パスを直接渡すのと同じ）\n"
     "  --port <番号>            MCP ポートを固定（既定は自動割り当て）\n"
     "  --token <値>             この MCP アクセストークンを使う（既定は自動生成）\n"
     "  --config <パス>          JSON ファイルからセキュリティポリシーを上書き\n"
     "  --read-only              読み取り専用で開く（エージェントの書き込み禁止）\n"
     "  --confirm                エージェントの変更ごとに差分確認を必須にする\n"
     "  --allow-dir <パス>       ファイルアクセスをこのディレクトリ以下に制限\n"
     "  --demo                   同梱のウェルカム文書を開く（既定）\n"
     "  --no-demo                空の文書で開始する\n"
     "  --theme <auto|light|dark>  エディタの配色テーマ\n"
     "  --lang <タグ>            表示言語: {0}\n"
     "  --font-size <pt>         エディタの文字サイズ、8..48\n"
     "  --no-webview             WebView2 フロントエンドを初期化しない（診断用）\n"
     "  --log                    監査ログを実行ファイルの隣に書き出す\n"
     "  --help                   このヘルプを表示\n"
     "\n"
     "エディタは 127.0.0.1 のみを待ち受けます。エンドポイント URL とローカルトークンは\n"
     "%LOCALAPPDATA%\\colaimdation\\mcp.json に書き出され、エージェントパネルに表示されます。\n"
     ""},
    {"cli.unknown_option", "unknown option: {0}", "未知的选项：{0}", "不明なオプション: {0}"},
    {"cli.missing_value", "missing value for {0}", "{0} 缺少参数值", "{0} の値がありません"},
    {"cli.port.range", "--port must be between 0 and 65535", "--port 必须在 0 到 65535 之间",
     "--port は 0〜65535 の範囲で指定してください"},
    {"cli.port.number", "--port needs a number, got '{0}'", "--port 需要数字，收到 '{0}'",
     "--port には数値を指定してください（'{0}'）"},
    {"cli.port.out_of_range", "--port is out of range: '{0}'", "--port 超出范围：'{0}'",
     "--port が範囲外です: '{0}'"},
    {"cli.font.number", "--font-size needs a number, got '{0}'", "--font-size 需要数字，收到 '{0}'",
     "--font-size には数値を指定してください（'{0}'）"},
    {"cli.font.range", "--font-size must be between 8 and 48", "--font-size 必须在 8 到 48 之间",
     "--font-size は 8〜48 の範囲で指定してください"},
    {"cli.lang.invalid", "--lang must be a language tag such as {0}",
     "--lang 需要语言标签，例如 {0}", "--lang には {0} のような言語タグを指定してください"},
    {"cli.lang.unknown", "unknown language '{0}'; falling back to English. Available: {1}",
     "未知语言 '{0}'，已回退到英文。可选语言：{1}",
     "不明な言語 '{0}' のため英語にフォールバックします。利用可能: {1}"},
    {"cli.config.unreadable", "cannot read the config file '{0}': {1}",
     "无法读取配置文件 '{0}'：{1}", "設定ファイル '{0}' を読み込めません: {1}"},
    {"cli.config.malformed", "cannot parse the config file '{0}': {1} (line {2})",
     "无法解析配置文件 '{0}'：{1}（第 {2} 行）",
     "設定ファイル '{0}' を解析できません: {1}（{2} 行目）"},
    {"cli.error.prefix", "Colaimdation: {0}\n\nSee --help.", "Colaimdation: {0}\n\n请运行 --help 查看用法。",
     "Colaimdation: {0}\n\n--help で使い方を確認してください。"},
    {"cli.com_failed", "Colaimdation: cannot initialise COM.", "Colaimdation: 无法初始化 COM。",
     "Colaimdation: COM を初期化できません。"},
    {"cli.classes_failed", "Colaimdation: cannot register its window classes: {0}",
     "Colaimdation: 无法注册窗口类：{0}", "Colaimdation: ウィンドウクラスを登録できません: {0}"},
    {"cli.warning.prefix", "Colaimdation: warning: {0}", "Colaimdation: 警告: {0}",
     "Colaimdation: 警告: {0}"},
    {"cli.mcp_unavailable", "Colaimdation: MCP server unavailable: {0}",
     "Colaimdation: MCP 服务不可用：{0}", "Colaimdation: MCP サーバーを利用できません: {0}"},
};

// Column offsets let one table serve every language without a per-language map.
size_t column_offset(const char* tag) {
    const std::string value = tag ? tag : kEnglish;
    if (value == kChineseSimplified) return offsetof(Row, zh);
    if (value == kJapanese) return offsetof(Row, ja);
    return offsetof(Row, en);
}

const char* column_value(const Row& row, size_t offset) {
    return *reinterpret_cast<const char* const*>(reinterpret_cast<const char*>(&row) + offset);
}

const std::map<std::string, const Row*>& english_index() {
    static const std::map<std::string, const Row*> index = [] {
        std::map<std::string, const Row*> map;
        for (const Row& row : kRows) map.emplace(row.key, &row);
        return map;
    }();
    return index;
}

std::string& active_tag_storage() {
    static std::string tag = kEnglish;
    return tag;
}

} // namespace

const std::vector<LanguageInfo>& languages() {
    static const std::vector<LanguageInfo> kLanguages = {
        {kEnglish, "English", "English"},
        {kChineseSimplified, "Chinese (Simplified)",
         "\xe7\xae\x80\xe4\xbd\x93\xe4\xb8\xad\xe6\x96\x87"},
        {kJapanese, "Japanese", "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e"},
    };
    return kLanguages;
}

bool is_supported(std::string_view tag) {
    const std::string lower = utf8::to_lower(tag);
    if (lower.empty()) return false;
    for (const LanguageInfo& language : languages()) {
        const std::string candidate = utf8::to_lower(language.tag);
        if (lower == candidate) return true;
        // Accept "zh-hans-cn", "zh_cn", "en-us" style inputs.
        if (lower.rfind(candidate, 0) == 0) {
            const char next = lower.size() > candidate.size() ? lower[candidate.size()] : '\0';
            if (next == '\0' || next == '-' || next == '_') return true;
        }
    }
    // Plain "zh" and "zh-cn" are the common spellings of Simplified Chinese.
    if (lower == "zh" || lower.rfind("zh-", 0) == 0 || lower.rfind("zh_", 0) == 0) return true;
    return false;
}

std::string normalize_tag(std::string_view tag) {
    const std::string lower = utf8::to_lower(tag);
    if (lower.empty()) return kEnglish;
    if (lower.rfind("ja", 0) == 0) {
        const char next = lower.size() > 2 ? lower[2] : '\0';
        if (next == '\0' || next == '-' || next == '_') return kJapanese;
    }
    if (lower.rfind("zh", 0) == 0) {
        const char next = lower.size() > 2 ? lower[2] : '\0';
        if (next == '\0' || next == '-' || next == '_') return kChineseSimplified;
    }
    if (lower.rfind("en", 0) == 0) {
        const char next = lower.size() > 2 ? lower[2] : '\0';
        if (next == '\0' || next == '-' || next == '_') return kEnglish;
    }
    return kEnglish;
}

std::string from_system_locale() {
    wchar_t buffer[LOCALE_NAME_MAX_LENGTH] = {};
    if (::GetUserDefaultLocaleName(buffer, LOCALE_NAME_MAX_LENGTH) > 0) {
        return normalize_tag(utf8::from_wide(buffer));
    }
    return kEnglish;
}

std::string native_name(std::string_view tag) {
    const std::string normalized = normalize_tag(tag);
    for (const LanguageInfo& language : languages()) {
        if (language.tag == normalized) return language.native_name;
    }
    return "English";
}

void set_language(std::string_view tag) {
    active_tag_storage() = is_supported(tag) ? normalize_tag(tag) : kEnglish;
}

const std::string& language() { return active_tag_storage(); }

std::string tr(std::string_view key) {
    const auto found = english_index().find(std::string(key));
    if (found == english_index().end()) return std::string(key);
    const size_t offset = column_offset(active_tag_storage().c_str());
    const char* value = column_value(*found->second, offset);
    if (value && *value) return std::string(value);
    return std::string(found->second->en);
}

std::string tr(std::string_view key, const std::vector<std::string>& arguments) {
    std::string text = tr(key);
    for (size_t i = 0; i < arguments.size(); ++i) {
        const std::string token = "{" + std::to_string(i) + "}";
        size_t position = 0;
        while ((position = text.find(token, position)) != std::string::npos) {
            text.replace(position, token.size(), arguments[i]);
            position += arguments[i].size();
        }
    }
    return text;
}

std::vector<std::string> keys_for(std::string_view tag) {
    (void)tag;
    std::vector<std::string> keys;
    keys.reserve(std::size(kRows));
    for (const Row& row : kRows) keys.emplace_back(row.key);
    return keys;
}

// Used by the tests to prove no language has a missing or empty column.
std::vector<std::string> untranslated_keys_for(std::string_view tag) {
    const size_t offset = column_offset(is_supported(tag) ? normalize_tag(tag).c_str() : kEnglish);
    std::vector<std::string> missing;
    for (const Row& row : kRows) {
        const char* value = column_value(row, offset);
        if (!value || !*value) missing.emplace_back(row.key);
    }
    return missing;
}

} // namespace colai::i18n
