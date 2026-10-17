// Colaimdation - MCP tool registry.
//
// Tools are described with a JSON Schema subset so any MCP-capable Agent can
// discover them, and implemented as small handlers over Workspace. Nothing in
// here touches the filesystem directly: every mutation goes through the
// document engine, which is what makes the editor's audit and rollback work.
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.h"
#include "core/workspace.h"

namespace colai::mcp {

// ---------------------------------------------------------------------------
// Protocol version advertised by this build.
// ---------------------------------------------------------------------------
inline constexpr const char* kProtocolVersion = "2025-06-18";
inline constexpr const char* kServerName = "Colaimdation";
inline constexpr const char* kServerTitle = "Colaimdation - live Markdown editor";

// Sessions are lightweight: they identify an Agent so the audit log and the
// change feed can attribute edits.
struct Session {
    std::string id;
    std::string client_name;
    std::string client_version;
    int64_t connected_ms = 0;
    int64_t last_seen_ms = 0;
    int64_t calls = 0;
};

struct ToolContext {
    Workspace* workspace = nullptr;
    Session* session = nullptr;
    // Fan-out to other MCP clients (SSE sinks) and, through it, the editor UI.
    std::function<void(const Json&)> emit;
    // Structured logging into the editor's activity panel.
    std::function<void(const std::string& level, const std::string& message)> log;
};

using ToolHandler = std::function<Json(const Json& arguments, ToolContext& context)>;

// Thrown by handlers (and the patch parser) to produce an MCP tool error.
struct ToolError {
    std::string code;
    std::string message;
    Json data;
};

[[noreturn]] void throw_tool_error(std::string code, std::string message, Json data = Json());

struct ToolDescriptor {
    std::string name;
    std::string title;
    std::string description;
    Json input_schema = Json::object();
    Json annotations = Json::object(); // hints: readOnlyHint, destructiveHint, ...
    ToolHandler handler;

    Json to_mcp_json() const;
};

class ToolRegistry {
public:
    void add(ToolDescriptor descriptor);
    const ToolDescriptor* find(std::string_view name) const;
    const std::vector<ToolDescriptor>& tools() const { return tools_; }
    size_t size() const { return tools_.size(); }

    // Returns the result of the call in MCP `CallToolResult` shape. Handler
    // errors become `isError` results rather than transport failures.
    Json call(const std::string& name, const Json& arguments, ToolContext& context) const;

private:
    std::vector<ToolDescriptor> tools_;
};

// ---------------------------------------------------------------------------
// Argument helpers shared by the tool implementations.
// ---------------------------------------------------------------------------
const Json* argument(const Json& arguments, std::string_view name);
std::string string_argument(const Json& arguments, std::string_view name,
                            std::string fallback = {});
bool bool_argument(const Json& arguments, std::string_view name, bool fallback = false);
int64_t int_argument(const Json& arguments, std::string_view name, int64_t fallback = 0);
// Accepts either an absolute byte offset or a 1-based line/column pair.
size_t offset_argument(const Json& arguments, std::string_view name, size_t fallback);
bool has_argument(const Json& arguments, std::string_view name);

// Resolves an offset argument against the live text. Accepts either a UTF-8 byte
// offset (integer) or a 1-based {"line": n, "column": m} object. Returns
// nullopt when the argument is absent.
std::optional<size_t> resolve_offset_argument(const Json& arguments, std::string_view name,
                                              std::string_view text);
// Same, but also accepts {"line": n} meaning "start of line n".
std::optional<size_t> resolve_line_argument(const Json& arguments, std::string_view name,
                                            std::string_view text);
// Parses {"start": ..., "end": ...} where each side may be an offset or a
// {line, column} object. Defaults to the whole document.
void resolve_range_argument(const Json& arguments, std::string_view text, size_t& begin,
                            size_t& end);

// Builders for the two content shapes MCP allows.
Json text_result(std::string text);
Json text_result_with_structured(std::string text, Json structured);
Json tool_error_result(std::string code, std::string message, Json data = Json());

// ---------------------------------------------------------------------------
// Tool groups
// ---------------------------------------------------------------------------
void register_document_read_tools(ToolRegistry& registry);
void register_document_write_tools(ToolRegistry& registry);
void register_agent_tools(ToolRegistry& registry);

// Registers every built-in tool.
void register_builtin_tools(ToolRegistry& registry);

// Parses the `patches` array accepted by apply_patch. Throws ToolError on bad
// input so the Agent gets an actionable message.
std::vector<Patch> parse_patches(const Json& arguments);

} // namespace colai::mcp
