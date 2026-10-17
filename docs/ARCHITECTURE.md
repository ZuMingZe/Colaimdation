# Colaimdation 架构说明## 目标与约束

产品目标只有一句话：**让任意 MCP Agent 在编辑器内存中的文档上协同写作，而不是读写磁盘文件。**

由此推出四条硬性设计约束：

1. **单一真相源**。文档只存在于一个地方：`Document`（内存 UTF-8 缓冲 + 块结构）。
   编辑器前端、MCP 工具、审计日志都从它派生，不存在"文件版本"和"内存版本"两个真相。
2. **所有修改都必须可寻址、可审计、可回滚**。因此没有"直接改文件"的旁路，
   所有写入都走 `Document::apply_patch()`。
3. **人类优先级最高**。Agent 的改动不能悄悄覆盖人的输入：并发用乐观并发控制
   （revision 冲突显式报错），必要时用 Diff 确认把决定权交回用户。
4. **默认不信任外部进程**。只监听回环、要求 Token、限制只能操作当前文档。

## 分层

```
        ┌──────────────────────────────────────────────────────────┐
        │ 外部 Agent（OpenClaw / DeepSeek Harness / WorkBuddy ...） │
        └───────────────┬──────────────────────────┬───────────────┘
                        │ MCP over HTTP (loopback) │ MCP over stdio
                        ▼                          ▼
             ┌────────────────────┐      ┌────────────────────────┐
             │ mcp/http_server    │      │ colaimdation_mcp_bridge│
             │ mcp/mcp_server     │◀─────│ mcp/stdio_server       │
             │ mcp/tool_registry  │ HTTP │ (转发到 /mcp)          │
             │ mcp/tools_*        │      └────────────────────────┘
             └─────────┬──────────┘
                       │（工具调用必须落到 UI 线程）
                       ▼
             ┌──────────────────────────────────────┐
             │ core/workspace                       │
             │  策略 · 审计 · 暂存确认 · 变更通知   │
             └─────────┬────────────────────────────┘
                       ▼
             ┌──────────────────────────────────────┐
             │ core/document                        │
             │  文本缓冲 · 块结构 · Patch · 撤销栈  │
             └─────────┬────────────────────────────┘
                       │ 通知 / 补丁 / 重载
                       ▼
             ┌──────────────────────────────────────┐
             │ app/webview_bridge (WebView2)        │
             └─────────┬────────────────────────────┘
                       ▼
             ┌──────────────────────────────────────┐
             │ web/ 编辑器前端（原地渲染 + 面板）   │
             └──────────────────────────────────────┘
```

`core/` 与 `mcp/` 不依赖 GUI，因此可以在没有窗口的情况下跑完整测试
（`tests/test_mcp.cpp` 就是起一个真的回环 HTTP 服务来跑端到端）。

## 关键设计决策

### 1. 文档模型：文本 + 块结构，而不是 DOM

`core/document.h` 持有 UTF-8 文本缓冲，`core/markdown.*` 从中解析出**块列表**，
每个块记录自己在源码中的精确字节区间：

```cpp
struct Block {
  BlockKind kind;      // heading / paragraph / code_block / table / list / ...
  size_t begin, end;                     // 整块
  size_t content_begin, content_end;     // 去掉标记后的内容（# 号、围栏、列表符号）
  int level;           // 标题级别或列表嵌套深度
  int ordinal;         // 同类型块中的序号
  std::string info;    // 围栏语言 / 表格对齐
};
```

收益：

- **块可寻址**：`replace_block(b7, text)` 会自动保留 `#`、代码围栏、列表符号，
  Agent 不需要重写整段 Markdown，也不容易破坏结构。
- **增量**：改动只需要 diff 一个块，前端只重绘那一块。
- **前端完全对齐**：`web/assets/render.js` 用同样的规则解析，块 id 与源码区间一致，
  所以 DOM ↔ 源码的偏移映射是精确的，不需要"猜测 HTML 对应哪段 Markdown"。

### 2. Patch 是唯一的写入口

```cpp
enum class PatchOp {
  InsertText, ReplaceRange, DeleteRange,       // 字节级
  ReplaceBlock, ReplaceBlockRaw, InsertBlock,  // 块级
  AppendSection, SetText,                      // 文档级
};
```

`apply_patches()` 分三阶段：**校验 → 拒绝重叠 → 倒序应用**。任何一个 patch 非法，
整批都不落地（原子性）。校验内容包括：偏移越界、UTF-8 边界对齐、批量数量与字节上限、
front matter 保护、行范围限制、策略开关。

批次应用后不记录"用户提交的那些 patch"，而是重新计算**净变化**
（`compute_minimal_change`），这样审计日志里一条记录 = 一次语义上的改动，
而不是一次工具调用的机械回放。

### 3. 历史、撤销与回滚是三件不同的事

| 机制 | 数据 | 用途 |
|---|---|---|
| `undo_stack_` | 每个真实编辑的 `text_before` 快照 | 用户 `Ctrl+Z` |
| `redo_stack_` | 被撤销的编辑 | `Ctrl+Y` |
| `history_` | 追加式记录（前后文本 / 来源 / 工具 / session） | 审计、回滚目标解析 |

`undo()` 自己也会写一条 `history_` 记录（工具名 `undo`），但**不会**进入 `undo_stack_`，
所以"连续撤销两次"就是撤销两次真实编辑，而不是撤销"上一次撤销"。
这是本项目里最容易写错的一处，`tests/test_document.cpp` 专门盯住了它。

`sequence` 的语义是"编辑发生前文档所处的版本号"：`after_mutation()` 先自增，
`record_edit()` 再记录 `sequence_ - 1`。所以 `plan_rollback_to_sequence(s)` 表示
"撤掉所有在版本 s 之后发生的编辑"，`rollback_to_revision` 因此直接把编辑的
`sequence` 当作切点。

回滚与撤销是两条路径：回滚走 `RollbackPlan`，可以
"回滚最后一次 Agent 改动 / 回滚到某个 revision / 回滚某个序列号之后的一切"，
并且**回滚本身是一条被审计的新编辑**——任何操作都不销毁历史。

### 4. 线程模型：workspace 归 UI 线程所有

MCP 请求来自 HTTP 工作线程，而 `Document` / `Workspace` 由 UI 线程独占。
两者的桥是 `IEditorBridge::post(job, timeout)`：

```
HTTP 线程                                UI 线程
   │  post(job)                             │
   ├── 放入 jobs_ 表 ──PostMessage(kMsgRunJob)──▶ │ 取出并执行 job
   │                                        │ job->result = ...
   │  wait_for(done)  ◀─── notify_all ──────┤
   ◀── 返回 result                          │
```

测试里的 `ScriptedBridge` 直接同步执行 job，所以核心逻辑可以无窗口测试；
真实实现 `app/webview_bridge.cpp` 走上面的消息派发。`post()` 会检测
"已经在 UI 线程"的情况并内联执行，避免自死锁。

### 5. 前端：原地渲染的编辑模型

`web/assets/editor.js` 的实现选择很朴素但很稳：

- 模型是**整篇文本**，不是 DOM。
- DOM 里每个块一个元素，`data-md-block` 记块号。
- **光标所在块渲染成源码，其余块渲染成 HTML**。这就是"原地实时 WYSIWYG"。
- 打字时直接读该块的 `textContent`（源码视图下是纯文本，和 Markdown 源一一对应），
  写回模型，再把 `(revision, 全文, 光标)` 发给宿主。
- **回车不交给浏览器**：输入法合成期间除外，`beforeinput` 里的 `insertParagraph` /
  `insertLineBreak` 一律 `preventDefault()`，改由 `insertLineBreak()` 往模型的光标处插一个
  `\n`、重排、再按源码偏移恢复光标。交给浏览器的代价很具体——活动块是**可编辑根里的可编辑
  块元素**，Chromium 收到回车时会把块**拆成兄弟副本**（副本连 `data-md-block` 一起复制），
  编辑器只认原来那个元素，于是换行进不了模型、新行上敲的字全部读错元素而被丢弃，段落中间
  回车还会把光标后那半段留在副本里丢掉。万一某个途径没能拦住（事件不可取消、拖放或合成
  事件），`_handleInput` 会把同号的兄弟元素合并回模型，至少不丢字。
- 块的**结尾换行**在活动块里渲染成一个独立的行盒（`<div data-md-term>`）：光标落在块元素
  末尾时，Chromium 会把新敲的字插到上一行行尾（在纯文本节点、`<br>` 结尾时都是如此），
  只有行盒能让"回车后在新行打字"真的落在新行上。读取时该行盒按它代表的那个 `\n` 计数，
  所以没有编辑过的块，DOM 反读出来的文本与 `block.source` 逐字节相同。
- 每次原地更新后都检查这一点：DOM 文本或结尾行盒与 `block.source` 不符就地重画这个块并
  恢复光标；引擎用别的途径（拖放、合成事件）拆出的多份元素则先合并回模型。
- **代码块用代码框编辑，不写源码**：写出 ` ```lang ` 再换行后，这个块变成"代码框"——
  语言标签 + 一个真正的 `<textarea>`（等宽、可 Tab 缩进、随内容长高）+ 下方一行可编辑的
  空行。围栏对用户不可见，也由编辑器维护：换行时立刻补上闭合围栏（否则在文档中间开的
  围栏会把下面整篇吞成代码，而用户再也看不到那行围栏去手工关闭它），用户在框里敲的每一
  次改动再按 `开栏 + 代码 + 闭栏` 写回模型，所以文件、Agent 看到的仍是普通围栏代码块。
  光标停在围栏那一行时（正在写语言）这一块仍是源码视图，点标题栏可以整块切回源码（改语言）。
  下方那行是块的终止行盒：在那里打字即"块后新起一段"，模型里就是围栏后的一个段落。
- **代码框里也要缩进和高亮**：缩进用普通编辑实现——回车沿用本行缩进，行尾是 `{`、`[`、`(`
  或以 `:` 结尾（且不是注释行）时多缩进一级，光标前是收尾括号时新行留在外层；`Tab`/`Shift+Tab`
  增减一级（`json`/`js`/`html` 等按两空格，其余按四空格），选中多行时整段一起移动；行首只有
  空白时敲 `}`/`)`/`]` 先退回一级。这些编辑走 `document.execCommand('insertText')` 而不是直接
  赋值 `value`，因为赋值会清掉文本框自己的撤销历史——按 `Tab` 之后 `Ctrl+Z` 应当能撤销缩进。
  高亮不能画在文本框里面（文本框只存纯文本），也不能改写光标所在的 DOM：颜色画在文本框**下面**
  的一层 `<pre>`（`.md-code-highlight`，与文本框同宽同字体同内边距，随横向滚动同步），
  文本框自身文字设为透明、只保留 `caret-color`；两者用同一套 highlight.js 规则
  （`Render.highlightCodeHtml`），所以编辑中和渲染后颜色一致。选区背景用半透明色，
  否则不透明的选区会把下面那层颜色盖住；输入法组字期间隐藏颜色层、显式显示文本框文字，
  否则组字串是看不见的。
- 结构性变化（出现/消失换行、块前缀变化）会重新解析并重建 DOM，
  然后按**块序号 + 块内偏移**恢复光标。
- Agent 改动到达时，先记录"当前块在视口中的相对位置"，重建后再把同一个块滚回原位，
  所以 Agent 改文档时你的阅读位置不会跳。

没有采用"把 contenteditable 的富文本反推成 Markdown"的做法，因为那必然是有损的，
而 Agent 需要看到的是准确的 Markdown。

### 6. 安全：为什么不给 Agent 一个文件路径参数

因为一旦有路径参数，就有"打开别的文件"的可能，权限隔离就只剩一句承诺。
所以工具集里**没有任何**接受路径的读取工具；唯一的 `save_document` 只能写当前文档，
且受 `allowed_directory` 与 `allow_save` 限制。

鉴权细节：Token 由 `BCryptGenRandom` 生成 32 字节，比较用常数时间实现；
HTTP 只绑定 `INADDR_LOOPBACK`，并校验 `Host` 头是回环（防止 DNS rebinding 类场景）。
`/health` 无需鉴权，方便探活。

### 7. Diff 确认的落地点
`require_confirmation=true` 时，`apply_agent_patches()` 不直接改文档，而是：

1. 把当前文本复制到一个临时 `Document`；
2. 在副本上试算 patch，得到 `revision_after` 和 unified diff；
3. 通过 `IEditorBridge::request_confirmation()` 把 diff 推给前端并**阻塞等待**；
4. 用户点 "Apply change" → 在真实文档上重放同一批 patch；点 "Reject" → 返回
   `rejected_by_user` 错误，文档一个字节都没动。

等待期间 UI 线程是活的（阻塞的是 HTTP 线程），所以面板可交互、窗口可响应。
预览失败（patch 本身非法）不会让文档处于半改状态。

### 8. 多语言：两个目录，一套规则

界面文字分两处，因为它们属于两个不同的世界：

| | 位置 | 覆盖 | 查找方式 |
|---|---|---|---|
| 外壳 | `src/core/i18n.cpp` 的 `kRows` 表 | 菜单、对话框、窗口标题、文件对话框、命令行帮助 | `i18n::tr(key)` |
| 前端 | `web/assets/i18n.js` 的 `PACKS` + `web/assets/lang/*.json` | 工具栏、面板、状态栏、Toast、空状态 | `ColaiI18n.t(key)` |

**为什么外壳用「一行一字符串、一列一语言」的表**：漏翻一个字符串是最常见的 i18n
缺陷，而表结构让「某行某列为空」在测试里一查即知（`untranslated_keys_for`）。
用 `offsetof` 算列偏移，一张表就能服务所有语言，不需要每语言一个 map。

**为什么前端语言包内嵌在 JS 里而不是 fetch 出来**：页面跑在
`connect-src 'none'` 的 CSP 下，一个本地编辑器也不该为了画自己的工具栏发网络请求。
`web/assets/lang/*.json` 是同一份内容的可读副本，方便翻译与合并，由测试保证两边键集合一致。

**为什么 MCP 工具描述不翻译**：那是 Agent 面向的协议接口。工具的 `name`、参数名与
`description` 保持英文，可预测性优先；翻译只作用于人看到的界面文字。

切换语言的三个入口最终汇聚到同一处：

```
工具栏语言菜单 ──postMessage {type:'language_changed'}──┐
原生菜单 视图→语言 ──apply_language(tag, true)─────────┤
命令行 --lang ──create() 时设置───────────────────────┘
                                                       ▼
                     i18n::set_language()  +  ColaiI18n.setLanguage()
                       ├─ 重建 HMENU（SetMenu + DrawMenuBar）
                       ├─ 重画窗口标题
                       ├─ 前端 applyToDom() 重写所有 data-i18n / data-i18n-title
                       ├─ 前端重渲染代码里拼出来的文字（状态栏、动态活动、审计页脚）
                       └─ 写入 %LOCALAPPDATA%\colaimdation\settings.json
```

`--lang` 会在**解析其它参数之前**预扫描一次，这样连参数错误都以用户要求的语言报出。
未知的语言标签不抛异常，而是记一条警告并回退到英文——打错一个字不该阻止人打开编辑器。

语言优先级：`--lang` > `settings.json` > 操作系统区域设置。

`colaimdation_get_workspace_info` 与 `/health` 会带 `language` 字段，方便排查
「为什么界面是这种语言」。

### 9. 一个窗口，多篇文档（标签页）

`Workspace` 里的 "the document" 从**一个成员**变成了**一个向量加一个活动下标**：

```cpp
std::vector<std::unique_ptr<Document>> documents_;
size_t active_ = 0;
Document& document() { return *documents_[active_]; }   // 所有工具看到的都是它
```

之所以只改到这里，是因为"Agent 只能操作当前文档"这条安全承诺本来就不需要
"只有一个 Document"，它需要的是**工具集里没有打开/切换文档的能力**。于是：

- MCP 工具、审计、Diff 确认、`save_document` 全部继续调用 `document()`，
  也就是**活动标签页**；`colaimdation_get_workspace_info` 额外返回
  `tabs` / `active_tab` / `tab_count`，让 Agent 知道自己看的是第几篇。
- 每个 `Document` 自带光标、撤销栈、行尾风格与历史，所以切标签就是切真相源，
  不需要在前端保存任何"每篇文档的状态"；前端只负责把标签栏画出来。
- 未命名文档没有路径，而 `Document` 的 id 是从路径派生的，所以新建标签页会给它
  一个独立 id（`doc-untitled-N`），否则两篇新文档的审计与待确认改动会混在一起。
- 待确认改动（`PendingChange`）会记下 `document_id`：人在 diff 面板等待期间切了标签，
  接受动作会以 `conflict` 失败，而不是把补丁打到另一篇文档上。
- `open_file` 会先查重（大小写与斜杠都归一化）：同一篇文件只允许一个标签页，
  否则两个标签页会争"谁最后保存"。第一次打开还会复用那个空的占位文档，
  避免"未命名"和刚打开的文件并排出现。

### 10. 排版工具栏：把 Markdown 语法藏起来

面向"会用 Markdown 排版、但不会写 Markdown 语法"的人，工具栏第二行是一排按钮。
实现的取舍是：**按钮不碰 DOM，只改模型**。

`editor.js` 暴露三个动作，全部走"改 `this.text` → 重新渲染 → 恢复光标/选区 →
`_notifyChange()`"这条路，和敲键盘完全同一条路径（宿主收到的仍是一次普通用户编辑，
去抖、审计、revision 校验都不用特殊照顾）：

| 动作 | 语义 |
|---|---|
| `insertBlock(kind)` | 空行就地把这一行变成该块；行里有字就在下面另起一行——"点 H2 然后打字"，且**永不改写已经写好的文字** |
| `wrapSelection(kind)` | 有选区就包住选区并保持选中；没有选区就插入空标记并把光标放中间（链接/图片/公式插入带占位符的词，占位符是选中的，第一次按键就替换它） |
| `tableAddRow` / `tableAddColumn` | 按当前光标所在的表格块，整行追加单元格；对齐行加 `---`，空行加空格，不会把正文行变成分隔线 |

列表续行放在 `insertLineBreak()`（也就是 Enter 的必经之路）：行尾回车沿用同一个标记
（有序列表自动 +1、任务清单补 `- [ ]`），而在**没有内容的列表项**上再回车会去掉标记
结束列表——否则不会写 Markdown 的人会被困在一条永远续下去的列表里。

界面上这些能力只是 `data-format="heading2"` 这样的属性 + 一个事件委托，
文案走前端语言包，所以切语言时按钮和提示一起变。

### 11. 外观与外壳的两条硬性约束

**主题是一个"偏好"，不是两个颜色值**。`data-theme` 只会是 `light` 或 `dark`：
`light`/`dark`/`auto` 是人的选择（`data-theme-preference`），`auto` 在页面里用
`matchMedia('(prefers-color-scheme: dark)')` 解析成具体主题，并监听系统变化。
这样样式表里只有一套深色 token，不用为"自动"再抄一遍。
`color-scheme` 也一起切，滚动条、输入框、光标这些原生控件才会跟着变。

**启动信息每次重新读**。页面在 `ready` 时会重新要一次 bootstrap，所以外壳里的
`EditorWindow::build_bootstrap()` 是"按当前设置现算"而不是启动时算好缓存的——
否则切完语言/外观再按 F5，页面会退回启动时的样子。

**编辑器不能被导航走**。WebView2 里没有后退按钮，一次原地导航就会把工具栏、标签页、
面板全部顶掉且无法返回（页面里点链接就是这种情况）。所以主机侧装了
`NavigationStarting`：**除了 `https://<虚拟主机>/...` 之外的任何导航都取消**，
并用 `ShellExecuteW` 交给系统默认浏览器；`NewWindowRequested`（`target="_blank"`、
`window.open`）同样处理。`about:`/`data:`/`javascript:` 只取消、不调浏览器。

## 数据流：一次 Agent 写入
```
tools/call apply_patch
  → mcp_server: 鉴权、解析 JSON-RPC
  → tool_registry: 分发到 tools_document.cpp
  → parse_patches: 把 JSON 转成 Patch（含 op/字段校验）
  → workspace.apply_agent_patches
      ├─ 策略：allow_write / 数量上限 / 字节上限 / front matter / 行范围
      ├─ 需要确认？→ stage_change（副本试算 + diff）→ request_confirmation
      └─ commit_patches → document.apply_patches（校验→拒绝重叠→倒序应用）
  → audit_.record（谁、哪个工具、哪个 session、字节增删、diff）
  → notify_document_changed → WebView2 PostWebMessageAsJson
  → 前端 applyDocumentChanged（保持视口锚点 + 高亮改动块）
  ← 返回 { status, revision_before, revision_after, diff, audit_id }
```

## 数据流：一次人工编辑

```
用户打字 → editor.js _handleInput
  → 读块 textContent 写回模型 → 去抖 220ms
  → postMessage { type:'edit', revision_before, text, caret }
  → workspace.apply_user_edit
      ├─ 文本相同 → { status:'unchanged' }（不触发任何重载）
      └─ 不同 → set_text(...) 记录为 source=user 的编辑
  → 广播 user_activity（给 Agent）
  → Agent 下次 get_user_changes 就能看到"人类改了什么"
```

## 数据流：打印与导出

打印、导出 PDF、导出图片共用同一套"纸张"准备，导出 Word 走引擎自己的块模型：

```
File 菜单 → 打印          → bridge.print_document()
                          → page: exportPrepare('print')   （渲染全部块、隐藏 UI 由 CSS 负责、切浅色）
                          → ICoreWebView2_16::ShowPrintUI(SYSTEM)
                             失败则回退 window.print()
                          → 页面 afterprint → exportFinish() 还原编辑器与主题
File 菜单 → 导出 PDF      → page: exportPrepare('pdf') → ICoreWebView2_7::PrintToPdf
                          → 完成后 exportFinish() + 结果提示
File 菜单 → 导出图片      → page: exportPrepare('image') 返回 {width,height}
                          → 控制器 bounds 长到文档高度（窗口裁切但引擎照常渲染整页）
                          → CapturePreview(PNG) → 写文件 → 还原 bounds + exportFinish()
File 菜单 → 导出 Word     → core/docx.h：md::parse(文档) → word/document.xml
                          → core/zip_writer.h 打包（[Content_Types].xml/_rels/.rels/…）→ .docx
```

要点：

- **打印不全 + 印出 UI** 的根因在样式：布局是 `100vh` 网格、编辑列 `overflow:auto`，所以引擎只印
  一屏；工具栏/状态栏等也没有被排除。`@media print` 现在解除高度与滚动、隐藏 chrome、强制浅色纸张，
  并避免标题与代码块被分页切断。`Ctrl+P` 仍交给引擎自带的打印（它能用），页面在 `beforeprint`
  里做同样的准备，所以两条入口的输出一致。
- **导出图片**为什么不用页面截图：编辑器是 100vh 网格，窗口截图只会得到一屏。这里把 WebView2 的
  渲染面长到文档高度再整张捕获，得到"整篇文档、无 UI"的图；捕获后立刻还原 bounds。
- **导出 Word**不引第三方库：`core/docx.cpp` 用引擎已有的块模型（`md::parse` / `parse_inline` /
  `parse_table` / `parse_list`）生成 OOXML，`core/zip_writer.h` 写 ZIP（stored + CRC-32）。
  公式按 LaTeX 源码、图片以占位文字呈现——宁可忠实标注，也不给出错误的排版。
- 三个导出都有单元测试（`tests/test_export.cpp`）与真实浏览器里的纸张布局检查
  （`tools/web_browser.js` 用 `Emulation.setEmulatedMedia('print')` 验证 chrome 隐藏、无裁切、浅色纸张，
  以及 `exportPrepare/exportFinish` 的前后状态）。

## 测试策略

- **引擎级单元测试**：
  `tests/test_{json,utf8,markdown,document,diff,audit_policy,workspace,command_line,i18n}.cpp`。
  刻意去测边界：CJK 与 emoji 的字节偏移、非法 UTF-8 拒绝、patch 重叠、撤销语义、
  CRLF 保留、BOM 剥离、审计容量淘汰、策略钳制、命令行前缀歧义、翻译完整性。
- **MCP 端到端**：`tests/test_mcp.cpp` 启动真实 `HttpServer`，发真实 HTTP 请求，
  断言鉴权、工具调用、冲突、确认、回滚、资源读取、通知、发现文件。

这套测试在开发过程中抓出了若干真实缺陷，每个都补了回归测试：

| 缺陷 | 现象 |
|---|---|
| `Json::operator[]` 用 `set` 而非 `find` | 读取 JSON 成员会把已有值覆盖成 null |
| `Json` 缺少 `const char*` 重载 | 字符串字面量绑定到 `operator[](size_t)` |
| `split_table_row` 丢最后一个格子 | 以 `\|` 结尾的表格行少一列、表格识别失败 |
| 列表延续条件过宽 | 空行后的有序列表被并入无序列表 |
| 编辑合并逻辑 | 连续两次输入被合并成一条，撤销一次退回两步 |
| `advance()` 反向按字节回退 | 越过多字节字符时跳过头 |
| `entries.size()` 在 `std::move` 之后取 | 审计/变更计数恒为 0 |
| HTTP 忽略 `Connection: close` | 客户端一直等 EOF，直到读超时 |
| 审计记录缺 `document_id` | 按文档过滤的审计查询查不到任何东西 |
| 命令行前缀匹配 `"--d..."` | `--demo` 落进 `--allow-dir` 分支并弹出错误框 |
| 编译期路径含非 ASCII | 源目录经 `/D` 传入时被按 ANSI 解码，测试读不到语言包 |
| 语言包漏键 | 前端某个标签显示为键名；现由测试逐键校验 |
| UI 线程消息投递到 dispatcher 窗口 | 那个窗口的过程是 `DefWindowProcW`，于是 `kMsgRunJob`/`kMsgNotify` 全被丢掉：打开文件标题变了但正文不动、保存没有提示、MCP 写入 15 秒后 `editor_unavailable` |
| 非 ASCII 项目路径 + Ninja 头文件依赖 | `.ninja_deps` 里写出乱码路径：头文件依赖静默失效（改了 `.h` 却链接旧布局目标文件，表现为堆内存损坏），随后构建直接 `FindFirstFileExA(...): ... is incorrect`；`tools/build.ps1` 现在映射盘符绕开 |
| 前端 `updateStatusbar(document)` 形参遮蔽 DOM `document` | 状态栏永远停在 `untitled.md / rev -`，且 `applicationReply` 中途抛错，撤销按钮/MCP 端点都不再更新 |
| 页面里点链接 = 原地导航 | WebView2 没有后退按钮：点一个文档里的链接，整个编辑器界面被网页顶掉且关不掉；现在一律取消导航并交给系统默认浏览器 |
| 文档里的链接能把编辑器窗口导航走 | 判断"是不是编辑器自己的页面"时只比对了虚拟主机前缀：相对链接（`other.md` → `https://colaimdation.local/other.md`）被当成编辑器页面放行，WebView2 在映射目录里找不到它，于是整窗变成 Edge 的"未找到文件"错误页；而"视图 → 重载前端"/F5 用的是 `location.reload()`，只会把那张错误页再刷一遍，窗口又没有后退按钮。现在只有 `https://<host>/index.html`（可带 `?`/`#`）算编辑器页面（`core/url_util.h`，带回归测试），其余导航一律取消：同主机 URL 不交给浏览器（那个主机只存在于窗口内），http/https/mailto/file 才送到默认浏览器；导航失败且失败的不是编辑器页面时自动把界面加载回来（最多两次，取消的导航不算失败）；重载前端/F5 改成重新导航到编辑器页面（页面重开后文档与光标由宿主重新下发） |
| bootstrap 在启动时算好并缓存 | 切完语言/外观再按 F5，页面退回启动时的语言/主题；改成每次 `ready` 现算 |
| `math_block` 的收尾 `$$` 没被剥掉 | 块的 `source` 以换行结尾，而收尾正则要求 `$$` 正好在串尾：只有"文件最后一个块"能渲染，其余都把一个多余的 `$$` 交给 KaTeX（`throwOnError:false` 于是画成红字错误），表现为"公式无法渲染" |
| 代码块的 `mousedown` 监听器是"每次重绘都挂一个" | 同一个元素后来变成打开状态的代码框时，旧监听器仍在：按一下就把 `preventDefault` + 光标重置到代码开头，于是**代码框里既不能点光标也不能拖选**；改成根节点上的委托监听 |
| `selectionchange` 60 ms 后激活/重绘所在块 | 拖选（以及 `Ctrl+A`、代码框内的选择）在出现后 60 ms 被重绘 + 折叠成光标，等于**鼠标选不中任何东西**；现在"有选区就不动它"，点击的块激活改在 `mouseup`（指针抬起）时做 |
| 跨越 `contenteditable=false` 块的选区复制为空 | Chrome 对"横跨渲染块"的选区复制出空剪贴板（有高亮、粘出来什么都没有），`Selection.toString()` 也是空；复制事件由编辑器接管，`Ctrl+A` 也从第一个文本节点选到最后一个（而不是 `selectNodeContents(root)`，那种范围复制同样是空） |
| `devtools` 命令被回送给页面 | 页面收到又发回宿主，无限来回：按 F12 单核跑满且 DevTools 从不打开；现在宿主直接调 `ICoreWebView2::OpenDevToolsWindow()` |
| 用户版仍暴露开发者工具与自检 | 加 `-DCOLAI_USER_BUILD=ON`（`tools/build.ps1 -User`）：菜单项、F12 与前端命令全部去掉，其余功能不变 |
| 源码模式的编辑只改了 `text`，没重解析 `blocks` | 渲染视图是按 `blocks` 画出来的：在源码模式里改完切回所见即所得，看到的还是改动前的文档（内容确实是新的，视图是旧的）；现在源码模式的每次输入都重新解析 |
| 源码模式下的文档替换没同步到文本框 | 切标签页/外部重载/Agent 改动走的是 `setDocument`，它只更新模型与渲染视图，`.source-view` 里还留着上一份文档；再敲一个字符就把旧文档写回新标签页。现在 `setDocument` 与 `applyHostText` 一样刷新文本框 |
| 代码块的源码视图被当作"渲染块" | 点标题栏切到源码后，代码块仍是 `code_block`，于是根节点上的委托 `mousedown` 又把它当成渲染块：`preventDefault` + 光标跳到代码开头 + 立刻变回代码框，表现为**代码块的源码里点不了光标、也拖选不了**；现在 `md-active`（已经可编辑的块）直接放行给浏览器 |
| 代码框下方那行画在框里 | 代码框的边框/底色就挂在块元素上，而"框后那一行"（写下一段的地方）是块元素的子节点：移到下一段时光标、以及刚敲下的字都还在代码框的边框和底色里，看起来像"没离开代码框"（内容其实已经按段落后处理）。现在边框/底色移到 `.md-code-frame`（标题 + 代码），那一行留在 frame **外面** |
| 代码框边界的 Backspace | 框后那行上按 Backspace 被 Chrome 当成"与上一行合并"：光标跑进代码框、落在最后一个字符后面，接着敲的字就变成代码；而框里没代码时又没法把整个框删掉。现在框内为空时 Backspace 移除整个围栏块（连同一行空行分隔，避免留下双空行），框后空行上的 Backspace 不再把光标交回代码框（框为空时同样移除整块）。紧跟在代码框**后面的普通块**开头也一样：没有可合并的文字，之前引擎自己合并就会把光标交回代码框；现在在根节点上拦下（`_handleBoundaryKeyDown`，只在 `activeIndex` 的那个块、且原始偏移为 0 时生效，代码框内的按键不受影响），前面是空框时顺带删掉空框 |
