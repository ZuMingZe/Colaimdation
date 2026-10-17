// Colaimdation - collaboration tools: save, caret control, change feed, audit,
// rollback and the security policy.
#include "mcp/tool_registry.h"

#include "core/diff.h"
#include "core/markdown.h"
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

std::string session_id(ToolContext& context) {
    return context.session ? context.session->id : std::string("anonymous");
}

// Runs `job` on the editor UI thread when a bridge is attached, so document
// access from an HTTP worker thread stays single-threaded.
template <typename Fn>
auto on_ui_thread(ToolContext& context, Fn&& job) -> decltype(job()) {
    Workspace& workspace = *context.workspace;
    if (IEditorBridge* bridge = workspace.bridge(); bridge && bridge->has_ui()) {
        using Result = decltype(job());
        Result result{};
        bool ran = false;
        std::optional<Json> posted = bridge->post([&]() -> Json {
            result = job();
            ran = true;
            return Json();
        });
        if (!posted || !ran) {
            throw_tool_error("editor_unavailable",
                             "the editor window did not respond in time; nothing was changed");
        }
        return result;
    }
    return job();
}

// ---------------------------------------------------------------------------
// save_document
// ---------------------------------------------------------------------------
Json tool_save_document(const Json& arguments, ToolContext& context) {
    Workspace& workspace = *context.workspace;
    const std::string path = string_argument(arguments, "path");
    const bool force = bool_argument(arguments, "force", false);

    if (!workspace.policy().allow_save) {
        throw_tool_error("permission_denied",
                         "saving is disabled by the editor policy (allow_save=false)");
    }
    if (!path.empty() && !workspace.path_allowed(path)) {
        throw_tool_error("permission_denied",
                         "the requested path is outside the directory this editor is allowed to write");
    }
    if (!force && !workspace.document().dirty()) {
        Json out = Json::object();
        out.set("status", Json("already_saved"));
        out.set("path", Json(workspace.document().path()));
        out.set("revision", Json(workspace.document().revision()));
        return text_result_with_structured("The document has no unsaved changes.\n", out);
    }

    std::string error;
    std::string saved_path;
    bool ok = on_ui_thread(context, [&]() {
        Status status = path.empty() ? workspace.save_file(&saved_path, &error)
                                     : workspace.save_file_as(path, &error);
        return status.ok;
    });

    Json out = Json::object();
    if (!ok) {
        out.set("status", Json("error"));
        out.set("error", Json(error));
        throw_tool_error("io_error", error, out);
    }
    out.set("status", Json("saved"));
    out.set("path", Json(path.empty() ? workspace.document().path() : path));
    out.set("revision", Json(workspace.document().revision()));
    out.set("bytes", Json(static_cast<int64_t>(workspace.document().size())));
    out.set("line_ending", Json(workspace.document().line_ending() == "\r\n" ? "crlf" : "lf"));
    if (context.emit) {
        Json notification = Json::object();
        notification.set("type", Json("document_saved"));
        notification.set("path", Json(out.str("path")));
        notification.set("session", Json(session_id(context)));
        context.emit(notification);
    }
    std::ostringstream text;
    text << "Saved " << out.str("path") << " (" << out.integer("bytes") << " bytes).\n";
    return text_result_with_structured(text.str(), out);
}

// ---------------------------------------------------------------------------
// set_selection
// ---------------------------------------------------------------------------
Json tool_set_selection(const Json& arguments, ToolContext& context) {
    Workspace& workspace = *context.workspace;
    Document& document = workspace.document();
    if (!workspace.policy().allow_selection_control) {
        throw_tool_error("permission_denied",
                         "selection control is disabled by the editor policy");
    }
    const auto caret = resolve_offset_argument(arguments, "caret", document.text());
    const auto anchor = resolve_offset_argument(arguments, "anchor", document.text());
    const auto start = resolve_offset_argument(arguments, "start", document.text());
    const auto end = resolve_offset_argument(arguments, "end", document.text());

    if (!caret && !start && !end) {
        throw_tool_error("invalid_argument",
                         "provide 'caret' (and optionally 'anchor'), or 'start' and 'end'");
    }
    size_t anchor_offset = 0;
    size_t head_offset = 0;
    if (start && end) {
        anchor_offset = *start;
        head_offset = *end;
    } else {
        head_offset = caret ? *caret : (end ? *end : 0);
        anchor_offset = anchor ? *anchor : (start ? *start : head_offset);
    }

    std::string error;
    const bool ok = on_ui_thread(context, [&]() {
        return workspace.set_caret(anchor_offset, head_offset, session_id(context), &error).ok;
    });
    if (!ok) throw_tool_error("invalid_argument", error);

    Json out = workspace.document().selection_json();
    out.set("status", Json("ok"));
    std::ostringstream text;
    text << "Caret moved to byte " << workspace.document().caret() << " (line "
         << workspace.document().selection_json().integer("head_line") << ").\n";
    return text_result_with_structured(text.str(), out);
}

// ---------------------------------------------------------------------------
// get_user_changes
// ---------------------------------------------------------------------------
Json tool_get_user_changes(const Json& arguments, ToolContext& context) {
    Workspace& workspace = *context.workspace;
    const std::string since = string_argument(arguments, "since_revision");
    const std::string session = string_argument(arguments, "session", session_id(context));
    const int64_t limit = std::max<int64_t>(1, int_argument(arguments, "max_changes", 20));
    const bool polling = bool_argument(arguments, "poll", false);

    Json result = polling ? workspace.poll_user_changes(session, since, static_cast<size_t>(limit))
                          : workspace.user_changes_since(since, static_cast<size_t>(limit));

    std::ostringstream text;
    const int64_t count = result.integer("change_count");
    if (count == 0) {
        text << "No edits by the writer since " << (since.empty() ? "the beginning" : since) << ".\n";
    } else {
        text << count << " edit(s) by the writer:\n";
        for (const Json& change : result["changes"].items()) {
            text << "  #" << change.integer("sequence") << " " << change.str("summary") << " ("
                 << change.integer("changed_bytes") << " bytes, revision "
                 << change.str("revision_before").substr(0, 8) << " -> "
                 << change.str("revision_after").substr(0, 8) << ")\n";
        }
    }
    text << "current revision: " << workspace.document().revision() << "\n";
    text << "current size: " << workspace.document().character_count() << " characters, "
         << workspace.document().block_count() << " blocks\n";
    return text_result_with_structured(text.str(), result);
}

// ---------------------------------------------------------------------------
// get_history
// ---------------------------------------------------------------------------
Json tool_get_history(const Json& arguments, ToolContext& context) {
    Workspace& workspace = *context.workspace;
    const int64_t limit = std::max<int64_t>(1, int_argument(arguments, "max_entries", 25));
    const bool with_text = bool_argument(arguments, "include_text", false);
    Json history = workspace.history_json(static_cast<size_t>(limit), with_text);

    std::ostringstream text;
    text << workspace.document().history().size() << " recorded edit(s), newest last:\n";
    for (const Json& edit : history["edits"].items()) {
        text << "  #" << edit.integer("sequence") << " " << edit.str("source") << "/"
             << edit.str("tool") << " - " << edit.str("summary") << " [" << edit.str("timestamp")
             << "]\n";
    }
    text << "can_undo: " << (history.boolean("can_undo") ? "yes" : "no")
         << ", can_redo: " << (history.boolean("can_redo") ? "yes" : "no") << "\n";
    return text_result_with_structured(text.str(), history);
}

// ---------------------------------------------------------------------------
// get_audit_log
// ---------------------------------------------------------------------------
Json tool_get_audit_log(const Json& arguments, ToolContext& context) {
    Workspace& workspace = *context.workspace;
    AuditQuery query;
    query.limit = static_cast<size_t>(std::max<int64_t>(1, int_argument(arguments, "limit", 25)));
    query.tool = string_argument(arguments, "tool");
    query.session = string_argument(arguments, "session");
    query.with_diff = bool_argument(arguments, "include_diff", false);
    const std::string source = string_argument(arguments, "source");
    if (!source.empty()) {
        const auto parsed = edit_source_from_name(source);
        if (!parsed) throw_tool_error("invalid_argument", "source must be user|agent|system|external");
        query.source = *parsed;
    }
    const std::string document_only = string_argument(arguments, "document_id", workspace.document().id());
    query.document_id = document_only;
    const int64_t since_ms = int_argument(arguments, "since_ms", 0);
    query.since_ms = since_ms;

    Json result = workspace.audit_json(query);
    std::ostringstream text;
    text << result.integer("count") << " audit entr(ies) (of " << result.integer("total_recorded")
         << " recorded):\n";
    for (const Json& entry : result["entries"].items()) {
        text << "  #" << entry.integer("id") << " " << entry.str("timestamp") << " "
             << entry.str("source") << "/" << entry.str("tool") << " [" << entry.str("status") << "] "
             << entry.str("summary");
        if (entry.contains("error")) text << "  ERROR " << entry["error"].str("code");
        text << "\n";
    }
    if (result["entries"].items().empty()) text << "  (nothing matched)\n";
    return text_result_with_structured(text.str(), result);
}

// ---------------------------------------------------------------------------
// rollback
// ---------------------------------------------------------------------------
Json tool_rollback(const Json& arguments, ToolContext& context) {
    Workspace& workspace = *context.workspace;
    if (!workspace.policy().allow_rollback) {
        throw_tool_error("permission_denied", "rollback is disabled by the editor policy");
    }
    const std::string revision = string_argument(arguments, "to_revision");
    const bool last = bool_argument(arguments, "last", revision.empty());
    // No hard-coded default here: the schema's "default" is only a hint that
    // clients may never send, so the value the handler sees when the caller
    // omits `source` is the same empty string a client would get. Treating that
    // as "agent" made the documented default ("source defaults to agent")
    // depend on the client honouring the hint, and `tool_set_selection`'s
    // start/end form of the same shape wants an explicit source.
    const std::string source = string_argument(arguments, "source");

    Json result;
    if (!revision.empty()) {
        result = on_ui_thread(context, [&]() {
            return workspace.rollback_to_revision(revision, "rollback");
        });
    } else if (last) {
        std::optional<EditSource> from;
        if (!source.empty() && utf8::to_lower(source) != "any") {
            from = edit_source_from_name(source);
            if (!from) throw_tool_error("invalid_argument", "source must be agent|user|system|any");
        }
        result = on_ui_thread(context, [&]() { return workspace.rollback_last(from, "rollback"); });
    } else {
        const int64_t sequence = int_argument(arguments, "to_sequence", -1);
        if (sequence < 0) {
            throw_tool_error("invalid_argument",
                             "provide 'last', 'to_revision' or 'to_sequence'");
        }
        result = on_ui_thread(context, [&]() {
            RollbackPlan plan = workspace.document().plan_rollback_to_sequence(
                static_cast<uint64_t>(sequence));
            if (!plan.ok) {
                Json failure = Json::object();
                failure.set("status", Json("error"));
                failure.set("error", Json(plan.error));
                return failure;
            }
            PatchOutcome outcome = workspace.document().apply_rollback(plan, EditSource::Agent,
                                                                      "rollback");
            Json out = Json::object();
            out.set("status", Json(outcome.ok ? "applied" : "error"));
            if (!outcome.ok) out.set("error", Json(outcome.error_message));
            out.set("revision", Json(workspace.document().revision()));
            out.set("reverted_edits", Json(static_cast<int64_t>(plan.edits_to_revert)));
            out.set("diff", Json(plan.diff));
            return out;
        });
        if (result.str("status") == "applied") {
            // Notify the editor so the front-end reloads the reverted content.
            if (context.emit) {
                Json notification = Json::object();
                notification.set("type", Json("document_changed"));
                notification.set("reason", Json("rollback"));
                notification.set("session", Json(session_id(context)));
                notification.set("revision", Json(workspace.document().revision()));
                notification.set("content", Json(workspace.document().text()));
                context.emit(notification);
            }
        }
    }

    if (result.str("status") == "error") {
        throw_tool_error("conflict", result.str("error"), result);
    }
    std::ostringstream text;
    text << "Rollback applied: " << result.integer("reverted_edits") << " edit(s) reverted.\n";
    text << "document is now at revision " << result.str("revision") << " ("
         << workspace.document().character_count() << " characters).\n";
    if (result.contains("diff")) text << "\n" << result.str("diff");
    return text_result_with_structured(text.str(), result);
}

// ---------------------------------------------------------------------------
// get_policy / set_policy
// ---------------------------------------------------------------------------
Json tool_get_policy(const Json&, ToolContext& context) {
    Json policy = context.workspace->policy_json();
    std::ostringstream text;
    text << "Writes: " << (policy.boolean("allow_write") ? "allowed" : "blocked") << "\n";
    text << "Human confirmation of agent edits: "
         << (policy.boolean("require_confirmation") ? "required" : "not required") << "\n";
    text << "Front matter protection: "
         << (policy.boolean("protect_front_matter") ? "on" : "off") << "\n";
    text << "Save: " << (policy.boolean("allow_save") ? "allowed" : "blocked")
         << ", rollback: " << (policy.boolean("allow_rollback") ? "allowed" : "blocked")
         << ", caret control: "
         << (policy.boolean("allow_selection_control") ? "allowed" : "blocked") << "\n";
    text << "Max patch size: " << policy.integer("max_patch_bytes") << " bytes, max "
         << policy.integer("max_patches_per_call") << " patches per call\n";
    if (!policy.str("allowed_directory").empty()) {
        text << "Filesystem scope: " << policy.str("allowed_directory") << "\n";
    }
    return text_result_with_structured(text.str(), policy);
}

Json tool_set_policy(const Json& arguments, ToolContext& context) {
    if (string_argument(arguments, "confirm") != "yes") {
        throw_tool_error("invalid_argument",
                         "set_policy changes security settings; pass \"confirm\": \"yes\" to acknowledge "
                         "that you are changing the rules the writer relies on");
    }
    Json patch = Json::object();
    for (const auto& entry : arguments.members().entries()) {
        if (entry.first == "confirm") continue;
        patch.set(entry.first, entry.second);
    }
    if (patch.members().empty()) {
        throw_tool_error("invalid_argument",
                         "provide at least one policy field to change, e.g. {\"allow_write\": false}");
    }
    Json updated = context.workspace->set_policy(patch);
    if (context.emit) {
        Json notification = Json::object();
        notification.set("type", Json("policy_changed"));
        notification.set("session", Json(session_id(context)));
        notification.set("policy", updated);
        context.emit(notification);
    }
    std::ostringstream text;
    text << "Security policy updated. Writes are now "
         << (updated.boolean("allow_write") ? "allowed" : "blocked") << ", confirmation "
         << (updated.boolean("require_confirmation") ? "required" : "not required") << ".\n";
    return text_result_with_structured(text.str(), updated);
}

// ---------------------------------------------------------------------------
// get_workspace_info
// ---------------------------------------------------------------------------
Json tool_get_workspace_info(const Json&, ToolContext& context) {
    Json status = context.workspace->status_json();
    status.set("tools_total", Json(static_cast<int64_t>(0)));
    if (context.session) {
        Json session = Json::object();
        session.set("id", Json(context.session->id));
        session.set("client_name", Json(context.session->client_name));
        session.set("client_version", Json(context.session->client_version));
        session.set("calls", Json(static_cast<int64_t>(context.session->calls)));
        status.set("session", std::move(session));
    }
    status.set("mcp_endpoint", Json(context.workspace->bridge() && context.workspace->bridge()->has_ui()
                                        ? "http://127.0.0.1 (see the editor's MCP panel for the port)"
                                        : "(headless)"));
    std::ostringstream text;
    text << "Colaimdation is running with \"" << status["document"].str("name") << "\" open";
    if (status.boolean("dirty")) text << " (unsaved changes)";
    text << ".\n";
    text << "Document: " << status["document"].integer("characters") << " characters, "
         << status["document"].integer("lines") << " lines, " << status["document"].integer("blocks")
         << " blocks, revision " << status["document"].str("revision") << "\n";
    text << "Writes: " << (status["policy"].boolean("allow_write") ? "allowed" : "blocked")
         << ", confirmation: "
         << (status["policy"].boolean("require_confirmation") ? "required" : "off") << "\n";
    text << "Pending confirmations: " << status["pending_changes"].size() << "\n";
    text << "Audit entries: " << status["audit"].integer("entries") << "\n";
    // With tabs open, say which document "the document" is: every tool reads and
    // writes the active tab, and the agent cannot switch it.
    if (status.integer("tab_count", 1) > 1) {
        text << "Open documents: " << status.integer("tab_count") << " (this one is tab "
             << status.integer("active_tab", 0) + 1 << ")\n";
    }
    if (context.session) text << "Your session: " << context.session->id << "\n";
    return text_result_with_structured(text.str(), status);
}

} // namespace

void register_agent_tools(ToolRegistry& registry) {
    registry.add(ToolDescriptor{
        "colaimdation_save_document",
        "Save the document to disk",
        "Write the open document back to its .md file (or to 'path' for Save As) using the file's "
        "original line endings. This is the only tool that touches the filesystem, and it can only "
        "write the document the editor has open - when several tabs are open, that is the active "
        "one.",
        object_schema(
            {
                {"path", string("Optional new path (Save As). Must be inside the allowed directory.")},
                {"force", boolean("Save even when there is nothing to save.", false)},
            },
            {}),
        annotations(false, false, true),
        tool_save_document});

    registry.add(ToolDescriptor{
        "colaimdation_set_selection",
        "Move the caret or selection",
        "Move the writer's caret and selection in the live editor, for example to highlight the "
        "passage you just rewrote or to focus the region you want to work on next. Coordinates are "
        "UTF-8 byte offsets or {line, column} objects.",
        object_schema(
            {
                {"caret", anything("Caret position as a byte offset or {line, column}.")},
                {"anchor", anything("Selection anchor; defaults to the caret (collapsed selection).")},
                {"start", anything("Selection start (alternative to caret+anchor).")},
                {"end", anything("Selection end (alternative to caret+anchor).")},
            },
            {}),
        annotations(false, false, true),
        tool_set_selection});

    registry.add(ToolDescriptor{
        "colaimdation_get_user_changes",
        "See what the writer changed",
        "Return the edits the human writer made in the editor, so you can follow their progress "
        "instead of guessing. Pass the revision you last read as since_revision; pass poll=true to "
        "use your session cursor and advance it atomically.",
        object_schema(
            {
                {"since_revision", string("Revision you last read. Omit to see all writer edits.")},
                {"session", string("Session label used for the poll cursor (defaults to your session).")},
                {"max_changes", integer("Maximum number of changes to return.", 1, 500, true)},
                {"poll", boolean("Use and advance this session's change cursor.", false)},
            },
            {}),
        annotations(true),
        tool_get_user_changes});

    registry.add(ToolDescriptor{
        "colaimdation_get_history",
        "List the edit history",
        "List the recorded edits to this document - who made them (user, agent or system), which "
        "tool, when, and how many bytes changed. Use the sequence numbers with "
        "colaimdation_rollback.",
        object_schema(
            {
                {"max_entries", integer("Maximum number of entries (newest last).", 1, 1000, true)},
                {"include_text", boolean("Include the before/after document text of each entry.",
                                          false)},
            },
            {}),
        annotations(true),
        tool_get_history});

    registry.add(ToolDescriptor{
        "colaimdation_get_audit_log",
        "Read the audit log",
        "Query the durable audit trail: every agent and user edit with its tool, session, status, "
        "byte delta and optional unified diff. This is the same trail the writer can inspect in the "
        "editor's audit panel.",
        object_schema(
            {
                {"limit", integer("Maximum number of entries (newest first).", 1, 500, true)},
                {"tool", string("Only entries made through this tool name.")},
                {"session", string("Only entries from this agent session.")},
                {"source", string_enum("Only entries from this actor.", {"user", "agent", "system",
                                                                         "external"})},
                {"document_id", string("Document id (defaults to the open document).")},
                {"since_ms", integer("Only entries at or after this Unix time in milliseconds.", 0,
                                     100000000000000LL / 1000, true)},
                {"include_diff", boolean("Include the unified diff of each entry.", false)},
            },
            {}),
        annotations(true),
        tool_get_audit_log});

    registry.add(ToolDescriptor{
        "colaimdation_rollback",
        "Undo edits",
        "Revert edits to the document: 'last' reverts the newest edit by the given source (agent by "
        "default), 'to_revision' reverts everything after that revision, and 'to_sequence' reverts "
        "everything after a history sequence number. The rollback is itself audited and reversible.",
        object_schema(
            {
                {"last", boolean("Revert the most recent edit.", true)},
                {"source", string_enum("Whose edit to revert with last=true. Omit or pass \"any\" to "
                                       "revert the newest edit regardless of who made it.",
                                       {"agent", "user", "system", "any"})},
                {"to_revision", string("Revert every edit made after this revision.")},
                {"to_sequence", integer("Revert every edit made after this sequence number.", 0,
                                        10000000, true)},
            },
            {}),
        annotations(false, true),
        tool_rollback});

    registry.add(ToolDescriptor{
        "colaimdation_get_policy",
        "Read the security policy",
        "Report the rules this editor enforces on Agents: whether writes are allowed, whether the "
        "writer must confirm each change in a diff preview, front-matter protection, size limits and "
        "the permitted directory.",
        object_schema({}, {}),
        annotations(true),
        tool_get_policy});

    registry.add(ToolDescriptor{
        "colaimdation_set_policy",
        "Change the security policy",
        "Update the editor's security policy, for example to stop writing while the writer reviews, "
        "or to loosen a size limit. Requires \"confirm\": \"yes\" because it changes the rules the "
        "writer relies on. Only the fields you pass are changed.",
        object_schema(
            {
                {"confirm", string("Must be exactly \"yes\".")},
                {"allow_write", boolean("Whether Agents may modify the document.", true)},
                {"require_confirmation", boolean("Stage every agent write for the writer to confirm.",
                                                  false)},
                {"protect_front_matter", boolean("Refuse edits to the YAML front matter.", false)},
                {"allow_save", boolean("Whether Agents may save the document.", true)},
                {"allow_rollback", boolean("Whether Agents may roll back edits.", true)},
                {"allow_selection_control", boolean("Whether Agents may move the caret.", true)},
                {"allow_multi_patch", boolean("Allow several patches in one apply_patch call.", true)},
                {"max_patch_bytes", integer("Maximum total size of one patch payload.", 0,
                                            64 * 1024 * 1024, true)},
                {"max_patches_per_call", integer("Maximum patches in one call.", 1, 4096, true)},
                {"allowed_directory", string("Only allow saving inside this directory.")},
                {"session_ttl_ms", integer("Idle session lifetime in milliseconds.", 0,
                                           1000000000, true)},
            },
            {"confirm"}),
        annotations(false, true),
        tool_set_policy});

    registry.add(ToolDescriptor{
        "colaimdation_get_workspace_info",
        "Get the editor status",
        "One call that answers \"what am I connected to?\": open file, whether it has unsaved "
        "changes, size, the active security policy, how many agent changes are waiting for the "
        "writer's confirmation, and your session id.",
        object_schema({}, {}),
        annotations(true),
        tool_get_workspace_info});
}

} // namespace colai::mcp
