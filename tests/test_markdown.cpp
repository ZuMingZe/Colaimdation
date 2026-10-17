// Markdown block parser.
#include "test_framework.h"

#include "core/markdown.h"
#include "core/utf8.h"

using namespace colai;

namespace {

const char* kSample = R"(---
title: Demo
tags: [a, b]
---

# Heading One

Some *emphasis* and **strong** text with `inline code` and a [link](https://example.com).

## Second heading ##

```python
def hello():
    return "world"
```

> a quote line
> continued here

- first item
- second item
  - nested item

1. one
2. two

| Name | Value | Notes |
|:-----|------:|:-----:|
| a    |     1 | mid   |
| b    |     2 | mid   |

---

$$
e = mc^2
$$

Last paragraph.
)";

size_t count_kind(const md::ParsedDocument& document, md::BlockKind kind) {
    size_t count = 0;
    for (const md::Block& block : document.blocks) {
        if (block.kind == kind) ++count;
    }
    return count;
}

const md::Block* first_of(const md::ParsedDocument& document, md::BlockKind kind) {
    for (const md::Block& block : document.blocks) {
        if (block.kind == kind) return &block;
    }
    return nullptr;
}

} // namespace

TEST_CASE("markdown/block kinds in one document") {
    const std::string text = kSample;
    const md::ParsedDocument document = md::parse(text);

    CHECK_EQ(count_kind(document, md::BlockKind::FrontMatter), size_t(1));
    CHECK_EQ(count_kind(document, md::BlockKind::Heading), size_t(2));
    CHECK_EQ(count_kind(document, md::BlockKind::CodeBlock), size_t(1));
    CHECK_EQ(count_kind(document, md::BlockKind::BlockQuote), size_t(1));
    CHECK_EQ(count_kind(document, md::BlockKind::List), size_t(2));
    CHECK_EQ(count_kind(document, md::BlockKind::Table), size_t(1));
    CHECK_EQ(count_kind(document, md::BlockKind::ThematicBreak), size_t(1));
    CHECK_EQ(count_kind(document, md::BlockKind::MathBlock), size_t(1));
    // Two paragraphs: the mixed inline-markup line and the closing sentence.
    CHECK_EQ(count_kind(document, md::BlockKind::Paragraph), size_t(2));
    CHECK(document.has_front_matter);
}

TEST_CASE("markdown/block spans cover the source exactly") {
    const std::string text = kSample;
    const md::ParsedDocument document = md::parse(text);
    // Spans may overlap by the blank lines between them, but each block's slice
    // must reproduce its own text and stay inside the document.
    for (size_t i = 0; i < document.blocks.size(); ++i) {
        const md::Block& block = document.blocks[i];
        CHECK(block.begin <= block.end);
        CHECK(block.end <= text.size());
        CHECK(block.content_begin <= block.content_end);
        CHECK(block.content_end <= text.size());
    }
    // The fenced code block's content excludes the fences and holds the body.
    const md::Block* code = first_of(document, md::BlockKind::CodeBlock);
    CHECK(code != nullptr);
    CHECK_EQ(code->info, std::string("python"));
    CHECK_STR_CONTAINS(std::string(code->content(text)), "def hello():");
    CHECK_STR_CONTAINS(std::string(code->content(text)), "return \"world\"");
    CHECK(std::string(code->content(text)).find("```") == std::string::npos);
}

TEST_CASE("markdown/headings, levels and outline") {
    const std::string text = kSample;
    const md::ParsedDocument document = md::parse(text);
    const md::Block* heading = first_of(document, md::BlockKind::Heading);
    CHECK(heading != nullptr);
    CHECK_EQ(heading->level, 1);
    CHECK_EQ(std::string(heading->content(text)), std::string("Heading One"));

    CHECK_EQ(document.outline.size(), size_t(2));
    CHECK_EQ(document.outline[0].level, 1);
    CHECK_EQ(document.outline[0].title, std::string("Heading One"));
    CHECK_EQ(document.outline[1].level, 2);
    // The closing hashes of "## Second heading ##" are stripped.
    CHECK_EQ(document.outline[1].title, std::string("Second heading"));
}

TEST_CASE("markdown/setext headings") {
    const std::string text = "Title\n=====\n\nSubtitle\n--------\n\nbody text\n";
    const md::ParsedDocument document = md::parse(text);
    CHECK_EQ(count_kind(document, md::BlockKind::Heading), size_t(2));
    CHECK_EQ(document.outline.size(), size_t(2));
    CHECK_EQ(document.outline[0].level, 1);
    CHECK_EQ(document.outline[0].title, std::string("Title"));
    CHECK_EQ(document.outline[1].level, 2);
    CHECK_EQ(document.outline[1].title, std::string("Subtitle"));
}

TEST_CASE("markdown/table structure and alignment") {
    const std::string text =
        "| Name | Value | Notes |\n|:-----|------:|:-----:|\n| a | 1 | mid |\n| b | 2 | mid |\n";
    const md::ParsedDocument document = md::parse(text);
    const md::Block* table = first_of(document, md::BlockKind::Table);
    CHECK(table != nullptr);
    const md::TableData data = md::parse_table(*table, text);
    CHECK_EQ(data.rows.size(), size_t(3)); // header + 2 body rows
    CHECK_EQ(data.columns, int(3));
    CHECK_EQ(data.rows[0][0].text, std::string("Name"));
    CHECK_EQ(data.rows[1][1].text, std::string("1"));
    CHECK_EQ(data.rows[2][2].text, std::string("mid"));
    CHECK_EQ(data.alignments.size(), size_t(3));
    CHECK_EQ(data.alignments[0], std::string("left"));
    CHECK_EQ(data.alignments[1], std::string("right"));
    CHECK_EQ(data.alignments[2], std::string("center"));
}

TEST_CASE("markdown/list items with nesting") {
    const std::string text = "- first item\n- second item\n  - nested item\n\n1. one\n2. two\n";
    const md::ParsedDocument document = md::parse(text);
    CHECK_EQ(count_kind(document, md::BlockKind::List), size_t(2));

    const md::Block* bullet = nullptr;
    const md::Block* ordered = nullptr;
    for (const md::Block& block : document.blocks) {
        if (block.kind != md::BlockKind::List) continue;
        if (block.ordered) ordered = &block;
        else bullet = &block;
    }
    CHECK(bullet != nullptr);
    CHECK(ordered != nullptr);
    CHECK_EQ(bullet->level, 2); // nested depth detected
    CHECK(!bullet->ordered);

    const auto items = md::parse_list(*bullet, text);
    CHECK_EQ(items.size(), size_t(3));
    CHECK_EQ(items[0].text, std::string("first item"));
    CHECK_EQ(items[0].depth, 1);
    CHECK_EQ(items[1].text, std::string("second item"));
    CHECK_EQ(items[2].text, std::string("nested item"));
    CHECK_EQ(items[2].depth, 2);

    const auto ordered_items = md::parse_list(*ordered, text);
    CHECK_EQ(ordered_items.size(), size_t(2));
    CHECK(ordered_items[0].ordered);
    CHECK_EQ(ordered_items[0].marker, std::string("1."));
    CHECK_EQ(ordered_items[1].text, std::string("two"));
}

TEST_CASE("markdown/front matter is not confused with a list or rule") {
    // A leading horizontal rule must NOT be read as front matter.
    const std::string rule_first = "---\n\nparagraph\n";
    const md::ParsedDocument a = md::parse(rule_first);
    CHECK(!a.has_front_matter);
    CHECK_EQ(a.blocks.size(), size_t(2));
    CHECK_EQ(a.blocks[0].kind, md::BlockKind::ThematicBreak);

    // Front matter with YAML content is detected and spans the whole block.
    const std::string yaml = "---\na: 1\nb: 2\n---\nbody\n";
    const md::ParsedDocument b = md::parse(yaml);
    CHECK(b.has_front_matter);
    CHECK_EQ(b.blocks[0].kind, md::BlockKind::FrontMatter);
    CHECK_STR_CONTAINS(std::string(b.blocks[0].slice(yaml)), "a: 1");
}

TEST_CASE("markdown/empty and whitespace documents") {
    const md::ParsedDocument empty = md::parse("");
    CHECK(empty.blocks.empty());
    CHECK_EQ(empty.word_count, size_t(0));
    CHECK_EQ(empty.block_at(0), -1);

    const md::ParsedDocument blanks = md::parse("\n\n   \n\n");
    CHECK(blanks.blocks.empty());
}

TEST_CASE("markdown/block_at maps offsets to the containing block") {
    const std::string text = "# A\n\npara one\n\npara two\n";
    const md::ParsedDocument document = md::parse(text);
    CHECK(document.blocks.size() >= 3);
    CHECK_EQ(document.blocks[0].kind, md::BlockKind::Heading);
    const size_t para_one = text.find("para one");
    const int index = document.block_containing(para_one + 3);
    CHECK(index >= 0);
    CHECK_EQ(document.blocks[static_cast<size_t>(index)].kind, md::BlockKind::Paragraph);
    CHECK_STR_CONTAINS(std::string(document.blocks[static_cast<size_t>(index)].slice(text)), "para one");
    // Offset 4 is the blank line between blocks and has no exact owner.
    CHECK_EQ(document.block_containing(4), -1);
}

TEST_CASE("markdown/indented and unclosed code blocks") {
    const std::string indented = "text\n\n    indented code\n    more code\n\nafter\n";
    const md::ParsedDocument a = md::parse(indented);
    const md::Block* code = first_of(a, md::BlockKind::CodeBlock);
    CHECK(code != nullptr);
    CHECK_STR_CONTAINS(std::string(code->content(indented)), "indented code");

    // An unterminated fence still produces a block reaching the end of input.
    const std::string unclosed = "```js\nlet x = 1;\n";
    const md::ParsedDocument b = md::parse(unclosed);
    const md::Block* fence = first_of(b, md::BlockKind::CodeBlock);
    CHECK(fence != nullptr);
    CHECK_EQ(fence->info, std::string("js"));
    CHECK_EQ(fence->end, unclosed.size());
    CHECK_STR_CONTAINS(std::string(fence->content(unclosed)), "let x = 1;");
}

TEST_CASE("markdown/inline spans") {
    const std::string text =
        "Text with `code`, a [link](https://x.test/a), an ![img](i.png), "
        "$a^2$ inline math and $$block math$$ plus **bold** and *italic* and ~~gone~~.";
    const auto spans = md::parse_inline(text);

    bool saw_code = false, saw_link = false, saw_image = false, saw_inline_math = false;
    bool saw_block_math = false, saw_strong = false, saw_emphasis = false, saw_strike = false;
    for (const auto& span : spans) {
        if (span.kind == "code") {
            saw_code = true;
            CHECK_EQ(span.text, std::string("code"));
        } else if (span.kind == "link") {
            saw_link = true;
            CHECK_EQ(span.text, std::string("link"));
            CHECK_EQ(span.target, std::string("https://x.test/a"));
        } else if (span.kind == "image") {
            saw_image = true;
            CHECK_EQ(span.target, std::string("i.png"));
        } else if (span.kind == "math_inline") {
            saw_inline_math = true;
            CHECK_EQ(span.text, std::string("a^2"));
        } else if (span.kind == "math_block") {
            saw_block_math = true;
            CHECK_EQ(span.text, std::string("block math"));
        } else if (span.kind == "strong") {
            saw_strong = true;
            CHECK_EQ(span.text, std::string("bold"));
        } else if (span.kind == "emphasis") {
            saw_emphasis = true;
            CHECK_EQ(span.text, std::string("italic"));
        } else if (span.kind == "strikethrough") {
            saw_strike = true;
            CHECK_EQ(span.text, std::string("gone"));
        }
    }
    CHECK(saw_code);
    CHECK(saw_link);
    CHECK(saw_image);
    CHECK(saw_inline_math);
    CHECK(saw_block_math);
    CHECK(saw_strong);
    CHECK(saw_emphasis);
    CHECK(saw_strike);
}

TEST_CASE("markdown/inline span offsets point at real text") {
    const std::string text = "prefix `snippet` suffix";
    const auto spans = md::parse_inline(text);
    CHECK_EQ(spans.size(), size_t(1));
    CHECK_EQ(spans[0].begin, size_t(7));
    CHECK_EQ(text.substr(spans[0].begin, spans[0].end - spans[0].begin), std::string("`snippet`"));
}

TEST_CASE("markdown/word and character counts") {
    const std::string text = "one two three 中文词语\n\nfour\n";
    const md::ParsedDocument document = md::parse(text);
    CHECK_EQ(document.character_count, utf8::length(text));
    // "中文词语" counts as one whitespace-delimited token.
    CHECK_EQ(document.word_count, size_t(5));
}

TEST_CASE("markdown/structure fingerprint tracks layout") {
    const std::string a = "# Title\n\nbody\n";
    const std::string b = "# Title\n\nbody changed\n";
    const std::string c = "# Title\n\nbody\n\n## New\n";
    const std::string fingerprint_a = md::structure_fingerprint(md::parse(a));
    CHECK_EQ(fingerprint_a, md::structure_fingerprint(md::parse(a)));
    // Same block layout, different text -> same structure fingerprint.
    CHECK_EQ(fingerprint_a, md::structure_fingerprint(md::parse(b)));
    // Extra block -> different fingerprint.
    CHECK_NE(fingerprint_a, md::structure_fingerprint(md::parse(c)));
}

TEST_CASE("markdown/block to json carries addressing data") {
    const std::string text = "# A\n\n```json\n{}\n```\n";
    const md::ParsedDocument document = md::parse(text);
    CHECK(document.blocks.size() >= 2);
    const Json heading = md::block_to_json(document.blocks[0], text, 0);
    CHECK_EQ(heading.str("block_id"), std::string("b0"));
    CHECK_EQ(heading.str("kind"), std::string("heading"));
    CHECK_EQ(heading.integer("level"), 1);
    CHECK_EQ(heading.str("text"), std::string("A"));

    const Json code = md::block_to_json(document.blocks[1], text, 1);
    CHECK_EQ(code.str("kind"), std::string("code_block"));
    CHECK_EQ(code.str("info"), std::string("json"));
}

TEST_CASE("markdown/kind name round trip") {
    const md::BlockKind kinds[] = {
        md::BlockKind::FrontMatter, md::BlockKind::Heading,   md::BlockKind::Paragraph,
        md::BlockKind::CodeBlock,   md::BlockKind::Table,     md::BlockKind::List,
        md::BlockKind::BlockQuote,  md::BlockKind::ThematicBreak, md::BlockKind::HtmlBlock,
        md::BlockKind::MathBlock,
    };
    for (md::BlockKind kind : kinds) {
        CHECK_EQ(md::kind_from_name(md::kind_name(kind)), kind);
    }
    CHECK_EQ(md::kind_from_name("nonsense"), md::BlockKind::Paragraph);
    CHECK_EQ(md::kind_from_name("CODE"), md::BlockKind::CodeBlock);
}

TEST_CASE("markdown/escaped pipes inside table cells") {
    const std::string text = "| a | b |\n|---|---|\n| x \\| y | z |\n";
    const md::ParsedDocument document = md::parse(text);
    const md::Block* table = first_of(document, md::BlockKind::Table);
    CHECK(table != nullptr);
    const md::TableData data = md::parse_table(*table, text);
    CHECK_EQ(data.columns, int(2));
    CHECK_EQ(data.rows.size(), size_t(2));
    CHECK_STR_CONTAINS(data.rows[1][0].text, "x");
}
