// Document engine: patches, history, rollback.
#include "test_framework.h"

#include "core/document.h"
#include "core/markdown.h"
#include "core/utf8.h"

using namespace colai;

namespace {

Document make_document(const std::string& text) {
    Document document;
    document.set_path("C:\\docs\\test.md");
    document.set_line_ending("\n");
    document.set_text(text, EditSource::System, "test", "setup", true);
    return document;
}

} // namespace

TEST_CASE("document/sequence identifies the document state") {
    // A fresh, never-mutated document is at sequence 0; every content change
    // (including a silent load) advances it, so two different texts can never
    // share a sequence number.
    Document document;
    CHECK_EQ(document.sequence(), uint64_t(0));
    document.set_text("abc", EditSource::System, "load", "load", true);
    CHECK_EQ(document.sequence(), uint64_t(1));

    Document other = make_document("abc");
    CHECK_EQ(other.sequence(), uint64_t(1));
    CHECK_EQ(other.history().size(), size_t(0));
    CHECK(other.apply_patch(Patch::insert(3, "d"), EditSource::Agent, "test").ok);
    CHECK_EQ(other.sequence(), uint64_t(2));
    CHECK_EQ(other.history().size(), size_t(1));
    CHECK_EQ(other.history().front().sequence, uint64_t(1));
}

TEST_CASE("document/identity and metadata") {
    Document document = make_document("# Title\n\nbody text here\n");
    CHECK(!document.id().empty());
    CHECK_EQ(document.display_name(), std::string("test.md"));
    CHECK_EQ(document.path(), std::string("C:\\docs\\test.md"));
    CHECK_EQ(document.line_count(), size_t(3));
    CHECK_EQ(document.character_count(), utf8::length(document.text()));
    CHECK_EQ(document.block_count(), size_t(2));
    CHECK_EQ(document.revision().size(), size_t(32));
    CHECK_EQ(document.revision(), make_revision(document.text()));
    CHECK_EQ(document.sequence(), uint64_t(1)); // the silent load counts as one change
}

TEST_CASE("document/insert text patch") {
    Document document = make_document("hello world");
    PatchOutcome outcome = document.apply_patch(Patch::insert(5, ","), EditSource::Agent, "test");
    CHECK(outcome.ok);
    CHECK_EQ(document.text(), std::string("hello, world"));
    CHECK_EQ(outcome.applied_count, size_t(1));
    CHECK_EQ(outcome.changes.size(), size_t(1));
    CHECK_EQ(outcome.changes[0].begin, size_t(5));
    CHECK_EQ(outcome.changes[0].end, size_t(5));
    CHECK_EQ(outcome.changes[0].inserted, std::string(","));
    CHECK_EQ(document.sequence(), uint64_t(2));
}

TEST_CASE("document/insert out of range is refused without touching the buffer") {
    Document document = make_document("abc");
    const std::string before = document.text();
    const uint64_t sequence_before = document.sequence();
    PatchOutcome outcome = document.apply_patch(Patch::insert(99, "x"), EditSource::Agent, "test");
    CHECK(!outcome.ok);
    CHECK_EQ(outcome.error_code, std::string("out_of_range"));
    CHECK_EQ(document.text(), before);
    CHECK_EQ(document.sequence(), sequence_before);
    CHECK_EQ(document.history().size(), size_t(0));
}

TEST_CASE("document/replace and delete range") {
    Document document = make_document("the quick brown fox");
    CHECK(document.apply_patch(Patch::replace(4, 9, "slow"), EditSource::Agent, "test").ok);
    CHECK_EQ(document.text(), std::string("the slow brown fox"));
    // Deleting the word plus its trailing space closes the gap exactly.
    CHECK(document.apply_patch(Patch::erase(4, 9), EditSource::Agent, "test").ok);
    CHECK_EQ(document.text(), std::string("the brown fox"));
    CHECK(document.apply_patch(Patch::replace(4, 4, "quick "), EditSource::Agent, "test").ok);
    CHECK_EQ(document.text(), std::string("the quick brown fox"));
}

TEST_CASE("document/replace block keeps the heading marker") {
    const std::string text = "# Title\n\npara one\n\n```py\nold()\n```\n";
    Document document = make_document(text);
    const md::ParsedDocument& structure = document.parsed();
    size_t heading_index = 0;
    size_t paragraph_index = 0;
    size_t code_index = 0;
    for (size_t i = 0; i < structure.blocks.size(); ++i) {
        switch (structure.blocks[i].kind) {
        case md::BlockKind::Heading: heading_index = i; break;
        case md::BlockKind::Paragraph: paragraph_index = i; break;
        case md::BlockKind::CodeBlock: code_index = i; break;
        default: break;
        }
    }

    CHECK(document.apply_patch(Patch::replace_block(heading_index, "Better Title"),
                              EditSource::Agent, "test")
              .ok);
    CHECK_STR_CONTAINS(document.text(), "# Better Title");

    CHECK(document.apply_patch(Patch::replace_block(paragraph_index, "para two"), EditSource::Agent,
                               "test")
              .ok);
    CHECK_STR_CONTAINS(document.text(), "para two");

    // The fenced code block keeps its fences and language when only the inner
    // text is replaced.
    CHECK(document.apply_patch(Patch::replace_block(code_index, "new()"), EditSource::Agent, "test")
              .ok);
    CHECK_STR_CONTAINS(document.text(), "```py");
    CHECK_STR_CONTAINS(document.text(), "new()");
    CHECK(document.text().find("old()") == std::string::npos);
}

TEST_CASE("document/replace block raw replaces markers too") {
    Document document = make_document("## Heading\n\nbody\n");
    CHECK(document.apply_patch(Patch::replace_block(0, "### Replaced", true), EditSource::Agent,
                               "test")
              .ok);
    CHECK_STR_CONTAINS(document.text(), "### Replaced");
    CHECK(document.text().find("## Heading") == std::string::npos);
}

TEST_CASE("document/insert block before and after") {
    Document document = make_document("# Title\n\nfirst\n");
    CHECK(document
              .apply_patch(Patch::insert_block(0, "intro", Patch::Position::After),
                           EditSource::Agent, "test")
              .ok);
    CHECK_STR_CONTAINS(document.text(), "intro");
    // "intro" now follows the heading and precedes the paragraph.
    const size_t heading_pos = document.text().find("# Title");
    const size_t intro_pos = document.text().find("intro");
    const size_t first_pos = document.text().find("first");
    CHECK(heading_pos < intro_pos);
    CHECK(intro_pos < first_pos);

    const size_t count_before = document.block_count();
    CHECK(document
              .apply_patch(Patch::insert_block(0, "prelude", Patch::Position::Before),
                           EditSource::Agent, "test")
              .ok);
    CHECK_EQ(document.block_count(), count_before + 1);
    CHECK(document.text().find("prelude") < document.text().find("# Title"));
}

TEST_CASE("document/append section") {
    Document document = make_document("# Title\n\nbody\n");
    CHECK(document.apply_patch(Patch::append_section("## Appendix\n\nmore"), EditSource::Agent,
                               "test")
              .ok);
    CHECK_STR_CONTAINS(document.text(), "## Appendix");
    CHECK_EQ(document.text().back(), '\n');
    // Exactly one blank line separates the old content from the new section.
    CHECK_STR_CONTAINS(document.text(), "body\n\n## Appendix");

    Document empty;
    empty.set_text("", EditSource::System, "t", "t", true);
    CHECK(empty.apply_patch(Patch::append_section("First"), EditSource::Agent, "test").ok);
    CHECK_EQ(empty.text(), std::string("First\n"));
}

TEST_CASE("document/block addressing by id, index and cursor") {
    Document document = make_document("# A\n\none\n\ntwo\n");

    Patch by_id;
    by_id.op = PatchOp::ReplaceBlock;
    by_id.block_id = "b2";
    by_id.text = "TWO";
    CHECK(document.apply_patch(by_id, EditSource::Agent, "test").ok);
    CHECK_STR_CONTAINS(document.text(), "TWO");

    Patch by_cursor;
    by_cursor.op = PatchOp::ReplaceBlock;
    by_cursor.relative_to_cursor = true;
    by_cursor.text = "ALPHA";
    document.set_selection(2, 2); // inside the heading
    CHECK(document.apply_patch(by_cursor, EditSource::Agent, "test").ok);
    CHECK_STR_CONTAINS(document.text(), "# ALPHA");

    Patch unknown = by_id;
    unknown.block_id = "b999";
    PatchOutcome outcome = document.apply_patch(unknown, EditSource::Agent, "test");
    CHECK(!outcome.ok);
    CHECK_STR_CONTAINS(outcome.error_message, "out of range");
}

TEST_CASE("document/multiple patches apply atomically") {
    Document document = make_document("aaa bbb ccc");
    std::vector<Patch> patches = {
        Patch::replace(0, 3, "AAA"),
        Patch::replace(4, 7, "BBB"),
        Patch::replace(8, 11, "CCC"),
    };
    PatchOutcome outcome = document.apply_patches(patches, EditSource::Agent, "test");
    CHECK(outcome.ok);
    CHECK_EQ(document.text(), std::string("AAA BBB CCC"));
    CHECK_EQ(outcome.applied_count, size_t(3));

    // One invalid patch in the batch means nothing is applied.
    const std::string before = document.text();
    PatchOutcome failed = document.apply_patches(
        {Patch::replace(0, 3, "xxx"), Patch::replace(100, 200, "yyy")}, EditSource::Agent, "test");
    CHECK(!failed.ok);
    CHECK_EQ(document.text(), before);
}

TEST_CASE("document/overlapping patches are refused") {
    Document document = make_document("0123456789");
    PatchOutcome outcome = document.apply_patches(
        {Patch::replace(0, 5, "a"), Patch::replace(3, 8, "b")}, EditSource::Agent, "test");
    CHECK(!outcome.ok);
    CHECK_STR_CONTAINS(outcome.error_message, "overlap");
    CHECK_EQ(document.text(), std::string("0123456789"));
}

TEST_CASE("document/set_text is exclusive in a batch") {
    Document document = make_document("abc");
    Patch set;
    set.op = PatchOp::SetText;
    set.text = "xyz";
    PatchOutcome outcome =
        document.apply_patches({set, Patch::insert(0, "!")}, EditSource::Agent, "test");
    CHECK(!outcome.ok);
    CHECK_STR_CONTAINS(outcome.error_message, "only patch");
}

TEST_CASE("document/utf-8 safety") {
    Document document = make_document("中文测试");
    // An offset in the middle of a CJK sequence snaps to the boundary.
    CHECK(document.apply_patch(Patch::insert(1, "X"), EditSource::Agent, "test").ok);
    CHECK_EQ(document.text(), std::string("X中文测试"));
    CHECK(utf8::is_valid(document.text()));

    // A patch that would produce invalid UTF-8 never reaches the buffer.
    Document other = make_document("abc");
    Patch bad;
    bad.op = PatchOp::SetText;
    bad.text = std::string("ok\xff\xfe");
    PatchOutcome outcome = other.apply_patch(bad, EditSource::Agent, "test");
    CHECK(!outcome.ok);
    CHECK_EQ(other.text(), std::string("abc"));
}

TEST_CASE("document/read only refuses writes") {
    Document document = make_document("abc");
    document.set_read_only(true);
    PatchOutcome outcome = document.apply_patch(Patch::insert(0, "x"), EditSource::Agent, "test");
    CHECK(!outcome.ok);
    CHECK_EQ(outcome.error_code, std::string("permission_denied"));
    CHECK_EQ(document.text(), std::string("abc"));
}

TEST_CASE("document/history records provenance and supports undo/redo") {
    Document document = make_document("start");
    document.apply_patch(Patch::insert(5, " one"), EditSource::Agent, "insert_text", "add one",
                         "session-a");
    document.apply_patch(Patch::insert(9, " two"), EditSource::Agent, "insert_text", "add two",
                         "session-b");
    CHECK_EQ(document.history().size(), size_t(2));
    const Edit* last = document.last_edit();
    CHECK(last != nullptr);
    CHECK_EQ(last->source, EditSource::Agent);
    CHECK_EQ(last->tool, std::string("insert_text"));
    CHECK_EQ(last->session, std::string("session-b"));
    CHECK_EQ(last->revision_before, make_revision("start one"));
    CHECK_EQ(last->revision_after, make_revision("start one two"));

    CHECK(document.can_undo());
    CHECK(document.undo().ok);
    CHECK_EQ(document.text(), std::string("start one"));
    CHECK(document.can_redo());
    CHECK(document.redo().ok);
    CHECK_EQ(document.text(), std::string("start one two"));
}

TEST_CASE("document/undo twice then undo once more") {
    Document document = make_document("a");
    document.apply_patch(Patch::insert(1, "b"), EditSource::Agent, "t");
    document.apply_patch(Patch::insert(2, "c"), EditSource::Agent, "t");
    CHECK_EQ(document.text(), std::string("abc"));
    CHECK(document.undo().ok);
    CHECK_EQ(document.text(), std::string("ab"));
    CHECK(document.undo().ok);
    CHECK_EQ(document.text(), std::string("a"));
    // An undo is itself recorded for auditing, but it is not undoable: the next
    // undo reverses the previous real edit, not the undo.
    CHECK(!document.can_undo());
    CHECK(document.can_redo());
    CHECK(document.redo().ok);
    CHECK_EQ(document.text(), std::string("ab"));
    CHECK(document.redo().ok);
    CHECK_EQ(document.text(), std::string("abc"));
}

TEST_CASE("document/undo reverses exactly one edit") {
    Document document = make_document("seed\n");
    document.apply_patch(Patch::insert(5, "first\n"), EditSource::Agent, "t");
    document.apply_patch(Patch::insert(document.size(), "second\n"), EditSource::Agent, "t");
    CHECK_STR_CONTAINS(document.text(), "second");
    CHECK(document.undo().ok);
    CHECK_STR_CONTAINS(document.text(), "first");
    CHECK(document.text().find("second") == std::string::npos);
}

TEST_CASE("document/rollback last agent edit keeps user edits") {
    Document document = make_document("base\n");
    document.apply_patch(Patch::insert(5, "user line\n"), EditSource::User, "editor");
    document.apply_patch(Patch::insert(document.size(), "agent line\n"), EditSource::Agent,
                         "insert_text");
    CHECK_STR_CONTAINS(document.text(), "agent line");

    RollbackPlan plan = document.plan_rollback_last(EditSource::Agent);
    CHECK(plan.ok);
    CHECK_EQ(plan.edits_to_revert, size_t(1));
    CHECK(document.apply_rollback(plan).ok);
    CHECK_STR_CONTAINS(document.text(), "user line");
    CHECK(document.text().find("agent line") == std::string::npos);
}

TEST_CASE("document/rollback plan reports when there is nothing to revert") {
    Document document = make_document("base\n");
    RollbackPlan plan = document.plan_rollback_last(EditSource::Agent);
    CHECK(!plan.ok);
    CHECK_STR_CONTAINS(plan.error, "no agent edit");
}

TEST_CASE("document/rollback to an earlier revision reverts several edits") {
    Document document = make_document("v0\n");
    const std::string revision_v0 = document.revision();
    document.apply_patch(Patch::insert(document.size(), "v1\n"), EditSource::Agent, "t");
    document.apply_patch(Patch::insert(document.size(), "v2\n"), EditSource::Agent, "t");
    document.apply_patch(Patch::insert(document.size(), "v3\n"), EditSource::Agent, "t");
    CHECK_STR_CONTAINS(document.text(), "v3");

    // Reverting everything after sequence 0 restores the original text.
    RollbackPlan plan = document.plan_rollback_to_sequence(0);
    CHECK(plan.ok);
    CHECK_EQ(plan.edits_to_revert, size_t(3));
    CHECK_EQ(document.apply_rollback(plan).ok, true);
    CHECK_EQ(document.revision(), revision_v0);
    CHECK_EQ(document.text(), std::string("v0\n"));

    // Reverting everything after the first edit keeps that one edit.
    Document second = make_document("v0\n");
    second.apply_patch(Patch::insert(second.size(), "v1\n"), EditSource::Agent, "t");
    const std::string revision_v1 = second.revision();
    second.apply_patch(Patch::insert(second.size(), "v2\n"), EditSource::Agent, "t");
    const RollbackPlan keep_first = second.plan_rollback_to_sequence(1);
    CHECK(keep_first.ok);
    CHECK_EQ(keep_first.edits_to_revert, size_t(1));
    CHECK_EQ(second.apply_rollback(keep_first).ok, true);
    CHECK_EQ(second.revision(), revision_v1);
    CHECK_EQ(second.text(), std::string("v0\nv1\n"));
}

TEST_CASE("document/undo cannot cross a cleared history") {
    Document document = make_document("abc");
    document.apply_patch(Patch::insert(0, "x"), EditSource::Agent, "t");
    document.clear_history();
    CHECK(!document.can_undo());
    PatchOutcome outcome = document.undo();
    CHECK(!outcome.ok);
    CHECK_EQ(outcome.error_code, std::string("conflict"));
}

TEST_CASE("document/history retention respects the limit") {
    Document document = make_document("x");
    document.set_history_limit(4, 1024 * 1024);
    for (int i = 0; i < 10; ++i) {
        document.apply_patch(Patch::insert(0, "y"), EditSource::Agent, "t");
    }
    CHECK(document.history().size() <= size_t(5));
    CHECK(!document.history().empty());
}

TEST_CASE("document/selection tracking") {
    Document document = make_document("hello world");
    document.set_selection(0, 5, EditSource::User, "editor");
    const Selection selection = document.selection();
    CHECK_EQ(selection.begin(), size_t(0));
    CHECK_EQ(selection.end(), size_t(5));
    CHECK_EQ(selection.length(), size_t(5));
    CHECK(!selection.collapsed());

    const Json json = document.selection_json();
    CHECK_EQ(json.integer("start"), 0);
    CHECK_EQ(json.integer("end"), 5);
    CHECK_EQ(json.str("text"), std::string("hello"));
    CHECK_EQ(json.integer("head_line"), 1);
    CHECK_EQ(json.integer("head_column"), 6);
    CHECK_EQ(json["block"].str("kind"), std::string("paragraph"));

    // Offsets past the end and mid-character offsets are clamped.
    document.set_selection(4, 9999, EditSource::Agent);
    CHECK_EQ(document.selection().end(), document.size());
}

TEST_CASE("document/insertion moves the caret to the end of the inserted text") {
    Document document = make_document("abc");
    document.set_selection(1, 1, EditSource::User);
    CHECK(document.apply_patch(Patch::insert(1, "XYZ"), EditSource::Agent, "t").ok);
    CHECK_EQ(document.text(), std::string("aXYZbc"));
    CHECK_EQ(document.caret(), size_t(4));
}

TEST_CASE("document/json views") {
    Document document = make_document("# Title\n\ntext\n");
    const Json info = document.info_json();
    CHECK_EQ(info.str("name"), std::string("test.md"));
    CHECK_EQ(info.integer("blocks"), 2);
    CHECK_EQ(info.integer("lines"), 3);
    CHECK(info.boolean("can_undo") == false);

    const Json full = document.to_json(true);
    CHECK_EQ(full.str("content"), document.text());
    CHECK_EQ(full.str("line_ending"), std::string("lf"));

    const Json structure = document.structure_json(0);
    CHECK_EQ(structure.integer("blocks_total"), 2);
    CHECK_EQ(structure["blocks"].size(), size_t(2));
    CHECK(!structure.boolean("truncated"));

    const Json limited = document.structure_json(1);
    CHECK_EQ(limited["blocks"].size(), size_t(1));
    CHECK(limited.boolean("truncated"));

    const Json outline = document.outline_json();
    CHECK_EQ(outline.size(), size_t(1));
    CHECK_EQ(outline[0].str("title"), std::string("Title"));
}

TEST_CASE("document/revision changes with content only") {
    Document document = make_document("abc");
    const std::string revision = document.revision();
    document.set_selection(0, 1, EditSource::User);
    CHECK_EQ(document.revision(), revision); // selection is not content
    document.apply_patch(Patch::insert(0, "x"), EditSource::User, "editor");
    CHECK_NE(document.revision(), revision);
}

TEST_CASE("document/set_text silent does not record history") {
    Document document = make_document("abc");
    document.set_text("xyz", EditSource::System, "load", "load", true);
    CHECK_EQ(document.text(), std::string("xyz"));
    CHECK_EQ(document.history().size(), size_t(0));
}

TEST_CASE("document/line endings are normalised on the way in") {
    Document document;
    document.set_text("a\r\nb\r\n", EditSource::System, "load", "load", true);
    CHECK_EQ(document.text(), std::string("a\nb\n"));
    document.set_line_ending("\r\n");
    CHECK_EQ(document.to_json(false).str("line_ending"), std::string("crlf"));
}
