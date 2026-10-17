# Colaimdation MCP 工具参考

- 传输：JSON-RPC 2.0 over HTTP（`POST http://127.0.0.1:<port>/mcp`），或经由
  `colaimdation_mcp_bridge.exe` 的 stdio。
- 协议版本：`2025-06-18`（`initialize` 会回显客户端请求的版本）。
- 鉴权：`Authorization: Bearer <token>`（也接受 `?token=`、`X-Colaimdation-Token`、Basic）。
- 工具名称、参数与描述是**协议接口，固定英文**；界面语言（`/health` 与
  `colaimdation_get_workspace_info` 里的 `language` 字段）只影响人看到的文字。
- 所有偏移量都是 **UTF-8 字节偏移**（不是字符数）。需要行列时传 `{"line": N, "column": M}`（1 起）。
- 块 id 形如 `"b7"`，来自 `colaimdation_get_structure`。执行写入前先取结构，不要凭记忆猜 id。
- **"当前文档"= 窗口里处于活动状态的那个标签页**。编辑器可以同时打开多篇文档，
  但工具集里没有任何"打开/切换/关闭文档"的工具：Agent 只能读写人正在看的那一篇，
  `colaimdation_get_workspace_info` 会返回 `tabs` / `active_tab` / `tab_count`，
  告诉你现在一共有几篇、当前是第几篇（1 起）。
- **端点默认是稳定的**：端口在第一次启动后会被记住（`settings.json` 的 `mcp_port`），
  只有 token 默认每次重新生成。希望连 token 也固定的，在 Agent 面板勾上
  "重启后仍使用同一个 token"，或在 `settings.json` 里写 `mcp_token`。

## 资源（resources）

| URI | 内容 |
|---|---|
| `colaimdation://document` | 当前文档全文（`text/markdown`） |
| `colaimdation://document/structure` | 块结构 JSON |

`resources/list`、`resources/read` 均已实现。

## 通知

服务端会向已连接的 SSE 客户端（`GET /notifications`）推送：

| 事件 | 负载 |
|---|---|
| `hello` | `{ server, protocol_version, revision }` |
| `ping` | 每 15 秒一次，含 `revision` 与 `character_count` |
| `message` | JSON-RPC 通知，主要是 `notifications/resources/updated` |

`notifications/resources/updated` 的参数：

```json
{
  "uri": "colaimdation://document",
  "tool": "colaimdation_apply_patch",
  "session": "sess-1a2b…",
  "is_error": false,
  "revision": "9f2c…",
  "character_count": 3245,
  "block_count": 29
}
```

---

## 只读工具

### `colaimdation_read_document`
读取当前文档。**这是唯一能看到内容的入口**（Agent 永远不读磁盘文件）。

| 参数 | 类型 | 说明 |
|---|---|---|
| `mode` | `content` \| `structure` \| `outline` \| `full` \| `all` | 返回内容（默认 `content`） |
| `start` / `end` | 整数或 `{line, column}` | 只读某区间；`end` 只给 `line` 表示读到该行末 |
| `content` / `structure` / `outline` | bool | 显式开关，与 `mode` 取并集 |

返回 `structuredContent`：`document`（元数据）、`selection`、`range`、可选的 `content` /
`structure` / `outline`。文本带行号前缀，便于引用。

### `colaimdation_get_document_info`
轻量元数据：`path`、`name`、`revision`、`sequence`、`dirty`、`bytes`、`characters`、
`lines`、`words`、`blocks`、`can_undo`、`can_redo`、`selection`、`policy`。

### `colaimdation_get_structure`
块列表。参数 `max_blocks`（0 = 全部）、`kind`（按类型过滤）。

每块包含：`block_id`、`index`、`kind`、`ordinal`、`byte_start`、`byte_end`、
`content_byte_start`、`content_byte_end`、`start_line`、`end_line`、`character_count`；
按类型附带 `text`（标题）、`code`/`info`（代码块）、`items`（列表）、`table`
（行列数与对齐）。另返回 `outline`、`fingerprint`、`truncated`。

### `colaimdation_get_outline`
标题大纲：`level`、`title`、`block_id`、`byte_offset`。

### `colaimdation_get_selection`
光标与选区：`anchor`/`head`/`start`/`end`/`length`、行列、`text`、所在 `block`，
以及可选的 `surrounding_text`（`context_characters` 控制前后各取多少字符）。
用它来知道**用户此刻在哪一段写作**。

### `colaimdation_get_block`
单块详情。定位方式三选一：`block_id` / `block_index` / `at_cursor`。
另可传 `inline`（默认 true）返回内联标记跨度（`code`、`link`、`image`、
`math_inline`、`math_block`、`strong`、`emphasis`、`strikethrough`）。

### `colaimdation_search`
子串搜索（非正则）。参数：`query`（必填）、`case_sensitive`、`max_results`、
`include_lines`。每个命中返回 `byte_start`/`byte_end`、`line`、`column`、
`block_id`、`block_kind`，可直接喂给 patch。

### `colaimdation_read_range`
按字节或行读取区间，带行号。大文件时比整篇读取便宜。

### `colaimdation_get_user_changes`
看**人类**改了什么。

| 参数 | 说明 |
|---|---|
| `since_revision` | 你上次读到的 revision；省略则返回全部人工编辑 |
| `session` | 轮询游标所属会话（默认当前会话） |
| `max_changes` | 上限（默认 20） |
| `poll` | `true` 时使用并推进该会话的游标（适合定时轮询） |

### `colaimdation_get_history`
编辑历史（含人工与 Agent）：`sequence`、`source`、`tool`、`session`、`summary`、
`revision_before/after`、`changed_bytes`、时间戳；`include_text=true` 时含前后全文。

### `colaimdation_get_audit_log`
持久审计日志。过滤：`limit`、`tool`、`session`、`source`（`user|agent|system|external`）、
`document_id`、`since_ms`、`include_diff`。

### `colaimdation_get_policy` / `colaimdation_get_workspace_info`
前者返回安全策略；后者是一次调用看全貌：当前文档、是否未保存、策略、
**待确认的 Agent 改动数量**、以及你的 session id。

---

## 写入工具

> 所有写入都受策略约束：`allow_write`、`max_patch_bytes`、`max_patches_per_call`、
> `protect_front_matter`、`allowed_line_ranges`，以及可选的 Diff 确认。
> 失败时返回 `isError: true`，`structuredContent.code` 为
> `permission_denied` / `out_of_range` / `invalid_argument` / `conflict` /
> `rejected_by_user` / `io_error` / `editor_unavailable` 之一。

### `colaimdation_apply_patch` ← 主要写入方式
在一个原子批次里应用若干增量 patch。

| 参数 | 说明 |
|---|---|
| `patches` | 数组，见下表；必填、非空 |
| `expected_revision` | 乐观并发：与当前 revision 不符则拒绝（`conflict`） |
| `force` | 忽略 `expected_revision` 不匹配 |
| `preview` | 只返回 diff 与 `resulting_content`，不改文档 |
| `summary` | 写入审计日志的一句话描述 |
| `bypass_confirmation` | 请求跳过确认（仅当策略 `allow_confirmation_bypass` 为真才生效） |

Patch 操作：

| `op` | 字段 | 语义 |
|---|---|---|
| `insert_text` | `offset`, `text` | 在偏移处插入 |
| `replace_range` | `start`, `end`, `text` | 替换区间（`text` 可为空 = 删除） |
| `delete_range` | `start`, `end` | 删除区间 |
| `replace_block` | `block_id`\|`block_index`\|`at_cursor`, `text` | 替换块**内容**，保留 `#`、代码围栏、列表符号 |
| `replace_block_raw` | `block_id`, `text` | 连标记一起替换 |
| `insert_block` | `block_id`, `text`, `position` | `position`: `before`\|`after`\|`start_of_document`\|`end_of_document` |
| `append_section` | `text` | 在文末追加（自动补一个空行分隔） |
| `set_text` | `text` | 整篇替换（必须独占一批） |

行为保证：
- **原子性**：任一 patch 非法或相互重叠 → 整批拒绝，文档不变。
- **UTF-8 安全**：偏移会吸附到字符边界；会产生非法 UTF-8 的写入被拒绝。
- **返回**：`status`、`revision_before`、`revision_after`、`applied_patches`、
  字节/字符/行/词计数、`dirty`、`caret`、`audit_id`，以及 **unified `diff`**。

### `colaimdation_insert_text`
`{ offset | {line,column}, text }` —— 单个 `insert_text` patch 的便捷封装。

### `colaimdation_replace_range`
`{ start, end, text }` —— 单个 `replace_range` 的便捷封装。

### `colaimdation_delete_range`
`{ start, end }` —— 单个 `delete_range` 的便捷封装。

### `colaimdation_replace_block`
`{ block_id | block_index | at_cursor, text }` —— 安全地重写一个块：
标题保留 `#`、代码块保留围栏与语言、列表保留符号。返回 `previous_content` 方便回溯。

### `colaimdation_save_document`
把当前文档写回磁盘（`path` 存在时即"另存为"，受 `allowed_directory` 限制）。
使用文件原本的行尾风格。这是**唯一触碰文件系统的工具**，且只能写编辑器当前打开的文档
（多标签页时就是活动标签页；未命名的新文档没有路径，会返回 `invalid_argument`，
需要人先在"另存为"里给它一个名字）。

### `colaimdation_set_selection`
移动光标/选区，用来提示用户"看这里"或聚焦下一步工作区域。
接受 `caret` + `anchor`，或 `start` + `end`。

### `colaimdation_rollback`
撤销编辑。

| 参数 | 说明 |
|---|---|
| `last` | 默认 `true`：撤销最近一次编辑 |
| `source` | `last=true` 时撤销谁的：`agent`（默认）/`user`/`system`/`any` |
| `to_revision` | 撤销该 revision 之后的所有编辑 |
| `to_sequence` | 撤销该序列号之后的所有编辑 |

回滚本身也会写一条审计记录，并且可以再被回滚。

### `colaimdation_set_policy`
修改安全策略，**必须传 `"confirm": "yes"`**。可改：`allow_write`、
`require_confirmation`、`protect_front_matter`、`allow_save`、`allow_rollback`、
`allow_selection_control`、`allow_multi_patch`、`max_patch_bytes`、
`max_patches_per_call`、`allowed_directory`、`session_ttl_ms`。
只改传入的字段；无法识别的键会在 `warnings` 里回报。

---

## 推荐的协同流程

```
1. colaimdation_get_workspace_info        确认连到了哪篇文档、有没有待确认改动
2. colaimdation_read_document             读全文（或 mode=structure）
3. colaimdation_get_selection             用户在写哪一段
4. colaimdation_get_structure            拿块 id
5. colaimdation_apply_patch              小步增量，带 expected_revision
   ↑ 若返回 conflict：重新 read_document，把改动重新应用到最新 revision
6. colaimdation_get_user_changes          感知人类后续的修改（可 poll=true 定时轮询）
7. colaimdation_save_document             用户要求时才保存
```

### 反模式

- ❌ 用 `set_text` 整篇重写 —— 会丢掉用户的并发编辑、审计意义也大幅降低。
  只在确实要替换整篇内容时使用。
- ❌ 不带 `expected_revision` 的连续写入 —— 会静默覆盖用户刚敲的字。
- ❌ 直接按字符数算偏移 —— 偏移是**字节**。中文一个字符占 3 字节。
  要么用 `colaimdation_search` / `get_block` 返回的偏移，要么传 `{line, column}`。
- ❌ 凭记忆猜 `block_id` —— 每次编辑后块 id 都可能变，先取结构。

## 错误码

| `code` | 含义 |
|---|---|
| `permission_denied` | 策略禁止（写入/保存/回滚/选区/受保护区域） |
| `out_of_range` | 偏移或区间超出文档 |
| `invalid_argument` | patch 字段缺失、重叠、批量超限、JSON 形状不对 |
| `conflict` | `expected_revision` 不匹配，或没有可回滚的编辑 |
| `rejected_by_user` | 用户在 Diff 面板点了拒绝 |
| `io_error` | 保存失败 |
| `editor_unavailable` | 编辑器窗口未在超时内响应（改动未落地） |
| `too_large` | 文档超过 `max_read_bytes` |
| `unknown_tool` | 工具名不存在（错误里会列出可用工具） |
