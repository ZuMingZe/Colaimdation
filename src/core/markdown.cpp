#include "core/markdown.h"

#include "core/utf8.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>

namespace colai::md {
namespace {

struct Line {
    size_t begin = 0; // offset of first character
    size_t end = 0;   // offset just past the content (excludes '\n' and '\r')
    size_t full_end = 0; // offset just past the terminating newline
};

bool is_space(char c) { return c == ' ' || c == '\t'; }

std::string_view rstrip(std::string_view text) {
    size_t end = text.size();
    while (end > 0 && is_space(text[end - 1])) --end;
    return text.substr(0, end);
}

std::string_view lstrip(std::string_view text) {
    size_t begin = 0;
    while (begin < text.size() && is_space(text[begin])) ++begin;
    return text.substr(begin);
}

bool starts_with(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

std::string_view trim_view(std::string_view text) { return rstrip(lstrip(text)); }

bool is_blank(std::string_view text) {
    for (char c : text) {
        if (!is_space(c) && c != '\r') return false;
    }
    return true;
}

size_t indent_of(std::string_view text) {
    size_t count = 0;
    for (char c : text) {
        if (c == ' ') ++count;
        else if (c == '\t') count += 4;
        else break;
    }
    return count;
}

// A thematic break: three or more of -, *, _ with optional spaces.
bool is_thematic_break(std::string_view line) {
    std::string_view t = trim_view(line);
    if (t.size() < 3) return false;
    const char marker = t[0];
    if (marker != '-' && marker != '*' && marker != '_') return false;
    size_t count = 0;
    for (char c : t) {
        if (c == marker) ++count;
        else if (!is_space(c)) return false;
    }
    return count >= 3;
}

struct HeadingPrefix {
    int level = 0;
    size_t content_begin = 0;
};

HeadingPrefix heading_prefix(std::string_view line) {
    HeadingPrefix out;
    size_t i = 0;
    while (i < line.size() && i < 4 && line[i] == ' ') ++i;
    if (i >= line.size() || line[i] != '#') return out;
    size_t hashes = 0;
    while (i + hashes < line.size() && line[i + hashes] == '#') ++hashes;
    if (hashes > 6) return out;
    if (i + hashes < line.size() && !is_space(line[i + hashes])) return out; // "#tag"
    out.level = static_cast<int>(hashes);
    size_t content = i + hashes;
    while (content < line.size() && is_space(line[content])) ++content;
    out.content_begin = content;
    return out;
}

bool is_setext_underline(std::string_view line, char& marker) {
    std::string_view t = rstrip(line);
    if (t.empty()) return false;
    if (t[0] != '=' && t[0] != '-') return false;
    for (char c : t) {
        if (c != t[0]) return false;
    }
    marker = t[0];
    return true;
}

struct ListMarker {
    bool matched = false;
    bool ordered = false;
    size_t marker_start = 0;
    size_t content_begin = 0;
    int marker_length = 0;
    std::string marker_text;
};

ListMarker list_marker(std::string_view line) {
    ListMarker out;
    size_t i = 0;
    while (i < line.size() && i < 8 && line[i] == ' ') ++i;
    if (i >= line.size()) return out;
    const char c = line[i];
    if (c == '-' || c == '*' || c == '+') {
        if (i + 1 < line.size() && !is_space(line[i + 1]) && line[i + 1] != '\0') return out;
        out.matched = true;
        out.ordered = false;
        out.marker_start = i;
        out.marker_length = 1;
        out.marker_text = std::string(1, c);
        size_t content = i + 1;
        while (content < line.size() && is_space(line[content])) ++content;
        out.content_begin = content;
        return out;
    }
    if (c >= '0' && c <= '9') {
        size_t j = i;
        while (j < line.size() && line[j] >= '0' && line[j] <= '9' && j - i < 9) ++j;
        if (j < line.size() && (line[j] == '.' || line[j] == ')')) {
            if (j + 1 >= line.size() || is_space(line[j + 1])) {
                out.matched = true;
                out.ordered = true;
                out.marker_start = i;
                out.marker_length = static_cast<int>(j - i + 1);
                out.marker_text = std::string(line.substr(i, j - i + 1));
                size_t content = j + 1;
                while (content < line.size() && is_space(line[content])) ++content;
                out.content_begin = content;
            }
        }
    }
    return out;
}

// Split a table row on unescaped pipes.
std::vector<TableCell> split_table_row(std::string_view line, size_t line_begin) {
    std::vector<TableCell> cells;
    size_t i = 0;
    // Skip leading pipe / whitespace.
    while (i < line.size() && is_space(line[i])) ++i;
    if (i < line.size() && line[i] == '|') ++i;
    size_t cell_start = i;
    auto flush = [&](size_t cell_end) {
        size_t begin = cell_start;
        while (begin < cell_end && is_space(line[begin])) ++begin;
        size_t end = cell_end;
        while (end > begin && is_space(line[end - 1])) --end;
        TableCell cell;
        cell.text = std::string(line.substr(begin, end - begin));
        cell.begin = line_begin + begin;
        cell.end = line_begin + end;
        cells.push_back(std::move(cell));
    };
    while (i < line.size()) {
        const char c = line[i];
        if (c == '\\' && i + 1 < line.size()) {
            i += 2;
            continue;
        }
        if (c == '|') {
            // A trailing pipe closes the row instead of starting another cell.
            bool only_space = true;
            for (size_t k = i + 1; k < line.size(); ++k) {
                if (!is_space(line[k])) {
                    only_space = false;
                    break;
                }
            }
            flush(i);
            if (only_space) return cells;
            cell_start = i + 1;
        }
        ++i;
    }
    if (cell_start < line.size() || cells.empty()) flush(line.size());
    return cells;
}

std::string alignment_of(std::string_view cell) {
    std::string_view t = trim_view(cell);
    if (t.empty()) return "none";
    const bool left = t.front() == ':';
    const bool right = t.back() == ':';
    if (left && right) return "center";
    if (left) return "left";
    if (right) return "right";
    return "none";
}

bool is_table_separator(const std::vector<TableCell>& cells) {
    if (cells.empty()) return false;
    for (const auto& cell : cells) {
        std::string_view t = trim_view(cell.text);
        if (t.empty()) return false;
        size_t dashes = 0;
        for (size_t i = 0; i < t.size(); ++i) {
            const char c = t[i];
            if (c == '-') {
                ++dashes;
            } else if (c == ':') {
                if (i != 0 && i + 1 != t.size()) return false;
            } else if (is_space(c)) {
                // allow padding
            } else {
                return false;
            }
        }
        if (dashes == 0) return false;
    }
    return true;
}

bool block_opener(const std::vector<Line>& lines, size_t index, std::string_view text) {
    const std::string_view line = text.substr(lines[index].begin, lines[index].end - lines[index].begin);
    if (is_blank(line)) return true;
    if (heading_prefix(line).level > 0) return true;
    if (is_thematic_break(line)) return true;
    if (list_marker(line).matched) return true;
    if (!trim_view(line).empty() && trim_view(line)[0] == '>') return true;
    const std::string_view t = trim_view(line);
    if (starts_with(t, "```") || starts_with(t, "~~~")) return true;
    if (starts_with(t, "$$")) return true;
    if (indent_of(line) >= 4 && !t.empty()) return true;
    // A table starts one line before its separator.
    if (index + 1 < lines.size()) {
        const std::string_view next =
            text.substr(lines[index + 1].begin, lines[index + 1].end - lines[index + 1].begin);
        if (line.find('|') != std::string_view::npos &&
            is_table_separator(split_table_row(next, lines[index + 1].begin))) {
            return true;
        }
    }
    return false;
}

bool html_block_start(std::string_view line, std::string& tag) {
    std::string_view t = lstrip(line);
    if (t.size() < 2 || t[0] != '<') return false;
    size_t i = 1;
    if (t[i] == '!' || t[i] == '?') {
        tag.clear();
        return true;
    }
    if (t[i] == '/') ++i;
    if (i >= t.size() || !std::isalpha(static_cast<unsigned char>(t[i]))) return false;
    size_t start = i;
    while (i < t.size() && (std::isalnum(static_cast<unsigned char>(t[i])) || t[i] == '-')) ++i;
    tag = utf8::to_lower(t.substr(start, i - start));
    return true;
}

// ---------------------------------------------------------------------------
// Line splitting (records both content and newline extents)
// ---------------------------------------------------------------------------
std::vector<Line> index_lines(std::string_view text) {
    std::vector<Line> lines;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        const size_t line_end = (nl == std::string_view::npos) ? text.size() : nl;
        Line line;
        line.begin = pos;
        line.end = line_end;
        if (line.end > line.begin && text[line.end - 1] == '\r') --line.end;
        line.full_end = (nl == std::string_view::npos) ? text.size() : nl + 1;
        lines.push_back(line);
        if (nl == std::string_view::npos) break;
        pos = nl + 1;
    }
    if (!lines.empty()) {
        const Line& last = lines.back();
        if (last.begin == text.size() && last.end == text.size()) {
            // Trailing empty line produced by a final newline: drop it.
            lines.pop_back();
        }
    }
    return lines;
}

} // namespace

std::string_view kind_name(BlockKind kind) {
    switch (kind) {
    case BlockKind::FrontMatter: return "front_matter";
    case BlockKind::Heading: return "heading";
    case BlockKind::Paragraph: return "paragraph";
    case BlockKind::CodeBlock: return "code_block";
    case BlockKind::Table: return "table";
    case BlockKind::List: return "list";
    case BlockKind::BlockQuote: return "block_quote";
    case BlockKind::ThematicBreak: return "thematic_break";
    case BlockKind::HtmlBlock: return "html_block";
    case BlockKind::MathBlock: return "math_block";
    }
    return "paragraph";
}

BlockKind kind_from_name(std::string_view name) {
    const std::string lower = utf8::to_lower(name);
    if (lower == "front_matter" || lower == "frontmatter" || lower == "yaml") return BlockKind::FrontMatter;
    if (lower == "heading" || lower == "header" || lower == "title") return BlockKind::Heading;
    if (lower == "paragraph" || lower == "para" || lower == "text") return BlockKind::Paragraph;
    if (lower == "code_block" || lower == "code" || lower == "fence") return BlockKind::CodeBlock;
    if (lower == "table") return BlockKind::Table;
    if (lower == "list" || lower == "bullet_list" || lower == "ordered_list") return BlockKind::List;
    if (lower == "block_quote" || lower == "quote" || lower == "blockquote") return BlockKind::BlockQuote;
    if (lower == "thematic_break" || lower == "hr" || lower == "rule") return BlockKind::ThematicBreak;
    if (lower == "html_block" || lower == "html" || lower == "raw_html") return BlockKind::HtmlBlock;
    if (lower == "math_block" || lower == "math" || lower == "formula") return BlockKind::MathBlock;
    return BlockKind::Paragraph;
}

int ParsedDocument::block_at(size_t offset) const {
    if (blocks.empty()) return -1;
    if (offset <= blocks.front().begin) return 0;
    int best = -1;
    for (size_t i = 0; i < blocks.size(); ++i) {
        if (blocks[i].begin <= offset) {
            best = static_cast<int>(i);
        } else {
            break;
        }
        if (blocks[i].begin <= offset && offset < blocks[i].end) return static_cast<int>(i);
    }
    return best;
}

int ParsedDocument::block_containing(size_t offset) const {
    for (size_t i = 0; i < blocks.size(); ++i) {
        if (blocks[i].begin <= offset && offset < blocks[i].end) return static_cast<int>(i);
    }
    return -1;
}

const Block* ParsedDocument::block(size_t index) const {
    return index < blocks.size() ? &blocks[index] : nullptr;
}

// ---------------------------------------------------------------------------
// Block parsing
// ---------------------------------------------------------------------------
ParsedDocument parse(std::string_view text) {
    ParsedDocument document;
    document.text = text;
    document.character_count = utf8::length(text);

    const std::vector<Line> lines = index_lines(text);
    auto line_view = [&](size_t i) {
        return text.substr(lines[i].begin, lines[i].end - lines[i].begin);
    };
    auto line_full = [&](size_t i) {
        return text.substr(lines[i].begin, lines[i].full_end - lines[i].begin);
    };

    size_t i = 0;

    // --- front matter -------------------------------------------------------
    if (!lines.empty()) {
        const std::string_view first = trim_view(line_view(0));
        if (first == "---" || first == "+++") {
            const std::string closing = first == "---" ? "---" : "+++";
            for (size_t j = 1; j < lines.size(); ++j) {
                const std::string_view candidate = trim_view(line_view(j));
                if (candidate == closing || (closing == "---" && candidate == "...")) {
                    Block block;
                    block.kind = BlockKind::FrontMatter;
                    block.begin = lines[0].begin;
                    block.end = lines[j].full_end;
                    block.content_begin = lines[0].full_end;
                    block.content_end = lines[j].begin;
                    block.info = closing;
                    document.has_front_matter = true;
                    document.blocks.push_back(std::move(block));
                    i = j + 1;
                    break;
                }
                // Bail out early if the block is not terminated nearby.
                if (j > 512) break;
            }
        }
    }

    // --- main loop ----------------------------------------------------------
    while (i < lines.size()) {
        const std::string_view raw_line = line_view(i);
        if (is_blank(raw_line)) {
            ++i;
            continue;
        }

        // Fenced code block
        {
            const std::string_view t = trim_view(raw_line);
            if (starts_with(t, "```") || starts_with(t, "~~~")) {
                const char fence = t[0];
                size_t fence_len = 0;
                while (fence_len < t.size() && t[fence_len] == fence) ++fence_len;
                std::string info = utf8::trim(t.substr(fence_len));
                // Keep only the language token for the fence info.
                for (size_t k = 0; k < info.size(); ++k) {
                    if (is_space(info[k])) {
                        info = info.substr(0, k);
                        break;
                    }
                }
                Block block;
                block.kind = BlockKind::CodeBlock;
                block.begin = lines[i].begin;
                block.content_begin = lines[i].full_end;
                block.info = info;
                size_t j = i + 1;
                for (; j < lines.size(); ++j) {
                    const std::string_view candidate = trim_view(line_view(j));
                    if (candidate.size() >= fence_len) {
                        size_t run = 0;
                        while (run < candidate.size() && candidate[run] == fence) ++run;
                        if (run >= fence_len && trim_view(candidate.substr(run)).empty()) break;
                    }
                }
                if (j < lines.size()) {
                    block.content_end = lines[j].begin;
                    block.end = lines[j].full_end;
                    i = j + 1;
                } else {
                    block.content_end = lines.empty() ? text.size() : lines.back().full_end;
                    block.end = text.size();
                    i = lines.size();
                }
                document.blocks.push_back(std::move(block));
                continue;
            }
        }

        // Math block
        {
            const std::string_view t = trim_view(raw_line);
            if (starts_with(t, "$$")) {
                const bool inline_close = t.size() > 2 && t.substr(2).find("$$") != std::string_view::npos;
                if (!inline_close) {
                    Block block;
                    block.kind = BlockKind::MathBlock;
                    block.begin = lines[i].begin;
                    block.content_begin = lines[i].full_end;
                    size_t j = i + 1;
                    for (; j < lines.size(); ++j) {
                        if (trim_view(line_view(j)).find("$$") != std::string_view::npos) break;
                    }
                    if (j < lines.size()) {
                        block.content_end = lines[j].begin;
                        block.end = lines[j].full_end;
                        i = j + 1;
                    } else {
                        block.content_end = lines.back().full_end;
                        block.end = text.size();
                        i = lines.size();
                    }
                    document.blocks.push_back(std::move(block));
                    continue;
                }
            }
        }

        // ATX heading
        {
            const HeadingPrefix prefix = heading_prefix(raw_line);
            if (prefix.level > 0) {
                Block block;
                block.kind = BlockKind::Heading;
                block.level = prefix.level;
                block.begin = lines[i].begin;
                block.content_begin = lines[i].begin + prefix.content_begin;
                std::string_view content = raw_line.substr(prefix.content_begin);
                content = rstrip(content);
                // Strip an optional closing run of '#'
                size_t hashes = 0;
                while (hashes < content.size() && content[content.size() - 1 - hashes] == '#') ++hashes;
                if (hashes > 0 && content.size() > hashes && is_space(content[content.size() - hashes - 1])) {
                    content = rstrip(content.substr(0, content.size() - hashes));
                }
                block.content_end = block.content_begin + content.size();
                block.end = lines[i].full_end;
                document.blocks.push_back(std::move(block));
                ++i;
                continue;
            }
        }

        // Thematic break
        if (is_thematic_break(raw_line)) {
            Block block;
            block.kind = BlockKind::ThematicBreak;
            block.begin = lines[i].begin;
            block.end = lines[i].full_end;
            block.content_begin = block.begin;
            block.content_end = lines[i].begin + rstrip(raw_line).size();
            document.blocks.push_back(std::move(block));
            ++i;
            continue;
        }

        // Block quote
        if (!trim_view(raw_line).empty() && trim_view(raw_line)[0] == '>') {
            Block block;
            block.kind = BlockKind::BlockQuote;
            block.begin = lines[i].begin;
            size_t j = i;
            size_t content_begin = 0;
            size_t content_end = 0;
            while (j < lines.size()) {
                std::string_view candidate = line_view(j);
                std::string_view tc = lstrip(candidate);
                if (tc.empty()) {
                    // A blank line continues the quote only if the next line is quoted.
                    if (j + 1 < lines.size() && starts_with(lstrip(line_view(j + 1)), ">")) {
                        ++j;
                        continue;
                    }
                    break;
                }
                if (tc[0] != '>') break;
                size_t offset = lines[j].begin + (candidate.size() - tc.size());
                if (offset + 1 < lines[j].end && is_space(text[offset + 1])) ++offset;
                size_t text_start = offset + 1;
                while (text_start < lines[j].end && is_space(text[text_start])) ++text_start;
                if (content_begin == 0) content_begin = text_start;
                content_end = lines[j].begin + rstrip(candidate).size();
                ++j;
            }
            block.end = lines[j - 1].full_end;
            block.content_begin = content_begin ? content_begin : block.begin;
            block.content_end = std::max(content_end, block.content_begin);
            document.blocks.push_back(std::move(block));
            i = j;
            continue;
        }

        // Table
        if (i + 1 < lines.size() && raw_line.find('|') != std::string_view::npos) {
            std::vector<TableCell> header_cells = split_table_row(raw_line, lines[i].begin);
            std::vector<TableCell> separator_cells =
                split_table_row(line_view(i + 1), lines[i + 1].begin);
            if (!header_cells.empty() && is_table_separator(separator_cells)) {
                Block block;
                block.kind = BlockKind::Table;
                block.begin = lines[i].begin;
                block.content_begin = lines[i].begin;
                // Alignment descriptors from the separator row.
                std::string alignment_json = "[";
                for (size_t c = 0; c < separator_cells.size(); ++c) {
                    if (c) alignment_json += ",";
                    alignment_json += "\"" + alignment_of(separator_cells[c].text) + "\"";
                }
                alignment_json += "]";
                block.info = alignment_json;
                size_t j = i + 2;
                size_t content_end = lines[i + 1].full_end;
                for (; j < lines.size(); ++j) {
                    const std::string_view candidate = line_view(j);
                    if (is_blank(candidate) || candidate.find('|') == std::string_view::npos) break;
                    content_end = lines[j].full_end;
                }
                block.content_end = content_end;
                block.end = (j > 0) ? lines[j - 1].full_end : content_end;
                document.blocks.push_back(std::move(block));
                i = j;
                continue;
            }
        }

        // List
        {
            const ListMarker opener = list_marker(raw_line);
            if (!opener.matched) {
                // fall through to the remaining block kinds below
            } else {
                Block block;
                block.kind = BlockKind::List;
                block.begin = lines[i].begin;
                block.ordered = opener.ordered;
                block.info = opener.marker_text;
                size_t j = i;
                size_t content_begin = 0;
                size_t content_end = 0;
                int max_depth = 1;
                while (j < lines.size()) {
                    const std::string_view candidate = line_view(j);
                    if (is_blank(candidate)) {
                        // A blank line ends the list unless the next non-blank
                        // line is either the same kind of list item or clearly
                        // indented as nested content. A *different* list kind
                        // after a blank line starts a new list block, which is
                        // what authors expect and what keeps block addressing
                        // stable.
                        size_t next = j + 1;
                        while (next < lines.size() && is_blank(line_view(next))) ++next;
                        if (next < lines.size()) {
                            const std::string_view next_line = line_view(next);
                            const ListMarker next_marker = list_marker(next_line);
                            if (next_marker.matched && next_marker.ordered == block.ordered) {
                                j = next;
                                continue;
                            }
                            if (indent_of(next_line) >= 6) {
                                j = next;
                                continue;
                            }
                        }
                        break;
                    }
                    const ListMarker marker = list_marker(candidate);
                    if (marker.matched) {
                        const size_t indent = indent_of(candidate);
                        const int depth = static_cast<int>(indent / 2) + 1;
                        max_depth = std::max(max_depth, depth);
                        if (content_begin == 0) content_begin = lines[j].begin + marker.content_begin;
                        content_end = lines[j].begin + rstrip(candidate).size();
                    } else if (indent_of(candidate) >= 6) {
                        // Continuation of the previous item's nested content.
                        content_end = lines[j].begin + rstrip(candidate).size();
                    } else {
                        break;
                    }
                    ++j;
                }
                block.level = max_depth;
                block.end = lines[j - 1].full_end;
                block.content_begin = content_begin ? content_begin : block.begin;
                block.content_end = std::max(content_end, block.content_begin);
                document.blocks.push_back(std::move(block));
                i = j;
                continue;
            }
        }

        // Indented code block
        if (indent_of(raw_line) >= 4) {
            Block block;
            block.kind = BlockKind::CodeBlock;
            block.begin = lines[i].begin;
            block.content_begin = lines[i].begin + 4;
            block.info = "";
            size_t j = i;
            size_t content_end = lines[i].end;
            while (j < lines.size()) {
                const std::string_view candidate = line_view(j);
                if (is_blank(candidate)) {
                    ++j;
                    continue;
                }
                if (indent_of(candidate) < 4) break;
                content_end = lines[j].end;
                ++j;
            }
            block.content_end = content_end;
            block.end = lines[j > 0 ? j - 1 : 0].full_end;
            document.blocks.push_back(std::move(block));
            i = j;
            continue;
        }

        // HTML block
        {
            std::string tag;
            if (html_block_start(raw_line, tag)) {
                Block block;
                block.kind = BlockKind::HtmlBlock;
                block.info = tag;
                block.begin = lines[i].begin;
                block.content_begin = lines[i].begin;
                const bool verbatim = (tag == "pre" || tag == "script" || tag == "style" ||
                                       tag == "textarea");
                size_t j = i;
                size_t content_end = lines[i].end;
                if (verbatim) {
                    const std::string closing = "</" + tag;
                    while (j < lines.size()) {
                        content_end = lines[j].end;
                        ++j;
                        if (utf8::to_lower(line_view(j - 1)).find(closing) != std::string::npos) break;
                    }
                } else {
                    ++j;
                    while (j < lines.size() && !is_blank(line_view(j))) {
                        content_end = lines[j].end;
                        ++j;
                    }
                }
                block.content_end = content_end;
                block.end = lines[j - 1].full_end;
                document.blocks.push_back(std::move(block));
                i = j;
                continue;
            }
        }

        // Paragraph (or setext heading)
        {
            Block block;
            block.kind = BlockKind::Paragraph;
            block.begin = lines[i].begin;
            block.content_begin = lines[i].begin;
            size_t j = i;
            size_t content_end = lines[i].end;
            int setext_level = 0;
            while (j + 1 < lines.size()) {
                const std::string_view next = line_view(j + 1);
                char marker = 0;
                if (is_setext_underline(next, marker) && !is_blank(line_view(j))) {
                    setext_level = (marker == '=') ? 1 : 2;
                    content_end = lines[j].end;
                    ++j;
                    break;
                }
                if (block_opener(lines, j + 1, text)) break;
                content_end = lines[j + 1].end;
                ++j;
            }
            if (setext_level > 0) {
                block.kind = BlockKind::Heading;
                block.level = setext_level;
            }
            block.content_end = content_end;
            block.end = lines[j].full_end;
            document.blocks.push_back(std::move(block));
            i = j + 1;
        }
    }

    // Ordinals per kind, and the heading outline.
    {
        std::map<int, int> kind_counters;
        for (size_t index = 0; index < document.blocks.size(); ++index) {
            Block& block = document.blocks[index];
            block.ordinal = kind_counters[static_cast<int>(block.kind)]++;
            if (block.kind == BlockKind::Heading) {
                ParsedDocument::OutlineEntry entry;
                entry.level = block.level;
                const std::string_view content = block.content(text);
                entry.title = std::string(content);
                entry.block_index = index;
                entry.offset = block.content_begin;
                document.outline.push_back(std::move(entry));
            }
        }
    }

    // Word count: whitespace-delimited tokens that contain a letter or digit.
    {
        size_t count = 0;
        bool in_word = false;
        bool has_alnum = false;
        auto flush = [&]() {
            if (in_word && has_alnum) ++count;
            in_word = false;
            has_alnum = false;
        };
        for (size_t k = 0; k < text.size();) {
            const unsigned char c = static_cast<unsigned char>(text[k]);
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                flush();
                ++k;
                continue;
            }
            in_word = true;
            if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c >= 0x80) {
                has_alnum = true;
            }
            k += static_cast<size_t>(utf8::sequence_length(c));
        }
        flush();
        document.word_count = count;
    }

    return document;
}

Json block_to_json(const Block& block, std::string_view source, size_t index) {
    Json out = Json::object();
    out.set("block_id", Json("b" + std::to_string(index)));
    out.set("index", Json(static_cast<int64_t>(index)));
    out.set("kind", Json(std::string(kind_name(block.kind))));
    out.set("ordinal", Json(static_cast<int64_t>(block.ordinal)));
    out.set("byte_start", Json(static_cast<int64_t>(block.begin)));
    out.set("byte_end", Json(static_cast<int64_t>(block.end)));
    out.set("content_byte_start", Json(static_cast<int64_t>(block.content_begin)));
    out.set("content_byte_end", Json(static_cast<int64_t>(block.content_end)));
    out.set("start_line", Json(static_cast<int64_t>(utf8::line_number_at(source, block.begin))));
    out.set("end_line", Json(static_cast<int64_t>(utf8::line_number_at(source, block.end))));
    out.set("character_count", Json(static_cast<int64_t>(utf8::length(block.slice(source)))));
    if (block.kind == BlockKind::Heading) {
        out.set("level", Json(static_cast<int64_t>(block.level)));
        out.set("text", Json(std::string(block.content(source))));
    }
    if (block.kind == BlockKind::CodeBlock || block.kind == BlockKind::HtmlBlock) {
        if (!block.info.empty()) out.set("info", Json(block.info));
        out.set("code", Json(std::string(block.content(source))));
    }
    if (block.kind == BlockKind::List) {
        out.set("ordered", Json(block.ordered));
        out.set("marker", Json(block.info));
        out.set("depth", Json(static_cast<int64_t>(block.level)));
        Json items = Json::array();
        for (const auto& item : parse_list(block, source)) {
            Json entry = Json::object();
            entry.set("depth", Json(static_cast<int64_t>(item.depth)));
            entry.set("ordered", Json(item.ordered));
            entry.set("marker", Json(item.marker));
            entry.set("text", Json(item.text));
            entry.set("byte_start", Json(static_cast<int64_t>(item.begin)));
            entry.set("byte_end", Json(static_cast<int64_t>(item.end)));
            entry.set("text_byte_start", Json(static_cast<int64_t>(item.text_begin)));
            entry.set("text_byte_end", Json(static_cast<int64_t>(item.text_end)));
            items.push(std::move(entry));
        }
        out.set("items", std::move(items));
    }
    if (block.kind == BlockKind::Table) {
        Json include_cells = Json::object();
        include_cells.set("header", Json(std::string(block.slice(source))));
        const TableData data = parse_table(block, source);
        include_cells.set("rows", Json(static_cast<int64_t>(data.rows.size())));
        include_cells.set("columns", Json(static_cast<int64_t>(data.columns)));
        Json alignments = Json::array();
        for (const auto& alignment : data.alignments) alignments.push(Json(alignment));
        include_cells.set("alignments", std::move(alignments));
        out.set("table", std::move(include_cells));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Tables
// ---------------------------------------------------------------------------
TableData parse_table(const Block& block, std::string_view source) {
    TableData data;
    if (block.kind != BlockKind::Table) return data;

    // Alignment row is already stored as a compact JSON array of strings.
    if (!block.info.empty()) {
        JsonParseError error;
        if (auto parsed = json_parse(block.info, &error); parsed && parsed->is_array()) {
            for (const auto& value : parsed->items()) data.alignments.push_back(value.as_string("none"));
        }
    }

    const std::string_view body = block.slice(source);
    const std::vector<Line> lines = index_lines(body);
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string_view line = body.substr(lines[i].begin, lines[i].end - lines[i].begin);
        if (i == 1) continue; // separator row
        if (is_blank(line)) continue;
        std::vector<TableCell> cells = split_table_row(line, block.begin + lines[i].begin);
        data.rows.push_back(std::move(cells));
    }
    data.header = data.rows.size() > 1;
    for (const auto& row : data.rows) {
        data.columns = std::max(data.columns, static_cast<int>(row.size()));
    }
    while (static_cast<int>(data.alignments.size()) < data.columns) data.alignments.push_back("none");
    return data;
}

// ---------------------------------------------------------------------------
// Lists
// ---------------------------------------------------------------------------
std::vector<ListItem> parse_list(const Block& block, std::string_view source) {
    std::vector<ListItem> items;
    if (block.kind != BlockKind::List) return items;
    const std::string_view body = block.slice(source);
    const std::vector<Line> lines = index_lines(body);
    ListItem* current = nullptr;
    for (const Line& line : lines) {
        const std::string_view view = body.substr(line.begin, line.end - line.begin);
        if (is_blank(view)) continue;
        const ListMarker marker = list_marker(view);
        if (marker.matched) {
            ListItem item;
            item.ordered = marker.ordered;
            item.marker = marker.marker_text;
            item.depth = static_cast<int>(indent_of(view) / 2) + 1;
            item.begin = block.begin + line.begin;
            item.end = block.begin + line.full_end;
            item.text_begin = block.begin + line.begin + marker.content_begin;
            item.text_end = block.begin + line.end;
            item.text = std::string(rstrip(view.substr(marker.content_begin)));
            items.push_back(std::move(item));
            current = &items.back();
        } else if (current) {
            // Continuation line belongs to the current item.
            const std::string_view stripped = trim_view(view);
            if (!stripped.empty()) {
                if (!current->text.empty()) current->text += "\n";
                current->text += std::string(stripped);
            }
            current->end = block.begin + line.full_end;
            current->text_end = block.begin + line.end;
        }
    }
    return items;
}

// ---------------------------------------------------------------------------
// Inline spans
// ---------------------------------------------------------------------------
std::vector<InlineSpan> parse_inline(std::string_view source) {
    std::vector<InlineSpan> spans;
    const size_t n = source.size();
    size_t i = 0;

    auto push = [&](std::string kind, size_t begin, size_t inner_begin, size_t inner_end, size_t end,
                    std::string target = {}) {
        InlineSpan span;
        span.kind = std::move(kind);
        span.begin = begin;
        span.end = end;
        span.text = std::string(source.substr(inner_begin, inner_end - inner_begin));
        span.target = std::move(target);
        spans.push_back(std::move(span));
    };

    while (i < n) {
        const char c = source[i];

        // Fenced or indented code regions are still scanned; fenced code is
        // reported as a single code span from fence to fence.
        if (c == '`') {
            size_t run = 0;
            while (i + run < n && source[i + run] == '`') ++run;
            const std::string fence(run, '`');
            const size_t search_from = i + run;
            const size_t close = source.find(fence, search_from);
            if (close != std::string_view::npos) {
                push("code", i, search_from, close, close + run);
                i = close + run;
                continue;
            }
            i += run;
            continue;
        }

        // Images and links
        if (c == '!' && i + 1 < n && source[i + 1] == '[') {
            const size_t label_end = source.find(']', i + 2);
            if (label_end != std::string_view::npos && label_end + 1 < n && source[label_end + 1] == '(') {
                const size_t target_end = source.find(')', label_end + 2);
                if (target_end != std::string_view::npos) {
                    push("image", i, i + 2, label_end, target_end + 1,
                         std::string(source.substr(label_end + 2, target_end - label_end - 2)));
                    i = target_end + 1;
                    continue;
                }
            }
        }
        if (c == '[') {
            const size_t label_end = source.find(']', i + 1);
            if (label_end != std::string_view::npos && label_end + 1 < n && source[label_end + 1] == '(') {
                const size_t target_end = source.find(')', label_end + 2);
                if (target_end != std::string_view::npos) {
                    push("link", i, i + 1, label_end, target_end + 1,
                         std::string(source.substr(label_end + 2, target_end - label_end - 2)));
                    i = target_end + 1;
                    continue;
                }
            }
        }

        // Math: $$ ... $$ then $ ... $
        if (c == '$') {
            const bool double_dollar = (i + 1 < n && source[i + 1] == '$');
            const std::string delimiter = double_dollar ? "$$" : "$";
            const size_t inner_begin = i + delimiter.size();
            const size_t close = source.find(delimiter, inner_begin);
            if (close != std::string_view::npos && close > inner_begin) {
                // A single '$' must not be followed by a space to count as math.
                const bool ok = double_dollar || (inner_begin < n && source[inner_begin] != ' ');
                if (ok) {
                    push(double_dollar ? "math_block" : "math_inline", i, inner_begin, close,
                         close + delimiter.size());
                    i = close + delimiter.size();
                    continue;
                }
            }
            ++i;
            continue;
        }

        // Strong / emphasis / strikethrough
        if (c == '*' || c == '_' || c == '~') {
            const bool double_marker = (i + 1 < n && source[i + 1] == c);
            const std::string delimiter = double_marker ? std::string(2, c) : std::string(1, c);
            const bool strikethrough = (c == '~' && double_marker);
            const size_t inner_begin = i + delimiter.size();
            const size_t close = source.find(delimiter, inner_begin);
            if (close != std::string_view::npos && close > inner_begin) {
                const char* kind = strikethrough ? "strikethrough"
                                                 : (double_marker ? "strong" : "emphasis");
                push(kind, i, inner_begin, close, close + delimiter.size());
                i = close + delimiter.size();
                continue;
            }
        }

        i += static_cast<size_t>(utf8::sequence_length(static_cast<unsigned char>(c)));
    }
    return spans;
}

std::string structure_fingerprint(const ParsedDocument& document) {
    // Layout identity, not content identity: byte offsets are deliberately
    // excluded so that editing a paragraph's text does not change the
    // fingerprint, while adding or removing a block does.
    std::string buffer;
    buffer.reserve(document.blocks.size() * 24);
    for (const Block& block : document.blocks) {
        buffer += kind_name(block.kind);
        buffer.push_back('/');
        buffer += std::to_string(block.level);
        buffer.push_back('/');
        buffer += std::to_string(block.ordinal);
        buffer.push_back('/');
        buffer += std::to_string(block.info.size());
        buffer.push_back(';');
    }
    return utf8::sha256_hex(buffer);
}

} // namespace colai::md
