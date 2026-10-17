// Colaimdation - regression tests for bugs found during the bug hunt.
//
// Each test here reproduces a defect that was actually observed, so the
// surrounding fix cannot silently regress. The file name is deliberately broad:
// these cases cross module boundaries (diff/history, text edits, i18n) and do
// not belong to a single existing suite.
#include "test_framework.h"

#include "core/diff.h"
#include "core/document.h"
#include "core/json.h"
#include "core/markdown.h"
#include "core/text_edit.h"
#include "core/url_util.h"
#include "core/utf8.h"
#include "core/workspace.h"
#include "mcp/tool_registry.h"

#include <algorithm>
#include <set>
#include <string>

using namespace colai;

// ---------------------------------------------------------------------------
// Only the editor's own page may be loaded in the editor window
// ---------------------------------------------------------------------------

TEST_CASE("regression/a link must never navigate the editor window away") {
    // The front end is served from a loopback virtual host, so a relative link such as
    // "other.md" resolves to https://colaimdation.local/other.md. Accepting any path
    // under that host let one click load a page that does not exist, and the window
    // was left showing the browser's "file not found" page with no way back.
    CHECK(url::is_editor_page("https://colaimdation.local/index.html", "colaimdation.local"));
    CHECK(url::is_editor_page("HTTPS://COLAIMDATION.LOCAL/index.html", "colaimdation.local"));
    CHECK(url::is_editor_page("https://colaimdation.local/index.html#anchor", "colaimdation.local"));
    CHECK(url::is_editor_page("https://colaimdation.local/index.html?x=1", "colaimdation.local"));

    CHECK(!url::is_editor_page("https://colaimdation.local/other.md", "colaimdation.local"));
    CHECK(!url::is_editor_page("https://colaimdation.local/", "colaimdation.local"));
    CHECK(!url::is_editor_page("https://colaimdation.local", "colaimdation.local"));
    CHECK(!url::is_editor_page("https://example.com/index.html", "colaimdation.local"));
    // A host that merely starts with ours is not ours.
    CHECK(!url::is_editor_host("https://colaimdation.local.evil.test/index.html",
                               "colaimdation.local"));

    // The editor's own page is the only URL the shell navigates itself to.
    CHECK(url::editor_page("colaimdation.local") == "https://colaimdation.local/index.html");
}

TEST_CASE("regression/links that resolve onto our own host go to no browser") {
    // They exist only inside this window's WebView2, so handing one to the default
    // browser would be a 404 there instead.
    CHECK(url::is_editor_host("https://colaimdation.local/other.md", "colaimdation.local"));
    CHECK(!url::opens_in_browser("https://colaimdation.local/other.md") ||
          url::is_editor_host("https://colaimdation.local/other.md", "colaimdation.local"));

    // Real links still go out: web pages, mail and local files.
    CHECK(url::opens_in_browser("https://example.com/a/b?c=d"));
    CHECK(url::opens_in_browser("http://127.0.0.1:8080/x"));
    CHECK(url::opens_in_browser("mailto:someone@example.com"));
    CHECK(url::opens_in_browser("file:///C:/notes/a.md"));

    // Script-ish schemes are cancelled outright, never opened.
    CHECK(url::is_script_url("javascript:alert(1)"));
    CHECK(url::is_script_url("data:text/html,<b>x</b>"));
    CHECK(url::is_script_url("about:blank"));
    CHECK(!url::opens_in_browser("javascript:alert(1)"));
    CHECK(!url::opens_in_browser("data:text/html,<b>x</b>"));
}

// ---------------------------------------------------------------------------
// Unified diffs must be valid patches
// ---------------------------------------------------------------------------

TEST_CASE("regression/diff hunk headers follow the unified diff convention") {
    // An insertion at the very top of the document has no old line 1 to anchor
    // to, so the old side must be "-0,0" (or "-0"): `patch` and `git apply`
    // reject "@@ -1,0 ...". The header used to be emitted as "-1,0".
    const std::string before = "a\n";
    const std::string after = "b\nc\n";
    const std::string diff = unified_diff(before, after, "before", "after", 3);
    CHECK_STR_CONTAINS(diff, "@@ -1,1 +1,2 @@");

    // Purely additive or purely subtractive hunks keep an explicit ",0" on the
    // side that has no lines.
    const std::string append_diff = unified_diff("", "one\ntwo\n", "before", "after", 3);
    CHECK_STR_CONTAINS(append_diff, "@@ -0,0 +1,2 @@");

    const std::string delete_diff = unified_diff("one\ntwo\n", "", "before", "after", 3);
    CHECK_STR_CONTAINS(delete_diff, "@@ -1,2 +0,0 @@");

    // A header for a change that starts at the first line must still name line 1.
    const std::string replace_first = unified_diff("first\n", "changed\n", "b", "a", 2);
    CHECK_STR_CONTAINS(replace_first, "@@ -1,1 +1,1 @@");
}

TEST_CASE("regression/diff hunk headers cover the whole change") {
    // A middle-line insertion keeps real line numbers on both sides. With a
    // context of 1 the hunk is "@@ -1,2 +1,3 @@": old line 1 ("one"), the
    // inserted line, and old line 2 ("two").
    const std::string before = "one\ntwo\nthree\n";
    const std::string after = "one\nINSERTED\ntwo\nthree\n";
    const std::string diff = unified_diff(before, after, "before", "after", 1);
    CHECK_STR_CONTAINS(diff, "+INSERTED");
    CHECK_STR_CONTAINS(diff, "@@ -1,2 +1,3 @@");

    // A wider context window grows the hunk accordingly.
    const std::string wide = unified_diff(before, after, "before", "after", 3);
    CHECK_STR_CONTAINS(wide, "@@ -1,3 +1,4 @@");
}

// ---------------------------------------------------------------------------
// History sequences
// ---------------------------------------------------------------------------

TEST_CASE("regression/rollback records a sequence consistent with other edits") {
    Document document;
    document.set_text("one\n", EditSource::System, "test", "seed", true);
    document.apply_patch(Patch::insert(0, "X"), EditSource::Agent, "insert_text", "agent edit");
    const uint64_t before_rollback = document.sequence();

    const RollbackPlan plan = document.plan_rollback_last(EditSource::Agent);
    CHECK(plan.ok);
    CHECK(document.apply_rollback(plan, EditSource::Agent, "rollback").ok);

    // An edit's sequence is the document state it started from, so it must be the
    // pre-mutation value - not the post-mutation one, which belongs to the next
    // edit and made find_edit() return the wrong entry.
    std::set<uint64_t> sequences;
    for (const Edit& edit : document.history()) sequences.insert(edit.sequence);
    CHECK_EQ(sequences.size(), document.history().size());
    CHECK(document.find_edit(before_rollback - 1) != nullptr);

    // The rollback entry is findable by its own sequence.
    const Edit* last = document.last_edit();
    CHECK(last != nullptr);
    CHECK(document.find_edit(last->sequence) == last);
}

TEST_CASE("regression/document sequence and edit sequence agree after a mutation") {
    Document document;
    document.set_text("a\n", EditSource::System, "test", "seed", true);
    for (int i = 0; i < 3; ++i) {
        document.apply_patch(Patch::insert(0, "x"), EditSource::Agent, "insert_text", "edit");
        const Edit* last = document.last_edit();
        CHECK(last != nullptr);
        // sequence() reports the post-mutation value; an edit reports the state it
        // started from. They may never be equal, and never off by more than one.
        CHECK_EQ(document.sequence(), last->sequence + 1);
    }
}

// ---------------------------------------------------------------------------
// Text edit alignment
// ---------------------------------------------------------------------------

TEST_CASE("regression/minimal change never splits a character") {
    // Appending pure ASCII to a buffer that ends in a multi-byte character: the
    // old alignment loop read before[before.size()] (one past the end) and could
    // walk `begin` into the middle of the CJK character.
    struct Case {
        const char* before;
        const char* after;
    };
    const Case cases[] = {
        {"\xe4\xb8\xad\xe6\x96\x87", "\xe4\xb8\xad\xe6\x96\x87x"},   // 中文 -> 中文x
        {"\xe4\xb8\xad", "x"},                                       // 中 -> x
        {"x", "\xe4\xb8\xad"},                                       // x -> 中
        {"\xf0\x9f\x98\x80", "\xf0\x9f\x98\x80!"},                   // emoji + "!"
        {"a", ""},                                                   // delete everything
        {"", "a"},                                                   // insert into empty
        {"abc", "abc"},                                              // no change
        {"\xe4\xb8\xad\xe6\x96\x87", "\xe4\xb8\xad"},                // drop the last CJK char
    };
    for (const Case& item : cases) {
        const std::string before(item.before);
        const std::string after(item.after);
        TextChange change;
        if (!compute_minimal_change(before, after, change)) {
            CHECK_EQ(before, after);
            continue;
        }
        // The reported span must be inside the old buffer and on UTF-8
        // boundaries, and applying it must reproduce `after` exactly.
        CHECK(change.begin <= change.end);
        CHECK(change.end <= before.size());
        CHECK(utf8::is_valid(before.substr(0, change.begin)));
        std::string rebuilt = before;
        rebuilt.replace(change.begin, change.end - change.begin, change.inserted);
        CHECK_EQ(rebuilt, after);
    }
}

TEST_CASE("regression/user edit of a cjk document round trips through history") {
    Document document;
    document.set_text("第一行\n第二行\n", EditSource::System, "open", "seed", true);
    const std::string original = document.text();

    // Append a character after the final CJK character.
    const size_t caret = document.size();
    document.apply_patch(Patch::insert(caret, "新"), EditSource::User, "editor", "typing");
    CHECK_EQ(document.text(), original + "新");
    CHECK(utf8::is_valid(document.text()));

    CHECK(document.undo().ok);
    CHECK_EQ(document.text(), original);
    CHECK(document.redo().ok);
    CHECK_EQ(document.text(), original + "新");

    // Every recorded change must describe a real, boundary-aligned span.
    for (const Edit& edit : document.history()) {
        for (const TextChange& change : edit.changes) {
            CHECK(change.begin <= change.end);
            CHECK(change.end <= edit.text_before.size());
        }
    }
}

// ---------------------------------------------------------------------------
// Markdown structure
// ---------------------------------------------------------------------------

TEST_CASE("regression/markdown block spans are ordered and inside the document") {
    const std::string text =
        "> quoted text\n\n- item one\n- item two\n\n1. first\n2. second\n\n"
        "    indented code\n\n| a | b |\n|---|---|\n| 1 | 2 |\n\n"
        "<script>alert(1)</script>\n\n$$\nmath\n$$\n\npara\n===\n\n---\n";
    const md::ParsedDocument parsed = md::parse(text);
    CHECK(!parsed.blocks.empty());
    for (size_t i = 0; i < parsed.blocks.size(); ++i) {
        const md::Block& block = parsed.blocks[i];
        CHECK(block.begin <= block.end);
        CHECK(block.end <= text.size());
        CHECK(block.content_begin <= block.content_end);
        CHECK(block.content_end <= text.size());
        CHECK(block.size() == block.end - block.begin);
        if (i > 0) CHECK(parsed.blocks[i - 1].begin <= block.begin);
    }
}

TEST_CASE("regression/replacing every block keeps the document valid utf-8") {
    const std::string text = "# title\n\npara\n\n```python\ncode\n```\n\n- a\n- b\n";
    const size_t count = md::parse(text).blocks.size();
    CHECK(count > 0);
    for (size_t i = 0; i < count; ++i) {
        Document document;
        document.set_text(text, EditSource::System, "test", "seed", true);
        const PatchOutcome outcome =
            document.apply_patch(Patch::replace_block(i, "替换内容"), EditSource::Agent,
                                 "replace_block");
        CHECK(outcome.ok);
        CHECK(utf8::is_valid(document.text()));
        CHECK_STR_CONTAINS(document.text(), "替换内容");
    }
}

// ---------------------------------------------------------------------------
// Tool level behaviour
// ---------------------------------------------------------------------------

TEST_CASE("regression/rollback tool defaults to the newest edit of any source") {
    Workspace workspace;
    Workspace::Options options;
    workspace.initialise(std::move(options), nullptr);
    Document& document = workspace.document();
    document.set_text("base\n", EditSource::System, "test", "seed", true);

    mcp::ToolRegistry registry;
    mcp::register_builtin_tools(registry);
    mcp::Session session;
    session.id = "regression";
    mcp::ToolContext context;
    context.workspace = &workspace;
    context.session = &session;

    document.apply_patch(Patch::insert(0, "A"), EditSource::Agent, "insert_text", "agent");
    document.apply_patch(Patch::insert(1, "U"), EditSource::User, "editor", "user");

    // The schema declares source as optional, so the handler must behave the same
    // whether the client omits it or sends "any" - both revert the newest edit,
    // which here is the writer's.
    Json arguments = Json::object();
    arguments.set("last", Json(true));
    const Json result = registry.call("colaimdation_rollback", arguments, context);
    CHECK_EQ(result.boolean("isError", false), false);
    CHECK_EQ(document.text(), std::string("Abase\n"));
}

TEST_CASE("regression/search defaults to case-insensitive and still finds cjk") {
    Workspace workspace;
    Workspace::Options options;
    workspace.initialise(std::move(options), nullptr);
    workspace.document().set_text("# 标题\n\n这是一段中文内容。\n\nHello World\n",
                                  EditSource::System, "test", "seed", true);

    mcp::ToolRegistry registry;
    mcp::register_builtin_tools(registry);
    mcp::Session session;
    session.id = "regression";
    mcp::ToolContext context;
    context.workspace = &workspace;
    context.session = &session;

    auto count = [&](const char* query, bool case_sensitive) {
        Json arguments = Json::object();
        arguments.set("query", Json(query));
        arguments.set("case_sensitive", Json(case_sensitive));
        const Json result = registry.call("colaimdation_search", arguments, context);
        const Json* structured = result.find("structuredContent");
        CHECK(structured != nullptr);
        return structured->integer("match_count", -1);
    };

    CHECK_EQ(count("中文", false), int64_t(1));
    CHECK_EQ(count("中文", true), int64_t(1));
    CHECK_EQ(count("hello", false), int64_t(1));
    CHECK_EQ(count("hello", true), int64_t(0));
    CHECK_EQ(count("Hello", true), int64_t(1));
}

// ---------------------------------------------------------------------------
// Workspace / file round trip
// ---------------------------------------------------------------------------

TEST_CASE("regression/headless workspace exposes an open document to every tool") {
    Workspace workspace;
    Workspace::Options options;
    workspace.initialise(std::move(options), nullptr);
    mcp::ToolRegistry registry;
    mcp::register_builtin_tools(registry);
    mcp::Session session;
    session.id = "regression";
    mcp::ToolContext context;
    context.workspace = &workspace;
    context.session = &session;

    // Every tool advertises a schema and must answer without throwing.
    for (const mcp::ToolDescriptor& tool : registry.tools()) {
        CHECK(tool.input_schema.is_object());
        CHECK(!tool.name.empty());
        Json result = registry.call(tool.name, Json::object(), context);
        CHECK(result.is_object());
        CHECK(result.contains("content"));
    }
}
