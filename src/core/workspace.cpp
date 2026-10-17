#include "core/workspace.h"

#include "core/diff.h"
#include "core/markdown.h"
#include "core/text_edit.h"
#include "core/utf8.h"

#include <algorithm>
#include <cstdio>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace colai {
namespace {

bool path_is_within(std::string_view candidate, std::string_view root) {
    if (root.empty()) return true;
    std::string a = utf8::to_lower(candidate);
    std::string b = utf8::to_lower(root);
    // Normalise separators.
    std::replace(a.begin(), a.end(), '/', '\\');
    std::replace(b.begin(), b.end(), '/', '\\');
    while (!b.empty() && b.back() == '\\') b.pop_back();
    if (a.size() < b.size()) return false;
    if (a.compare(0, b.size(), b) != 0) return false;
    return a.size() == b.size() || a[b.size()] == '\\';
}

std::string directory_of(std::string_view path) {
    const size_t slash = path.find_last_of("\\/");
    return slash == std::string_view::npos ? std::string{} : std::string(path.substr(0, slash));
}

// Two tabs must never hold the same file: they would fight over which one saves
// last. Windows paths ignore case and accept both separators.
bool same_path(std::string_view a, std::string_view b) {
    if (a.empty() || b.empty()) return false;
    std::string left = utf8::to_lower(a);
    std::string right = utf8::to_lower(b);
    std::replace(left.begin(), left.end(), '/', '\\');
    std::replace(right.begin(), right.end(), '/', '\\');
    while (left.size() > 1 && left.back() == '\\') left.pop_back();
    while (right.size() > 1 && right.back() == '\\') right.pop_back();
    return left == right;
}

} // namespace

Json PendingChange::to_json() const {
    Json out = Json::object();
    out.set("id", Json(id));
    out.set("tool", Json(tool));
    out.set("session", Json(session));
    out.set("summary", Json(summary));
    out.set("revision_before", Json(revision_before));
    out.set("revision_after", Json(revision_after));
    out.set("document_id", Json(document_id));
    out.set("created_ms", Json(created_ms));
    out.set("created", Json(iso8601(created_ms)));
    out.set("diff", Json(diff));
    Json patch_list = Json::array();
    for (const Patch& patch : patches) {
        Json entry = Json::object();
        entry.set("op", Json(std::string(patch_op_name(patch.op))));
        entry.set("byte_start", Json(static_cast<int64_t>(patch.range_start ? patch.range_start
                                                                           : patch.offset)));
        entry.set("byte_end", Json(static_cast<int64_t>(patch.range_end)));
        if (patch.block_index.has_value()) {
            entry.set("block_index", Json(static_cast<int64_t>(*patch.block_index)));
        }
        entry.set("text", Json(patch.text));
        patch_list.push(std::move(entry));
    }
    out.set("patches", std::move(patch_list));
    return out;
}

// ---------------------------------------------------------------------------
// Workspace
// ---------------------------------------------------------------------------
Workspace::Workspace() { documents_.push_back(make_untitled_document()); }

Workspace::~Workspace() { shutdown(); }

void Workspace::adopt_document_text() {
    last_broadcast_revision_ = document().revision();
}

bool Workspace::initialise(Options options, std::string* error) {
    policy_ = std::move(options.policy);
    state_dir_ = std::move(options.state_dir);
    if (policy_.token.empty()) policy_.token = Policy::generate_token();
    document().set_history_limit(policy_.audit_history_limit, policy_.audit_byte_budget);
    audit_.set_capacity(4000, 32ull * 1024 * 1024);

    if (policy_.audit_enabled && !options.audit_path.empty()) {
        std::string audit_error;
        if (!audit_.open_file(options.audit_path, &audit_error)) {
            // A failed audit log must not stop the editor, but it must be loud.
            if (error) *error = audit_error;
        }
    }

    {
        AuditRecord record;
        record.status = AuditStatus::Info;
        record.tool = "startup";
        record.summary = "Colaimdation started";
        record.source = EditSource::System;
        record.bytes_before = document().size();
        record.bytes_after = document().size();
        audit_record(std::move(record));
    }

    if (!options.initial_file.empty()) {
        std::string open_error;
        Status status = open_file(options.initial_file, &open_error);
        if (!status.ok && error) *error = open_error;
    } else if (options.demo_content) {
        set_demo_content(options.initial_file);
    }
    return true;
}

void Workspace::shutdown() {
    audit_.stop();
    audit_.flush();
    audit_.close_file();
}

Document& Workspace::document() {
    // Every workspace keeps at least one document (see close_tab), so the active
    // one always exists. MCP tools and the shell only ever need "the document the
    // writer is looking at".
    return *documents_[active_];
}

const Document& Workspace::document() const { return *documents_[active_]; }

std::unique_ptr<Document> Workspace::make_untitled_document() {
    auto document = std::make_unique<Document>();
    // Each tab needs an identity of its own: Document derives its id from the path
    // and untitled documents have none, so two new tabs would otherwise share one
    // id and their audit entries and pending changes could not be told apart.
    document->set_id("doc-untitled-" + std::to_string(++untitled_counter_));
    return document;
}

bool Workspace::active_is_placeholder() const {
    const Document& document = *documents_[active_];
    return document.path().empty() && document.text().empty() && !document.dirty() &&
           document.history().empty();
}

void Workspace::new_document() {
    documents_.push_back(make_untitled_document());
    active_ = documents_.size() - 1;
    documents_[active_]->set_history_limit(policy_.audit_history_limit, policy_.audit_byte_budget);
    notify_document_loaded();
    notify_tabs_changed();
}

Status Workspace::activate_tab(size_t index, std::string* error) {
    if (index >= documents_.size()) {
        if (error) *error = "no such tab: " + std::to_string(index + 1);
        return status_error("out_of_range", error ? *error : "no such tab");
    }
    if (index == active_) return status_ok();
    active_ = index;
    // The new document has never been announced to the front-end, so clear the
    // liveness cursor and push it whole (text, caret, dirty flag, blocks).
    notify_document_loaded();
    notify_tabs_changed();
    return status_ok();
}

Status Workspace::close_tab(size_t index, std::string* error) {
    if (index >= documents_.size()) {
        if (error) *error = "no such tab: " + std::to_string(index + 1);
        return status_error("out_of_range", error ? *error : "no such tab");
    }
    documents_.erase(documents_.begin() + static_cast<std::ptrdiff_t>(index));
    if (documents_.empty()) {
        // The editor is never left without a document to type into.
        documents_.push_back(make_untitled_document());
    }
    if (active_ > index) --active_;
    if (active_ >= documents_.size()) active_ = documents_.size() - 1;
    notify_document_loaded();
    notify_tabs_changed();
    return status_ok();
}

Json Workspace::tabs_json() const {
    Json out = Json::object();
    Json tabs = Json::array();
    for (size_t i = 0; i < documents_.size(); ++i) {
        const Document& document = *documents_[i];
        Json entry = Json::object();
        entry.set("index", Json(static_cast<int64_t>(i)));
        entry.set("id", Json(document.id()));
        entry.set("path", Json(document.path()));
        entry.set("name", Json(document.display_name()));
        entry.set("dirty", Json(document.dirty()));
        entry.set("active", Json(i == active_));
        entry.set("bytes", Json(static_cast<int64_t>(document.size())));
        tabs.push(std::move(entry));
    }
    out.set("tabs", std::move(tabs));
    out.set("active", Json(static_cast<int64_t>(active_)));
    out.set("count", Json(static_cast<int64_t>(documents_.size())));
    return out;
}

void Workspace::notify_tabs_changed() {
    if (!bridge_) return;
    Json message = tabs_json();
    message.set("type", Json("tabs"));
    bridge_->notify(message);
}

void Workspace::set_demo_content(std::string fallback_path) {
    // The welcome text is not a file: only a caller that has a real path for it
    // (the tests) may give the document one. Handing the document a path it does
    // not own made Ctrl+S write the literal name "untitled.md" into the working
    // directory instead of opening Save As.
    document().set_path(std::move(fallback_path));
    document().clear_history();
    document().set_text(demo_document(), EditSource::System, "welcome", "welcome document", true);
    document().set_selection(0, 0, EditSource::System);
    // After set_text, which marks the buffer dirty: showing the welcome text is
    // not a change the writer made, so the editor must not open claiming that the
    // "document" has unsaved edits.
    document().set_dirty(false);
    notify_document_loaded();
}

// ---------------------------------------------------------------------------
// File I/O
// ---------------------------------------------------------------------------
Status Workspace::read_text_file(const std::string& path, std::string* text, std::string* error) {
    std::string line_ending;
    return read_text_file(path, text, &line_ending, error);
}

Status Workspace::read_text_file(const std::string& path, std::string* text, std::string* line_ending,
                                 std::string* error) {
    const std::wstring wide = utf8::to_wide(path);
    HANDLE handle = ::CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD code = ::GetLastError();
        if (error) {
            *error = code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND
                         ? "file not found: " + path
                         : "cannot open '" + path + "' (error " + std::to_string(code) + ")";
        }
        return status_error("not_found", error ? *error : "cannot open file");
    }
    LARGE_INTEGER size{};
    ::GetFileSizeEx(handle, &size);
    if (size.QuadPart > 256ll * 1024 * 1024) {
        ::CloseHandle(handle);
        if (error) *error = "refusing to open a file larger than 256 MB";
        return status_error("too_large", error ? *error : "file too large");
    }
    std::string buffer(static_cast<size_t>(size.QuadPart), '\0');
    size_t total = 0;
    while (total < buffer.size()) {
        DWORD read = 0;
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(buffer.size() - total, 1u << 20));
        if (!::ReadFile(handle, buffer.data() + total, chunk, &read, nullptr) || read == 0) break;
        total += read;
    }
    ::CloseHandle(handle);
    buffer.resize(total);

    // Strip a UTF-8 BOM.
    if (buffer.size() >= 3 && static_cast<unsigned char>(buffer[0]) == 0xEF &&
        static_cast<unsigned char>(buffer[1]) == 0xBB &&
        static_cast<unsigned char>(buffer[2]) == 0xBF) {
        buffer.erase(0, 3);
    }

    if (!utf8::is_valid(buffer)) {
        // Likely a legacy ANSI/GBK file: convert through the system code page so
        // the writer sees readable text instead of mojibake.
        const int need = ::MultiByteToWideChar(CP_ACP, 0, buffer.data(),
                                               static_cast<int>(buffer.size()), nullptr, 0);
        if (need > 0) {
            std::wstring wide_text(static_cast<size_t>(need), L'\0');
            ::MultiByteToWideChar(CP_ACP, 0, buffer.data(), static_cast<int>(buffer.size()),
                                  wide_text.data(), need);
            buffer = utf8::from_wide(wide_text);
        }
    }
    // The original line ending must be detected before normalisation, because
    // saving has to reproduce the file the user opened.
    if (line_ending) *line_ending = detect_line_ending(buffer);
    normalize_newlines(buffer);
    *text = std::move(buffer);
    return status_ok();
}

Status Workspace::write_text_file(const std::string& path, std::string_view text,
                                  std::string* error, bool with_utf8_bom) {
    const std::string directory = directory_of(path);
    if (!directory.empty()) {
        ::CreateDirectoryW(utf8::to_wide(directory).c_str(), nullptr);
    }
    const std::wstring wide = utf8::to_wide(path);
    HANDLE handle = ::CreateFileW(wide.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        if (error) {
            *error = "cannot write '" + path + "' (error " + std::to_string(::GetLastError()) + ")";
        }
        return status_error("io_error", error ? *error : "cannot write file");
    }
    // A UTF-8 BOM is optional for the document itself, but files meant to be read
    // by other Windows tooling (PowerShell 5.1, older editors) need one: without
    // it those tools decode the file with the ANSI code page and mangle any
    // non-ASCII path inside it.
    const char bom[3] = {'\xEF', '\xBB', '\xBF'};
    size_t written_total = 0;
    if (with_utf8_bom) {
        DWORD written = 0;
        if (!::WriteFile(handle, bom, 3, &written, nullptr) || written != 3) {
            ::CloseHandle(handle);
            if (error) *error = "write failed (error " + std::to_string(::GetLastError()) + ")";
            return status_error("io_error", error ? *error : "write failed");
        }
    }
    while (written_total < text.size()) {
        DWORD written = 0;
        const DWORD chunk =
            static_cast<DWORD>(std::min<size_t>(text.size() - written_total, 1u << 20));
        if (!::WriteFile(handle, text.data() + written_total, chunk, &written, nullptr)) {
            ::CloseHandle(handle);
            if (error) *error = "write failed (error " + std::to_string(::GetLastError()) + ")";
            return status_error("io_error", error ? *error : "write failed");
        }
        written_total += written;
    }
    ::FlushFileBuffers(handle);
    ::CloseHandle(handle);
    return status_ok();
}

bool Workspace::path_allowed(std::string_view path, std::string* reason) const {
    if (policy_.allowed_directory.empty()) return true;
    if (path_is_within(path, policy_.allowed_directory)) return true;
    if (reason) {
        *reason = "path is outside the permitted directory '" + policy_.allowed_directory + "'";
    }
    return false;
}

Status Workspace::open_file(const std::string& path, std::string* error) {
    if (!path_allowed(path, error)) {
        return status_error("permission_denied", error ? *error : "path not allowed");
    }

    // Already open? Bring its tab forward instead of reading the file a second
    // time; two tabs of one file would fight over which one saves last.
    for (size_t i = 0; i < documents_.size(); ++i) {
        if (same_path(documents_[i]->path(), path)) {
            return activate_tab(i, error);
        }
    }

    std::string text;
    std::string line_ending;
    Status status = read_text_file(path, &text, &line_ending, error);
    if (!status.ok) return status; // a file that did not load must not open a tab

    // The first open fills the empty placeholder document instead of leaving an
    // "untitled" tab next to the file the writer actually asked for.
    if (!active_is_placeholder()) {
        documents_.push_back(make_untitled_document());
        active_ = documents_.size() - 1;
    }
    Document& target = *documents_[active_];
    target.set_history_limit(policy_.audit_history_limit, policy_.audit_byte_budget);
    adopt_file_text(target, path, std::move(text), line_ending, "open", "opened " + path);
    notify_document_loaded();
    notify_tabs_changed();
    return status_ok();
}

void Workspace::adopt_file_text(Document& target, const std::string& path, std::string text,
                                const std::string& line_ending, const std::string& tool,
                                const std::string& summary) {
    target.set_path(path);
    target.set_line_ending(line_ending.empty() ? "\n" : line_ending);
    target.clear_history();
    target.set_text(std::move(text), EditSource::System, tool, summary, true);
    target.set_selection(0, 0, EditSource::System);
    target.set_dirty(false); // loading is not an unsaved change

    AuditRecord record;
    record.status = AuditStatus::Opened;
    record.tool = tool;
    record.source = EditSource::System;
    record.summary = summary;
    record.document_id = target.id();
    record.revision_after = target.revision();
    record.bytes_after = target.size();
    audit_record(std::move(record));
}

Status Workspace::save_file(std::string* saved_path, std::string* error) {
    if (document().path().empty()) {
        if (error) *error = "the document has no path yet; use save_file_as";
        return status_error("invalid_argument", error ? *error : "no path");
    }
    const std::string path = document().path();
    Status status = save_file_as(path, error);
    if (status.ok && saved_path) *saved_path = document().path();
    return status;
}

Status Workspace::save_file_as(const std::string& path, std::string* error) {
    if (!path_allowed(path, error)) {
        return status_error("permission_denied", error ? *error : "path not allowed");
    }
    const std::string payload = apply_line_ending(document().text(), document().line_ending());
    Status status = write_text_file(path, payload, error);
    if (!status.ok) {
        AuditRecord record;
        record.status = AuditStatus::Rejected;
        record.tool = "save";
        record.source = EditSource::System;
        record.summary = "save failed: " + path;
        record.document_id = document().id();
        record.error_code = "io_error";
        record.error_message = error ? *error : "write failed";
        audit_record(std::move(record));
        return status;
    }
    document().set_path(path);
    document().set_dirty(false);
    {
        AuditRecord record;
        record.status = AuditStatus::Saved;
        record.tool = "save";
        record.source = EditSource::System;
        record.summary = "saved " + path;
        record.document_id = document().id();
        record.revision_after = document().revision();
        record.bytes_after = payload.size();
        audit_record(std::move(record));
    }
    if (bridge_) {
        Json message = Json::object();
        message.set("type", "document_saved");
        message.set("path", Json(path));
        message.set("name", Json(document().display_name()));
        message.set("revision", Json(document().revision()));
        // The document is clean again after a successful write; saying so lets the
        // page clear its own dirty marker instead of guessing.
        message.set("dirty", Json(false));
        message.set("bytes", Json(static_cast<int64_t>(payload.size())));
        bridge_->notify(message);
        // A first save gives an untitled tab its file name, so the tab bar has to
        // be refreshed as well.
        notify_tabs_changed();
    }
    return status_ok();
}

Status Workspace::reload_from_disk(std::string* error) {
    const std::string path = document().path();
    if (path.empty()) {
        if (error) *error = "the document has no path";
        return status_error("invalid_argument", error ? *error : "no path");
    }
    if (!path_allowed(path, error)) {
        return status_error("permission_denied", error ? *error : "path not allowed");
    }
    // Read straight into the active document: going through open_file() would find
    // this path already open (its own tab) and merely activate it.
    std::string text;
    std::string line_ending;
    Status status = read_text_file(path, &text, &line_ending, error);
    if (!status.ok) return status;
    adopt_file_text(document(), path, std::move(text), line_ending, "reload",
                    "reloaded " + path);
    notify_document_loaded();
    notify_tabs_changed();
    return status_ok();
}

// ---------------------------------------------------------------------------
// Audit helpers
// ---------------------------------------------------------------------------
void Workspace::audit_record(AuditRecord record) {
    if (!policy_.audit_enabled) return;
    // Every entry belongs to the document that was open when it happened, which
    // is what makes a per-document audit query meaningful.
    if (record.document_id.empty()) record.document_id = document().id();
    if (record.timestamp_ms == 0) record.timestamp_ms = now_ms();
    audit_.record(std::move(record));
}

// ---------------------------------------------------------------------------
// Notifications to the editor front-end
// ---------------------------------------------------------------------------
void Workspace::notify_document_changed(const std::string& reason, const std::string& tool,
                                        const std::string& session, const PatchOutcome* outcome) {
    // The revision acts as a bridge-liveness gate: an empty value means the
    // front-end has not been told about the current document yet, so the next
    // change must be pushed even if the revision is unchanged.
    if (bridge_ && !last_broadcast_revision_.empty()) {
        Json message = Json::object();
        message.set("type", "document_changed");
        message.set("reason", Json(reason));
        message.set("tool", Json(tool));
        if (!session.empty()) message.set("session", Json(session));
        message.set("revision", Json(document().revision()));
        message.set("sequence", Json(static_cast<int64_t>(document().sequence())));
        message.set("dirty", Json(document().dirty()));
        message.set("bytes", Json(static_cast<int64_t>(document().size())));
        message.set("characters", Json(static_cast<int64_t>(document().character_count())));
        message.set("lines", Json(static_cast<int64_t>(document().line_count())));
        message.set("words", Json(static_cast<int64_t>(document().word_count())));
        message.set("blocks", Json(static_cast<int64_t>(document().block_count())));
        message.set("caret", Json(static_cast<int64_t>(document().caret())));
        message.set("pending", Json(pending_json()));
        if (outcome && outcome->ok) {
            Json changes = Json::array();
            for (const TextChange& change : outcome->changes) {
                Json entry = Json::object();
                entry.set("byte_start", Json(static_cast<int64_t>(change.begin)));
                entry.set("byte_end", Json(static_cast<int64_t>(change.end)));
                entry.set("inserted", Json(change.inserted));
                changes.push(std::move(entry));
            }
            message.set("changes", std::move(changes));
            message.set("content", Json(document().text()));
        } else {
            // The front-end re-reads the document when it cannot patch locally.
            message.set("content", Json(document().text()));
        }
        // Generation counter so the front-end can ignore stale notifications.
        Json marker = Json::object();
        marker.set("source", Json("host"));
        message.set("_host", std::move(marker));
        last_broadcast_revision_ = document().revision();
        last_broadcast_reason_ = reason;
        bridge_->notify(message);
    }
}

void Workspace::notify_document_loaded() {
    last_broadcast_revision_.clear();
    if (!bridge_) return;
    // reload_document() already carries the document text, its block structure and
    // the outline; sending a second copy of the same payload doubled the cost of
    // every open and tab switch.
    bridge_->reload_document(document());
    last_broadcast_revision_ = document().revision();
}

// ---------------------------------------------------------------------------
// Editor -> workspace
// ---------------------------------------------------------------------------
Json Workspace::apply_user_edit(const EditReport& report) {
    std::string incoming = report.text;
    normalize_newlines(incoming);

    const bool revision_matches = report.revision_before.empty() ||
                                  report.revision_before == document().revision();
    const std::string before = document().text();
    Selection previous = document().selection();

    Json response = Json::object();
    if (incoming == before) {
        document().set_selection(report.selection_anchor, report.selection_head, EditSource::User,
                                "editor");
        response.set("status", Json("unchanged"));
        response.set("revision", Json(document().revision()));
        return response;
    }

    PatchOutcome outcome = document().set_text(
        incoming, EditSource::User, "editor",
        revision_matches ? "user edit" : "user edit (resynchronised with editor)");

    document().set_selection(report.selection_anchor, report.selection_head, EditSource::User,
                            "editor");

    const Edit* edit = document().last_edit();
    if (edit) {
        AuditRecord record;
        record.status = AuditStatus::Applied;
        record.source = EditSource::User;
        record.tool = "editor";
        record.summary = revision_matches ? "user edited the document" : "user edit after resync";
        record.document_sequence = edit->sequence;
        record.revision_before = edit->revision_before;
        record.revision_after = edit->revision_after;
        record.changes = edit->changes;
        record.bytes_before = before.size();
        record.bytes_after = document().size();
        record.reversible = true;
        record.diff = unified_diff(before, document().text(), "before", "after", 2);
        audit_record(std::move(record));
    }

    response.set("status", Json(revision_matches ? "applied" : "resynced"));
    response.set("revision", Json(document().revision()));
    response.set("sequence", Json(static_cast<int64_t>(document().sequence())));
    response.set("bytes", Json(static_cast<int64_t>(document().size())));
    response.set("caret", Json(static_cast<int64_t>(document().caret())));
    response.set("dirty", Json(true));
    response.set("reload", Json(false));
    (void)outcome;
    (void)previous;

    // Let connected agents know the human is writing, without echoing the whole
    // document back to the editor that just typed it.
    if (bridge_ && !last_broadcast_revision_.empty()) {
        Json message = Json::object();
        message.set("type", "user_activity");
        message.set("revision", Json(document().revision()));
        message.set("sequence", Json(static_cast<int64_t>(document().sequence())));
        message.set("dirty", Json(true));
        message.set("bytes", Json(static_cast<int64_t>(document().size())));
        message.set("characters", Json(static_cast<int64_t>(document().character_count())));
        message.set("lines", Json(static_cast<int64_t>(document().line_count())));
        message.set("words", Json(static_cast<int64_t>(document().word_count())));
        bridge_->notify(message);
        last_broadcast_revision_ = document().revision();
    }
    return response;
}

Json Workspace::update_selection(size_t anchor, size_t head) {
    document().set_selection(anchor, head, EditSource::User, "editor");
    Json response = Json::object();
    response.set("status", Json("ok"));
    response.set("selection", document().selection_json());
    return response;
}

// ---------------------------------------------------------------------------
// Agent writes
// ---------------------------------------------------------------------------
bool Workspace::front_matter_touched(const std::vector<Patch>& patches, std::string* detail) const {
    if (!policy_.protect_front_matter) return false;
    const md::ParsedDocument& structure = document().parsed();
    for (const Patch& patch : patches) {
        std::optional<size_t> index = document().resolve_block(patch, nullptr);
        if (index && structure.blocks[*index].kind == md::BlockKind::FrontMatter) {
            if (detail) *detail = "block b" + std::to_string(*index) + " is the front matter";
            return true;
        }
        // Range edits that intersect the front matter also count.
        size_t begin = patch.op == PatchOp::InsertText ? patch.offset : patch.range_start;
        size_t end = patch.op == PatchOp::InsertText ? patch.offset : patch.range_end;
        for (const md::Block& block : structure.blocks) {
            if (block.kind != md::BlockKind::FrontMatter) continue;
            if (begin < block.end && end > block.begin) {
                if (detail) *detail = "the edit intersects the front matter";
                return true;
            }
        }
    }
    return false;
}

std::string Workspace::stage_change(const std::vector<Patch>& patches, const std::string& tool,
                                    const std::string& session, const std::string& summary) {
    PendingChange change;
    change.id = "pending-" + std::to_string(++pending_counter_);
    change.tool = tool;
    change.session = session;
    change.summary = summary;
    change.document_id = document().id();
    change.revision_before = document().revision();
    change.created_ms = now_ms();
    change.patches = patches;

    // Preview the result without mutating the real document.
    Document preview;
    preview.set_text(document().text(), EditSource::System, "preview", "preview", true);
    preview.set_selection(document().selection().anchor, document().selection().head);
    preview.set_history_limit(2, 1024 * 1024);
    PatchOutcome outcome = preview.apply_patches(patches, EditSource::Agent, tool, summary, session);
    if (outcome.ok) {
        change.revision_after = preview.revision();
        change.diff = unified_diff(document().text(), preview.text(), "current",
                                   "proposed by " + (session.empty() ? std::string("agent") : session));
    } else {
        change.revision_after = document().revision();
        change.diff = "// preview failed: " + outcome.error_message + "\n";
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.push_back(change);
        while (pending_.size() > 8) pending_.erase(pending_.begin());
    }
    return change.id;
}

Workspace::WriteOutcome Workspace::commit_patches(const std::vector<Patch>& patches,
                                                 const std::string& tool, const std::string& session,
                                                 const std::string& summary) {
    WriteOutcome result;
    const std::string before = document().text();
    const std::string revision_before = document().revision();

    PatchOutcome outcome =
        document().apply_patches(patches, EditSource::Agent, tool, summary, session);
    result.revision_before = revision_before;
    result.revision_after = document().revision();

    if (!outcome.ok) {
        result.error_code = outcome.error_code.empty() ? "invalid_argument" : outcome.error_code;
        result.error_message = outcome.error_message;
        AuditRecord record;
        record.status = AuditStatus::Rejected;
        record.source = EditSource::Agent;
        record.tool = tool;
        record.session = session;
        record.summary = summary;
        record.error_code = result.error_code;
        record.error_message = result.error_message;
        record.bytes_before = before.size();
        record.bytes_after = before.size();
        record.revision_before = revision_before;
        record.revision_after = revision_before;
        record.document_id = document().id();
        result.audit_id = audit_.record(std::move(record));
        return result;
    }

    result.ok = true;
    result.applied = outcome.applied_count;
    result.diff = unified_diff(before, document().text(), "before", "after", 2);

    AuditRecord record;
    record.status = AuditStatus::Applied;
    record.source = EditSource::Agent;
    record.tool = tool;
    record.session = session;
    record.summary = summary;
    record.document_sequence = outcome.sequence;
    record.revision_before = revision_before;
    record.revision_after = document().revision();
    record.changes = outcome.changes;
    record.bytes_before = before.size();
    record.bytes_after = document().size();
    record.reversible = true;
    record.diff = result.diff;
    record.document_id = document().id();
    result.audit_id = audit_.record(std::move(record));

    notify_document_changed("agent_patch", tool, session, &outcome);
    return result;
}

Workspace::WriteOutcome Workspace::apply_agent_patches(const std::vector<Patch>& patches,
                                                       const std::string& tool,
                                                       const std::string& session,
                                                       const std::string& summary,
                                                       bool bypass_confirmation) {
    WriteOutcome result;
    if (!policy_.allow_write) {
        result.error_code = "permission_denied";
        result.error_message = "writing is disabled by the editor policy (allow_write=false)";
        return result;
    }
    if (patches.empty()) {
        result.ok = true;
        result.revision_before = document().revision();
        result.revision_after = document().revision();
        return result;
    }
    if (!policy_.allow_multi_patch && patches.size() > 1) {
        result.error_code = "permission_denied";
        result.error_message = "multi-patch calls are disabled by the editor policy";
        return result;
    }
    if (patches.size() > policy_.max_patches_per_call) {
        result.error_code = "invalid_argument";
        result.error_message = "too many patches (limit " +
                               std::to_string(policy_.max_patches_per_call) + ")";
        return result;
    }
    size_t payload = 0;
    for (const Patch& patch : patches) payload += patch.text.size();
    if (payload > policy_.max_patch_bytes) {
        result.error_code = "invalid_argument";
        result.error_message = "patch payload exceeds max_patch_bytes (" +
                               std::to_string(policy_.max_patch_bytes) + ")";
        return result;
    }

    std::string front_matter_detail;
    if (front_matter_touched(patches, &front_matter_detail)) {
        result.error_code = "permission_denied";
        result.error_message = "the front matter is protected: " + front_matter_detail;
        return result;
    }

    // Line-range restrictions from the policy.
    if (!policy_.allowed_line_ranges.empty()) {
        for (const Patch& patch : patches) {
            size_t begin = patch.op == PatchOp::InsertText ? patch.offset : patch.range_start;
            size_t end = patch.op == PatchOp::InsertText ? patch.offset : patch.range_end;
            const size_t line_begin = utf8::line_number_at(document().text(), begin);
            const size_t line_end = utf8::line_number_at(document().text(), end);
            bool allowed = false;
            for (const auto& range : policy_.allowed_line_ranges) {
                if (line_begin >= range.first && line_end <= range.second) {
                    allowed = true;
                    break;
                }
            }
            if (!allowed) {
                result.error_code = "permission_denied";
                result.error_message = "lines " + std::to_string(line_begin) + "-" +
                                       std::to_string(line_end) +
                                       " are outside the permitted line ranges";
                return result;
            }
        }
    }

    const bool needs_confirmation =
        policy_.require_confirmation && !(bypass_confirmation && policy_.allow_confirmation_bypass);

    if (needs_confirmation && bridge_) {
        const std::string id = stage_change(patches, tool, session, summary);
        result.staged = true;
        result.pending_id = id;
        result.error_code.clear();
        result.error_message.clear();

        Json pending_message;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_message = pending_.back().to_json();
        }
        AuditRecord preview;
        preview.status = AuditStatus::Previewed;
        preview.source = EditSource::Agent;
        preview.tool = tool;
        preview.session = session;
        preview.summary = summary + " (awaiting confirmation)";
        preview.revision_before = document().revision();
        preview.revision_after = document().revision();
        preview.bytes_before = document().size();
        preview.bytes_after = document().size();
        preview.diff = pending_message.str("diff");
        result.audit_id = audit_.record(std::move(preview));

        const bool accepted =
            bridge_->request_confirmation(pending_message, policy_.confirmation_timeout_ms);

        PendingChange change;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto it = pending_.begin(); it != pending_.end(); ++it) {
                if (it->id == id) {
                    change = *it;
                    pending_.erase(it);
                    break;
                }
            }
        }

        if (!accepted) {
            result.ok = false;
            result.error_code = "rejected_by_user";
            result.error_message = "the user rejected this change in the diff preview";
            AuditRecord rejected;
            rejected.status = AuditStatus::Rejected;
            rejected.source = EditSource::Agent;
            rejected.tool = tool;
            rejected.session = session;
            rejected.summary = summary + " (rejected by user)";
            rejected.error_code = result.error_code;
            rejected.error_message = result.error_message;
            rejected.revision_before = document().revision();
            rejected.revision_after = document().revision();
            audit_.record(std::move(rejected));
            return result;
        }

        // The writer can switch tabs while the diff panel waits for an answer. The
        // previewed patches were computed against another document, so applying
        // them now would corrupt whichever document became active.
        if (!change.document_id.empty() && change.document_id != document().id()) {
            result.ok = false;
            result.staged = false;
            result.error_code = "conflict";
            result.error_message =
                "the editor switched to another document while this change was waiting for "
                "confirmation; nothing was applied";
            AuditRecord moved;
            moved.status = AuditStatus::Rejected;
            moved.source = EditSource::Agent;
            moved.tool = tool;
            moved.session = session;
            moved.summary = summary + " (document changed while waiting)";
            moved.error_code = result.error_code;
            moved.error_message = result.error_message;
            moved.revision_before = document().revision();
            moved.revision_after = document().revision();
            audit_.record(std::move(moved));
            return result;
        }
    }

    const bool was_staged = result.staged;
    const std::string staged_id = result.pending_id;
    result = commit_patches(patches, tool, session, summary);
    result.staged = was_staged;
    result.pending_id = was_staged ? staged_id : std::string();
    return result;
}

Status Workspace::set_caret(size_t anchor, size_t head, const std::string& session,
                            std::string* error) {
    (void)session;
    if (!policy_.allow_selection_control) {
        if (error) *error = "selection control is disabled by the editor policy";
        return status_error("permission_denied", error ? *error : "not allowed");
    }
    if (anchor > document().size() || head > document().size()) {
        if (error) {
            *error = "selection offset is past the end of the document (" +
                     std::to_string(document().size()) + " bytes)";
        }
        return status_error("out_of_range", error ? *error : "offset out of range");
    }
    document().set_selection(anchor, head, EditSource::Agent, "set_selection");
    if (bridge_) {
        Json message = Json::object();
        message.set("type", "selection_set");
        message.set("anchor", Json(static_cast<int64_t>(document().selection().anchor)));
        message.set("head", Json(static_cast<int64_t>(document().selection().head)));
        bridge_->notify(message);
    }
    return status_ok();
}

// ---------------------------------------------------------------------------
// Confirmation
// ---------------------------------------------------------------------------
std::optional<PendingChange> Workspace::pending(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const PendingChange& change : pending_) {
        if (id.empty() || change.id == id) return change;
    }
    return std::nullopt;
}

Json Workspace::pending_json() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Json out = Json::array();
    for (const PendingChange& change : pending_) out.push(change.to_json());
    return out;
}

Json Workspace::resolve_pending(const std::string& id, bool accept, const std::string& reason) {
    Json response = Json::object();
    PendingChange change;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = pending_.begin(); it != pending_.end(); ++it) {
            if (it->id == id) {
                change = *it;
                found = true;
                break;
            }
        }
        confirmation_.id = id;
        confirmation_.accepted = accept;
        confirmation_.reason = reason;
        confirmation_.decided = true;
    }
    confirmation_cv_.notify_all();

    if (!found) {
        response.set("status", Json("unknown"));
        response.set("error", Json("no pending change with id '" + id + "'"));
        return response;
    }

    if (!accept) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = pending_.begin(); it != pending_.end(); ++it) {
            if (it->id == id) {
                pending_.erase(it);
                break;
            }
        }
        response.set("status", Json("rejected"));
        response.set("id", Json(id));
        return response;
    }

    // Applied by the waiting tool thread after the bridge returns; the front-end
    // only reports the decision here.
    response.set("status", Json("accepted"));
    response.set("id", Json(id));
    return response;
}

// ---------------------------------------------------------------------------
// History / rollback
// ---------------------------------------------------------------------------
Json Workspace::history_json(size_t limit, bool with_text) const {
    Json out = Json::object();
    out.set("revision", Json(document().revision()));
    out.set("edits", document().history_json(limit, with_text));
    out.set("can_undo", Json(document().can_undo()));
    out.set("can_redo", Json(document().can_redo()));
    return out;
}

Json Workspace::rollback_last(std::optional<EditSource> source, const std::string& tool) {
    Json response = Json::object();
    if (!policy_.allow_rollback) {
        response.set("status", Json("error"));
        response.set("error", Json("rollback is disabled by the editor policy"));
        return response;
    }
    if (source.has_value() && *source == EditSource::Agent && !policy_.allow_agent_rollback) {
        response.set("status", Json("error"));
        response.set("error", Json("agent-initiated rollback is disabled by the editor policy"));
        return response;
    }
    RollbackPlan plan = document().plan_rollback_last(source);
    if (!plan.ok) {
        response.set("status", Json("error"));
        response.set("error", Json(plan.error));
        return response;
    }
    PatchOutcome outcome = document().apply_rollback(plan, EditSource::Agent,
                                                    tool.empty() ? "rollback" : tool);
    if (!outcome.ok) {
        response.set("status", Json("error"));
        response.set("error", Json(outcome.error_message));
        return response;
    }
    AuditRecord record;
    record.status = AuditStatus::RolledBack;
    record.source = EditSource::Agent;
    record.tool = tool.empty() ? "rollback" : tool;
    record.summary = "rolled back to " + plan.revision.substr(0, 12);
    record.revision_before = plan.revision;
    record.revision_after = document().revision();
    record.changes = outcome.changes;
    record.bytes_before = plan.text.size();
    record.bytes_after = document().size();
    record.reversible = true;
    record.diff = plan.diff;
    audit_.record(std::move(record));

    notify_document_changed("rollback", tool, {}, &outcome);
    response.set("status", Json("applied"));
    response.set("revision", Json(document().revision()));
    response.set("sequence", Json(static_cast<int64_t>(document().sequence())));
    response.set("reverted_edits", Json(static_cast<int64_t>(plan.edits_to_revert)));
    response.set("bytes", Json(static_cast<int64_t>(document().size())));
    return response;
}

Json Workspace::rollback_to_revision(const std::string& revision, const std::string& tool) {
    Json response = Json::object();
    // Resolve the revision to the edit sequence that produced it.
    uint64_t sequence = 0;
    bool found = false;
    for (const Edit& edit : document().history()) {
        // Rolling back "to" a revision means reverting every edit that came
        // after the one that produced it; an edit's sequence is the state it
        // started from, so that value is the cut-off.
        if (edit.revision_after == revision || edit.revision_before == revision) {
            sequence = edit.sequence;
            found = true;
            break;
        }
    }
    if (!found) {
        response.set("status", Json("error"));
        response.set("error", Json("unknown revision '" + revision + "'"));
        return response;
    }
    RollbackPlan plan = document().plan_rollback_to_sequence(sequence);
    if (!plan.ok) {
        response.set("status", Json("error"));
        response.set("error", Json(plan.error));
        return response;
    }
    PatchOutcome outcome =
        document().apply_rollback(plan, EditSource::Agent, tool.empty() ? "rollback" : tool);
    if (!outcome.ok) {
        response.set("status", Json("error"));
        response.set("error", Json(outcome.error_message));
        return response;
    }
    AuditRecord record;
    record.status = AuditStatus::RolledBack;
    record.source = EditSource::Agent;
    record.tool = tool.empty() ? "rollback" : tool;
    record.summary = "rolled back to revision " + revision.substr(0, 12);
    record.revision_before = revision;
    record.revision_after = document().revision();
    record.changes = outcome.changes;
    record.bytes_before = plan.text.size();
    record.bytes_after = document().size();
    record.reversible = true;
    record.diff = plan.diff;
    audit_.record(std::move(record));

    notify_document_changed("rollback", tool, {}, &outcome);
    response.set("status", Json("applied"));
    response.set("revision", Json(document().revision()));
    response.set("reverted_edits", Json(static_cast<int64_t>(plan.edits_to_revert)));
    return response;
}

// ---------------------------------------------------------------------------
// Change notification
// ---------------------------------------------------------------------------
Json Workspace::user_changes_since(const std::string& revision, size_t limit) const {
    Json out = Json::object();
    Json edits = Json::array();
    // `revision` is the state the caller last read, so an edit whose *result* is
    // that revision is already known; everything after it is news.
    for (const Edit& edit : document().history()) {
        if (edit.source != EditSource::User) continue;
        if (!revision.empty() && edit.revision_after == revision) {
            edits = Json::array();
            continue;
        }
        edits.push(edit.to_json(false));
        if (limit > 0 && edits.size() >= limit) break;
    }
    // Count before the array is moved into the result object.
    const size_t change_count = edits.size();
    out.set("revision", Json(document().revision()));
    out.set("since", Json(revision));
    out.set("changes", std::move(edits));
    out.set("change_count", Json(static_cast<int64_t>(change_count)));
    return out;
}

Json Workspace::poll_user_changes(const std::string& session, const std::string& since_revision,
                                  size_t limit) {
    std::string cursor = since_revision;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& entry : sessions_) {
            if (entry.first == session) {
                if (cursor.empty()) cursor = entry.second.revision;
                entry.second.last_seen_ms = now_ms();
                entry.second.revision = document().revision();
                break;
            }
        }
        bool found = false;
        for (const auto& entry : sessions_) {
            if (entry.first == session) found = true;
        }
        if (!found) {
            sessions_.emplace_back(session, SessionState{document().revision(), now_ms()});
        }
    }
    Json result = user_changes_since(cursor, limit);
    result.set("session", Json(session));
    result.set("cursor_revision", Json(cursor));
    return result;
}

// ---------------------------------------------------------------------------
// Policy / status
// ---------------------------------------------------------------------------
Json Workspace::policy_json() const {
    Json out = policy_.to_json();
    out.set("effective_document", Json(document().id()));
    out.set("open_path", Json(document().path()));
    return out;
}

Json Workspace::set_policy(const Json& patch) {
    std::vector<std::string> warnings;
    policy_.apply_json(patch, &warnings);
    if (policy_.token.empty()) policy_.token = Policy::generate_token();
    document().set_history_limit(policy_.audit_history_limit, policy_.audit_byte_budget);
    audit_.set_capacity(std::max<size_t>(64, policy_.audit_history_limit * 4), 32ull * 1024 * 1024);

    AuditRecord record;
    record.status = AuditStatus::Info;
    record.source = EditSource::User;
    record.tool = "set_policy";
    record.summary = "security policy updated";
    record.diff = patch.dump(true);
    audit_.record(std::move(record));

    Json out = policy_json();
    if (!warnings.empty()) {
        Json list = Json::array();
        for (const std::string& warning : warnings) list.push(Json(warning));
        out.set("warnings", std::move(list));
    }
    return out;
}

Json Workspace::audit_json(const AuditQuery& query) const {
    Json out = Json::object();
    Json entries = Json::array();
    for (const AuditRecord& record : audit_.query(query)) {
        entries.push(record.to_json(query.with_diff));
    }
    // Capture the count before the array is moved out.
    const size_t count = entries.size();
    out.set("entries", std::move(entries));
    out.set("count", Json(static_cast<int64_t>(count)));
    out.set("total_recorded", Json(static_cast<int64_t>(audit_.size())));
    out.set("log_path", Json(audit_.file_path()));
    return out;
}

Json Workspace::status_json() const {
    Json out = Json::object();
    out.set("document", document().to_json(false));
    out.set("selection", document().selection_json());
    out.set("pending_changes", pending_json());
    out.set("policy", policy_.to_json());
    // The tab list travels with every status reply so a platform that misses the
    // push notification still ends up showing the right tabs.
    Json tabs = tabs_json();
    out.set("tabs", tabs["tabs"]);
    out.set("active_tab", tabs["active"]);
    out.set("tab_count", tabs["count"]);
    out.set("audit", [&] {
        Json audit = Json::object();
        audit.set("entries", Json(static_cast<int64_t>(audit_.size())));
        audit.set("file", Json(audit_.file_path()));
        return audit;
    }());
    out.set("bridge_ready", Json(bridge_ != nullptr && bridge_->has_ui()));
    return out;
}

// ---------------------------------------------------------------------------
// First-run document
// ---------------------------------------------------------------------------
std::string demo_document() {
    return R"(---
title: Colaimdation
subtitle: 本地原地实时渲染 Markdown 编辑器 · 内置 MCP 服务
---

# Colaimdation

一款**本地** Markdown 编辑器：光标所在行显示源码，其余内容实时渲染。
它内置了一个 **MCP Server**，任意支持 MCP 的 Agent 都能接入，
在**编辑器内存中的文档**上和你协同写作 —— 而不是绕过编辑器直接读写磁盘文件。

## 快速开始

1. 在下方任意位置输入文字，当前行会显示 Markdown 源码，其余行立即渲染。
2. 把 Agent 指向编辑器的 MCP 端点（启动时会写入 `colaimdation.mcp.json`）。
3. Agent 调用 `colaimdation_read_document` 读取全文，调用
   `colaimdation_apply_patch` 做增量修改，你的窗口会实时刷新。

> **权限隔离**：Agent 只能操作编辑器当前打开的这一篇文档，
> 无法访问本机其他文件。所有改动都记录在审计日志中，可以一键回滚。

## 内联样式

| 能力 | 语法 | 说明 |
|:-----|:-----|:-----|
| 强调 | `**粗体**` | 渲染为 **粗体** |
| 代码 | `` `code` `` | 行内等宽字体 |
| 公式 | `$E = mc^2$` | 行内公式 $E = mc^2$ |
| 链接 | `[文字](url)` | [Markdown 规范](https://commonmark.org) |

行内公式示例：质能方程 $E = mc^2$，质能关系的完整形式 $E^2 = (pc)^2 + (m_0c^2)^2$。

## 代码块

```cpp
// Agent 通过 MCP 调用工具，编辑器实时反映改动。
Patch patch;
patch.op = PatchOp::ReplaceBlock;
patch.block_id = "b7";
patch.text = "新的段落内容";
document.apply_patch(patch, EditSource::Agent, "replace_block");
```

```python
# 也可以直接用 HTTP 调 MCP 端点
import json, urllib.request
print(json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/list"}))
```

## 数学公式

块级公式：

$$
\int_{-\infty}^{\infty} e^{-x^2}\,dx = \sqrt{\pi}
$$

矩阵：

$$
\begin{pmatrix} a & b \\ c & d \end{pmatrix}
\begin{pmatrix} x \\ y \end{pmatrix}
=
\begin{pmatrix} ax + by \\ cx + dy \end{pmatrix}
$$

## 流程图

```mermaid
flowchart LR
    A[你手动编辑] --> B{编辑器内存文档}
    C[外部 Agent] -->|MCP 工具调用| B
    B --> D[实时渲染刷新]
    B --> E[审计日志 / 回滚]
    B --> F[保存到 .md 文件]
```

## 时序图

```mermaid
sequenceDiagram
    participant You as 你
    participant Ed as Colaimdation
    participant Ag as Agent
    Ag->>Ed: tools/call apply_patch
    Ed->>Ed: 校验偏移量 / 策略 / 权限
    Ed-->>You: 实时渲染更新
    Ed-->>Ag: 返回新 revision
    You->>Ed: 手动继续修改
    Ed-->>Ag: notifications/user_activity
```

## 双向感知

- **Agent → 你**：Agent 的改动会立刻在这里渲染出来，并标注来源。
- **你 → Agent**：你手动编辑后，Agent 可以通过
  `colaimdation_get_user_changes` 感知你的写作进度。

## 任务清单

- [x] 打开 / 保存本地 `.md` 文件
- [x] 光标行源码 + 其余实时渲染
- [x] 内置 MCP Server（HTTP + stdio）
- [x] 增量 Patch 修改文档
- [x] Diff 预览确认
- [x] 审计日志与一键回滚
- [ ] 你的下一个想法

---

*提示：按 `Ctrl+S` 保存，`Ctrl+Z` / `Ctrl+Y` 撤销重做，`Ctrl+/` 切换纯源码模式。*
)";
}

} // namespace colai
