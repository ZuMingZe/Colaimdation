# Colaimdation

> 一款本地**原地实时渲染**的 Markdown 编辑器，内置 **MCP Server**。
> 任意支持 MCP 的外部 Agent（OpenClaw / DeepSeek Harness / WorkBuddy 等）都可以接入，
> 在**编辑器内存中的文档**上和你协同写作，而不是绕过编辑器直接读写磁盘文件。

C++20 / Win32 / WebView2，无第三方运行时依赖（Mermaid、KaTeX、highlight.js 已内置到 `third_party/vendor`）。

---

## 这是什么

| | 传统做法 | Colaimdation |
|---|---|---|
| Agent 看到的文档 | 磁盘上的文件 | 编辑器**内存中**的文档（含你尚未保存的修改） |
| Agent 改文档的方式 | 整篇重写文件 | **增量 Patch**（按字节偏移 / 按块 id） |
| 你的输入 | 被覆盖 / 冲突 | 永远不会丢：冲突会变成明确的错误 |
| Agent 的改动 | 静默发生 | 实时渲染 + 审计日志 + 一键回滚 |
| 每个 Agent | 各写一个适配器 | 统一的 MCP 协议，一次接入全部可用 |
| 访问范围 | 整个文件系统 | **只能操作当前打开的那一篇文档**（多标签页时即活动标签页） |

## 功能

### 基础编辑能力
- 打开 / 保存本地 `.md` 文件（自动识别并保留原文件的 CRLF/LF 行尾，支持 UTF-8 BOM 与 GBK 回退）
- **同时打开多篇文档**：每个文件一个标签页，标签上带未保存标记，点标签切换（`Ctrl+Tab` /
  `Ctrl+Shift+Tab`）、`Ctrl+W` 关闭；每篇文档各自保留光标、撤销历史与行尾风格，
  关掉未保存的标签会先问你要不要保存。Agent 始终只作用于**当前活动的那一篇**
- **不会 Markdown 也能排版**：工具栏第二行是排版条，点按钮即可插入标题、列表
  （回车自动续行）、任务清单、引用、代码块、分隔线、表格（可加行/加列），
  以及粗体、斜体、行内代码、链接、图片、公式——语法由编辑器写，人只需要写字
- **鼠标选中 / 复制粘贴**：拖选、双击选词、`Ctrl+A` 全选、`Ctrl+C/X/V` 复制剪切粘贴都正常；
  代码框是真正的文本框（鼠标点哪光标就落哪、框内可拖选），`Ctrl+A` 在代码框里选的是框里的代码，
  在正文里则是整篇（含渲染成 HTML 的那些块）
- **原地实时 WYSIWYG**：光标所在行显示 Markdown 源码，其余内容实时渲染
- **代码块用代码框编辑**：写出 ` ```lang ` 再换行，这一块立刻变成带语言标签的代码框，
  直接在框里写代码即可（自动长高、围着框的行由编辑器维护，且会自动补上闭合围栏）；
  光标停在围栏那一行时仍是源码，点标题栏可整块切回源码去改语言
- **代码框里自动缩进 + 语法高亮**：回车沿用本行缩进，行尾是 `{`、`[`、`(` 或以 `:` 结尾时
  自动多缩进一级，行首敲 `}`/`)`/`]` 会先退回一级；`Tab` / `Shift+Tab` 增减一级
  （`json`/`js`/`html` 等按两空格，其余按四空格；选中多行则整段一起移动），
  而且编写过程中代码就是高亮的——颜色画在文本框下面的图层上，光标、选区和输入法都不受影响
- 支持 Mermaid 流程图/时序图、KaTeX 行内与块级公式、表格、任务列表、代码高亮
- **打印与导出都在「文件」菜单里**：`打印`（`Ctrl+P`）只打印文档本身——工具栏、标签页、
  状态栏、侧栏都不会出现，编辑器不再只打印一屏（滚动容器和固定高度在打印样式里被解除），
  光标所在块也按渲染结果打印、不会印出 Markdown 记号；纸张始终是浅色，深色主题不会印成黑页。
  还有 `导出为 PDF`、`导出为 Word`、`导出为图片`：PDF 由 WebView2 直接生成，图片是整篇文档
  （按内容高度、1.5× 分辨率）的一张 PNG，Word 是真正的 `.docx`（标题/段落/粗斜体/行内代码/
  链接/列表/引用/代码块/表格/分隔线，公式按 LaTeX 源码、图片以占位文字呈现）
- **链接不会顶掉编辑器**：文档里的链接（含 `target="_blank"`）一律交给**系统默认浏览器**
  打开；编辑器的页面本身无法被导航走——WebView2 里没有后退按钮，一旦导航走整个界面就没了。
  指向本机文件的相对链接（如 `other.md`）不会去加载：那个地址只存在于窗口内部，点了只是不动，
  绝不会把窗口变成浏览器的"未找到文件"页；万一前端真的加载失败，程序会自动把界面加载回来，
  也可以用 视图 →「重载前端」/`F5` 随时回到编辑器
- 标准编辑操作：增删文字、选区、光标管理；`Ctrl+Z` / `Ctrl+Y` 撤销重做；文档快照与历史
- `Ctrl+/` 切换整篇源码模式（大文件或精确编辑时更顺手）

### 多语言界面
内置**英文 / 简体中文 / 日本語**三种界面语言，菜单、对话框、窗口标题、状态栏、
Agent 面板、审计面板、Diff 面板与提示消息全部跟随切换。

- 工具栏右上角的语言按钮（地球图标）随时切换，**界面即时更新，无需重启**
- 菜单 **视图 → 语言** 也可切换（当前语言带单选标记）
- 首次启动**跟随操作系统语言**；手动选择后会记住，下次启动沿用
- 命令行 `--lang en|zh-Hans|ja` 可临时覆盖（也接受 `zh-CN`、`ja-JP` 这类标签）
- 选择持久化在 `%LOCALAPPDATA%\colaimdation\settings.json`
- 命令行帮助文本（`--help`）同样随 `--lang` 切换

### 外观（浅色 / 深色）
- 工具栏的**外观按钮**（半圆图标）在 浅色 / 深色 / 跟随系统 之间切换，**即时生效**
- “跟随系统”会读取操作系统当前的浅色/深色偏好，并在系统切换时自动跟随
- 窗口内所有表面（工具栏、标签页、面板、代码高亮、Mermaid 图表、滚动条与表单控件）
  都跟随主题
- 选择持久化在 `settings.json`，下次启动沿用；命令行 `--theme light|dark|auto` 可临时覆盖
- `F5` 重载前端后仍然保持当前外观与语言（启动信息是每次都重新读取的，不是启动时缓存的）

MCP 工具的**名称、参数与描述保持英文**：那是 Agent 面向的协议接口，翻译反而会降低
可预测性。界面语言只影响人看到的文字。

### Agent 协同核心能力
1. **编辑器内置 MCP Server**
   - HTTP：`http://127.0.0.1:<port>/mcp`（只监听回环地址）
   - stdio：`colaimdation_mcp_bridge.exe`（供只能启动进程的宿主使用）
   - 不需要为任何 Agent 单独开发适配器
2. **Agent 不直接读写磁盘文件**，通过 MCP 工具操作编辑器内存中的文档：
   - 读取：文档信息、全文/区间、块结构、大纲、光标选区、搜索
   - 写入：增量 Patch（插入 / 替换区间 / 替换块 / 插入块 / 追加章节 / 整篇替换）
   - 保存：`colaimdation_save_document`（唯一触碰磁盘的工具，且只能写当前文档）
3. **双向感知**
   - Agent → 你：改动立刻渲染，并高亮来源块
   - 你 → Agent：`colaimdation_get_user_changes` / `notifications/resources/updated` 让 Agent 感知你的写作进度
4. **安全控制**
   - 本地 Token 鉴权（256-bit，只绑定 127.0.0.1）
   - 可开启 **Diff 预览确认**：Agent 的改动先给你看差异，确认后才落地
   - **审计日志**（内存 + JSONL 落盘）+ **一键回滚**
   - **权限隔离**：只能操作当前打开的文档，无法访问本机其他文件

## 快速开始

### 环境要求
- Windows 10/11
- Visual Studio 2022（含 C++ 工具集）
- CMake 3.24+ 与 Ninja
- Microsoft Edge WebView2 Runtime（Win10/11 通常已预装）

### 构建

```powershell
# 1) 拉取 WebView2 SDK 到 third_party/（首次需要，之后可跳过）
powershell -File tools/fetch_webview2.ps1

# 2) 构建（脚本自己导入 vcvars64 环境，不需要开发者命令行）
powershell -File tools/build.ps1

# 2b) 只想给最终用户一个版本：不含"开发者工具/编辑器自检"，产物在 build-user\bin
powershell -File tools/build.ps1 -User

# 3) 跑测试
powershell -File tools/build.ps1 -Test
```

脚本同时兼容 PowerShell 7（`pwsh`）与 Windows PowerShell 5.1（`powershell`）。

> **项目路径含非 ASCII 字符（例如中文目录）时**，`tools/build.ps1` 会自动把一个盘符
> （`X:`、`Y:`…）映射到项目根目录，并用它去调用 CMake/Ninja。原因很具体：这台机器上的
> Ninja 用 ANSI 路径记录头文件依赖，中文目录会让 `.ninja_deps` 里出现乱码路径，下一次
> 构建会直接报 `FindFirstFileExA(...): The filename ... is incorrect`，而且在此之前头文件
> 依赖就已经悄悄失效（改了 `.h` 却不重编，链接到旧布局的目标文件，表现为崩溃或堆内存损坏）。
> 产物照旧落在 `<项目>\build-core\bin`，因为映射的就是同一个目录。因此**这种路径下请用脚本
> 构建**，别用下面的裸 CMake 命令。

产物在 `build-core/bin/`：

| 文件 | 说明 |
|---|---|
| `colaimdation.exe` | 编辑器（Win32 + WebView2 外壳，含 MCP Server） |
| `colaimdation_mcp_bridge.exe` | MCP stdio 桥（把 stdio 转发到运行中的编辑器） |
| `colaimdation_tests.exe` | 单元与端到端测试（190+ 个） |
| `web/` | 编辑器前端与内置渲染库 |

**用户版**（`tools/build.ps1 -User`，产物在 `build-user/bin/`）与开发版的唯一区别是：
菜单里没有 **视图 → 开发者工具**（F12 也不再打开 DevTools）和 **视图 → 编辑器自检**，
其余功能完全一致。发布给非开发人员时用它。

### 图标

程序图标在 `ico/`：`无标题.png` 是原图，`colaimdation.ico` 是编译进 `colaimdation.exe` /
`colaimdation_mcp_bridge.exe` 的多尺寸图标（16/24/32/48/64/128/256，Explorer、任务栏、
Alt+Tab、标题栏各取所需，不会拿一张图拉伸）。换图标只要替换 `ico/` 里的 PNG 再跑：

```powershell
python tools/make_icon.py        # 也可显式指定：make_icon.py 源图.png 输出.ico
powershell -File tools/build.ps1 # 图标由 src/app/app_icon.rc 编进可执行文件
```

路径全是 ASCII 时也可以走标准 CMake：

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
ctest --test-dir build --output-on-failure
```

### 运行

```powershell
# 打开欢迎文档
build-core\bin\colaimdation.exe --demo

# 打开一个文件
build-core\bin\colaimdation.exe notes.md

# 固定端口 + 要求每次改动都确认
build-core\bin\colaimdation.exe --port 41777 --confirm notes.md

# 一条命令完成：构建 → 启动 → 健康检查 → 调用工具 → 退出
powershell -File tools/run.ps1 -Smoke
```

启动后端点与 Token 会写到 `%LOCALAPPDATA%\colaimdation\mcp.json`，
也可以从菜单 **Agent → Copy endpoint and token** 复制。

### 让端点固定下来（重启不用再发给 AI）

默认情况下**端口会被记住**：第一次启动随便挑一个空闲端口，之后就固定用它，
所以 `http://127.0.0.1:<port>/mcp` 这个地址不会再变。Token 默认每次启动重新生成，
如果你希望 Agent 配置里那份**带 token 的完整端点**也一直有效：

- 打开 Agent 面板 → **重启后仍使用同一个 token** 勾上（写入 `settings.json`），
  这个 token 就一直沿用到你点 **重新生成 token** 为止
- **MCP 端口** 一栏可以写死一个端口（0 = 自动挑一个并记住）；被别的程序占用时会自动
  退回随机端口并在面板上提示，不会把端口号写回去

也可以直接编辑 `%LOCALAPPDATA%\colaimdation\settings.json`：

```jsonc
{
  "mcp_port": 41777,          // 0 = 自动挑一个并记住
  "mcp_token": "……64 位十六进制……"  // 留空 = 每次启动重新生成
}
```

命令行 `--port` / `--token` 仍然只对当次运行生效，不会改写上面的偏好。

## 接入你的 Agent

### MCP HTTP

```jsonc
{
  "mcpServers": {
    "colaimdation": {
      "url": "http://127.0.0.1:41777/mcp",
      "headers": { "Authorization": "Bearer <启动时生成的 token>" }
    }
  }
}
```

Token 缺失或错误一律 `401`；`/health` 无需 Token，便于探活。

### MCP stdio

宿主只能启动子进程时用桥接程序，它会自动读取 `mcp.json` 并把 stdio 转发到运行中的编辑器：

```jsonc
{
  "mcpServers": {
    "colaimdation": {
      "command": "E:\\path\\to\\colaimdation_mcp_bridge.exe",
      "args": []
    }
  }
}
```

也可显式指定：`colaimdation_mcp_bridge.exe --port 41777 --token <token>`。

### 典型一次协同

```
Agent → colaimdation_get_document_info        看看现在是什么文档
Agent → colaimdation_get_structure            拿到块 id（b0、b1、…）
Agent → colaimdation_get_selection            你在哪一段工作
Agent → colaimdation_apply_patch              expected_revision + replace_block(b7, ...)
      ← { status: "applied", revision_after, diff }
用户   ← 编辑器实时刷新，并高亮被改动的块
用户   手动接着改 → Agent 用 get_user_changes 感知
```

工具清单与参数见 **[docs/MCP.md](docs/MCP.md)**；设计取舍见 **[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)**。

## 安全模型

- **绑定**：MCP HTTP 只 `bind(127.0.0.1)`，局域网不可达；同时校验 `Host` 头必须是回环地址。
- **鉴权**：默认要求 `Authorization: Bearer <token>`（也支持 `?token=`、`X-Colaimdation-Token`、Basic）。
  Token 由系统 CSPRNG 生成 32 字节，比较使用常数时间实现。
- **权限隔离**：不提供"打开别的文件"的工具。所有读写的都是当前文档；
  `allowed_directory` 可以进一步限制保存路径。
- **人工兜底**：`require_confirmation=true` 时，Agent 的写入先在 `Document` 的**副本**上试算并生成 diff，
  只有你在 Diff 面板点 "Apply change" 才真正落地；拒绝会作为 `rejected_by_user` 回给 Agent。
- **可追溯**：每次改动都进审计日志（谁、哪个工具、哪个 session、字节增删、可选 unified diff）。
  `colaimdation_rollback` 可回滚最后一次 Agent 改动、回滚到某个 revision，
  或回滚某个序列号之后的所有改动。回滚本身也是一条被审计的编辑。
- **限额**：单次 patch 字节数、单次 patch 数量、读取上限、会话 TTL 都可在
  `colaimdation_set_policy` 或 `--config` JSON 里调整。

## 命令行

| 参数 | 说明 |
|---|---|
| `--file <path>` | 打开指定文档（也可直接写路径） |
| `--port <n>` | 固定 MCP 端口（默认由系统分配，之后沿用上次的端口） |
| `--token <value>` | 指定 MCP Token（默认随机生成；`settings.json` 里的 `mcp_token` 可长期固定） |
| `--config <path>` | 从 JSON 读取安全策略覆盖项 |
| `--read-only` | 禁止 Agent 写入 |
| `--confirm` | 每次 Agent 改动都要 Diff 确认 |
| `--allow-dir <path>` | 限制文件访问目录 |
| `--demo` / `--no-demo` | 是否打开欢迎文档（默认打开） |
| `--theme <auto\|light\|dark>` / `--font-size <pt>` | 外观 |
| `--lang <en\|zh-Hans\|ja>` | 界面语言（可用 `zh-CN`、`ja-JP` 等标签） |
| `--no-webview` | 不初始化前端（诊断用） |
| `--log` | 把审计日志写到程序目录 |
| `--help` | 帮助（跟随 `--lang`） |

`--config` 文件示例：

```json
{
  "require_confirmation": true,
  "protect_front_matter": true,
  "allowed_directory": "C:\\notes",
  "max_patch_bytes": 65536,
  "allowed_line_ranges": [[1, 200]]
}
```

## 快捷键

| 快捷键 | 作用 |
|---|---|
| `Ctrl+N` / `Ctrl+O` / `Ctrl+S` | 新建（新标签页）/ 打开（新标签页）/ 保存 |
| `Ctrl+Shift+S` | 另存为 |
| `Ctrl+W` | 关闭当前标签页 |
| `Ctrl+Tab` / `Ctrl+Shift+Tab` | 下一个 / 上一个文档 |
| `Ctrl+Z` / `Ctrl+Y` | 撤销 / 重做 |
| `Ctrl+A` / `Ctrl+C` / `Ctrl+X` / `Ctrl+V` | 全选 / 复制 / 剪切 / 粘贴（代码框内作用于框内代码） |
| `Ctrl+Shift+Z` | 回滚最后一次 Agent 改动 |
| `Ctrl+/` | 切换源码模式 |
| `Ctrl+Shift+A` / `Ctrl+Shift+L` | Agent 面板 / 审计面板 |
| `F5` / `F12` | 重载前端 / 开发者工具 |
| 工具栏地球图标 | 切换界面语言（英文 / 简体中文 / 日本語） |

## 目录结构

```
CMakeLists.txt              构建定义（core 静态库 + 编辑器 + 桥 + 测试）
ico/                        程序图标：原图 PNG + 多尺寸 colaimdation.ico（tools/make_icon.py 生成）
src/core/                   文档引擎：UTF-8、JSON、Markdown AST、Patch、审计、策略、i18n、设置
src/mcp/                    MCP：JSON-RPC 分发、HTTP 服务、stdio 桥、22 个工具
src/app/                    Win32/WebView2 外壳、编辑器窗口、消息桥、命令行、入口
web/                        编辑器前端（原地渲染、Agent 面板、Diff 面板、审计面板）
web/assets/lang/            前端语言包（en / zh-Hans / ja，随产品一同发布）
third_party/webview2/       WebView2 SDK（由 tools/fetch_webview2.ps1 获取）
third_party/vendor/         Mermaid / KaTeX / highlight.js（离线内置）
tests/                      197 个测试：单元 + 真实 HTTP 端到端 + 多语言完整性 + 标签页
tools/                      构建、运行、依赖获取脚本 + 前端 Node 检查（含 web_format.js）
docs/                       MCP 工具参考、架构说明
```

## 新增一种语言

1. 在 `src/core/i18n.cpp` 的 `kRows` 表里为每种语言各加一列（表结构是
   「一行一字符串，一列一语言」，漏翻译会被测试直接抓出来）。
2. 在 `web/assets/i18n.js` 的 `PACKS` 里加一份前端语言包，并在 `web/assets/lang/`
   放一份同名 JSON（便于翻译与校对）。
3. 在 `i18n::languages()` 与前端 `LANGUAGES` 里登记 tag 与本地名称。
4. `build-core\bin\colaimdation_tests.exe --filter=i18n` 会校验：
   - 每个语言的每个键都已翻译（非空）
   - 菜单快捷键提示（`Ctrl+S` 等）与 `&` 访问键在翻译后仍然存在
   - 占位符 `{0}` 数量与替换正确
   - 前端每个语言包与运行时 bundle 的键集合完整
   - 中日文语言包确实包含 CJK 文本且彼此不同

## 测试

```powershell
build-core\bin\colaimdation_tests.exe                 # 全部
build-core\bin\colaimdation_tests.exe --filter=mcp    # 只跑 MCP 端到端
ctest --test-dir build-core --output-on-failure       # 分组跑
```

覆盖范围：JSON（含代理对、深度限制）、UTF-8（含非法序列、边界对齐）、
Markdown 解析（块结构、表格对齐、列表嵌套、内联标记）、
Document（Patch 校验、重叠拒绝、UTF-8 安全、撤销/重做/回滚）、Diff、
审计日志落盘、安全策略、Workspace（确认流程、权限拦截、文件读写、
**多标签页的打开/切换/关闭与每个标签各自保存**）、
命令行解析、多语言（目录完整性、占位符、访问键、前端语言包、设置持久化），
以及**真实回环 HTTP 上的 MCP 端到端**
（鉴权、工具调用、并发冲突、资源读取、通知、发现文件）。

前端另有三组 Node 检查（无头浏览器找不到时会自动跳过）：
`tools/web_probe.js`、`tools/web_typing.js`、`tools/web_format.js`
（最后一个是排版工具栏：每个按钮写出的 Markdown、光标落点、表格加行加列、
选区包装）。菜单 **视图 → 检查编辑器** 会在真正运行的 WebView2 里跑同一批引擎自检，
把报告直接显示出来。

## 已知边界

- 仅 Windows（Win32 + WebView2）。
- Markdown 解析器是自研的行级 CommonMark 子集，覆盖标题、段落、围栏/缩进代码、引用、
  列表、表格、分隔线、HTML 块、块级公式、front matter；不做完整 CommonMark 合规
  （不处理引用式链接、HTML 内联嵌套等）。
- 编辑模型是"块级内容可编辑"：光标所在块显示源码。同一块内可自由编辑；
  跨块的结构操作会触发重新解析并重排 DOM，光标位置会被保留，
  但极端嵌套场景的体验不如专用编辑器（如 CodeMirror）细腻。
- 未实现三方合并：并发编辑采用"乐观并发 + revision 冲突报错"策略。

## 许可

项目自身尚未声明许可（见 `LICENSE`）。第三方组件许可见各包目录下的 `LICENSE`
（Mermaid MIT、KaTeX MIT、highlight.js BSD-3-Clause、WebView2 SDK 微软许可），
具体版本记录在 `third_party/vendor/VERSIONS.json`。
