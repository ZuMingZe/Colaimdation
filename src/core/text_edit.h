// Colaimdation - text edit primitives.
//
// Everything the Agent does lands here: a patch is validated against the
// current UTF-8 buffer and applied as a single, well-formed edit so that the
// editor front-end can mirror it without re-reading the whole document.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace colai {

struct TextChange {
    size_t begin = 0;       // first replaced byte
    size_t end = 0;         // one past the last replaced byte
    std::string inserted;   // replacement text (may be empty)
};

// Validates and applies `change` to `text` in place. Offsets are clamped to
// the buffer, never overrun it, and are aligned to UTF-8 sequence boundaries
// when `snap_to_grapheme` is true (default), so a patch can never split a
// multi-byte character - important for CJK round-tripping.
bool apply_text_change(std::string& text, const TextChange& change, bool snap_to_grapheme = true);

// Applies several changes at once. Changes must be non-overlapping; they are
// applied from the end of the buffer backwards so earlier offsets stay valid.
bool apply_text_changes(std::string& text, const std::vector<TextChange>& changes,
                        bool snap_to_grapheme = true);

// Line/character prefix-suffix diff: returns the minimal single change that
// turns `before` into `after`. Returns false when the strings are equal.
bool compute_minimal_change(std::string_view before, std::string_view after, TextChange& out);

// Normalizes line endings to '\n'. Returns true when something changed.
bool normalize_newlines(std::string& text);

// Detects the dominant line ending: "\n", "\r\n" or "\r".
std::string detect_line_ending(std::string_view text);

// Converts '\n' to the requested ending.
std::string apply_line_ending(std::string_view text, std::string_view ending);

// Offset helpers used by both the editor bridge and the tools.
size_t offset_at_line_column(std::string_view text, size_t line, size_t column);
void line_column_at_offset(std::string_view text, size_t offset, size_t& line, size_t& column);

} // namespace colai
