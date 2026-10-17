// Diff helpers.
#include "test_framework.h"

#include "core/diff.h"

using namespace colai;

TEST_CASE("diff/identical inputs produce no diff") {
    const std::string text = "# T\n\nbody\n";
    CHECK(diff_lines(text, text).size() == size_t(3));
    CHECK(unified_diff(text, text).empty());
    const Json json = diff_to_json(text, text);
    CHECK(!json.boolean("changed"));
    CHECK_EQ(json.integer("added_lines"), 0);
    CHECK_EQ(json.integer("removed_lines"), 0);
}

TEST_CASE("diff/single line replacement") {
    const std::string before = "one\ntwo\nthree\n";
    const std::string after = "one\nTWO\nthree\n";
    const auto lines = diff_lines(before, after);
    size_t adds = 0, removes = 0, contexts = 0;
    for (const DiffLine& line : lines) {
        if (line.kind == DiffLine::Kind::Insert) ++adds;
        if (line.kind == DiffLine::Kind::Delete) ++removes;
        if (line.kind == DiffLine::Kind::Context) ++contexts;
    }
    CHECK_EQ(adds, size_t(1));
    CHECK_EQ(removes, size_t(1));
    CHECK_EQ(contexts, size_t(2));
}

TEST_CASE("diff/pure insertion and pure deletion") {
    const std::string before = "a\nc\n";
    const std::string after = "a\nb\nc\n";
    const auto inserted = diff_lines(before, after);
    size_t adds = 0, removes = 0;
    for (const DiffLine& line : inserted) {
        if (line.kind == DiffLine::Kind::Insert) ++adds;
        if (line.kind == DiffLine::Kind::Delete) ++removes;
    }
    CHECK_EQ(adds, size_t(1));
    CHECK_EQ(removes, size_t(0));

    const auto deleted = diff_lines(after, before);
    adds = removes = 0;
    for (const DiffLine& line : deleted) {
        if (line.kind == DiffLine::Kind::Insert) ++adds;
        if (line.kind == DiffLine::Kind::Delete) ++removes;
    }
    CHECK_EQ(adds, size_t(0));
    CHECK_EQ(removes, size_t(1));
}

TEST_CASE("diff/unified output shape") {
    std::string before;
    std::string after;
    for (int i = 1; i <= 20; ++i) {
        before += "line " + std::to_string(i) + "\n";
        after += "line " + std::to_string(i) + "\n";
    }
    const std::string replaced = "LINE 10\n";
    after.replace(after.find("line 10\n"), replaced.size(), replaced);

    const std::string unified = unified_diff(before, after, "current", "proposed");
    CHECK(!unified.empty());
    CHECK_STR_CONTAINS(unified, "--- current");
    CHECK_STR_CONTAINS(unified, "+++ proposed");
    CHECK_STR_CONTAINS(unified, "@@");
    CHECK_STR_CONTAINS(unified, "-line 10");
    CHECK_STR_CONTAINS(unified, "+LINE 10");
    // Distant unchanged lines are outside the context window.
    CHECK(unified.find("line 1\n") == std::string::npos);
    // Context lines are marked with a leading space.
    CHECK_STR_CONTAINS(unified, " line 9");
}

TEST_CASE("diff/structured json counts lines and bytes") {
    const std::string before = "a\nb\n";
    const std::string after = "a\nb\nc\n";
    const Json json = diff_to_json(before, after);
    CHECK(json.boolean("changed"));
    CHECK_EQ(json.integer("added_lines"), 1);
    CHECK_EQ(json.integer("removed_lines"), 0);
    CHECK_EQ(json.integer("byte_delta"), 2);
    CHECK_EQ(json["lines"].size(), size_t(3));

    bool saw_add = false;
    for (const Json& line : json["lines"].items()) {
        if (line.str("op") == "add") {
            saw_add = true;
            CHECK_EQ(line.str("text"), std::string("c"));
            CHECK_EQ(line.integer("new_line"), 3);
        }
    }
    CHECK(saw_add);
}

TEST_CASE("diff/empty to content and back") {
    const std::string empty;
    const std::string content = "# New\n\nbody\n";
    const Json added = diff_to_json(empty, content);
    CHECK_EQ(added.integer("added_lines"), 3);
    CHECK_EQ(added.integer("removed_lines"), 0);

    const Json removed = diff_to_json(content, empty);
    CHECK_EQ(removed.integer("added_lines"), 0);
    CHECK_EQ(removed.integer("removed_lines"), 3);
}

TEST_CASE("diff/large region degrades without exploding") {
    std::string before;
    std::string after;
    for (int i = 0; i < 3000; ++i) {
        before += "old " + std::to_string(i) + "\n";
        after += "new " + std::to_string(i) + "\n";
    }
    const Json json = diff_to_json(before, after);
    CHECK(json.integer("added_lines") > 0);
    CHECK(json.integer("removed_lines") > 0);
}

TEST_CASE("diff/crlf and cjk content survive") {
    const std::string before = "第一行\n第二行\n";
    const std::string after = "第一行\n第二行改了\n";
    const std::string unified = unified_diff(before, after);
    CHECK_STR_CONTAINS(unified, "第二行改了");
}
