// Colaimdation - the export paths (Word package, and the paper layout the shell asks
// the page for).
//
// The Word export is written from the same parsed block model the editor renders, so
// these cases check both the mapping (a heading is a heading, a table is a table) and
// the parts of the package that make it open in Word at all.
#include "test_framework.h"

#include "core/docx.h"
#include "core/zip_writer.h"

#include <string>

using namespace colai;

TEST_CASE("export/word: the package carries every part Word needs") {
    const std::string package = docx::from_markdown("# Title\n\ntext\n", "Title");
    // A ZIP, with the parts stored (not compressed), so their names are visible.
    CHECK_EQ(package.substr(0, 4), std::string("PK\x03\x04", 4));
    CHECK_STR_CONTAINS(package, "[Content_Types].xml");
    CHECK_STR_CONTAINS(package, "_rels/.rels");
    CHECK_STR_CONTAINS(package, "word/document.xml");
    CHECK_STR_CONTAINS(package, "word/_rels/document.xml.rels");
    CHECK_STR_CONTAINS(package, "docProps/core.xml");
    CHECK_STR_CONTAINS(package, "application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml");
}

TEST_CASE("export/word: the CRC-32 the package stores is the standard one") {
    // The one value every CRC-32 implementation is checked against.
    CHECK_EQ(zip::crc32("123456789"), 0xCBF43926u);
    CHECK_EQ(zip::crc32(""), 0u);
}

TEST_CASE("export/word: blocks keep their shape") {
    const std::string xml = docx::document_xml(
        "# Heading one\n"
        "\n"
        "A paragraph with **bold** and *italic* and `code`.\n"
        "\n"
        "- first\n"
        "- second\n"
        "\n"
        "| a | b |\n"
        "| --- | ---: |\n"
        "| 1 | 2 |\n"
        "\n"
        "```cpp\n"
        "int x = 1;\n"
        "```\n"
        "\n"
        "> quoted\n"
        "\n"
        "---\n");

    // Heading: a larger, bold paragraph.
    CHECK_STR_CONTAINS(xml, "<w:b/>");
    CHECK_STR_CONTAINS(xml, "<w:sz w:val=\"40\"/>");
    CHECK_STR_CONTAINS(xml, "Heading one");
    // Inline emphasis and inline code.
    CHECK_STR_CONTAINS(xml, "bold");
    CHECK_STR_CONTAINS(xml, "italic");
    CHECK_STR_CONTAINS(xml, "F2F4F7");           // the shading of inline code
    // A list keeps its items, one paragraph each, with a bullet.
    CHECK_STR_CONTAINS(xml, "first");
    CHECK_STR_CONTAINS(xml, "second");
    CHECK_STR_CONTAINS(xml, "\xE2\x80\xA2 ");     // the bullet glyph
    // The table became a real Word table with borders and alignment.
    CHECK_STR_CONTAINS(xml, "<w:tbl>");
    CHECK_STR_CONTAINS(xml, "<w:tblBorders>");
    CHECK_STR_CONTAINS(xml, "<w:jc w:val=\"right\"/>");
    // Code keeps its monospace font and its lines.
    CHECK_STR_CONTAINS(xml, "Consolas");
    CHECK_STR_CONTAINS(xml, "int x = 1;");
    // A quote gets the left border, a rule a bottom one.
    CHECK_STR_CONTAINS(xml, "<w:left w:val=\"single\"");
    CHECK_STR_CONTAINS(xml, "<w:bottom w:val=\"single\"");
    CHECK_STR_CONTAINS(xml, "quoted");
    // A4, so the file prints the way the editor prints.
    CHECK_STR_CONTAINS(xml, "<w:pgSz w:w=\"11906\" w:h=\"16838\"/>");
}

TEST_CASE("export/word: links become hyperlinks and text is escaped") {
    const std::string markdown =
        "See [the site](https://example.com/a?b=1&c=2) and <not-a-tag>.\n";
    const std::string xml = docx::document_xml(markdown);
    CHECK_STR_CONTAINS(xml, "<w:hyperlink r:id=\"rId1\">");
    CHECK_STR_CONTAINS(xml, "the site");
    // Markup in the text must not survive as markup; the ampersand in the URL has to
    // be escaped in the relationship as well.
    CHECK_STR_CONTAINS(xml, "&lt;not-a-tag&gt;");
    CHECK(xml.find("<not-a-tag>") == std::string::npos);

    const std::string package = docx::from_markdown(markdown, "doc");
    CHECK_STR_CONTAINS(package, "TargetMode=\"External\"");
    CHECK_STR_CONTAINS(package, "https://example.com/a?b=1&amp;c=2");
}

TEST_CASE("export/word: an empty document still produces a usable file") {
    const std::string package = docx::from_markdown("", "untitled");
    CHECK_STR_CONTAINS(package, "<w:body>");
    CHECK_STR_CONTAINS(package, "<w:sectPr>");
    CHECK(package.size() > 500);
}

TEST_CASE("export/word: a document with a BOM and CRLF endings survives") {
    const std::string markdown = "\xEF\xBB\xBF# Title\r\n\r\nbody\r\n";
    const std::string xml = docx::document_xml(markdown);
    CHECK_STR_CONTAINS(xml, "Title");
    CHECK_STR_CONTAINS(xml, "body");
    CHECK(xml.find('\r') == std::string::npos);
}
