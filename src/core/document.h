// Colaimdation - the in-memory document.
//
// This is the object Agents talk to. It owns the UTF-8 buffer, the Markdown
// block structure, the caret/selection state, a patch-based edit API, undo /
// redo history and the rollback information used by the audit log.
//
// Design rules:
//   * Every mutation goes through apply_patch(), so every mutation is
//     addressable, auditable and reversible.
//   * Offsets are UTF-8 byte offsets into text(); line/column helpers exist
//     for Agents that think in lines.
//   * The class is not internally synchronised: the host serialises calls onto
//     the UI thread (see app/dispatcher.h). Tools never block on file I/O.
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.h"
#include "core/markdown.h"
#include "core/text_edit.h"

namespace colai {

// ---------------------------------------------------------------------------
// Patches
// ---------------------------------------------------------------------------
enum class PatchOp {
    InsertText,      // insert text at offset
    ReplaceRange,    // replace [range_start, range_end) with text
    DeleteRange,     // delete [range_start, range_end)
    ReplaceBlock,    // replace a block's inner content, keeping its markers
    ReplaceBlockRaw, // replace the block including its markers
    InsertBlock,     // insert a new block before/after an existing block
    AppendSection,   // append a new section at the end of the document
    SetText,         // replace the whole document (used for resync / import)
};

std::string_view patch_op_name(PatchOp op);
std::optional<PatchOp> patch_op_from_name(std::string_view name);

struct Patch {
    PatchOp op = PatchOp::InsertText;

    // Character (byte) offsets, half-open ranges.
    size_t offset = 0;
    size_t range_start = 0;
    size_t range_end = 0;

    // Block addressing: either block_index, or block_id ("b12"), or cursor.
    std::optional<size_t> block_index;
    std::string block_id;
    bool relative_to_cursor = false;

    // New content.
    std::string text;

    // InsertBlock placement.
    enum class Position { Before, After, EndOfDocument, StartOfDocument };
    Position position = Position::After;

    // Optional diagnostics for the audit log.
    std::string note;

    static Patch insert(size_t offset, std::string text);
    static Patch replace(size_t begin, size_t end, std::string text);
    static Patch erase(size_t begin, size_t end);
    static Patch replace_block(size_t index, std::string text, bool raw = false);
    static Patch insert_block(size_t index, std::string text, Position position = Position::After);
    static Patch append_section(std::string text);
};

struct PatchOutcome {
    bool ok = false;
    std::string error_code;    // invalid_argument | out_of_range | conflict | ...
    std::string error_message;
    size_t applied_count = 0;
    std::vector<TextChange> changes; // net byte changes applied to the buffer
    size_t caret = 0;                // suggested caret position afterwards
    size_t selection_anchor = 0;
    size_t selection_head = 0;
    std::string revision_sha256;
    uint64_t sequence = 0;
};

// ---------------------------------------------------------------------------
// Selection / caret
// ---------------------------------------------------------------------------
struct Selection {
    size_t anchor = 0; // where the selection started
    size_t head = 0;   // the caret (== anchor for a collapsed selection)

    bool collapsed() const { return anchor == head; }
    size_t begin() const { return anchor < head ? anchor : head; }
    size_t end() const { return anchor < head ? head : anchor; }
    size_t length() const { return end() - begin(); }
};

// ---------------------------------------------------------------------------
// History
// ---------------------------------------------------------------------------
enum class EditSource { User, Agent, System, External };

std::string_view edit_source_name(EditSource source);
std::optional<EditSource> edit_source_from_name(std::string_view name);

struct Edit {
    uint64_t sequence = 0;          // monotonically increasing per document
    std::string revision_before;
    std::string revision_after;
    EditSource source = EditSource::Agent;
    std::string tool;               // MCP tool name, or "editor" / "open"
    std::string session;            // agent session label
    std::string summary;            // one-line human description
    std::string note;
    std::vector<TextChange> changes;
    std::string text_before;        // snapshot used for rollback / undo
    std::string text_after;
    Selection selection_before;
    Selection selection_after;
    std::string diff;               // lazy unified diff, optional
    int64_t timestamp_ms = 0;
    bool rollback_of = false;       // this edit is itself a rollback

    Json to_json(bool with_text = false) const;
};

struct RollbackPlan {
    bool ok = false;
    std::string error;
    uint64_t target_sequence = 0;
    std::string revision;
    std::string text;
    Selection selection;
    size_t edits_to_revert = 0;
    std::string diff;
};

// ---------------------------------------------------------------------------
// Document
// ---------------------------------------------------------------------------
class Document {
public:
    Document();

    // --- identity / file state ---------------------------------------------
    const std::string& id() const { return id_; }
    void set_id(std::string id) { id_ = std::move(id); }

    const std::string& path() const { return path_; }
    void set_path(std::string path); // also derives a stable id when unset

    const std::string& display_name() const { return display_name_; }
    void set_display_name(std::string name) { display_name_ = std::move(name); }

    bool dirty() const { return dirty_; }
    void set_dirty(bool dirty) { dirty_ = dirty; }

    const std::string& line_ending() const { return line_ending_; }
    void set_line_ending(std::string ending) { line_ending_ = std::move(ending); }

    bool read_only() const { return read_only_; }
    void set_read_only(bool value) { read_only_ = value; }

    // --- content ------------------------------------------------------------
    const std::string& text() const { return text_; }
    size_t size() const { return text_.size(); }
    size_t character_count() const;
    size_t line_count() const;
    size_t word_count() const;

    const std::string& revision() const { return revision_; }
    uint64_t sequence() const { return sequence_; }

    // Replaces the whole buffer. Used when opening a file or when the editor
    // front-end resynchronises. Records history unless `silent` is true.
    PatchOutcome set_text(std::string text, EditSource source, std::string tool,
                          std::string summary, bool silent = false);

    // --- caret / selection --------------------------------------------------
    Selection selection() const { return selection_; }
    void set_selection(size_t anchor, size_t head, EditSource source = EditSource::Agent,
                       std::string tool = {});
    size_t caret() const { return selection_.head; }

    // --- structure ----------------------------------------------------------
    // Parsed lazily and cached until the text changes.
    const md::ParsedDocument& parsed() const;
    void invalidate_structure() const { structure_valid_ = false; }

    int block_at(size_t offset) const;
    int block_containing(size_t offset) const;
    const md::Block* block(size_t index) const;
    size_t block_count() const { return parsed().blocks.size(); }

    // Resolves a block reference to an index; nullopt when it cannot be found.
    std::optional<size_t> resolve_block(const Patch& patch, std::string* error) const;

    // --- mutation -----------------------------------------------------------
    PatchOutcome apply_patch(const Patch& patch, EditSource source, std::string tool = {},
                             std::string summary = {}, std::string session = {});
    PatchOutcome apply_patches(const std::vector<Patch>& patches, EditSource source,
                               std::string tool = {}, std::string summary = {},
                               std::string session = {});

    // --- history ------------------------------------------------------------
    const std::deque<Edit>& history() const { return history_; }
    const Edit* find_edit(uint64_t sequence) const;
    const Edit* last_edit() const { return history_.empty() ? nullptr : &history_.back(); }

    bool can_undo() const { return !undo_stack_.empty(); }
    bool can_redo() const { return !redo_stack_.empty(); }
    PatchOutcome undo(EditSource source = EditSource::User);
    PatchOutcome redo(EditSource source = EditSource::User);

    // Plans reverting everything applied after `sequence` (exclusive).
    RollbackPlan plan_rollback_to_sequence(uint64_t sequence) const;
    // Plans reverting the last agent-originated edit (or the last edit overall
    // when `source` is nullopt).
    RollbackPlan plan_rollback_last(std::optional<EditSource> source = EditSource::Agent) const;
    // Applies a plan produced above; records the rollback as its own edit.
    PatchOutcome apply_rollback(const RollbackPlan& plan, EditSource source = EditSource::User,
                                std::string tool = {});

    void set_history_limit(size_t edits, size_t byte_budget);
    void clear_history();

    // --- serialisation ------------------------------------------------------
    Json to_json(bool include_content = false) const;
    Json selection_json() const;
    Json history_json(size_t limit, bool with_text) const;
    Json structure_json(int max_blocks) const;
    Json outline_json() const;

    Json info_json() const;

private:
    void record_edit(const std::string& text_before, Selection selection_before, EditSource source,
                     std::string tool, std::string summary, std::string session,
                     std::vector<TextChange> changes);
    void after_mutation();

    std::string id_;
    std::string path_;
    std::string display_name_;
    std::string text_;
    std::string revision_;
    std::string line_ending_ = "\n";
    uint64_t sequence_ = 0;
    bool dirty_ = false;
    bool read_only_ = false;
    Selection selection_;

    mutable md::ParsedDocument parsed_;
    mutable bool structure_valid_ = false;

    std::deque<Edit> history_;     // append-only audit trail
    std::deque<Edit> undo_stack_;  // edits a plain undo can revert, newest last
    std::deque<Edit> redo_stack_;  // edits undone and re-appliable
    size_t history_limit_ = 400;
    size_t history_byte_budget_ = 64ull * 1024 * 1024;
};

// ---------------------------------------------------------------------------
// Helpers shared with the tools
// ---------------------------------------------------------------------------
std::string make_revision(std::string_view text);
int64_t now_ms();
std::string iso8601(int64_t unix_ms);

} // namespace colai
