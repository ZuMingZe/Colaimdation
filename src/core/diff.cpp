// Colaimdation - diff helpers (line diff + unified output + rollback plans).
#include "core/diff.h"

#include <algorithm>
#include <map>
#include <sstream>

#include "core/text_edit.h"
#include "core/utf8.h"

namespace colai {
namespace {

// LCS via dynamic programming over lines, with a cheap common-prefix/suffix
// trim so the quadratic table only sees the changed region. Documents are
// human-sized Markdown files, so this is comfortably fast; a pathological
// input is bounded by kMaxMatrixCells.
constexpr size_t kMaxMatrixCells = 4'000'000;

std::vector<std::string_view> split_keep(std::string_view text) {
    std::vector<std::string_view> lines;
    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find('\n', start);
        if (nl == std::string_view::npos) {
            if (start < text.size()) lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, nl - start));
        start = nl + 1;
    }
    return lines;
}

} // namespace

std::vector<DiffLine> diff_lines(std::string_view before, std::string_view after) {
    const std::vector<std::string_view> a = split_keep(before);
    const std::vector<std::string_view> b = split_keep(after);

    std::vector<DiffLine> result;

    // Common prefix.
    size_t prefix = 0;
    while (prefix < a.size() && prefix < b.size() && a[prefix] == b[prefix]) ++prefix;
    // Common suffix.
    size_t suffix = 0;
    while (suffix < a.size() - prefix && suffix < b.size() - prefix &&
           a[a.size() - 1 - suffix] == b[b.size() - 1 - suffix]) {
        ++suffix;
    }

    const size_t a_mid = a.size() - prefix - suffix;
    const size_t b_mid = b.size() - prefix - suffix;

    for (size_t i = 0; i < prefix; ++i) {
        result.push_back({DiffLine::Kind::Context, a[i], i + 1, i + 1});
    }

    std::vector<DiffLine> middle;

    if (a_mid == 0 && b_mid == 0) {
        // identical apart from nothing
    } else if (a_mid == 0) {
        for (size_t j = 0; j < b_mid; ++j) {
            middle.push_back({DiffLine::Kind::Insert, b[prefix + j], 0, prefix + j + 1});
        }
    } else if (b_mid == 0) {
        for (size_t i = 0; i < a_mid; ++i) {
            middle.push_back({DiffLine::Kind::Delete, a[prefix + i], prefix + i + 1, 0});
        }
    } else if (a_mid * b_mid > kMaxMatrixCells) {
        // Degrade to a coarse replace block for very large regions.
        for (size_t i = 0; i < a_mid; ++i) {
            middle.push_back({DiffLine::Kind::Delete, a[prefix + i], prefix + i + 1, 0});
        }
        for (size_t j = 0; j < b_mid; ++j) {
            middle.push_back({DiffLine::Kind::Insert, b[prefix + j], 0, prefix + j + 1});
        }
    } else {
        const size_t rows = a_mid + 1;
        const size_t cols = b_mid + 1;
        std::vector<uint32_t> table(rows * cols, 0);
        auto at = [&](size_t i, size_t j) -> uint32_t& { return table[i * cols + j]; };
        for (size_t i = a_mid; i-- > 0;) {
            for (size_t j = b_mid; j-- > 0;) {
                if (a[prefix + i] == b[prefix + j]) {
                    at(i, j) = at(i + 1, j + 1) + 1;
                } else {
                    at(i, j) = std::max(at(i + 1, j), at(i, j + 1));
                }
            }
        }
        size_t i = 0;
        size_t j = 0;
        while (i < a_mid && j < b_mid) {
            if (a[prefix + i] == b[prefix + j]) {
                middle.push_back({DiffLine::Kind::Context, a[prefix + i], prefix + i + 1, prefix + j + 1});
                ++i;
                ++j;
            } else if (at(i + 1, j) >= at(i, j + 1)) {
                middle.push_back({DiffLine::Kind::Delete, a[prefix + i], prefix + i + 1, 0});
                ++i;
            } else {
                middle.push_back({DiffLine::Kind::Insert, b[prefix + j], 0, prefix + j + 1});
                ++j;
            }
        }
        while (i < a_mid) {
            middle.push_back({DiffLine::Kind::Delete, a[prefix + i], prefix + i + 1, 0});
            ++i;
        }
        while (j < b_mid) {
            middle.push_back({DiffLine::Kind::Insert, b[prefix + j], 0, prefix + j + 1});
            ++j;
        }
    }

    result.insert(result.end(), middle.begin(), middle.end());

    for (size_t k = 0; k < suffix; ++k) {
        const size_t index = a.size() - suffix + k;
        result.push_back({DiffLine::Kind::Context, a[index], index + 1, b.size() - suffix + k + 1});
    }
    return result;
}

std::string unified_diff(std::string_view before, std::string_view after,
                         std::string_view before_label, std::string_view after_label,
                         int context) {
    const std::vector<DiffLine> lines = diff_lines(before, after);
    bool has_change = false;
    for (const DiffLine& line : lines) {
        if (line.kind != DiffLine::Kind::Context) {
            has_change = true;
            break;
        }
    }
    if (!has_change) return {};

    if (context < 0) context = 0;

    // Determine which lines to print: every changed line plus `context` lines
    // around it, merging overlapping windows.
    std::vector<bool> keep(lines.size(), false);
    for (size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].kind == DiffLine::Kind::Context) continue;
        const size_t from = i > static_cast<size_t>(context) ? i - static_cast<size_t>(context) : 0;
        const size_t to = std::min(lines.size() - 1, i + static_cast<size_t>(context));
        for (size_t k = from; k <= to; ++k) keep[k] = true;
    }

    std::string out;
    out += "--- ";
    out += std::string(before_label);
    out += "\n+++ ";
    out += std::string(after_label);
    out += "\n";

    size_t i = 0;
    while (i < lines.size()) {
        if (!keep[i]) {
            ++i;
            continue;
        }
        // Start a hunk at i, extending while the next kept line is contiguous.
        size_t end = i;
        if (i > 0) {
            // A gap before this hunk is rendered as a skipped marker below.
        }
        size_t j = i;
        size_t last_kept = i;
        while (j < lines.size()) {
            if (keep[j]) {
                last_kept = j;
                ++j;
                continue;
            }
            // Allow small gaps inside one hunk.
            size_t gap = 0;
            size_t k = j;
            while (k < lines.size() && !keep[k]) {
                ++gap;
                ++k;
            }
            if (gap <= static_cast<size_t>(context) * 2 && k < lines.size()) {
                j = k;
                continue;
            }
            break;
        }
        end = last_kept;

        size_t old_start = 0, new_start = 0, old_count = 0, new_count = 0;
        for (size_t k = i; k <= end; ++k) {
            if (lines[k].kind != DiffLine::Kind::Insert) {
                if (old_start == 0) old_start = lines[k].old_line;
                ++old_count;
            }
            if (lines[k].kind != DiffLine::Kind::Delete) {
                if (new_start == 0) new_start = lines[k].new_line;
                ++new_count;
            }
        }
        // Unified-diff convention: a zero count is written as ",0", and when a
        // side has no lines at all its start is 0 (an insertion at the very top
        // has no old line 1 to anchor to). Getting this wrong makes `patch` and
        // `git apply` reject the hunk, which matters because these diffs are
        // handed to the user and stored in the audit log.
        if (old_count == 0) old_start = 0;
        if (new_count == 0) new_start = 0;
        char header[96];
        std::snprintf(header, sizeof(header), "@@ -%zu,%zu +%zu,%zu @@\n", old_start, old_count,
                      new_start, new_count);
        out += header;
        for (size_t k = i; k <= end; ++k) {
            const DiffLine& line = lines[k];
            const char marker = line.kind == DiffLine::Kind::Insert
                                    ? '+'
                                    : (line.kind == DiffLine::Kind::Delete ? '-' : ' ');
            out.push_back(marker);
            out.append(line.text.data(), line.text.size());
            out.push_back('\n');
        }
        i = end + 1;
    }
    return out;
}

Json diff_to_json(std::string_view before, std::string_view after) {
    const std::vector<DiffLine> lines = diff_lines(before, after);
    size_t added = 0;
    size_t removed = 0;
    size_t unchanged = 0;
    Json hunks = Json::array();
    for (const DiffLine& line : lines) {
        switch (line.kind) {
        case DiffLine::Kind::Insert: ++added; break;
        case DiffLine::Kind::Delete: ++removed; break;
        case DiffLine::Kind::Context: ++unchanged; break;
        }
        Json entry = Json::object();
        entry.set("op", Json(line.kind == DiffLine::Kind::Insert
                                 ? "add"
                                 : (line.kind == DiffLine::Kind::Delete ? "remove" : "context")));
        entry.set("text", Json(std::string(line.text)));
        if (line.old_line) entry.set("old_line", Json(static_cast<int64_t>(line.old_line)));
        if (line.new_line) entry.set("new_line", Json(static_cast<int64_t>(line.new_line)));
        hunks.push(std::move(entry));
    }
    Json out = Json::object();
    out.set("added_lines", Json(static_cast<int64_t>(added)));
    out.set("removed_lines", Json(static_cast<int64_t>(removed)));
    out.set("unchanged_lines", Json(static_cast<int64_t>(unchanged)));
    out.set("changed", Json(added > 0 || removed > 0));
    out.set("lines", std::move(hunks));
    // Byte delta helps an agent reason about budget impact.
    out.set("byte_delta", Json(static_cast<int64_t>(after.size()) - static_cast<int64_t>(before.size())));
    return out;
}

} // namespace colai
