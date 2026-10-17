// Colaimdation - Markdown block parser.
//
// The parser produces the *structure* an Agent needs in order to edit a
// document surgically: every block records its exact byte span in the UTF-8
// source, so a tool can target "the third paragraph" or "the fenced code block
// with language python" without the Agent ever reading or writing a file.
//
// Line-oriented CommonMark subset, deliberately small and dependency-free:
//   front matter, ATX/setext headings, fenced + indented code, block quotes,
//   bullet/ordered lists, tables, thematic breaks, HTML blocks, math blocks,
//   paragraphs.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.h"

namespace colai::md {

enum class BlockKind {
    FrontMatter,
    Heading,
    Paragraph,
    CodeBlock,
    Table,
    List,
    BlockQuote,
    ThematicBreak,
    HtmlBlock,
    MathBlock,
};

std::string_view kind_name(BlockKind kind);
BlockKind kind_from_name(std::string_view name); // unknown -> Paragraph

struct Block {
    BlockKind kind = BlockKind::Paragraph;
    // Half-open byte span of the whole block in the source document.
    size_t begin = 0;
    size_t end = 0;
    // Heading level (1..6) or list nesting depth (1-based) depending on kind.
    int level = 0;
    // Index of this block within its parent container (0-based).
    int ordinal = 0;
    // Fence info string / table alignment source. Never includes delimiters.
    std::string info;
    // Ordered list when true, bullet list when false.
    bool ordered = false;
    // Position of the first character of the block's *content*, and the
    // position just past the content. Lets tools address inner text directly.
    size_t content_begin = 0;
    size_t content_end = 0;

    size_t size() const { return end - begin; }
    std::string_view slice(std::string_view source) const {
        if (begin > source.size()) return {};
        return source.substr(begin, (end > source.size() ? source.size() : end) - begin);
    }
    std::string_view content(std::string_view source) const {
        if (content_begin > source.size()) return {};
        return source.substr(content_begin,
                             (content_end > source.size() ? source.size() : content_end) -
                                 content_begin);
    }
};

// A parsed document: the original text plus its block structure. Views in
// `blocks` point into `text`, so a ParsedDocument must outlive its uses.
struct ParsedDocument {
    std::string_view text;
    std::vector<Block> blocks;
    bool has_front_matter = false;
    // Markdown heading outline (level + title), useful for agents navigating.
    struct OutlineEntry {
        int level = 1;
        std::string title;
        size_t block_index = 0;
        size_t offset = 0;
    };
    std::vector<OutlineEntry> outline;
    size_t word_count = 0;
    size_t character_count = 0;

    // Index of the block containing `offset`, or the nearest preceding block.
    // Returns -1 for an empty document.
    int block_at(size_t offset) const;
    // Exact containing block only (no nearest-preceding fallback).
    int block_containing(size_t offset) const;
    const Block* block(size_t index) const;
};

ParsedDocument parse(std::string_view text);

// Block address used by the MCP tools. At most one of the fields is set; the
// first matching key wins in the order: index, offset, kind+ordinal, heading.
struct BlockAddress {
    bool has_index = false;
    int64_t index = -1;
    bool has_offset = false;
    int64_t offset = 0;
    // Match the n-th block (0-based) of a given kind.
    bool has_kind = false;
    BlockKind kind = BlockKind::Paragraph;
    int64_t kind_ordinal = 0;
    // Match the n-th heading of a given level (0-based); level 0 = any level.
    bool has_heading = false;
    int level = 0;
    int64_t heading_ordinal = 0;
};

Json block_to_json(const Block& block, std::string_view source, size_t index);

// ---------------------------------------------------------------------------
// Sub-structure extraction
// ---------------------------------------------------------------------------
struct TableCell {
    std::string text;
    size_t begin = 0;
    size_t end = 0;
};

struct TableData {
    std::vector<std::vector<TableCell>> rows;
    std::vector<std::string> alignments; // "left" | "center" | "right" | "none"
    int columns = 0;
    bool header = false;
};

TableData parse_table(const Block& block, std::string_view source);

struct ListItem {
    int depth = 0;      // 1-based nesting depth
    bool ordered = false;
    std::string marker; // "-", "*", "+", "1." ...
    std::string text;   // item text without the marker
    size_t begin = 0;   // start of the item line
    size_t end = 0;     // end of the item (including nested children)
    size_t text_begin = 0;
    size_t text_end = 0;
};

std::vector<ListItem> parse_list(const Block& block, std::string_view source);

// Inline markup spans, used for "find the inline code / link / math" tools.
struct InlineSpan {
    std::string kind; // "code" | "link" | "image" | "math_inline" | "math_block" | "strong" | "emphasis" | "strikethrough"
    std::string text; // inner text
    std::string target; // link/image destination, if any
    size_t begin = 0;
    size_t end = 0;
};

std::vector<InlineSpan> parse_inline(std::string_view source);

// Logical revision of the document structure, used for change detection.
std::string structure_fingerprint(const ParsedDocument& document);

} // namespace colai::md
