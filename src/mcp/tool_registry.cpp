#include "mcp/tool_registry.h"

#include "core/text_edit.h"
#include "core/utf8.h"

#include <algorithm>

namespace colai::mcp {
namespace {

Json schema_object(std::initializer_list<JsonObject::Entry> properties,
                   std::vector<std::string> required = {}) {
    Json schema = Json::object();
    schema.set("type", Json("object"));
    Json props = Json::object();
    for (const auto& entry : properties) props.set(entry.first, entry.second);
    schema.set("properties", std::move(props));
    if (!required.empty()) {
        Json list = Json::array();
        for (const std::string& name : required) list.push(Json(name));
        schema.set("required", std::move(list));
    }
    schema.set("additionalProperties", Json(false));
    return schema;
}

} // namespace

// ---------------------------------------------------------------------------
// Schema helpers used by the tool definition files.
// ---------------------------------------------------------------------------
namespace schema {

Json string(std::string description) {
    Json out = Json::object();
    out.set("type", Json("string"));
    out.set("description", Json(std::move(description)));
    return out;
}

Json string_enum(std::string description, std::initializer_list<const char*> values) {
    Json out = string(std::move(description));
    Json list = Json::array();
    for (const char* value : values) list.push(Json(value));
    out.set("enum", std::move(list));
    return out;
}

Json integer(std::string description, int64_t minimum, int64_t maximum, bool has_maximum) {
    Json out = Json::object();
    out.set("type", Json("integer"));
    out.set("description", Json(std::move(description)));
    out.set("minimum", Json(minimum));
    if (has_maximum) out.set("maximum", Json(maximum));
    return out;
}

Json boolean(std::string description, bool default_value) {
    Json out = Json::object();
    out.set("type", Json("boolean"));
    out.set("description", Json(std::move(description)));
    out.set("default", Json(default_value));
    return out;
}

Json array_of(Json items, std::string description) {
    Json out = Json::object();
    out.set("type", Json("array"));
    out.set("description", Json(std::move(description)));
    out.set("items", std::move(items));
    return out;
}

Json object_schema(std::initializer_list<JsonObject::Entry> properties,
                   std::vector<std::string> required) {
    return schema_object(properties, std::move(required));
}

Json anything(std::string description) {
    Json out = Json::object();
    out.set("description", Json(std::move(description)));
    return out;
}

} // namespace schema

// ---------------------------------------------------------------------------
// ToolError
// ---------------------------------------------------------------------------
void throw_tool_error(std::string code, std::string message, Json data) {
    throw ToolError{std::move(code), std::move(message), std::move(data)};
}

// ---------------------------------------------------------------------------
// ToolDescriptor / ToolRegistry
// ---------------------------------------------------------------------------
Json ToolDescriptor::to_mcp_json() const {
    Json out = Json::object();
    out.set("name", Json(name));
    if (!title.empty()) out.set("title", Json(title));
    out.set("description", Json(description));
    out.set("inputSchema", input_schema);
    if (!annotations.members().empty()) out.set("annotations", annotations);
    return out;
}

void ToolRegistry::add(ToolDescriptor descriptor) {
    if (find(descriptor.name)) {
        throw std::runtime_error("duplicate MCP tool name: " + descriptor.name);
    }
    tools_.push_back(std::move(descriptor));
}

const ToolDescriptor* ToolRegistry::find(std::string_view name) const {
    for (const ToolDescriptor& tool : tools_) {
        if (tool.name == name) return &tool;
    }
    return nullptr;
}

Json ToolRegistry::call(const std::string& name, const Json& arguments,
                        ToolContext& context) const {
    const ToolDescriptor* tool = find(name);
    if (!tool) {
        return tool_error_result("unknown_tool", "no tool named '" + name + "'",
                                 Json([&] {
                                     Json data = Json::object();
                                     Json list = Json::array();
                                     for (const ToolDescriptor& entry : tools_) {
                                         list.push(Json(entry.name));
                                     }
                                     data.set("available_tools", std::move(list));
                                     return data;
                                 }()));
    }
    if (!arguments.is_object() && !arguments.is_null()) {
        return tool_error_result("invalid_argument", "tool arguments must be a JSON object");
    }
    try {
        return tool->handler(arguments.is_null() ? Json::object() : arguments, context);
    } catch (const ToolError& error) {
        return tool_error_result(error.code, error.message, error.data);
    } catch (const std::exception& error) {
        return tool_error_result("internal_error", error.what());
    }
}

// ---------------------------------------------------------------------------
// Argument helpers
// ---------------------------------------------------------------------------
const Json* argument(const Json& arguments, std::string_view name) {
    if (!arguments.is_object()) return nullptr;
    return arguments.find(name);
}

bool has_argument(const Json& arguments, std::string_view name) {
    const Json* value = argument(arguments, name);
    return value != nullptr && !value->is_null();
}

std::string string_argument(const Json& arguments, std::string_view name, std::string fallback) {
    const Json* value = argument(arguments, name);
    if (!value || value->is_null()) return fallback;
    if (value->is_string()) return value->as_string();
    if (value->is_number() || value->is_bool()) return value->as_text();
    throw_tool_error("invalid_argument",
                     "argument '" + std::string(name) + "' must be a string");
}

bool bool_argument(const Json& arguments, std::string_view name, bool fallback) {
    const Json* value = argument(arguments, name);
    if (!value || value->is_null()) return fallback;
    if (!value->is_bool()) {
        throw_tool_error("invalid_argument",
                         "argument '" + std::string(name) + "' must be a boolean");
    }
    return value->as_bool();
}

int64_t int_argument(const Json& arguments, std::string_view name, int64_t fallback) {
    const Json* value = argument(arguments, name);
    if (!value || value->is_null()) return fallback;
    if (!value->is_number() && !value->is_string()) {
        throw_tool_error("invalid_argument",
                         "argument '" + std::string(name) + "' must be an integer");
    }
    return value->as_int(fallback);
}

size_t offset_argument(const Json& arguments, std::string_view name, size_t fallback) {
    const Json* value = argument(arguments, name);
    if (!value || value->is_null()) return fallback;
    if (value->is_number()) return static_cast<size_t>(std::max<int64_t>(0, value->as_int()));
    throw_tool_error("invalid_argument", "argument '" + std::string(name) +
                                             "' must be a UTF-8 byte offset (integer)");
}

// ---------------------------------------------------------------------------
// Offset resolution against the live document text
// ---------------------------------------------------------------------------
namespace {

// Reads an offset-like value: an integer byte offset, or {"line", "column"}.
size_t resolve_one(const Json& value, std::string_view name, std::string_view text,
                   bool line_only_ok) {
    if (value.is_number()) {
        return utf8::clamp_to_boundary(text, static_cast<size_t>(std::max<int64_t>(0, value.as_int())));
    }
    if (!value.is_object()) {
        throw_tool_error("invalid_argument",
                         "'" + std::string(name) +
                             "' must be a byte offset (integer) or a {\"line\", \"column\"} object");
    }
    const int64_t line = std::max<int64_t>(1, value.integer("line", 1));
    if (!value.contains("column")) {
        if (!line_only_ok) {
            throw_tool_error("invalid_argument", "'" + std::string(name) +
                                                     "' object needs both 'line' and 'column'");
        }
        return offset_at_line_column(text, static_cast<size_t>(line), 1);
    }
    const int64_t column = std::max<int64_t>(1, value.integer("column", 1));
    return offset_at_line_column(text, static_cast<size_t>(line), static_cast<size_t>(column));
}

} // namespace

std::optional<size_t> resolve_offset_argument(const Json& arguments, std::string_view name,
                                              std::string_view text) {
    const Json* value = argument(arguments, name);
    if (!value || value->is_null()) return std::nullopt;
    return resolve_one(*value, name, text, false);
}

std::optional<size_t> resolve_line_argument(const Json& arguments, std::string_view name,
                                            std::string_view text) {
    const Json* value = argument(arguments, name);
    if (!value || value->is_null()) return std::nullopt;
    return resolve_one(*value, name, text, true);
}

void resolve_range_argument(const Json& arguments, std::string_view text, size_t& begin,
                            size_t& end) {
    begin = 0;
    end = text.size();
    if (const Json* start = argument(arguments, "start")) {
        if (!start->is_null()) begin = resolve_one(*start, "start", text, true);
    }
    if (const Json* finish = argument(arguments, "end")) {
        if (!finish->is_null()) end = resolve_one(*finish, "end", text, true);
    }
    // An `end` line without a column means "through the end of that line".
    if (const Json* finish = argument(arguments, "end")) {
        if (finish->is_object() && !finish->contains("column") && finish->contains("line")) {
            end = utf8::line_end(text, end);
        }
    }
    if (end < begin) std::swap(begin, end);
}

// ---------------------------------------------------------------------------
// Results
// ---------------------------------------------------------------------------
Json text_result(std::string text) {
    Json content = Json::array();
    Json block = Json::object();
    block.set("type", Json("text"));
    block.set("text", Json(std::move(text)));
    content.push(std::move(block));
    Json out = Json::object();
    out.set("content", std::move(content));
    return out;
}

Json text_result_with_structured(std::string text, Json structured) {
    Json out = text_result(std::move(text));
    out.set("structuredContent", std::move(structured));
    return out;
}

Json tool_error_result(std::string code, std::string message, Json data) {
    Json payload = Json::object();
    payload.set("code", Json(std::move(code)));
    payload.set("message", Json(std::move(message)));
    if (!data.is_null()) payload.set("data", std::move(data));

    Json block = Json::object();
    block.set("type", Json("text"));
    block.set("text", Json(payload.dump(true)));
    Json content = Json::array();
    content.push(std::move(block));

    Json out = Json::object();
    out.set("content", std::move(content));
    out.set("isError", Json(true));
    out.set("structuredContent", std::move(payload));
    return out;
}

// ---------------------------------------------------------------------------
// Patch parsing
// ---------------------------------------------------------------------------
namespace {

Patch::Position parse_position(const std::string& text) {
    const std::string lower = utf8::to_lower(text);
    if (lower == "before") return Patch::Position::Before;
    if (lower == "after") return Patch::Position::After;
    if (lower == "start" || lower == "start_of_document") return Patch::Position::StartOfDocument;
    if (lower == "end" || lower == "end_of_document") return Patch::Position::EndOfDocument;
    throw_tool_error("invalid_argument",
                     "position must be one of before|after|start_of_document|end_of_document");
}

Patch parse_one_patch(const Json& entry, size_t index) {
    if (!entry.is_object()) {
        throw_tool_error("invalid_argument",
                         "patches[" + std::to_string(index) + "] must be an object");
    }
    const std::string op_text = entry.str("op");
    if (op_text.empty()) {
        throw_tool_error("invalid_argument",
                         "patches[" + std::to_string(index) + "] is missing 'op'");
    }
    const auto op = patch_op_from_name(op_text);
    if (!op) {
        throw_tool_error("invalid_argument", "unknown patch op '" + op_text + "' (patches[" +
                                                 std::to_string(index) + "])");
    }

    Patch patch;
    patch.op = *op;
    patch.note = entry.str("note");

    switch (*op) {
    case PatchOp::InsertText: {
        if (!entry.contains("offset")) {
            throw_tool_error("invalid_argument",
                             "insert_text requires 'offset' (patches[" + std::to_string(index) + "])");
        }
        patch.offset = static_cast<size_t>(std::max<int64_t>(0, entry.integer("offset")));
        patch.text = entry.str("text");
        break;
    }
    case PatchOp::ReplaceRange:
    case PatchOp::DeleteRange: {
        if (!entry.contains("start") || !entry.contains("end")) {
            throw_tool_error("invalid_argument", std::string(patch_op_name(*op)) +
                                                     " requires 'start' and 'end' (patches[" +
                                                     std::to_string(index) + "])");
        }
        patch.range_start = static_cast<size_t>(std::max<int64_t>(0, entry.integer("start")));
        patch.range_end = static_cast<size_t>(std::max<int64_t>(0, entry.integer("end")));
        patch.text = entry.str("text");
        break;
    }
    case PatchOp::ReplaceBlock:
    case PatchOp::ReplaceBlockRaw: {
        patch.block_id = entry.str("block_id");
        if (entry.contains("block_index")) {
            patch.block_index = static_cast<size_t>(std::max<int64_t>(0, entry.integer("block_index")));
        }
        patch.relative_to_cursor = entry.boolean("at_cursor", false);
        if (patch.block_id.empty() && !patch.block_index.has_value() && !patch.relative_to_cursor) {
            throw_tool_error("invalid_argument",
                             std::string(patch_op_name(*op)) +
                                 " requires 'block_id', 'block_index' or 'at_cursor' (patches[" +
                                 std::to_string(index) + "])");
        }
        if (!entry.contains("text")) {
            throw_tool_error("invalid_argument", std::string(patch_op_name(*op)) +
                                                     " requires 'text' (patches[" +
                                                     std::to_string(index) + "])");
        }
        patch.text = entry.str("text");
        break;
    }
    case PatchOp::InsertBlock: {
        patch.block_id = entry.str("block_id");
        if (entry.contains("block_index")) {
            patch.block_index = static_cast<size_t>(std::max<int64_t>(0, entry.integer("block_index")));
        }
        patch.relative_to_cursor = entry.boolean("at_cursor", false);
        patch.position = parse_position(entry.str("position", "after"));
        patch.text = entry.str("text");
        break;
    }
    case PatchOp::AppendSection: {
        if (!entry.contains("text")) {
            throw_tool_error("invalid_argument",
                             "append_section requires 'text' (patches[" + std::to_string(index) + "])");
        }
        patch.text = entry.str("text");
        break;
    }
    case PatchOp::SetText: {
        if (!entry.contains("text")) {
            throw_tool_error("invalid_argument",
                             "set_text requires 'text' (patches[" + std::to_string(index) + "])");
        }
        patch.text = entry.str("text");
        break;
    }
    }
    return patch;
}

} // namespace

std::vector<Patch> parse_patches(const Json& arguments) {
    const Json* raw = argument(arguments, "patches");
    if (!raw || raw->is_null()) {
        throw_tool_error("invalid_argument", "apply_patch requires a non-empty 'patches' array");
    }
    if (!raw->is_array()) {
        throw_tool_error("invalid_argument", "'patches' must be an array of patch objects");
    }
    if (raw->items().empty()) {
        throw_tool_error("invalid_argument", "'patches' must contain at least one patch");
    }
    std::vector<Patch> patches;
    patches.reserve(raw->items().size());
    size_t index = 0;
    for (const Json& entry : raw->items()) {
        patches.push_back(parse_one_patch(entry, index));
        ++index;
    }
    return patches;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------
void register_builtin_tools(ToolRegistry& registry) {
    register_document_read_tools(registry);
    register_document_write_tools(registry);
    register_agent_tools(registry);
}

} // namespace colai::mcp
