#include "core/document.h"

#include "core/diff.h"
#include "core/utf8.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace colai {
namespace {

constexpr size_t kMaxPatchesPerCall = 512;

} // namespace

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------
std::string_view patch_op_name(PatchOp op) {
    switch (op) {
    case PatchOp::InsertText: return "insert_text";
    case PatchOp::ReplaceRange: return "replace_range";
    case PatchOp::DeleteRange: return "delete_range";
    case PatchOp::ReplaceBlock: return "replace_block";
    case PatchOp::ReplaceBlockRaw: return "replace_block_raw";
    case PatchOp::InsertBlock: return "insert_block";
    case PatchOp::AppendSection: return "append_section";
    case PatchOp::SetText: return "set_text";
    }
    return "unknown";
}

std::optional<PatchOp> patch_op_from_name(std::string_view name) {
    const std::string lower = utf8::to_lower(name);
    if (lower == "insert_text" || lower == "insert") return PatchOp::InsertText;
    if (lower == "replace_range" || lower == "replace") return PatchOp::ReplaceRange;
    if (lower == "delete_range" || lower == "delete") return PatchOp::DeleteRange;
    if (lower == "replace_block") return PatchOp::ReplaceBlock;
    if (lower == "replace_block_raw") return PatchOp::ReplaceBlockRaw;
    if (lower == "insert_block") return PatchOp::InsertBlock;
    if (lower == "append_section" || lower == "append") return PatchOp::AppendSection;
    if (lower == "set_text") return PatchOp::SetText;
    return std::nullopt;
}

std::string_view edit_source_name(EditSource source) {
    switch (source) {
    case EditSource::User: return "user";
    case EditSource::Agent: return "agent";
    case EditSource::System: return "system";
    case EditSource::External: return "external";
    }
    return "unknown";
}

std::optional<EditSource> edit_source_from_name(std::string_view name) {
    const std::string lower = utf8::to_lower(name);
    if (lower == "user" || lower == "human" || lower == "editor") return EditSource::User;
    if (lower == "agent" || lower == "mcp" || lower == "ai") return EditSource::Agent;
    if (lower == "system" || lower == "host") return EditSource::System;
    if (lower == "external" || lower == "file") return EditSource::External;
    return std::nullopt;
}

std::string make_revision(std::string_view text) {
    return utf8::sha256_hex(text).substr(0, 32);
}

int64_t now_ms() {
    FILETIME file_time{};
    ::GetSystemTimeAsFileTime(&file_time);
    ULARGE_INTEGER value{};
    value.LowPart = file_time.dwLowDateTime;
    value.HighPart = file_time.dwHighDateTime;
    // 100-ns intervals since 1601-01-01 -> milliseconds since 1970-01-01.
    return static_cast<int64_t>((value.QuadPart - 116444736000000000ULL) / 10000ULL);
}

std::string iso8601(int64_t unix_ms) {
    // Portable-enough UTC formatter: no locale, no timezone database.
    const int64_t seconds = unix_ms / 1000;
    const int64_t millis = unix_ms % 1000;
    int64_t days = seconds / 86400;
    int64_t remainder = seconds % 86400;
    if (remainder < 0) {
        remainder += 86400;
        --days;
    }
    const int hour = static_cast<int>(remainder / 3600);
    const int minute = static_cast<int>((remainder % 3600) / 60);
    const int second = static_cast<int>(remainder % 60);

    // Civil-from-days (Howard Hinnant's algorithm), epoch 1970-01-01.
    int64_t z = days + 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const auto doe = static_cast<int64_t>(z - era * 146097);
    const auto yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t y = yoe + era * 400;
    const auto doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const auto mp = (5 * doy + 2) / 153;
    const int64_t d = doy - (153 * mp + 2) / 5 + 1;
    const int64_t m = mp + (mp < 10 ? 3 : -9);
    const int64_t year = y + (m <= 2 ? 1 : 0);

    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "%04lld-%02lld-%02lldT%02d:%02d:%02d.%03lldZ",
                  static_cast<long long>(year), static_cast<long long>(m),
                  static_cast<long long>(d), hour, minute, second,
                  static_cast<long long>(millis));
    return buffer;
}

// ---------------------------------------------------------------------------
// Patch builders
// ---------------------------------------------------------------------------
Patch Patch::insert(size_t offset_, std::string text_) {
    Patch patch;
    patch.op = PatchOp::InsertText;
    patch.offset = offset_;
    patch.text = std::move(text_);
    return patch;
}

Patch Patch::replace(size_t begin, size_t end, std::string text_) {
    Patch patch;
    patch.op = PatchOp::ReplaceRange;
    patch.range_start = begin;
    patch.range_end = end;
    patch.text = std::move(text_);
    return patch;
}

Patch Patch::erase(size_t begin, size_t end) {
    Patch patch;
    patch.op = PatchOp::DeleteRange;
    patch.range_start = begin;
    patch.range_end = end;
    return patch;
}

Patch Patch::replace_block(size_t index, std::string text_, bool raw) {
    Patch patch;
    patch.op = raw ? PatchOp::ReplaceBlockRaw : PatchOp::ReplaceBlock;
    patch.block_index = index;
    patch.block_id = "b" + std::to_string(index);
    patch.text = std::move(text_);
    return patch;
}

Patch Patch::insert_block(size_t index, std::string text_, Position position) {
    Patch patch;
    patch.op = PatchOp::InsertBlock;
    patch.block_index = index;
    patch.block_id = "b" + std::to_string(index);
    patch.text = std::move(text_);
    patch.position = position;
    return patch;
}

Patch Patch::append_section(std::string text_) {
    Patch patch;
    patch.op = PatchOp::AppendSection;
    patch.text = std::move(text_);
    return patch;
}

// ---------------------------------------------------------------------------
// Document
// ---------------------------------------------------------------------------
Document::Document() {
    // Sequence 0 means "never mutated": parsing the empty document must not be
    // counted as an edit.
    revision_ = make_revision(text_);
    parsed_ = md::parse(text_);
    structure_valid_ = true;
}

void Document::set_path(std::string path) {
    path_ = std::move(path);
    const size_t slash = path_.find_last_of("\\/");
    display_name_ = slash == std::string::npos ? path_ : path_.substr(slash + 1);
    if (id_.empty()) {
        id_ = "doc-" + utf8::sha256_hex(utf8::to_lower(path_)).substr(0, 12);
    }
}

size_t Document::character_count() const { return utf8::length(text_); }

size_t Document::line_count() const {
    // Logical line count: a trailing newline terminates the last line rather
    // than starting an empty one.
    if (text_.empty()) return 0;
    size_t lines = 1;
    for (size_t i = 0; i < text_.size(); ++i) {
        if (text_[i] == '\n' && i + 1 < text_.size()) ++lines;
    }
    return lines;
}

size_t Document::word_count() const { return parsed().word_count; }

const md::ParsedDocument& Document::parsed() const {
    if (!structure_valid_) {
        parsed_ = md::parse(text_);
        structure_valid_ = true;
    }
    return parsed_;
}

void Document::after_mutation() {
    parsed_ = md::parse(text_);
    structure_valid_ = true;
    revision_ = make_revision(text_);
    ++sequence_;
}

int Document::block_at(size_t offset) const { return parsed().block_at(offset); }

int Document::block_containing(size_t offset) const { return parsed().block_containing(offset); }

const md::Block* Document::block(size_t index) const { return parsed().block(index); }

std::optional<size_t> Document::resolve_block(const Patch& patch, std::string* error) const {
    const auto& document = parsed();
    if (document.blocks.empty()) {
        if (error) *error = "the document has no blocks";
        return std::nullopt;
    }
    if (patch.block_index.has_value()) {
        const size_t index = *patch.block_index;
        if (index >= document.blocks.size()) {
            if (error) {
                *error = "block index " + std::to_string(index) + " is out of range (0.." +
                         std::to_string(document.blocks.size() - 1) + ")";
            }
            return std::nullopt;
        }
        return index;
    }
    if (!patch.block_id.empty()) {
        std::string id = patch.block_id;
        if (!id.empty() && (id[0] == 'b' || id[0] == 'B')) id = id.substr(1);
        try {
            const size_t index = static_cast<size_t>(std::stoull(id));
            if (index < document.blocks.size()) return index;
            if (error) {
                *error = "block index " + std::to_string(index) + " is out of range (0.." +
                         std::to_string(document.blocks.size() - 1) + ")";
            }
            return std::nullopt;
        } catch (...) {
        }
        if (error) *error = "unknown block id '" + patch.block_id + "'";
        return std::nullopt;
    }
    if (patch.relative_to_cursor) {
        const int index = document.block_containing(selection_.head);
        if (index >= 0) return static_cast<size_t>(index);
        if (error) *error = "the caret is not inside a block";
        return std::nullopt;
    }
    if (error) *error = "no block was addressed (provide block_id, block_index or relative_to_cursor)";
    return std::nullopt;
}

void Document::set_selection(size_t anchor, size_t head, EditSource source, std::string tool) {
    const size_t limit = text_.size();
    const Selection before = selection_;
    Selection next;
    next.anchor = utf8::clamp_to_boundary(text_, std::min(anchor, limit));
    next.head = utf8::clamp_to_boundary(text_, std::min(head, limit));
    if (next.anchor == before.anchor && next.head == before.head) return;
    selection_ = next;

    // Fold caret movement into the newest user edit so the audit log shows what
    // the writer was looking at, not a stream of selection events.
    if (source == EditSource::User && !history_.empty()) {
        Edit& last = history_.back();
        if (last.source == EditSource::User && last.tool == "editor" &&
            last.revision_after == revision_) {
            last.selection_after = selection_;
        }
    }
    (void)tool;
}

// ---------------------------------------------------------------------------
// Mutation
// ---------------------------------------------------------------------------
PatchOutcome Document::set_text(std::string text, EditSource source, std::string tool,
                                std::string summary, bool silent) {
    Patch patch;
    patch.op = PatchOp::SetText;
    patch.text = std::move(text);

    if (!silent) {
        return apply_patches({patch}, source, std::move(tool), std::move(summary));
    }

    const std::string before = text_;
    normalize_newlines(patch.text);
    text_ = std::move(patch.text);
    selection_.anchor = utf8::clamp_to_boundary(text_, std::min(selection_.anchor, text_.size()));
    selection_.head = utf8::clamp_to_boundary(text_, std::min(selection_.head, text_.size()));
    after_mutation();
    dirty_ = true;

    PatchOutcome outcome;
    outcome.ok = true;
    outcome.applied_count = before == text_ ? 0 : 1;
    outcome.caret = selection_.head;
    outcome.selection_anchor = selection_.anchor;
    outcome.selection_head = selection_.head;
    outcome.revision_sha256 = revision_;
    outcome.sequence = sequence_;
    return outcome;
}

PatchOutcome Document::apply_patch(const Patch& patch, EditSource source, std::string tool,
                                   std::string summary, std::string session) {
    return apply_patches({patch}, source, std::move(tool), std::move(summary), std::move(session));
}

PatchOutcome Document::apply_patches(const std::vector<Patch>& patches, EditSource source,
                                     std::string tool, std::string summary, std::string session) {
    PatchOutcome outcome;
    if (patches.empty()) {
        outcome.ok = true;
        outcome.revision_sha256 = revision_;
        outcome.sequence = sequence_;
        outcome.caret = selection_.head;
        outcome.selection_anchor = selection_.anchor;
        outcome.selection_head = selection_.head;
        return outcome;
    }
    if (patches.size() > kMaxPatchesPerCall) {
        outcome.error_code = "invalid_argument";
        outcome.error_message =
            "too many patches in one call (limit " + std::to_string(kMaxPatchesPerCall) + ")";
        return outcome;
    }
    if (read_only_) {
        outcome.error_code = "permission_denied";
        outcome.error_message = "this document is opened read-only";
        return outcome;
    }

    const std::string previous_text = text_;
    const Selection previous_selection = selection_;
    const md::ParsedDocument& structure = parsed();

    // ---- phase 1: validate ------------------------------------------------
    std::vector<TextChange> changes;
    changes.reserve(patches.size());

    for (const Patch& patch : patches) {
        TextChange change;
        switch (patch.op) {
        case PatchOp::InsertText: {
            if (patch.offset > text_.size()) {
                outcome.error_code = "out_of_range";
                outcome.error_message = "insert offset " + std::to_string(patch.offset) +
                                        " is past the end of the document (" +
                                        std::to_string(text_.size()) + " bytes)";
                return outcome;
            }
            const size_t offset = utf8::clamp_to_boundary(text_, patch.offset);
            change.begin = offset;
            change.end = offset;
            change.inserted = patch.text;
            break;
        }
        case PatchOp::ReplaceRange:
        case PatchOp::DeleteRange: {
            size_t begin = patch.range_start;
            size_t end = patch.range_end;
            if (begin > end) std::swap(begin, end);
            if (end > text_.size()) {
                outcome.error_code = "out_of_range";
                outcome.error_message = "range [" + std::to_string(begin) + ", " +
                                        std::to_string(end) + ") exceeds the document size (" +
                                        std::to_string(text_.size()) + " bytes)";
                return outcome;
            }
            change.begin = utf8::clamp_to_boundary(text_, begin);
            change.end = utf8::clamp_to_boundary(text_, end);
            if (patch.op == PatchOp::ReplaceRange) change.inserted = patch.text;
            break;
        }
        case PatchOp::ReplaceBlock:
        case PatchOp::ReplaceBlockRaw: {
            std::string error;
            const auto index = resolve_block(patch, &error);
            if (!index) {
                outcome.error_code = "invalid_argument";
                outcome.error_message = error;
                return outcome;
            }
            const md::Block& target = structure.blocks[*index];
            const bool raw = patch.op == PatchOp::ReplaceBlockRaw;
            const size_t begin = raw ? target.begin : target.content_begin;
            const size_t end = raw ? target.end : target.content_end;
            change.begin = std::min(begin, text_.size());
            change.end = std::max(change.begin, std::min(end, text_.size()));
            change.inserted = patch.text;
            break;
        }
        case PatchOp::InsertBlock: {
            std::string error;
            const auto index = resolve_block(patch, &error);
            if (!index) {
                outcome.error_code = "invalid_argument";
                outcome.error_message = error;
                return outcome;
            }
            const md::Block& target = structure.blocks[*index];
            std::string block_text = patch.text;
            normalize_newlines(block_text);
            if (!block_text.empty() && block_text.back() != '\n') block_text.push_back('\n');

            size_t at = 0;
            switch (patch.position) {
            case Patch::Position::Before: at = target.begin; break;
            case Patch::Position::After: at = target.end; break;
            case Patch::Position::EndOfDocument: at = text_.size(); break;
            case Patch::Position::StartOfDocument: at = 0; break;
            }
            at = utf8::clamp_to_boundary(text_, std::min(at, text_.size()));
            change.begin = at;
            change.end = at;
            change.inserted = block_text;
            break;
        }
        case PatchOp::AppendSection: {
            std::string section = patch.text;
            normalize_newlines(section);
            std::string payload;
            if (!text_.empty()) {
                // One blank line between the existing content and the new section.
                if (text_.back() != '\n') payload += "\n";
                payload += "\n";
            }
            payload += section;
            if (payload.empty() || payload.back() != '\n') payload.push_back('\n');
            change.begin = text_.size();
            change.end = text_.size();
            change.inserted = std::move(payload);
            break;
        }
        case PatchOp::SetText: {
            if (patches.size() > 1) {
                outcome.error_code = "invalid_argument";
                outcome.error_message = "set_text must be the only patch in a call";
                return outcome;
            }
            std::string replacement = patch.text;
            normalize_newlines(replacement);
            change.begin = 0;
            change.end = text_.size();
            change.inserted = std::move(replacement);
            break;
        }
        }
        if (change.begin != change.end || !change.inserted.empty()) {
            changes.push_back(std::move(change));
        }
    }

    if (changes.empty()) {
        outcome.ok = true;
        outcome.applied_count = 0;
        outcome.revision_sha256 = revision_;
        outcome.sequence = sequence_;
        outcome.caret = selection_.head;
        outcome.selection_anchor = selection_.anchor;
        outcome.selection_head = selection_.head;
        return outcome;
    }

    // ---- phase 2: reject overlaps ----------------------------------------
    std::vector<size_t> order(changes.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        if (changes[a].begin != changes[b].begin) return changes[a].begin < changes[b].begin;
        return changes[a].end < changes[b].end;
    });
    for (size_t i = 1; i < order.size(); ++i) {
        const TextChange& previous = changes[order[i - 1]];
        const TextChange& current = changes[order[i]];
        if (current.begin < previous.end) {
            outcome.error_code = "invalid_argument";
            outcome.error_message = "patch ranges overlap (" + std::to_string(previous.begin) + ".." +
                                    std::to_string(previous.end) + " vs " +
                                    std::to_string(current.begin) + ".." +
                                    std::to_string(current.end) + ")";
            return outcome;
        }
    }

    // ---- phase 3: apply ---------------------------------------------------
    std::string next = text_;
    for (size_t i = order.size(); i-- > 0;) {
        const TextChange& change = changes[order[i]];
        next.replace(change.begin, change.end - change.begin, change.inserted);
    }
    if (!utf8::is_valid(next)) {
        outcome.error_code = "invalid_argument";
        outcome.error_message = "the patch would produce invalid UTF-8";
        return outcome;
    }

    // Net change list against the original buffer: one coherent edit for the
    // editor front-end and the audit log.
    std::vector<TextChange> net;
    if (changes.size() == 1 && changes.front().begin == 0 &&
        changes.front().end == previous_text.size()) {
        net = changes;
    } else {
        TextChange single;
        if (compute_minimal_change(previous_text, next, single)) net.push_back(single);
    }

    text_ = std::move(next);
    after_mutation();
    dirty_ = true;

    // Caret: end of the edited region, clamped to the new buffer.
    {
        size_t caret = selection_.head;
        bool caret_touched = false;
        for (const TextChange& change : changes) {
            if (caret >= change.begin && caret <= change.end) {
                caret_touched = true;
                break;
            }
        }
        if (caret_touched && !net.empty()) {
            caret = net.front().begin + net.front().inserted.size();
        }
        selection_.head = utf8::clamp_to_boundary(text_, std::min(caret, text_.size()));
        selection_.anchor = selection_.head;
    }

    record_edit(previous_text, previous_selection, source, tool, summary, session, net);

    outcome.ok = true;
    outcome.applied_count = changes.size();
    outcome.changes = net;
    outcome.caret = selection_.head;
    outcome.selection_anchor = selection_.anchor;
    outcome.selection_head = selection_.head;
    outcome.revision_sha256 = revision_;
    outcome.sequence = sequence_;
    return outcome;
}

// ---------------------------------------------------------------------------
// History
// ---------------------------------------------------------------------------
void Document::record_edit(const std::string& text_before, Selection selection_before,
                           EditSource source, std::string tool, std::string summary,
                           std::string session, std::vector<TextChange> changes) {
    Edit edit;
    // after_mutation() has already advanced sequence_, so the state this edit
    // started from was the previous value.
    edit.sequence = sequence_ > 0 ? sequence_ - 1 : 0;
    edit.revision_before = make_revision(text_before);
    edit.revision_after = revision_;
    edit.source = source;
    edit.tool = std::move(tool);
    edit.session = std::move(session);
    edit.summary = std::move(summary);
    edit.changes = std::move(changes);
    edit.text_before = text_before;
    edit.text_after = text_;
    edit.selection_before = selection_before;
    edit.selection_after = selection_;
    edit.timestamp_ms = now_ms();

    // Every mutation gets exactly one history entry: merging rapid edits would
    // make a single undo revert more than the user (or agent) asked for.
    history_.push_back(edit);
    undo_stack_.push_back(edit);
    redo_stack_.clear();

    // Enforce the retention budget: count first, then evict from the front.
    while (history_.size() > history_limit_) history_.pop_front();
    size_t bytes = 0;
    for (const Edit& entry : history_) bytes += entry.text_before.size() + entry.text_after.size();
    while (bytes > history_byte_budget_ && history_.size() > 1) {
        bytes -= history_.front().text_before.size() + history_.front().text_after.size();
        history_.pop_front();
    }
}

const Edit* Document::find_edit(uint64_t sequence) const {
    for (const Edit& edit : history_) {
        if (edit.sequence == sequence) return &edit;
    }
    return nullptr;
}

PatchOutcome Document::undo(EditSource source) {
    PatchOutcome outcome;
    if (undo_stack_.empty()) {
        outcome.error_code = "conflict";
        outcome.error_message = "nothing to undo";
        return outcome;
    }
    Edit edit = undo_stack_.back();
    undo_stack_.pop_back();

    const std::string before = text_;
    const Selection selection_before = selection_;
    set_text(edit.text_before, EditSource::System, "undo", "undo " + edit.summary, true);
    selection_ = edit.selection_before;
    dirty_ = true;
    const std::string after = text_;
    redo_stack_.push_back(edit);

    // The undo itself is recorded so the audit trail stays complete.
    Edit record;
    record.sequence = sequence_ > 0 ? sequence_ - 1 : 0;
    record.revision_before = make_revision(before);
    record.revision_after = revision_;
    record.source = source;
    record.tool = "undo";
    record.summary = "undo: " + edit.summary;
    record.text_before = before;
    record.text_after = after;
    record.selection_before = selection_before;
    record.selection_after = selection_;
    record.timestamp_ms = now_ms();
    TextChange change;
    if (compute_minimal_change(before, after, change)) record.changes.push_back(change);
    history_.push_back(record);

    outcome.ok = true;
    outcome.applied_count = 1;
    outcome.changes = record.changes;
    outcome.caret = selection_.head;
    outcome.selection_anchor = selection_.anchor;
    outcome.selection_head = selection_.head;
    outcome.revision_sha256 = revision_;
    outcome.sequence = sequence_;
    return outcome;
}

PatchOutcome Document::redo(EditSource source) {
    PatchOutcome outcome;
    if (redo_stack_.empty()) {
        outcome.error_code = "conflict";
        outcome.error_message = "nothing to redo";
        return outcome;
    }
    Edit edit = redo_stack_.back();
    redo_stack_.pop_back();

    const std::string before = text_;
    const Selection selection_before = selection_;
    set_text(edit.text_after, EditSource::System, "redo", "redo " + edit.summary, true);
    selection_ = edit.selection_after;
    dirty_ = true;
    const std::string after = text_;
    // The redone edit goes back on the undo stack; the redo itself does not, so
    // an explicit undo always reverses the most recent real edit.
    undo_stack_.push_back(edit);

    Edit record;
    record.sequence = sequence_ > 0 ? sequence_ - 1 : 0;
    record.revision_before = make_revision(before);
    record.revision_after = revision_;
    record.source = source;
    record.tool = "redo";
    record.summary = "redo: " + edit.summary;
    record.text_before = before;
    record.text_after = after;
    record.selection_before = selection_before;
    record.selection_after = selection_;
    record.timestamp_ms = now_ms();
    TextChange change;
    if (compute_minimal_change(before, after, change)) record.changes.push_back(change);
    history_.push_back(record);

    outcome.ok = true;
    outcome.applied_count = 1;
    outcome.changes = record.changes;
    outcome.caret = selection_.head;
    outcome.selection_anchor = selection_.anchor;
    outcome.selection_head = selection_.head;
    outcome.revision_sha256 = revision_;
    outcome.sequence = sequence_;
    return outcome;
}

RollbackPlan Document::plan_rollback_to_sequence(uint64_t sequence) const {
    RollbackPlan plan;
    // Everything strictly after `sequence` is reverted; the resulting text is
    // the oldest reverted edit's "before" snapshot.
    const Edit* oldest = nullptr;
    size_t count = 0;
    for (const Edit& edit : history_) {
        if (edit.sequence <= sequence) continue;
        if (!oldest) oldest = &edit;
        ++count;
        if (count > 200) {
            plan.error = "refusing to roll back more than 200 edits in one step";
            return plan;
        }
    }
    if (!oldest) {
        plan.error = "no edits found after sequence " + std::to_string(sequence);
        return plan;
    }
    plan.ok = true;
    plan.target_sequence = sequence;
    plan.text = oldest->text_before;
    plan.selection = oldest->selection_before;
    plan.revision = make_revision(plan.text);
    plan.edits_to_revert = count;
    plan.diff = unified_diff(text_, plan.text, "current", "after_rollback");
    return plan;
}

RollbackPlan Document::plan_rollback_last(std::optional<EditSource> source) const {
    RollbackPlan plan;
    const Edit* target = nullptr;
    for (auto it = history_.rbegin(); it != history_.rend(); ++it) {
        if (it->rollback_of) continue;
        if (source.has_value() && it->source != *source) continue;
        target = &*it;
        break;
    }
    if (!target) {
        plan.error = source.has_value()
                         ? std::string("no ") + std::string(edit_source_name(*source)) +
                               " edit to roll back"
                         : std::string("no edit to roll back");
        return plan;
    }
    plan.ok = true;
    plan.target_sequence = target->sequence;
    plan.text = target->text_before;
    plan.selection = target->selection_before;
    plan.revision = make_revision(plan.text);
    plan.edits_to_revert = 1;
    plan.diff = unified_diff(text_, plan.text, "current", "after_rollback");
    return plan;
}

PatchOutcome Document::apply_rollback(const RollbackPlan& plan, EditSource source, std::string tool) {
    PatchOutcome outcome;
    if (!plan.ok) {
        outcome.error_code = "conflict";
        outcome.error_message = plan.error.empty() ? "invalid rollback plan" : plan.error;
        return outcome;
    }
    const std::string before = text_;
    const Selection selection_before = selection_;
    set_text(plan.text, EditSource::System, tool.empty() ? "rollback" : tool, "rollback", true);
    selection_ = plan.selection;
    dirty_ = true;
    const std::string after = text_;

    Edit record;
    // Same convention as record_edit(): an edit's sequence is the state it
    // started from. set_text(silent) has already advanced sequence_, so the
    // pre-mutation value is sequence_ - 1. Using sequence_ here made the
    // rollback entry collide with the *next* edit's number, which broke
    // find_edit() and made `rollback --to_sequence` revert one edit too many.
    record.sequence = sequence_ > 0 ? sequence_ - 1 : 0;
    record.revision_before = make_revision(before);
    record.revision_after = revision_;
    record.source = source;
    record.tool = tool.empty() ? "rollback" : tool;
    record.summary = "rollback to revision " + plan.revision.substr(0, 12) + " (" +
                     std::to_string(plan.edits_to_revert) + " edit(s) reverted)";
    record.rollback_of = true;
    record.text_before = before;
    record.text_after = after;
    record.selection_before = selection_before;
    record.selection_after = selection_;
    record.timestamp_ms = now_ms();
    TextChange change;
    if (compute_minimal_change(before, after, change)) record.changes.push_back(change);
    history_.push_back(std::move(record));
    undo_stack_.clear();
    redo_stack_.clear();

    outcome.ok = true;
    outcome.applied_count = 1;
    outcome.changes = record.changes;
    outcome.caret = selection_.head;
    outcome.selection_anchor = selection_.anchor;
    outcome.selection_head = selection_.head;
    outcome.revision_sha256 = revision_;
    outcome.sequence = sequence_;
    return outcome;
}

void Document::set_history_limit(size_t edits, size_t byte_budget) {
    history_limit_ = std::max<size_t>(1, edits);
    history_byte_budget_ = std::max<size_t>(1024 * 1024, byte_budget);
    while (history_.size() > history_limit_) history_.pop_front();
}

void Document::clear_history() {
    history_.clear();
    undo_stack_.clear();
    redo_stack_.clear();
}

// ---------------------------------------------------------------------------
// Serialisation
// ---------------------------------------------------------------------------
Json Edit::to_json(bool with_text) const {
    Json out = Json::object();
    out.set("sequence", Json(static_cast<int64_t>(sequence)));
    out.set("revision_before", Json(revision_before));
    out.set("revision_after", Json(revision_after));
    out.set("source", Json(std::string(edit_source_name(source))));
    out.set("tool", Json(tool));
    if (!session.empty()) out.set("session", Json(session));
    out.set("summary", Json(summary));
    out.set("rollback", Json(rollback_of));
    out.set("timestamp_ms", Json(timestamp_ms));
    out.set("timestamp", Json(iso8601(timestamp_ms)));
    Json change_list = Json::array();
    for (const TextChange& change : changes) {
        Json entry = Json::object();
        entry.set("byte_start", Json(static_cast<int64_t>(change.begin)));
        entry.set("byte_end", Json(static_cast<int64_t>(change.end)));
        entry.set("removed_bytes", Json(static_cast<int64_t>(change.end - change.begin)));
        entry.set("inserted_bytes", Json(static_cast<int64_t>(change.inserted.size())));
        entry.set("inserted", Json(change.inserted));
        change_list.push(std::move(entry));
    }
    out.set("changes", std::move(change_list));
    out.set("changed_bytes",
            Json(static_cast<int64_t>(text_after.size()) - static_cast<int64_t>(text_before.size())));
    if (with_text) {
        out.set("text_before", Json(text_before));
        out.set("text_after", Json(text_after));
    }
    return out;
}

Json Document::selection_json() const {
    Json out = Json::object();
    out.set("anchor", Json(static_cast<int64_t>(selection_.anchor)));
    out.set("head", Json(static_cast<int64_t>(selection_.head)));
    out.set("collapsed", Json(selection_.collapsed()));
    out.set("start", Json(static_cast<int64_t>(selection_.begin())));
    out.set("end", Json(static_cast<int64_t>(selection_.end())));
    out.set("length", Json(static_cast<int64_t>(selection_.length())));
    size_t anchor_line = 0, anchor_column = 0, head_line = 0, head_column = 0;
    line_column_at_offset(text_, selection_.anchor, anchor_line, anchor_column);
    line_column_at_offset(text_, selection_.head, head_line, head_column);
    out.set("anchor_line", Json(static_cast<int64_t>(anchor_line)));
    out.set("anchor_column", Json(static_cast<int64_t>(anchor_column)));
    out.set("head_line", Json(static_cast<int64_t>(head_line)));
    out.set("head_column", Json(static_cast<int64_t>(head_column)));
    out.set("text", selection_.collapsed()
                        ? Json()
                        : Json(text_.substr(selection_.begin(), selection_.length())));

    Json block_info = Json();
    const int index = block_containing(selection_.head);
    if (index >= 0) {
        const md::Block& target = parsed().blocks[static_cast<size_t>(index)];
        block_info = Json::object();
        block_info.set("block_id", Json("b" + std::to_string(index)));
        block_info.set("index", Json(static_cast<int64_t>(index)));
        block_info.set("kind", Json(std::string(md::kind_name(target.kind))));
        block_info.set("byte_start", Json(static_cast<int64_t>(target.begin)));
        block_info.set("byte_end", Json(static_cast<int64_t>(target.end)));
        block_info.set("offset_in_block", Json(static_cast<int64_t>(selection_.head - target.begin)));
    }
    out.set("block", std::move(block_info));
    return out;
}

Json Document::to_json(bool include_content) const {
    Json out = Json::object();
    out.set("id", Json(id_));
    out.set("path", Json(path_));
    out.set("name", Json(display_name_));
    out.set("revision", Json(revision_));
    out.set("sequence", Json(static_cast<int64_t>(sequence_)));
    out.set("dirty", Json(dirty_));
    out.set("read_only", Json(read_only_));
    out.set("line_ending",
            Json(line_ending_ == "\r\n" ? "crlf" : (line_ending_ == "\r" ? "cr" : "lf")));
    out.set("bytes", Json(static_cast<int64_t>(text_.size())));
    out.set("characters", Json(static_cast<int64_t>(character_count())));
    out.set("lines", Json(static_cast<int64_t>(line_count())));
    out.set("words", Json(static_cast<int64_t>(parsed().word_count)));
    out.set("blocks", Json(static_cast<int64_t>(parsed().blocks.size())));
    out.set("has_front_matter", Json(parsed().has_front_matter));
    out.set("can_undo", Json(can_undo()));
    out.set("can_redo", Json(can_redo()));
    out.set("selection", selection_json());
    out.set("edits_recorded", Json(static_cast<int64_t>(history_.size())));
    if (include_content) out.set("content", Json(text_));
    return out;
}

Json Document::history_json(size_t limit, bool with_text) const {
    Json out = Json::array();
    const size_t total = history_.size();
    const size_t start = (limit > 0 && total > limit) ? total - limit : 0;
    for (size_t i = start; i < total; ++i) {
        out.push(history_[i].to_json(with_text));
    }
    return out;
}

Json Document::structure_json(int max_blocks) const {
    const md::ParsedDocument& document = parsed();
    Json out = Json::object();
    out.set("revision", Json(revision_));
    out.set("sequence", Json(static_cast<int64_t>(sequence_)));
    out.set("has_front_matter", Json(document.has_front_matter));
    out.set("blocks_total", Json(static_cast<int64_t>(document.blocks.size())));
    out.set("fingerprint", Json(md::structure_fingerprint(document)));
    Json blocks = Json::array();
    const size_t limit =
        max_blocks <= 0
            ? document.blocks.size()
            : std::min<size_t>(static_cast<size_t>(max_blocks), document.blocks.size());
    for (size_t i = 0; i < limit; ++i) {
        blocks.push(md::block_to_json(document.blocks[i], text_, i));
    }
    out.set("blocks", std::move(blocks));
    out.set("truncated", Json(limit < document.blocks.size()));
    return out;
}

Json Document::outline_json() const {
    const md::ParsedDocument& document = parsed();
    Json out = Json::array();
    for (const auto& entry : document.outline) {
        Json item = Json::object();
        item.set("level", Json(static_cast<int64_t>(entry.level)));
        item.set("title", Json(entry.title));
        item.set("block_id", Json("b" + std::to_string(entry.block_index)));
        item.set("block_index", Json(static_cast<int64_t>(entry.block_index)));
        item.set("byte_offset", Json(static_cast<int64_t>(entry.offset)));
        out.push(std::move(item));
    }
    return out;
}

Json Document::info_json() const {
    const md::ParsedDocument& document = parsed();
    Json out = Json::object();
    out.set("id", Json(id_));
    out.set("path", Json(path_));
    out.set("name", Json(display_name_));
    out.set("revision", Json(revision_));
    out.set("sequence", Json(static_cast<int64_t>(sequence_)));
    out.set("dirty", Json(dirty_));
    out.set("read_only", Json(read_only_));
    out.set("bytes", Json(static_cast<int64_t>(text_.size())));
    out.set("characters", Json(static_cast<int64_t>(character_count())));
    out.set("lines", Json(static_cast<int64_t>(line_count())));
    out.set("words", Json(static_cast<int64_t>(document.word_count)));
    out.set("blocks", Json(static_cast<int64_t>(document.blocks.size())));
    out.set("has_front_matter", Json(document.has_front_matter));
    out.set("edits_recorded", Json(static_cast<int64_t>(history_.size())));
    out.set("can_undo", Json(can_undo()));
    out.set("can_redo", Json(can_redo()));
    return out;
}

} // namespace colai
