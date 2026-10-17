// Colaimdation - document tools.
//
// Every tool here reads or writes the document that the editor has open, via
// Workspace. No tool takes a path: that is the permission-isolation guarantee.
#include "mcp/tool_registry.h"

#include "core/diff.h"
#include "core/markdown.h"
#include "core/text_edit.h"
#include "core/utf8.h"

#include <algorithm>
#include <sstream>

namespace colai::mcp {
namespace schema {
Json string(std::string description);
Json string_enum(std::string description, std::initializer_list<const char*> values);
Json integer(std::string description, int64_t minimum, int64_t maximum, bool has_maximum);
Json boolean(std::string description, bool default_value);
Json array_of(Json items, std::string description);
Json object_schema(std::initializer_list<JsonObject::Entry> properties,
                   std::vector<std::string> required);
Json anything(std::string description);
} // namespace schema

namespace {

using schema::anything;
using schema::array_of;
using schema::boolean;
using schema::integer;
using schema::object_schema;
using schema::string;
using schema::string_enum;

Json annotations(bool read_only, bool destructive = false, bool idempotent = false) {
    Json out = Json::object();
    out.set("readOnlyHint", Json(read_only));
    out.set("destructiveHint", Json(destructive));
    out.set("idempotentHint", Json(idempotent));
    out.set("openWorldHint", Json(false));
    return out;
}

Workspace& require_workspace(ToolContext& context) {
    if (!context.workspace) throw_tool_error("internal_error", "no workspace is attached to this server");
    return *context.workspace;
}

Document& require_document(ToolContext& context) { return require_workspace(context).document(); }

std::string session_id(ToolContext& context) {
    return context.session ? context.session->id : std::string("anonymous");
}

void note_activity(ToolContext& context, const std::string& message) {
    if (context.log) context.log("info", message);
}

// Human-readable "line N, block bK (heading)" locator for a byte offset.
std::string locator(const Document& document, size_t offset) {
    const size_t line = utf8::line_number_at(document.text(), offset);
    const size_t column = utf8::column_at(document.text(), offset);
    std::string out = "line " + std::to_string(line) + ", column " + std::to_string(column);
    const int index = document.block_containing(offset);
    if (index >= 0) {
        const md::Block& block = document.parsed().blocks[static_cast<size_t>(index)];
        out += ", block b" + std::to_string(index) + " (" + std::string(md::kind_name(block.kind)) + ")";
    }
    return out;
}

// Prefixes every line with its 1-based number so Agents can reason about
// positions without a second call.
std::string numbered(std::string_view text, size_t first_line) {
    std::ostringstream out;
    size_t line = first_line;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        const bool last = end == std::string_view::npos;
        if (last) end = text.size();
        out << line << "\t" << text.substr(start, end - start);
        if (!last) out << "\n";
        if (last) break;
        start = end + 1;
        ++line;
    }
    return out.str();
}

Json patch_outcome_json(const Workspace::WriteOutcome& outcome, const Document& document,
                        const std::string& revision_before) {
    Json out = Json::object();
    out.set("status", Json(outcome.ok ? "applied" : "rejected"));
    out.set("revision_before", Json(revision_before));
    out.set("revision_after", Json(outcome.revision_after));
    out.set("applied_patches", Json(static_cast<int64_t>(outcome.applied)));
    out.set("document_revision", Json(document.revision()));
    out.set("bytes", Json(static_cast<int64_t>(document.size())));
    out.set("characters", Json(static_cast<int64_t>(document.character_count())));
    out.set("lines", Json(static_cast<int64_t>(document.line_count())));
    out.set("words", Json(static_cast<int64_t>(document.word_count())));
    out.set("dirty", Json(document.dirty()));
    out.set("caret", Json(static_cast<int64_t>(document.caret())));
    out.set("staged_for_confirmation", Json(outcome.staged));
    if (outcome.staged && !outcome.pending_id.empty()) {
        out.set("pending_id", Json(outcome.pending_id));
    }
    if (outcome.audit_id > 0) out.set("audit_id", Json(outcome.audit_id));
    if (!outcome.diff.empty()) out.set("diff", Json(outcome.diff));
    if (!outcome.ok) {
        Json error = Json::object();
        error.set("code", Json(outcome.error_code));
        error.set("message", Json(outcome.error_message));
        out.set("error", std::move(error));
    }
    return out;
}

std::string write_summary_text(const Workspace::WriteOutcome& outcome, const Document& document,
                               const std::string& revision_before) {
    std::ostringstream text;
    if (outcome.ok) {
        text << "Applied " << outcome.applied << " patch(es).\n";
        if (outcome.staged) {
            text << "The change is staged and waiting for the writer to confirm it in the diff "
                    "preview panel.\n";
        }
        text << "Document is now " << document.character_count() << " characters / "
             << document.line_count() << " lines (" << document.block_count() << " blocks).\n";
        text << "revision: " << revision_before.substr(0, 12) << " -> "
             << outcome.revision_after.substr(0, 12) << "\n";
        if (!outcome.diff.empty()) text << "\n" << outcome.diff;
    } else {
        text << "The change was not applied: " << outcome.error_code << " - " << outcome.error_message
             << "\n";
    }
    return text.str();
}

// ---------------------------------------------------------------------------
// read_document
// ---------------------------------------------------------------------------
Json tool_read_document(const Json& arguments, ToolContext& context) {
    Document& document = require_document(context);
    Workspace& workspace = require_workspace(context);
    const std::string mode = string_argument(arguments, "mode", "content");
    const bool include_content =
        mode == "content" || mode == "full" || mode == "all" || bool_argument(arguments, "content", false);
    const bool include_structure = mode == "structure" || mode == "full" || mode == "all" ||
                                   bool_argument(arguments, "structure", false);
    const bool include_outline =
        mode == "full" || mode == "all" || bool_argument(arguments, "outline", false);

    const size_t total = document.size();
    if (total > workspace.policy().max_read_bytes) {
        throw_tool_error("too_large",
                         "the document is larger than the configured max_read_bytes; read it in "
                         "ranges with start/end",
                         Json([&] {
                             Json data = Json::object();
                             data.set("bytes", Json(static_cast<int64_t>(total)));
                             data.set("max_read_bytes",
                                      Json(static_cast<int64_t>(workspace.policy().max_read_bytes)));
                             return data;
                         }()));
    }

    size_t begin = 0;
    size_t end = total;
    resolve_range_argument(arguments, document.text(), begin, end);
    const bool ranged = begin != 0 || end != total;

    Json out = Json::object();
    out.set("document", document.to_json(false));
    out.set("selection", document.selection_json());
    out.set("range", Json([&] {
        Json range = Json::object();
        range.set("byte_start", Json(static_cast<int64_t>(begin)));
        range.set("byte_end", Json(static_cast<int64_t>(end)));
        range.set("start_line", Json(static_cast<int64_t>(utf8::line_number_at(document.text(), begin))));
        range.set("end_line", Json(static_cast<int64_t>(utf8::line_number_at(document.text(), end))));
        return range;
    }()));

    std::ostringstream text;
    text << "Document: " << document.display_name() << "\n";
    text << "path: " << (document.path().empty() ? "(unsaved)" : document.path()) << "\n";
    text << "revision: " << document.revision() << "\n";
    text << "size: " << document.character_count() << " characters, " << document.line_count()
         << " lines, " << document.block_count() << " blocks, " << document.word_count() << " words"
         << (document.dirty() ? "  [unsaved changes]" : "") << "\n";
    text << "policy: writes " << (workspace.policy().allow_write ? "allowed" : "blocked")
         << ", confirmation "
         << (workspace.policy().require_confirmation ? "required" : "not required") << "\n";

    if (include_content) {
        const std::string slice(document.text().substr(begin, end - begin));
        text << "\n--- content (" << (ranged ? "range" : "full") << ") ---\n";
        text << numbered(slice, utf8::line_number_at(document.text(), begin));
        out.set("content", Json(slice));
    }
    if (include_structure) {
        out.set("structure", document.structure_json(0));
    }
    if (include_outline) {
        out.set("outline", document.outline_json());
    }
    if (include_content) {
        const Json* content = out.find("content");
        (void)content;
    }

    Json result = text_result_with_structured(text.str(), out);
    if (include_content) {
        // Structured view also carries the text so Agents can avoid re-reading.
        result["structuredContent"].set("content", Json(std::string(document.text().substr(begin, end - begin))));
    }
    note_activity(context, "agent " + session_id(context) + " read the document");
    return result;
}

// ---------------------------------------------------------------------------
// get_document_info
// ---------------------------------------------------------------------------
Json tool_get_document_info(const Json&, ToolContext& context) {
    Document& document = require_document(context);
    Json info = document.info_json();
    info.set("selection", document.selection_json());
    info.set("policy", require_workspace(context).policy_json());
    std::ostringstream text;
    text << document.display_name() << " - " << document.character_count() << " characters, "
         << document.line_count() << " lines, " << document.block_count() << " blocks\n";
    text << "revision " << document.revision() << (document.dirty() ? " (unsaved)" : "") << "\n";
    return text_result_with_structured(text.str(), info);
}

// ---------------------------------------------------------------------------
// get_structure
// ---------------------------------------------------------------------------
Json tool_get_structure(const Json& arguments, ToolContext& context) {
    Document& document = require_document(context);
    const int64_t max_blocks = int_argument(arguments, "max_blocks", 0);
    const std::string kind_filter = utf8::to_lower(string_argument(arguments, "kind"));
    Json structure = document.structure_json(static_cast<int>(max_blocks));

    if (!kind_filter.empty()) {
        Json filtered = Json::array();
        for (const Json& block : structure["blocks"].items()) {
            if (utf8::to_lower(block.str("kind")) != kind_filter) continue;
            filtered.push(block);
        }
        structure.set("blocks", std::move(filtered));
        structure.set("filtered_by_kind", Json(kind_filter));
    }
    structure.set("outline", document.outline_json());
    structure.set("selection", document.selection_json());

    std::ostringstream text;
    text << document.block_count() << " blocks, revision " << document.revision() << "\n\n";
    text << "id\tkind\tlines\tordinal\tsummary\n";
    for (const Json& block : structure["blocks"].items()) {
        text << block.str("block_id") << "\t" << block.str("kind") << "\t" << block.integer("start_line")
             << "-" << block.integer("end_line") << "\t" << block.integer("ordinal") << "\t";
        if (block.contains("text")) {
            std::string summary = block.str("text");
            if (summary.size() > 60) summary = summary.substr(0, 57) + "...";
            text << summary;
        } else if (block.contains("code")) {
            std::string summary = block.str("code");
            std::replace(summary.begin(), summary.end(), '\n', ' ');
            if (summary.size() > 60) summary = summary.substr(0, 57) + "...";
            text << "[" << block.str("info", "code") << "] " << summary;
        } else if (block.str("kind") == "table") {
            text << block["table"].integer("rows") << " rows x " << block["table"].integer("columns")
                 << " columns";
        } else if (block.str("kind") == "list") {
            text << block["items"].size() << " items";
        } else {
            text << "(bytes " << block.integer("byte_start") << "-" << block.integer("byte_end") << ")";
        }
        text << "\n";
    }
    if (structure.boolean("truncated")) {
        text << "\n(showing the first " << structure["blocks"].size()
             << " blocks; raise max_blocks to see more)\n";
    }
    return text_result_with_structured(text.str(), structure);
}

// ---------------------------------------------------------------------------
// get_outline
// ---------------------------------------------------------------------------
Json tool_get_outline(const Json&, ToolContext& context) {
    Document& document = require_document(context);
    Json outline = document.outline_json();
    Json out = Json::object();
    out.set("outline", outline);
    out.set("revision", Json(document.revision()));
    std::ostringstream text;
    if (outline.items().empty()) {
        text << "The document has no headings.\n";
    } else {
        for (const Json& entry : outline.items()) {
            text << std::string(static_cast<size_t>(entry.integer("level", 1) - 1) * 2, ' ')
                 << "- " << entry.str("title") << "  (" << entry.str("block_id") << ", line "
                 << utf8::line_number_at(document.text(),
                                         static_cast<size_t>(entry.integer("byte_offset")))
                 << ")\n";
        }
    }
    return text_result_with_structured(text.str(), out);
}

// ---------------------------------------------------------------------------
// get_selection
// ---------------------------------------------------------------------------
Json tool_get_selection(const Json& arguments, ToolContext& context) {
    Document& document = require_document(context);
    Json selection = document.selection_json();
    const size_t context_chars =
        static_cast<size_t>(std::max<int64_t>(0, int_argument(arguments, "context_characters", 200)));
    if (context_chars > 0) {
        const Selection current = document.selection();
        const size_t begin = current.begin() > context_chars ? current.begin() - context_chars : 0;
        const size_t end = std::min(document.size(), current.end() + context_chars);
        selection.set("surrounding_text", Json(std::string(document.text().substr(begin, end - begin))));
        selection.set("surrounding_byte_start", Json(static_cast<int64_t>(begin)));
        selection.set("surrounding_byte_end", Json(static_cast<int64_t>(end)));
    }
    std::ostringstream text;
    if (document.selection().collapsed()) {
        text << "Caret at " << locator(document, document.caret()) << " (byte "
             << document.caret() << ").\n";
    } else {
        text << "Selection of " << document.selection().length() << " bytes from "
             << locator(document, document.selection().begin()) << " to "
             << locator(document, document.selection().end()) << ".\n";
        text << "--- selected text ---\n" << document.selection_json().str("text") << "\n";
    }
    return text_result_with_structured(text.str(), selection);
}

// ---------------------------------------------------------------------------
// get_block
// ---------------------------------------------------------------------------
Json tool_get_block(const Json& arguments, ToolContext& context) {
    Document& document = require_document(context);
    Patch address;
    address.block_id = string_argument(arguments, "block_id");
    if (has_argument(arguments, "block_index")) {
        address.block_index = static_cast<size_t>(int_argument(arguments, "block_index"));
    }
    address.relative_to_cursor = bool_argument(arguments, "at_cursor", false);
    std::string error;
    const auto index = document.resolve_block(address, &error);
    if (!index) {
        throw_tool_error("invalid_argument", error, Json([&] {
                             Json data = Json::object();
                             data.set("blocks_total", Json(static_cast<int64_t>(document.block_count())));
                             return data;
                         }()));
    }
    const md::Block& block = document.parsed().blocks[*index];
    Json out = md::block_to_json(block, document.text(), *index);
    out.set("content", Json(std::string(block.content(document.text()))));
    out.set("raw", Json(std::string(block.slice(document.text()))));
    const Json* inline_argument = argument(arguments, "inline");
    if (!inline_argument || inline_argument->as_bool(true)) {
        Json spans = Json::array();
        for (const auto& span : md::parse_inline(block.slice(document.text()))) {
            Json entry = Json::object();
            entry.set("kind", Json(span.kind));
            entry.set("text", Json(span.text));
            if (!span.target.empty()) entry.set("target", Json(span.target));
            entry.set("byte_start", Json(static_cast<int64_t>(span.begin)));
            entry.set("byte_end", Json(static_cast<int64_t>(span.end)));
            spans.push(std::move(entry));
        }
        out.set("inline", std::move(spans));
    }
    std::ostringstream text;
    text << "Block " << out.str("block_id") << " (" << out.str("kind") << "), lines "
         << out.integer("start_line") << "-" << out.integer("end_line") << ", bytes "
         << out.integer("byte_start") << "-" << out.integer("byte_end") << "\n";
    text << "--- raw source ---\n" << out.str("raw");
    return text_result_with_structured(text.str(), out);
}

// ---------------------------------------------------------------------------
// search
// ---------------------------------------------------------------------------
Json tool_search(const Json& arguments, ToolContext& context) {
    Document& document = require_document(context);
    const std::string query = string_argument(arguments, "query");
    if (query.empty()) throw_tool_error("invalid_argument", "'query' must not be empty");
    const bool case_sensitive = bool_argument(arguments, "case_sensitive", false);
    const int64_t limit = std::max<int64_t>(1, int_argument(arguments, "max_results", 25));
    const bool with_lines = bool_argument(arguments, "include_lines", true);

    const std::string haystack_storage = case_sensitive ? document.text() : utf8::to_lower(document.text());
    const std::string needle = case_sensitive ? query : utf8::to_lower(query);
    const std::string_view haystack = haystack_storage;
    const std::string_view text = document.text();

    Json matches = Json::array();
    size_t position = 0;
    while (matches.items().size() < static_cast<size_t>(limit)) {
        const size_t found = haystack.find(needle, position);
        if (found == std::string::npos) break;
        const size_t line_begin = utf8::line_start(text, found);
        const size_t line_end = utf8::line_end(text, found);
        Json match = Json::object();
        match.set("byte_start", Json(static_cast<int64_t>(found)));
        match.set("byte_end", Json(static_cast<int64_t>(found + needle.size())));
        match.set("line", Json(static_cast<int64_t>(utf8::line_number_at(text, found))));
        match.set("column", Json(static_cast<int64_t>(utf8::column_at(text, found))));
        const int block_index = document.block_containing(found);
        if (block_index >= 0) {
            match.set("block_id", Json("b" + std::to_string(block_index)));
            match.set("block_kind",
                      Json(std::string(md::kind_name(
                          document.parsed().blocks[static_cast<size_t>(block_index)].kind))));
        }
        if (with_lines) {
            match.set("line_text", Json(std::string(text.substr(line_begin, line_end - line_begin))));
        }
        matches.push(std::move(match));
        position = found + std::max<size_t>(1, needle.size());
    }

    Json out = Json::object();
    out.set("query", Json(query));
    out.set("case_sensitive", Json(case_sensitive));
    out.set("matches", matches);
    out.set("match_count", Json(static_cast<int64_t>(matches.items().size())));
    out.set("truncated", Json(matches.items().size() >= static_cast<size_t>(limit)));
    out.set("revision", Json(document.revision()));

    std::ostringstream text_out;
    if (matches.items().empty()) {
        text_out << "No matches for \"" << query << "\".\n";
    } else {
        text_out << matches.items().size() << " match(es) for \"" << query << "\":\n";
        for (const Json& match : matches.items()) {
            text_out << "  line " << match.integer("line") << ", col " << match.integer("column")
                     << " (byte " << match.integer("byte_start") << ", "
                     << match.str("block_id", "?") << "): " << match.str("line_text") << "\n";
        }
    }
    return text_result_with_structured(text_out.str(), out);
}

// ---------------------------------------------------------------------------
// read_range
// ---------------------------------------------------------------------------
Json tool_read_range(const Json& arguments, ToolContext& context) {
    Document& document = require_document(context);
    if (!has_argument(arguments, "start") && !has_argument(arguments, "end")) {
        throw_tool_error("invalid_argument", "read_range requires 'start' and/or 'end'");
    }
    size_t begin = 0;
    size_t end = document.size();
    resolve_range_argument(arguments, document.text(), begin, end);
    const std::string slice(document.text().substr(begin, end - begin));
    Json out = Json::object();
    out.set("content", Json(slice));
    out.set("byte_start", Json(static_cast<int64_t>(begin)));
    out.set("byte_end", Json(static_cast<int64_t>(end)));
    out.set("start_line", Json(static_cast<int64_t>(utf8::line_number_at(document.text(), begin))));
    out.set("end_line", Json(static_cast<int64_t>(utf8::line_number_at(document.text(), end))));
    out.set("revision", Json(document.revision()));
    std::ostringstream text;
    text << "Lines " << out.integer("start_line") << "-" << out.integer("end_line") << " (bytes "
         << begin << "-" << end << "):\n";
    text << numbered(slice, static_cast<size_t>(out.integer("start_line")));
    return text_result_with_structured(text.str(), out);
}

// ---------------------------------------------------------------------------
// apply_patch
// ---------------------------------------------------------------------------
Json tool_apply_patch(const Json& arguments, ToolContext& context) {
    Workspace& workspace = require_workspace(context);
    Document& document = workspace.document();
    std::vector<Patch> patches = parse_patches(arguments);

    const std::string expected = string_argument(arguments, "expected_revision");
    const bool force = bool_argument(arguments, "force", false);
    const std::string revision_before = document.revision();

    if (!expected.empty() && expected != revision_before && !force) {
        throw_tool_error("conflict",
                         "the document has changed since you read it; call "
                         "colaimdation_read_document and re-apply, or pass force=true",
                         Json([&] {
                             Json data = Json::object();
                             data.set("expected_revision", Json(expected));
                             data.set("current_revision", Json(revision_before));
                             return data;
                         }()));
    }

    const bool preview = bool_argument(arguments, "preview", false);
    if (preview) {
        Document staged;
        staged.set_text(document.text(), EditSource::System, "preview", "preview", true);
        staged.set_selection(document.selection().anchor, document.selection().head);
        PatchOutcome outcome =
            staged.apply_patches(patches, EditSource::Agent, "apply_patch", "preview",
                                 session_id(context));
        if (!outcome.ok) {
            throw_tool_error(outcome.error_code, outcome.error_message);
        }
        Json out = Json::object();
        out.set("status", Json("preview"));
        out.set("revision_before", Json(revision_before));
        out.set("revision_after", Json(staged.revision()));
        out.set("diff", Json(unified_diff(document.text(), staged.text(), "current", "proposed")));
        out.set("resulting_content", Json(staged.text()));
        out.set("byte_delta", Json(static_cast<int64_t>(staged.size()) -
                                   static_cast<int64_t>(document.size())));
        std::ostringstream text;
        text << "Preview only - nothing was changed.\n\n" << out.str("diff");
        return text_result_with_structured(text.str(), out);
    }

    const std::string summary = string_argument(
        arguments, "summary",
        "agent applied " + std::to_string(patches.size()) + " patch(es) via apply_patch");
    const bool bypass = bool_argument(arguments, "bypass_confirmation", false);

    // Mutating a document from another thread must be marshalled onto the UI
    // thread, which is what the bridge does.
    std::string error_code;
    std::string error_message;
    Workspace::WriteOutcome outcome;
    bool executed = false;
    if (IEditorBridge* bridge = workspace.bridge(); bridge && bridge->has_ui()) {
        std::optional<Json> posted = bridge->post([&]() -> Json {
            outcome = workspace.apply_agent_patches(patches, "apply_patch", session_id(context),
                                                    summary, bypass);
            executed = true;
            return Json();
        });
        if (!posted) {
            throw_tool_error("editor_unavailable",
                             "the editor window did not respond in time; the change was not applied");
        }
    } else {
        outcome = workspace.apply_agent_patches(patches, "apply_patch", session_id(context), summary,
                                               bypass);
        executed = true;
    }
    (void)executed;
    (void)error_code;
    (void)error_message;

    if (!outcome.ok) {
        throw_tool_error(outcome.error_code, outcome.error_message,
                         patch_outcome_json(outcome, workspace.document(), revision_before));
    }
    Json structured = patch_outcome_json(outcome, workspace.document(), revision_before);
    return text_result_with_structured(
        write_summary_text(outcome, workspace.document(), revision_before), structured);
}

// ---------------------------------------------------------------------------
// insert_text / replace_range / delete_range
// ---------------------------------------------------------------------------
Json simple_write(ToolContext& context, const std::vector<Patch>& patches, const std::string& tool,
                  const std::string& summary) {
    Workspace& workspace = require_workspace(context);
    const std::string revision_before = workspace.document().revision();
    Workspace::WriteOutcome outcome;
    if (IEditorBridge* bridge = workspace.bridge(); bridge && bridge->has_ui()) {
        std::optional<Json> posted = bridge->post([&]() -> Json {
            outcome = workspace.apply_agent_patches(patches, tool, session_id(context), summary,
                                                    false);
            return Json();
        });
        if (!posted) {
            throw_tool_error("editor_unavailable",
                             "the editor window did not respond in time; the change was not applied");
        }
    } else {
        outcome = workspace.apply_agent_patches(patches, tool, session_id(context), summary, false);
    }
    if (!outcome.ok) {
        throw_tool_error(outcome.error_code, outcome.error_message,
                         patch_outcome_json(outcome, workspace.document(), revision_before));
    }
    return text_result_with_structured(
        write_summary_text(outcome, workspace.document(), revision_before),
        patch_outcome_json(outcome, workspace.document(), revision_before));
}

Json tool_insert_text(const Json& arguments, ToolContext& context) {
    Document& document = context.workspace->document();
    const auto offset = resolve_offset_argument(arguments, "offset", document.text());
    if (!offset) throw_tool_error("invalid_argument", "insert_text requires 'offset'");
    const Json* text = argument(arguments, "text");
    if (!text || text->is_null()) throw_tool_error("invalid_argument", "insert_text requires 'text'");
    const std::string content = text->as_string();

    const size_t anchor = *offset;
    Patch patch = Patch::insert(anchor, content);
    Json result = simple_write(context, {patch}, "insert_text", "agent inserted text");
    result["structuredContent"].set("inserted_at", Json(static_cast<int64_t>(anchor)));
    result["structuredContent"].set("inserted_characters",
                                    Json(static_cast<int64_t>(utf8::length(content))));
    return result;
}

Json tool_replace_range(const Json& arguments, ToolContext& context) {
    Document& document = context.workspace->document();
    size_t begin = 0;
    size_t end = 0;
    resolve_range_argument(arguments, document.text(), begin, end);
    if (begin == end && !has_argument(arguments, "start")) {
        throw_tool_error("invalid_argument", "replace_range requires 'start' and 'end'");
    }
    const Json* text = argument(arguments, "text");
    if (!text || text->is_null()) {
        throw_tool_error("invalid_argument", "replace_range requires 'text'");
    }
    const std::string replaced =
        std::string(document.text().substr(begin, end - begin));
    Json result = simple_write(context, {Patch::replace(begin, end, text->as_string())},
                               "replace_range", "agent replaced a range");
    result["structuredContent"].set("replaced_text", Json(replaced));
    return result;
}

Json tool_delete_range(const Json& arguments, ToolContext& context) {
    Document& document = context.workspace->document();
    size_t begin = 0;
    size_t end = 0;
    resolve_range_argument(arguments, document.text(), begin, end);
    if (begin == end) {
        throw_tool_error("invalid_argument",
                         "delete_range needs a non-empty range ('start' before 'end')");
    }
    const std::string removed = std::string(document.text().substr(begin, end - begin));
    Json result = simple_write(context, {Patch::erase(begin, end)}, "delete_range",
                               "agent deleted a range");
    result["structuredContent"].set("deleted_text", Json(removed));
    result["structuredContent"].set("deleted_bytes", Json(static_cast<int64_t>(removed.size())));
    return result;
}

// ---------------------------------------------------------------------------
// replace_block
// ---------------------------------------------------------------------------
Json tool_replace_block(const Json& arguments, ToolContext& context) {
    Document& document = context.workspace->document();
    Patch patch;
    patch.op = PatchOp::ReplaceBlock;
    patch.block_id = string_argument(arguments, "block_id");
    if (has_argument(arguments, "block_index")) {
        patch.block_index = static_cast<size_t>(int_argument(arguments, "block_index"));
    }
    patch.relative_to_cursor = bool_argument(arguments, "at_cursor", false);
    if (patch.block_id.empty() && !patch.block_index.has_value() && !patch.relative_to_cursor) {
        throw_tool_error("invalid_argument",
                         "replace_block requires 'block_id', 'block_index' or 'at_cursor'");
    }
    const Json* text = argument(arguments, "text");
    if (!text || text->is_null()) throw_tool_error("invalid_argument", "replace_block requires 'text'");
    patch.text = text->as_string();

    std::string error;
    const auto index = document.resolve_block(patch, &error);
    if (!index) throw_tool_error("invalid_argument", error);
    const md::Block& block = document.parsed().blocks[*index];
    const std::string previous = std::string(block.content(document.text()));
    const std::string kind(md::kind_name(block.kind));

    Json result = simple_write(context, {patch}, "replace_block",
                               "agent replaced block b" + std::to_string(*index) + " (" + kind + ")");
    result["structuredContent"].set("block_id", Json("b" + std::to_string(*index)));
    result["structuredContent"].set("block_kind", Json(kind));
    result["structuredContent"].set("previous_content", Json(previous));
    return result;
}

} // namespace

void register_document_read_tools(ToolRegistry& registry) {
    registry.add(ToolDescriptor{
        "colaimdation_read_document",
        "Read the document",
        "Read the Markdown document that is open in the Colaimdation editor. Returns the text (with "
        "line numbers), metadata, the caret/selection and optionally the block structure. This is "
        "the only way to see the content: Agents never read the file from disk, so reads always "
        "reflect what the writer currently sees.",
        object_schema(
            {
                {"mode", string_enum("What to include. 'content' returns the text, 'structure' the "
                                     "block map, 'full' both plus the outline.",
                                     {"content", "structure", "outline", "full", "all"})},
                {"start", anything("First byte offset, or {\"line\": N, \"column\": M} (1-based).")},
                {"end", anything("One-past-last byte offset, or {\"line\": N, \"column\": M}. A line "
                                 "without a column means through the end of that line.")},
                {"content", boolean("Include the document text.", true)},
                {"structure", boolean("Include the block structure.", false)},
                {"outline", boolean("Include the heading outline.", false)},
            },
            {}),
        annotations(true),
        tool_read_document});

    registry.add(ToolDescriptor{
        "colaimdation_get_document_info",
        "Get document metadata",
        "Compact metadata for the open document: path, revision, size, block count, dirty flag, "
        "caret/selection and the active security policy. Cheap to call before deciding what to edit.",
        object_schema({}, {}),
        annotations(true),
        tool_get_document_info});

    registry.add(ToolDescriptor{
        "colaimdation_get_structure",
        "Get the block structure",
        "List the document's blocks with stable ids (b0, b1, ...), kind, line and byte ranges. Use "
        "these ids with colaimdation_replace_block or patch ops replace_block/insert_block to edit "
        "precisely without copying the whole document.",
        object_schema(
            {
                {"max_blocks", integer("Maximum number of blocks to return (0 = all).", 0, 100000, true)},
                {"kind", string_enum("Only return blocks of this kind.", {"heading", "paragraph",
                                                                          "code_block", "table",
                                                                          "list", "block_quote",
                                                                          "thematic_break",
                                                                          "html_block", "math_block",
                                                                          "front_matter"})},
            },
            {}),
        annotations(true),
        tool_get_structure});

    registry.add(ToolDescriptor{
        "colaimdation_get_outline",
        "Get the heading outline",
        "Return the document's heading tree with levels, titles and the block id of each heading.",
        object_schema({}, {}),
        annotations(true),
        tool_get_outline});

    registry.add(ToolDescriptor{
        "colaimdation_get_selection",
        "Read the caret and selection",
        "Report where the writer's caret is and what they have selected, plus the text around it. "
        "Use this to collaborate on the passage the writer is working on right now.",
        object_schema(
            {
                {"context_characters", integer("How many characters of surrounding text to include on "
                                               "each side.",
                                               0, 100000, true)},
            },
            {}),
        annotations(true),
        tool_get_selection});

    registry.add(ToolDescriptor{
        "colaimdation_get_block",
        "Read one block",
        "Return a single block by id, index or caret position: exact byte/line span, raw source, "
        "inner content, sub-structure (table rows, list items) and inline markup spans.",
        object_schema(
            {
                {"block_id", string("Block id from colaimdation_get_structure, e.g. \"b7\".")},
                {"block_index", integer("Zero-based block index (block_id is preferred).", 0, 100000, true)},
                {"at_cursor", boolean("Use the block containing the writer's caret.", false)},
                {"inline", boolean("Include inline markup spans.", true)},
            },
            {}),
        annotations(true),
        tool_get_block});

    registry.add(ToolDescriptor{
        "colaimdation_search",
        "Search the document",
        "Case-insensitive substring search over the live document text. Returns byte offsets, line "
        "and column numbers and the owning block id for each match, ready to feed into a patch.",
        object_schema(
            {
                {"query", string("Text to search for (plain substring, not a regular expression).")},
                {"case_sensitive", boolean("Match case.", false)},
                {"max_results", integer("Maximum number of matches.", 1, 1000, true)},
                {"include_lines", boolean("Include the full text of each matching line.", true)},
            },
            {"query"}),
        annotations(true),
        tool_search});

    registry.add(ToolDescriptor{
        "colaimdation_read_range",
        "Read a range",
        "Read an exact byte or line range of the open document with line numbers. Cheaper than "
        "reading the whole document on large files.",
        object_schema(
            {
                {"start", anything("First byte offset, or {\"line\": N, \"column\": M}.")},
                {"end", anything("One-past-last byte offset, or {\"line\": N, \"column\": M}.")},
            },
            {"start", "end"}),
        annotations(true),
        tool_read_range});
}

void register_document_write_tools(ToolRegistry& registry) {
    registry.add(ToolDescriptor{
        "colaimdation_apply_patch",
        "Patch the document (incremental edits)",
        "Apply one or more incremental patches to the open document. This is the primary write path: "
        "prefer it over rewriting the whole document because patches are validated, audited and "
        "reversible, and the editor re-renders only the affected region.\n"
        "Patch ops: insert_text {offset, text}; replace_range {start, end, text}; delete_range "
        "{start, end}; replace_block {block_id|block_index|at_cursor, text}; replace_block_raw "
        "{block_id, text}; insert_block {block_id, text, position: before|after|start_of_document|"
        "end_of_document}; append_section {text}; set_text {text}.\n"
        "Offsets are UTF-8 byte offsets. Batches are atomic: if any patch is invalid nothing is "
        "applied. Set preview=true to see the diff without changing anything.",
        object_schema(
            {
                {"patches", array_of(
                                object_schema(
                                    {
                                        {"op", string_enum("Patch operation.",
                                                           {"insert_text", "replace_range",
                                                            "delete_range", "replace_block",
                                                            "replace_block_raw", "insert_block",
                                                            "append_section", "set_text"})},
                                        {"offset", anything("insert_text: UTF-8 byte offset.")},
                                        {"start", anything("replace_range/delete_range: first byte "
                                                           "offset or {line, column}.")},
                                        {"end", anything("replace_range/delete_range: one-past-last "
                                                         "byte offset or {line, column}.")},
                                        {"block_id", string("Block id such as \"b3\".")},
                                        {"block_index", integer("Zero-based block index.", 0, 100000, true)},
                                        {"at_cursor", boolean("Address the block at the caret.", false)},
                                        {"position", string_enum("insert_block: where to insert.",
                                                                 {"before", "after",
                                                                  "start_of_document",
                                                                  "end_of_document"})},
                                        {"text", string("The text to insert or the replacement text.")},
                                        {"note", string("Optional note recorded in the audit log.")},
                                    },
                                    {"op"}),
                                "Ordered list of patch objects.")},
                {"expected_revision", string("Optimistic concurrency: reject the write if the document "
                                             "revision differs (get it from read_document).")},
                {"force", boolean("Apply even when expected_revision does not match.", false)},
                {"preview", boolean("Return the diff without applying anything.", false)},
                {"summary", string("One-line description recorded in the audit log.")},
                {"bypass_confirmation", boolean("Ask to skip the user's diff confirmation (only "
                                                "honoured when the policy allows it).",
                                                false)},
            },
            {"patches"}),
        annotations(false, true),
        tool_apply_patch});

    registry.add(ToolDescriptor{
        "colaimdation_insert_text",
        "Insert text at an offset",
        "Insert text at a UTF-8 byte offset (or a {line, column} position) without touching anything "
        "else. Convenience wrapper around a single insert_text patch.",
        object_schema(
            {
                {"offset", anything("UTF-8 byte offset, or {\"line\": N, \"column\": M}.")},
                {"text", string("Text to insert.")},
            },
            {"offset", "text"}),
        annotations(false, true),
        tool_insert_text});

    registry.add(ToolDescriptor{
        "colaimdation_replace_range",
        "Replace a range",
        "Replace an exact byte or line range with new text. Convenience wrapper around a single "
        "replace_range patch.",
        object_schema(
            {
                {"start", anything("First byte offset, or {\"line\": N, \"column\": M}.")},
                {"end", anything("One-past-last byte offset, or {\"line\": N, \"column\": M}.")},
                {"text", string("Replacement text (may be empty to delete).")},
            },
            {"start", "end", "text"}),
        annotations(false, true),
        tool_replace_range});

    registry.add(ToolDescriptor{
        "colaimdation_delete_range",
        "Delete a range",
        "Delete an exact byte or line range. Convenience wrapper around a single delete_range patch.",
        object_schema(
            {
                {"start", anything("First byte offset, or {\"line\": N, \"column\": M}.")},
                {"end", anything("One-past-last byte offset, or {\"line\": N, \"column\": M}.")},
            },
            {"start", "end"}),
        annotations(false, true),
        tool_delete_range});

    registry.add(ToolDescriptor{
        "colaimdation_replace_block",
        "Replace one block's content",
        "Replace the inner content of a block identified by id, index or caret position, keeping its "
        "Markdown markers: a heading keeps its #s, a fenced code block keeps its fences and language, "
        "a list keeps its bullets. This is the safest way to rewrite a paragraph or code block.",
        object_schema(
            {
                {"block_id", string("Block id such as \"b3\".")},
                {"block_index", integer("Zero-based block index.", 0, 100000, true)},
                {"at_cursor", boolean("Use the block containing the writer's caret.", false)},
                {"text", string("New inner content.")},
            },
            {"text"}),
        annotations(false, true),
        tool_replace_block});
}

} // namespace colai::mcp
