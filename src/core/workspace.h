// Colaimdation - workspace.
//
// The workspace is the single writer of the open documents and the only place
// where MCP tool calls turn into document mutations. It owns:
//
//   * the documents the editor has open (one tab each). Exactly one of them is
//     *active*, and every read/write/MCP tool talks to that one - permission
//     isolation still means "the document the writer is looking at", with no API
//     for a tool to open or switch documents,
//   * the security policy and the audit log,
//   * the staging area for human-in-the-loop confirmation,
//   * the bridge to the editor UI (caret/selection push, patch application,
//     diff preview, save, notifications).
//
// Threading: every mutation runs on the UI thread through IEditorBridge.
// Tool calls arrive on an HTTP/stdio thread and post a job, then wait.
#pragma once

#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "core/audit.h"
#include "core/document.h"
#include "core/json.h"
#include "core/policy.h"

namespace colai {

// ---------------------------------------------------------------------------
// Editor bridge: implemented by the Win32/WebView2 layer, faked in tests.
// ---------------------------------------------------------------------------
class IEditorBridge {
public:
    virtual ~IEditorBridge() = default;

    // Runs `job` on the UI thread and returns its result. Returns nullopt when
    // the UI is gone or the job did not finish within `timeout_ms`.
    virtual std::optional<Json> post(std::function<Json()> job, int timeout_ms = 15000) = 0;
    // Thread-safe push of a notification/command into the editor front-end.
    virtual void notify(const Json& message) = 0;
    // Asks the user to accept/reject a staged change (diff preview panel).
    // Returns the decision; called from the waiting tool thread.
    virtual bool request_confirmation(const Json& pending, int64_t timeout_ms) = 0;
    // Reloads the front-end from the document text (used after open/rollback).
    virtual void reload_document(const Document& document) = 0;
    virtual bool has_ui() const = 0;
};

// ---------------------------------------------------------------------------
// Pending (staged) change
// ---------------------------------------------------------------------------
struct PendingChange {
    std::string id;
    std::string tool;
    std::string session;
    std::string summary;
    std::string revision_before;
    std::string revision_after;
    // The document this change was previewed against. With several tabs open the
    // writer can switch away while the diff panel is waiting for an answer, and
    // the change must not land in whatever document happens to be active then.
    std::string document_id;
    std::string diff;
    int64_t created_ms = 0;
    std::vector<Patch> patches;

    Json to_json() const;
};

// ---------------------------------------------------------------------------
// Workspace
// ---------------------------------------------------------------------------
class Workspace {
public:
    struct Options {
        Policy policy;
        std::string audit_path;
        std::string state_dir;
        std::string initial_file;
        bool demo_content = false;
    };

    Workspace();
    ~Workspace();

    Workspace(const Workspace&) = delete;
    Workspace& operator=(const Workspace&) = delete;

    // --- lifetime -----------------------------------------------------------
    bool initialise(Options options, std::string* error = nullptr);
    void shutdown();

    IEditorBridge* bridge() const { return bridge_; }
    void set_bridge(IEditorBridge* bridge) { bridge_ = bridge; }

    Document& document();
    const Document& document() const;
    Policy& policy() { return policy_; }
    const Policy& policy() const { return policy_; }
    AuditLog& audit() { return audit_; }

    // --- files --------------------------------------------------------------
    // Reads a Markdown file into a document: it becomes a new tab (or the current
    // tab is reused when it is an untouched empty one) and is activated. Opening
    // a file that is already open just activates its tab.
    Status open_file(const std::string& path, std::string* error = nullptr);
    // Writes the active document back to its file with the original line endings.
    Status save_file(std::string* saved_path = nullptr, std::string* error = nullptr);
    // Saves to a new path (Save As) and adopts it.
    Status save_file_as(const std::string& path, std::string* error = nullptr);
    Status reload_from_disk(std::string* error = nullptr);

    // --- open documents (tabs) ---------------------------------------------
    // Adds an empty, untitled tab and activates it.
    void new_document();
    size_t tab_count() const { return documents_.size(); }
    size_t active_tab() const { return active_; }
    // Random access to a tab's document, for the shell (dirty checks, tab bar).
    // Tools keep using document(): they only ever see the active one.
    Document& document_at(size_t index) { return *documents_[index]; }
    const Document& document_at(size_t index) const { return *documents_[index]; }
    // Activates a tab. Offsets are NOT converted: each document keeps its own
    // caret, selection and history.
    Status activate_tab(size_t index, std::string* error = nullptr);
    // Removes a tab. Closing the last one leaves a fresh empty document behind, so
    // the editor always has somewhere to type. The caller is responsible for the
    // "save first?" question.
    Status close_tab(size_t index, std::string* error = nullptr);
    // The tab list as the editor front-end wants it, plus the tab list payload
    // pushed after every open/close/activation.
    Json tabs_json() const;
    void notify_tabs_changed();

    static Status read_text_file(const std::string& path, std::string* text,
                                 std::string* error = nullptr);
    // Same, but also reports the line ending the file used so that saving can
    // reproduce it byte for byte.
    static Status read_text_file(const std::string& path, std::string* text,
                                 std::string* line_ending, std::string* error);
    static Status write_text_file(const std::string& path, std::string_view text,
                                  std::string* error = nullptr,
                                  bool with_utf8_bom = false);

    // --- editor -> workspace -----------------------------------------------
    struct EditReport {
        std::string revision_before;
        std::string text;          // full new document text
        size_t caret = 0;          // byte offset
        size_t selection_anchor = 0;
        size_t selection_head = 0;
        std::string hint;          // "typing", "paste", "command"...
    };
    // Applies a user edit reported by the editor front-end. The front-end is
    // authoritative for what the user typed, but the change is still recorded
    // as a net patch so the audit log stays meaningful.
    Json apply_user_edit(const EditReport& report);
    Json update_selection(size_t anchor, size_t head);

    // --- agent -> workspace -------------------------------------------------
    struct WriteOutcome {
        bool ok = false;
        std::string error_code;
        std::string error_message;
        bool staged = false;      // waiting for confirmation
        std::string pending_id;
        std::string revision_before;
        std::string revision_after;
        size_t applied = 0;
        std::string diff;
        int64_t audit_id = 0;
    };

    WriteOutcome apply_agent_patches(const std::vector<Patch>& patches, const std::string& tool,
                                     const std::string& session, const std::string& summary,
                                     bool bypass_confirmation = false);
    Status set_caret(size_t anchor, size_t head, const std::string& session,
                     std::string* error = nullptr);

    // --- confirmation -------------------------------------------------------
    std::optional<PendingChange> pending(const std::string& id = {}) const;
    Json resolve_pending(const std::string& id, bool accept, const std::string& reason);
    Json pending_json() const;

    // --- history / rollback -------------------------------------------------
    Json rollback_last(std::optional<EditSource> source, const std::string& tool);
    Json rollback_to_revision(const std::string& revision, const std::string& tool);
    Json history_json(size_t limit, bool with_text) const;

    // --- change notification ------------------------------------------------
    // Returns (and folds) the edits made since `revision` was current, so an
    // Agent can notice the human typing.
    Json user_changes_since(const std::string& revision, size_t limit) const;
    // Marks `revision` as seen by a session and returns the unseen edits.
    Json poll_user_changes(const std::string& session, const std::string& since_revision,
                           size_t limit);

    // --- policy -------------------------------------------------------------
    Json set_policy(const Json& patch);
    Json policy_json() const;
    Json status_json() const;
    Json audit_json(const AuditQuery& query) const;

    // Security check for a filesystem path against `policy.allowed_directory`.
    bool path_allowed(std::string_view path, std::string* reason = nullptr) const;

    // Loads the built-in welcome document (used with --demo and on first run).
    void set_demo_content(std::string fallback_path = {});

    // Re-synchronises the change-notification cursor with the current document
    // revision. Call this after mutating Document directly (tests, imports) so
    // the next notification is computed from the right baseline.
    void adopt_document_text();

    // Pushes the whole document to the editor front-end. The shell has to call
    // this once its bridge exists: the document may already have been opened
    // (and the welcome text installed) while the workspace was still headless,
    // in which case the page would otherwise never receive any content.
    void notify_document_loaded();

private:
    void audit_record(AuditRecord record);
    void notify_document_changed(const std::string& reason, const std::string& tool,
                                 const std::string& session, const PatchOutcome* outcome);
    WriteOutcome commit_patches(const std::vector<Patch>& patches, const std::string& tool,
                                const std::string& session, const std::string& summary);
    bool front_matter_touched(const std::vector<Patch>& patches, std::string* detail) const;
    std::string stage_change(const std::vector<Patch>& patches, const std::string& tool,
                             const std::string& session, const std::string& summary);
    // A brand-new tab: no path (so Save asks for one) and an identity of its own
    // (the id is normally derived from the path, which untitled documents lack).
    std::unique_ptr<Document> make_untitled_document();
    // True when the active document is an empty, unmodified, path-less document -
    // the placeholder the first open should fill instead of adding a second tab.
    bool active_is_placeholder() const;
    // Adopts freshly read file content into a document: path, line ending, text,
    // caret, clean dirty flag and the audit entry. Shared by open and reload.
    void adopt_file_text(Document& target, const std::string& path, std::string text,
                         const std::string& line_ending, const std::string& tool,
                         const std::string& summary);

    mutable std::mutex mutex_;          // guards pending_ and sessions_
    std::vector<std::unique_ptr<Document>> documents_;
    size_t active_ = 0;
    int64_t untitled_counter_ = 0;
    Policy policy_;
    AuditLog audit_;
    IEditorBridge* bridge_ = nullptr;
    std::string state_dir_;

    std::vector<PendingChange> pending_;
    int64_t pending_counter_ = 0;
    struct ConfirmState {
        std::string id;
        bool decided = false;
        bool accepted = false;
        std::string reason;
    };
    ConfirmState confirmation_;
    std::condition_variable confirmation_cv_;

    struct SessionState {
        std::string revision;
        int64_t last_seen_ms = 0;
    };
    std::vector<std::pair<std::string, SessionState>> sessions_;
    std::string last_broadcast_revision_;
    std::string last_broadcast_reason_;
};

// The text shipped with the editor as a first-run document. It doubles as a
// self-test of the renderer: headings, lists, tables, code, math and a Mermaid
// diagram all in one place.
std::string demo_document();

} // namespace colai
