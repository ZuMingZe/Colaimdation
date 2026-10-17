#include "core/text_edit.h"

#include "core/utf8.h"

#include <algorithm>

namespace colai {
namespace {

size_t snap(std::string_view text, size_t offset, bool enabled) {
    if (!enabled) return std::min(offset, text.size());
    return utf8::clamp_to_boundary(text, std::min(offset, text.size()));
}

// Shrink the changed region so that offsets land on UTF-8 boundaries in both
// the old and the new buffer.
void align_change(std::string_view before, std::string_view after, TextChange& change) {
    size_t begin = std::min(change.begin, before.size());
    size_t end = std::min(change.end, before.size());
    // `before[begin]` is read to test the boundary, so `begin` must stay strictly
    // inside the buffer. Letting it reach before.size() read one byte past the
    // end of the string; a stray continuation byte there would have walked
    // `begin` backwards into the middle of a character.
    while (begin > 0 && begin < before.size() &&
           (static_cast<unsigned char>(before[begin]) & 0xC0) == 0x80) {
        --begin;
    }
    while (end < before.size() && end > begin &&
           (static_cast<unsigned char>(before[end]) & 0xC0) == 0x80) {
        --end;
    }
    const size_t removed = end - begin;
    const size_t inserted = change.inserted.size();
    if (removed == inserted) return; // nothing to realign
    change.begin = begin;
    change.end = end;
    (void)after;
}

} // namespace

bool apply_text_change(std::string& text, const TextChange& change, bool snap_to_grapheme) {
    size_t begin = snap(text, change.begin, snap_to_grapheme);
    size_t end = snap(text, change.end, snap_to_grapheme);
    if (end < begin) std::swap(begin, end);
    text.replace(begin, end - begin, change.inserted);
    return true;
}

bool apply_text_changes(std::string& text, const std::vector<TextChange>& changes,
                        bool snap_to_grapheme) {
    if (changes.empty()) return true;
    std::vector<TextChange> ordered;
    ordered.reserve(changes.size());
    for (const TextChange& change : changes) {
        TextChange copy = change;
        copy.begin = snap(text, copy.begin, snap_to_grapheme);
        copy.end = snap(text, copy.end, snap_to_grapheme);
        if (copy.end < copy.begin) std::swap(copy.begin, copy.end);
        if (copy.begin == copy.end && copy.inserted.empty()) continue;
        ordered.push_back(std::move(copy));
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const TextChange& a, const TextChange& b) { return a.begin < b.begin; });
    // Reject overlapping edits rather than silently corrupting the document.
    for (size_t i = 1; i < ordered.size(); ++i) {
        if (ordered[i].begin < ordered[i - 1].end) return false;
    }
    for (size_t i = ordered.size(); i-- > 0;) {
        text.replace(ordered[i].begin, ordered[i].end - ordered[i].begin, ordered[i].inserted);
    }
    return true;
}

bool compute_minimal_change(std::string_view before, std::string_view after, TextChange& out) {
    if (before == after) return false;

    size_t prefix = 0;
    const size_t max_prefix = std::min(before.size(), after.size());
    while (prefix < max_prefix && before[prefix] == after[prefix]) ++prefix;

    size_t suffix = 0;
    const size_t max_suffix = std::min(before.size() - prefix, after.size() - prefix);
    while (suffix < max_suffix && before[before.size() - 1 - suffix] == after[after.size() - 1 - suffix]) {
        ++suffix;
    }

    out.begin = prefix;
    out.end = before.size() - suffix;
    out.inserted.assign(after.substr(prefix, after.size() - suffix - prefix));
    align_change(before, after, out);
    return true;
}

bool normalize_newlines(std::string& text) {
    if (text.find('\r') == std::string::npos) return false;
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r') {
            out.push_back('\n');
            if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
        } else {
            out.push_back(text[i]);
        }
    }
    text = std::move(out);
    return true;
}

std::string detect_line_ending(std::string_view text) {
    size_t crlf = 0;
    size_t lf = 0;
    size_t cr = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r') {
            if (i + 1 < text.size() && text[i + 1] == '\n') {
                ++crlf;
                ++i;
            } else {
                ++cr;
            }
        } else if (text[i] == '\n') {
            ++lf;
        }
    }
    if (crlf >= lf && crlf >= cr && crlf > 0) return "\r\n";
    if (cr > lf && cr > crlf) return "\r";
    return "\n";
}

std::string apply_line_ending(std::string_view text, std::string_view ending) {
    if (ending == "\n" || ending.empty()) return std::string(text);
    std::string out;
    out.reserve(text.size() + text.size() / 16);
    for (char c : text) {
        if (c == '\n') {
            out += ending;
        } else {
            out.push_back(c);
        }
    }
    return out;
}

size_t offset_at_line_column(std::string_view text, size_t line, size_t column) {
    if (line < 1) line = 1;
    if (column < 1) column = 1;
    size_t current_line = 1;
    size_t pos = 0;
    while (current_line < line && pos < text.size()) {
        if (text[pos] == '\n') ++current_line;
        ++pos;
    }
    if (current_line < line) return text.size();
    return utf8::advance(text, pos, static_cast<int>(column - 1));
}

void line_column_at_offset(std::string_view text, size_t offset, size_t& line, size_t& column) {
    line = utf8::line_number_at(text, offset);
    column = utf8::column_at(text, offset);
}

} // namespace colai
