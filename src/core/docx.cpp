#include "core/docx.h"

#include "core/markdown.h"
#include "core/utf8.h"
#include "core/zip_writer.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace colai::docx {
namespace {

// ---------------------------------------------------------------------------
// XML helpers
// ---------------------------------------------------------------------------

// Text for an XML text node: the five entities, and none of the control characters
// XML forbids.
std::string xml_text(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 16);
    for (const char raw : text) {
        const unsigned char byte = static_cast<unsigned char>(raw);
        switch (raw) {
        case '&': out += "&amp;"; continue;
        case '<': out += "&lt;"; continue;
        case '>': out += "&gt;"; continue;
        case '"': out += "&quot;"; continue;
        case '\'': out += "&apos;"; continue;
        case '\t': out += "&#9;"; continue;
        default: break;
        }
        if (byte < 0x20 && raw != '\n' && raw != '\r') continue;
        out.push_back(raw);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Runs and paragraphs
// ---------------------------------------------------------------------------

struct RunStyle {
    bool bold = false;
    bool italic = false;
    bool strike = false;
    bool mono = false;
    bool shaded = false;   // inline code and code-block lines
    int half_points = 0;   // 0 = the body size (22 half-points = 11pt)
    std::string color;     // empty = automatic
};

std::string run_properties(const RunStyle& style) {
    std::string out;
    if (style.mono) {
        out += "<w:rFonts w:ascii=\"Consolas\" w:hAnsi=\"Consolas\" w:cs=\"Consolas\"/>";
    }
    if (style.bold) out += "<w:b/>";
    if (style.italic) out += "<w:i/>";
    if (style.strike) out += "<w:strike/>";
    if (!style.color.empty()) out += "<w:color w:val=\"" + style.color + "\"/>";
    if (style.half_points > 0) {
        out += "<w:sz w:val=\"" + std::to_string(style.half_points) + "\"/>";
        out += "<w:szCs w:val=\"" + std::to_string(style.half_points) + "\"/>";
    }
    if (style.shaded) out += "<w:shd w:val=\"clear\" w:color=\"auto\" w:fill=\"F2F4F7\"/>";
    if (out.empty()) return {};
    return "<w:rPr>" + out + "</w:rPr>";
}

std::string run(std::string_view text, const RunStyle& style) {
    if (text.empty()) return {};
    return "<w:r>" + run_properties(style) +
           "<w:t xml:space=\"preserve\">" + xml_text(text) + "</w:t></w:r>";
}

struct ParagraphStyle {
    int indent_left = 0;
    int indent_hanging = 0;
    int space_before = 0;
    int space_after = 120;
    bool quote = false;    // a left border, the way a block quote reads
    bool rule = false;     // a horizontal rule, drawn as a bottom border
    std::string alignment; // "left" | "center" | "right" | empty
};

std::string paragraph_properties(const ParagraphStyle& style) {
    std::string out;
    if (style.quote || style.rule) {
        out += "<w:pBdr>";
        if (style.quote) {
            out += "<w:left w:val=\"single\" w:sz=\"18\" w:space=\"10\" w:color=\"B9C0CC\"/>";
        }
        if (style.rule) {
            out += "<w:bottom w:val=\"single\" w:sz=\"6\" w:space=\"1\" w:color=\"C8CDD8\"/>";
        }
        out += "</w:pBdr>";
    }
    if (style.indent_left > 0 || style.indent_hanging > 0) {
        out += "<w:ind w:left=\"" + std::to_string(style.indent_left) + "\"";
        if (style.indent_hanging > 0) {
            out += " w:hanging=\"" + std::to_string(style.indent_hanging) + "\"";
        }
        out += "/>";
    }
    if (style.space_before > 0 || style.space_after > 0) {
        out += "<w:spacing w:before=\"" + std::to_string(style.space_before) +
               "\" w:after=\"" + std::to_string(style.space_after) +
               "\" w:line=\"276\" w:lineRule=\"auto\"/>";
    }
    if (!style.alignment.empty()) out += "<w:jc w:val=\"" + style.alignment + "\"/>";
    if (out.empty()) return {};
    return "<w:pPr>" + out + "</w:pPr>";
}

std::string paragraph(const std::string& runs, const ParagraphStyle& style) {
    return "<w:p>" + paragraph_properties(style) + runs + "</w:p>";
}

// ---------------------------------------------------------------------------
// Inline content
// ---------------------------------------------------------------------------

// Hyperlinks need a relationship each; they are collected while the body is built and
// written out as word/_rels/document.xml.rels afterwards, so the ids match by
// construction.
struct Links {
    std::vector<std::string> targets;

    std::string add(std::string_view target) {
        targets.emplace_back(target);
        return "rId" + std::to_string(targets.size());
    }
};

constexpr int kMaxInlineDepth = 8;

std::string inline_runs(std::string_view text, RunStyle style, Links& links, int depth);

std::string span_runs(const md::InlineSpan& span, RunStyle style, Links& links, int depth) {
    if (span.kind == "strong") {
        style.bold = true;
        return inline_runs(span.text, style, links, depth + 1);
    }
    if (span.kind == "emphasis") {
        style.italic = true;
        return inline_runs(span.text, style, links, depth + 1);
    }
    if (span.kind == "strikethrough") {
        style.strike = true;
        return inline_runs(span.text, style, links, depth + 1);
    }
    if (span.kind == "code") {
        RunStyle code = style;
        code.mono = true;
        code.shaded = true;
        return run(span.text, code);
    }
    if (span.kind == "link") {
        const std::string id = links.add(span.target.empty() ? span.text : span.target);
        const std::string label = span.text.empty() ? span.target : span.text;
        return "<w:hyperlink r:id=\"" + id + "\">" +
               inline_runs(label, style, links, depth + 1) + "</w:hyperlink>";
    }
    if (span.kind == "image") {
        // Embedding the picture would mean shipping an image decoder; a marker is
        // honest, a missing picture is not.
        RunStyle marker = style;
        marker.italic = true;
        marker.color = "6B7280";
        return run("[image: " + (span.text.empty() ? span.target : span.text) + "]", marker);
    }
    if (span.kind == "math_inline" || span.kind == "math_block") {
        RunStyle math = style;
        math.mono = true;
        return run(span.text, math);
    }
    return run(span.text, style);
}

// The inline parser reports only *marked* spans, so the plain text between them comes
// from the source itself (each span carries its own offsets).
std::string inline_runs(std::string_view text, RunStyle style, Links& links, int depth) {
    if (text.empty()) return {};
    if (depth > kMaxInlineDepth) return run(text, style);
    const std::vector<md::InlineSpan> spans = md::parse_inline(text);
    std::string out;
    size_t cursor = 0;
    for (const md::InlineSpan& span : spans) {
        if (span.begin > cursor && span.begin <= text.size()) {
            out += run(text.substr(cursor, span.begin - cursor), style);
        }
        out += span_runs(span, style, links, depth);
        cursor = span.end > cursor ? std::min(span.end, text.size()) : cursor;
    }
    if (cursor < text.size()) out += run(text.substr(cursor), style);
    return out;
}

// ---------------------------------------------------------------------------
// Blocks
// ---------------------------------------------------------------------------

int heading_half_points(int level) {
    switch (level) {
    case 1: return 40;   // 20pt
    case 2: return 32;
    case 3: return 28;
    case 4: return 26;
    case 5: return 24;
    default: return 22;
    }
}

std::vector<std::string> lines_of(std::string_view text) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        const bool last = end == std::string_view::npos;
        if (last) end = text.size();
        std::string line(text.substr(start, end - start));
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
        if (last) break;
        start = end + 1;
    }
    return lines;
}

// A list item may still carry a task checkbox, which Word has no equivalent for and
// which is worth keeping as a glyph.
std::string task_prefix(std::string& text) {
    size_t at = 0;
    while (at < text.size() && (text[at] == ' ' || text[at] == '\t')) ++at;
    if (at + 3 <= text.size() && text[at] == '[' && text[at + 2] == ']') {
        const char mark = text[at + 1];
        if (mark == ' ' || mark == 'x' || mark == 'X') {
            text.erase(0, at + 3);
            while (!text.empty() && text.front() == ' ') text.erase(0, 1);
            return mark == ' ' ? "\xE2\x98\x90 " : "\xE2\x98\x91 ";
        }
    }
    text.erase(0, at);
    return {};
}

std::string heading_block(const md::Block& block, std::string_view source, Links& links) {
    RunStyle style;
    style.bold = true;
    style.half_points = heading_half_points(block.level);
    ParagraphStyle layout;
    layout.space_before = block.level <= 2 ? 240 : 180;
    layout.space_after = 120;
    return paragraph(inline_runs(block.content(source), style, links, 0), layout);
}

std::string code_block(const md::Block& block, std::string_view source) {
    RunStyle style;
    style.mono = true;
    style.shaded = true;
    style.half_points = 19;   // 9.5pt, so a line of code fits the page width
    std::string out;
    // One paragraph per line: a single paragraph would re-wrap like prose, and a copied
    // line would come out glued to its neighbours.
    for (const std::string& line : lines_of(block.content(source))) {
        ParagraphStyle layout;
        layout.indent_left = 240;
        layout.space_after = 0;
        layout.space_before = 0;
        out += paragraph(run(line.empty() ? " " : line, style), layout);
    }
    return out;
}

std::string quote_block(const md::Block& block, std::string_view source, Links& links) {
    RunStyle style;
    style.italic = true;
    style.color = "3F4653";
    std::string out;
    for (std::string line : lines_of(block.content(source))) {
        size_t at = 0;
        while (at < line.size() && (line[at] == '>' || line[at] == ' ')) ++at;
        line.erase(0, at);
        ParagraphStyle layout;
        layout.indent_left = 360;
        layout.quote = true;
        out += paragraph(inline_runs(line, style, links, 0), layout);
    }
    return out;
}

std::string list_block(const md::Block& block, std::string_view source, Links& links) {
    std::string out;
    int counter = 0;
    for (const md::ListItem& item : md::parse_list(block, source)) {
        counter += 1;
        std::string text = item.text;
        std::string prefix = task_prefix(text);
        if (prefix.empty()) {
            if (item.ordered) {
                // "3." / "3)" keep their own number; anything else counts.
                std::string digits;
                for (const char ch : item.marker) {
                    if (ch >= '0' && ch <= '9') digits.push_back(ch);
                }
                prefix = (digits.empty() ? std::to_string(counter) : digits) + ". ";
            } else {
                prefix = "\xE2\x80\xA2 ";   // a bullet
            }
        }
        ParagraphStyle layout;
        layout.indent_left = 360 + std::max(0, block.level - 1) * 360;
        layout.indent_hanging = 240;
        layout.space_after = 60;
        out += paragraph(run(prefix, RunStyle{}) + inline_runs(text, RunStyle{}, links, 0), layout);
    }
    return out;
}

std::string table_block(const md::Block& block, std::string_view source, Links& links) {
    const md::TableData table = md::parse_table(block, source);
    if (table.rows.empty()) return {};

    std::string out = "<w:tbl><w:tblPr><w:tblW w:w=\"0\" w:type=\"auto\"/><w:tblBorders>";
    for (const char* side : {"top", "left", "bottom", "right", "insideH", "insideV"}) {
        out += std::string("<w:") + side +
               " w:val=\"single\" w:sz=\"6\" w:space=\"0\" w:color=\"B9C0CC\"/>";
    }
    out += "</w:tblBorders></w:tblPr><w:tblGrid/>";

    for (size_t row_index = 0; row_index < table.rows.size(); ++row_index) {
        const std::vector<md::TableCell>& row = table.rows[row_index];
        out += "<w:tr>";
        for (size_t index = 0; index < row.size(); ++index) {
            RunStyle style;
            // The first row of a table with a header is the header row.
            style.bold = table.header && row_index == 0;
            ParagraphStyle layout;
            layout.space_after = 0;
            if (index < table.alignments.size()) {
                const std::string& alignment = table.alignments[index];
                if (alignment == "center" || alignment == "right" || alignment == "left") {
                    layout.alignment = alignment;
                }
            }
            out += "<w:tc><w:tcPr><w:tcW w:w=\"0\" w:type=\"auto\"/></w:tcPr>" +
                   paragraph(inline_runs(row[index].text, style, links, 0), layout) + "</w:tc>";
        }
        out += "</w:tr>";
    }
    out += "</w:tbl>";
    // Word wants a paragraph after a table, or two tables in a row merge into one.
    out += paragraph("", ParagraphStyle{});
    return out;
}

std::string front_matter_block(const md::Block& block, std::string_view source) {
    RunStyle style;
    style.italic = true;
    style.color = "6B7280";
    style.half_points = 20;
    std::string out;
    for (const std::string& line : lines_of(block.content(source))) {
        if (line.empty()) continue;
        ParagraphStyle layout;
        layout.space_after = 0;
        out += paragraph(run(line, style), layout);
    }
    return out;
}

std::string mono_block(const md::Block& block, std::string_view source, const char* color,
                       const char* alignment) {
    RunStyle style;
    style.mono = true;
    if (color) style.color = color;
    ParagraphStyle layout;
    layout.indent_left = 240;
    if (alignment) layout.alignment = alignment;
    return paragraph(run(block.content(source), style), layout);
}

std::string thematic_break() {
    ParagraphStyle layout;
    layout.rule = true;
    layout.space_before = 120;
    layout.space_after = 120;
    return paragraph("", layout);
}

// The paragraphs of the document body, with the hyperlink relationships collected.
std::string body_xml(std::string_view markdown, Links& links) {
    const md::ParsedDocument parsed = md::parse(markdown);
    std::string body;
    for (const md::Block& block : parsed.blocks) {
        switch (block.kind) {
        case md::BlockKind::FrontMatter: body += front_matter_block(block, markdown); break;
        case md::BlockKind::Heading: body += heading_block(block, markdown, links); break;
        case md::BlockKind::Paragraph:
            body += paragraph(inline_runs(block.content(markdown), RunStyle{}, links, 0),
                              ParagraphStyle{});
            break;
        case md::BlockKind::CodeBlock: body += code_block(block, markdown); break;
        case md::BlockKind::BlockQuote: body += quote_block(block, markdown, links); break;
        case md::BlockKind::List: body += list_block(block, markdown, links); break;
        case md::BlockKind::Table: body += table_block(block, markdown, links); break;
        case md::BlockKind::ThematicBreak: body += thematic_break(); break;
        case md::BlockKind::MathBlock:
            body += mono_block(block, markdown, nullptr, "center");
            break;
        case md::BlockKind::HtmlBlock:
            body += mono_block(block, markdown, "4B5563", nullptr);
            break;
        }
    }
    if (body.empty()) body = paragraph("", ParagraphStyle{});
    return body;
}

// A4 with 2cm margins: the exported file prints the way the editor prints.
const char* kSection =
    "<w:sectPr><w:pgSz w:w=\"11906\" w:h=\"16838\"/>"
    "<w:pgMar w:top=\"1134\" w:right=\"1134\" w:bottom=\"1134\" w:left=\"1134\" "
    "w:header=\"708\" w:footer=\"708\" w:gutter=\"0\"/></w:sectPr>";

std::string hyperlink_relationships(const Links& links) {
    std::string out;
    for (size_t index = 0; index < links.targets.size(); ++index) {
        out += "<Relationship Id=\"rId" + std::to_string(index + 1) +
               "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/"
               "hyperlink\" Target=\"" +
               xml_text(links.targets[index]) + "\" TargetMode=\"External\"/>";
    }
    return out;
}

} // namespace

std::string document_xml(std::string_view markdown) {
    Links links;
    return std::string("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
                       "<w:document "
                       "xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\" "
                       "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/"
                       "relationships\"><w:body>") +
           body_xml(markdown, links) + kSection + "</w:body></w:document>";
}

std::string from_markdown(std::string_view markdown, std::string_view title) {
    Links links;
    const std::string body = body_xml(markdown, links);

    const std::string content_types =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
        "<Default Extension=\"rels\" "
        "ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
        "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
        "<Override PartName=\"/word/document.xml\" "
        "ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document."
        "main+xml\"/>"
        "<Override PartName=\"/docProps/core.xml\" "
        "ContentType=\"application/vnd.openxmlformats-package.core-properties+xml\"/>"
        "</Types>";

    const std::string package_rels =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<Relationships "
        "xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" "
        "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/"
        "officeDocument\" Target=\"word/document.xml\"/>"
        "<Relationship Id=\"rId2\" "
        "Type=\"http://schemas.openxmlformats.org/package/2006/relationships/metadata/"
        "core-properties\" Target=\"docProps/core.xml\"/>"
        "</Relationships>";

    const std::string document =
        std::string("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
                    "<w:document "
                    "xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\" "
                    "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/"
                    "relationships\"><w:body>") +
        body + kSection + "</w:body></w:document>";

    const std::string document_rels =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<Relationships "
        "xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">" +
        hyperlink_relationships(links) + "</Relationships>";

    const std::string core =
        std::string("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
                    "<cp:coreProperties "
                    "xmlns:cp=\"http://schemas.openxmlformats.org/package/2006/metadata/"
                    "core-properties\" xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
                    "<dc:title>") +
        xml_text(title) + "</dc:title></cp:coreProperties>";

    std::vector<zip::Entry> entries;
    entries.push_back({"[Content_Types].xml", content_types});
    entries.push_back({"_rels/.rels", package_rels});
    entries.push_back({"word/document.xml", document});
    entries.push_back({"word/_rels/document.xml.rels", document_rels});
    entries.push_back({"docProps/core.xml", core});
    return zip::build(entries);
}

} // namespace colai::docx
