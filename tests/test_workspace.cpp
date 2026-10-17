// Workspace: staging, confirmation, policy enforcement, notifications.
#include "test_framework.h"

#include "core/markdown.h"
#include "core/utf8.h"
#include "core/workspace.h"

#include <cstdio>
#include <fstream>
#include <sstream>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

using namespace colai;

namespace {

class FakeBridge : public IEditorBridge {
public:
    std::optional<Json> post(std::function<Json()> job, int) override { return job(); }

    void notify(const Json& message) override { messages.push_back(message); }

    bool request_confirmation(const Json& pending_message, int64_t) override {
        ++confirm_requests;
        last_pending = pending_message;
        if (!decision_callback) return default_decision;
        return decision_callback(pending_message);
    }

    void reload_document(const Document&) override { ++reloads; }

    bool has_ui() const override { return true; }

    std::vector<Json> messages;
    int confirm_requests = 0;
    int reloads = 0;
    bool default_decision = true;
    std::function<bool(const Json&)> decision_callback;
    Json last_pending;
};

struct Fixture {
    Workspace workspace;
    FakeBridge bridge;

    explicit Fixture(bool confirmation = false, std::string text = "# Title\n\nbody\n") {
        Workspace::Options options;
        options.policy = Policy::defaults();
        options.policy.audit_enabled = true;
        options.demo_content = false;
        std::string error;
        workspace.initialise(std::move(options), &error);
        workspace.set_bridge(&bridge);
        workspace.set_demo_content("C:\\docs\\test.md");
        workspace.document().set_text(std::move(text), EditSource::System, "setup", "setup", true);
        workspace.document().clear_history();
        // The tests below assert on edit counts, so start from a clean trail.
        workspace.audit().clear();
        workspace.adopt_document_text();
    }
};

std::string temp_path(const std::string& name) {
    char buffer[MAX_PATH] = {};
    ::GetTempPathA(MAX_PATH, buffer);
    return std::string(buffer) + "colaimdation_ws_" + name;
}

size_t message_count(const FakeBridge& bridge, const std::string& type) {
    size_t count = 0;
    for (const Json& message : bridge.messages) {
        if (message.str("type") == type) ++count;
    }
    return count;
}

} // namespace

TEST_CASE("workspace/demo document exercises the renderer") {
    const std::string demo = demo_document();
    CHECK(demo.size() > 200);
    CHECK_STR_CONTAINS(demo, "# ");
    CHECK_STR_CONTAINS(demo, "```");
    CHECK_STR_CONTAINS(demo, "|:---");
    CHECK_STR_CONTAINS(demo, "$$");
    CHECK_STR_CONTAINS(demo, "mermaid");
    // It must be valid UTF-8 and parse into a sensible number of blocks.
    CHECK(utf8::is_valid(demo));
    const md::ParsedDocument parsed = md::parse(demo);
    CHECK(parsed.blocks.size() > size_t(8));
    CHECK(!parsed.outline.empty());
}

TEST_CASE("workspace/agent patch applies, notifies and audits") {
    Fixture fixture;
    const size_t before = fixture.bridge.messages.size();
    Workspace::WriteOutcome outcome = fixture.workspace.apply_agent_patches(
        {Patch::append_section("## Written by agent")}, "apply_patch", "session-1", "append section");
    CHECK(outcome.ok);
    CHECK_EQ(outcome.applied, size_t(1));
    CHECK(!outcome.staged);
    CHECK(outcome.audit_id > 0);
    CHECK_STR_CONTAINS(fixture.workspace.document().text(), "## Written by agent");
    CHECK(!outcome.diff.empty());
    CHECK(fixture.bridge.messages.size() > before);
    CHECK(message_count(fixture.bridge, "document_changed") >= size_t(1));

    // The audit trail records the agent write with its session.
    AuditQuery query;
    query.tool = "apply_patch";
    const auto records = fixture.workspace.audit().query(query);
    CHECK(records.size() >= size_t(1));
    CHECK_EQ(records[0].session, std::string("session-1"));
    CHECK_EQ(records[0].source, EditSource::Agent);
    CHECK(records[0].reversible);
}

TEST_CASE("workspace/write disabled by policy") {
    Fixture fixture;
    fixture.workspace.policy().allow_write = false;
    Workspace::WriteOutcome outcome = fixture.workspace.apply_agent_patches(
        {Patch::append_section("nope")}, "apply_patch", "s", "blocked");
    CHECK(!outcome.ok);
    CHECK_EQ(outcome.error_code, std::string("permission_denied"));
    CHECK(fixture.workspace.document().text().find("nope") == std::string::npos);
}

TEST_CASE("workspace/front matter protection") {
    Fixture fixture(false, "---\ntitle: keep\n---\n\nbody\n");
    fixture.workspace.policy().protect_front_matter = true;
    Workspace::WriteOutcome outcome = fixture.workspace.apply_agent_patches(
        {Patch::replace_block(0, "title: changed")}, "replace_block", "s", "edit front matter");
    CHECK(!outcome.ok);
    CHECK_EQ(outcome.error_code, std::string("permission_denied"));
    CHECK_STR_CONTAINS(fixture.workspace.document().text(), "title: keep");

    // Once the flag is off the same edit is allowed.
    fixture.workspace.policy().protect_front_matter = false;
    CHECK(fixture.workspace
              .apply_agent_patches({Patch::replace_block(0, "title: changed")}, "replace_block", "s",
                                   "edit front matter")
              .ok);
    CHECK_STR_CONTAINS(fixture.workspace.document().text(), "title: changed");
}

TEST_CASE("workspace/line range restriction") {
    Fixture fixture(false, "line1\nline2\nline3\nline4\nline5\n");
    fixture.workspace.policy().allowed_line_ranges = {{1, 2}};

    // An edit on lines 1-2 is inside the permitted range.
    CHECK(fixture.workspace
              .apply_agent_patches({Patch::replace(0, 5, "LINE1")}, "replace_range", "s", "ok")
              .ok);

    // An edit on line 5 is outside it.
    const size_t offset = fixture.workspace.document().text().find("line5");
    Workspace::WriteOutcome blocked = fixture.workspace.apply_agent_patches(
        {Patch::replace(offset, offset + 5, "LINE5")}, "replace_range", "s", "denied");
    CHECK(!blocked.ok);
    CHECK_EQ(blocked.error_code, std::string("permission_denied"));
    CHECK_STR_CONTAINS(blocked.error_message, "line");
}

TEST_CASE("workspace/patch size and count limits") {
    Fixture fixture;
    fixture.workspace.policy().max_patches_per_call = 2;
    std::vector<Patch> patches = {Patch::insert(0, "a"), Patch::insert(0, "b"), Patch::insert(0, "c")};
    Workspace::WriteOutcome too_many =
        fixture.workspace.apply_agent_patches(patches, "apply_patch", "s", "many");
    CHECK(!too_many.ok);
    CHECK_STR_CONTAINS(too_many.error_message, "too many patches");

    fixture.workspace.policy().max_patches_per_call = 64;
    fixture.workspace.policy().max_patch_bytes = 4;
    Workspace::WriteOutcome too_big = fixture.workspace.apply_agent_patches(
        {Patch::insert(0, "0123456789")}, "apply_patch", "s", "big");
    CHECK(!too_big.ok);
    CHECK_STR_CONTAINS(too_big.error_message, "max_patch_bytes");
}

TEST_CASE("workspace/confirmation accepted then applied") {
    Fixture fixture;
    fixture.workspace.policy().require_confirmation = true;
    fixture.bridge.default_decision = true;

    Workspace::WriteOutcome outcome = fixture.workspace.apply_agent_patches(
        {Patch::append_section("## Confirmed")}, "apply_patch", "session-x", "needs review");
    CHECK(outcome.ok);
    CHECK(outcome.staged);
    CHECK_EQ(fixture.bridge.confirm_requests, 1);
    CHECK_STR_CONTAINS(fixture.workspace.document().text(), "## Confirmed");

    // The preview carried a diff the user could read.
    CHECK_STR_CONTAINS(fixture.bridge.last_pending.str("diff"), "## Confirmed");
    CHECK_EQ(fixture.bridge.last_pending.str("tool"), std::string("apply_patch"));

    // After the decision the staging area is empty.
    CHECK(fixture.workspace.pending_json().size() == size_t(0));
}

TEST_CASE("workspace/confirmation rejected leaves the document untouched") {
    Fixture fixture;
    fixture.workspace.policy().require_confirmation = true;
    fixture.bridge.default_decision = false;
    const std::string before = fixture.workspace.document().text();

    Workspace::WriteOutcome outcome = fixture.workspace.apply_agent_patches(
        {Patch::append_section("## Rejected")}, "apply_patch", "session-x", "needs review");
    CHECK(!outcome.ok);
    CHECK_EQ(outcome.error_code, std::string("rejected_by_user"));
    CHECK_EQ(fixture.workspace.document().text(), before);
    CHECK(fixture.workspace.pending_json().size() == size_t(0));

    // A rejection is still auditable.
    AuditQuery query;
    query.status = AuditStatus::Rejected;
    CHECK(fixture.workspace.audit().query(query).size() >= size_t(1));
}

TEST_CASE("workspace/staged change is inspectable before it is applied") {
    Fixture fixture;
    fixture.workspace.policy().require_confirmation = true;
    Json staged;
    fixture.bridge.decision_callback = [&](const Json& pending) {
        staged = pending;
        return false;
    };
    fixture.workspace.apply_agent_patches({Patch::append_section("staged text")}, "apply_patch",
                                          "s", "inspect");
    CHECK(!staged.is_null());
    CHECK_EQ(staged.str("summary"), std::string("inspect"));
    CHECK(!staged.str("id").empty());
    CHECK_EQ(staged.str("revision_before"), fixture.workspace.document().revision());
    CHECK(!staged.str("revision_after").empty());
    CHECK(staged["patches"].size() == size_t(1));
}

TEST_CASE("workspace/user edit is recorded and broadcast") {
    Fixture fixture(false, "hello\n");
    Workspace::EditReport report;
    report.revision_before = fixture.workspace.document().revision();
    report.text = "hello world\n";
    report.caret = 12;
    report.selection_anchor = 12;
    report.selection_head = 12;
    const Json response = fixture.workspace.apply_user_edit(report);
    CHECK_EQ(response.str("status"), std::string("applied"));
    CHECK_EQ(fixture.workspace.document().text(), std::string("hello world\n"));
    CHECK_EQ(fixture.workspace.document().caret(), size_t(12));
    CHECK(message_count(fixture.bridge, "user_activity") >= size_t(1));

    // The edit is in history as a user edit with a net change.
    const Edit* edit = fixture.workspace.document().last_edit();
    CHECK(edit != nullptr);
    CHECK_EQ(edit->source, EditSource::User);
    CHECK_EQ(edit->changes.size(), size_t(1));
    CHECK_STR_CONTAINS(edit->changes[0].inserted, "world");
}

TEST_CASE("workspace/user edit with a stale revision resynchronises") {
    Fixture fixture(false, "base\n");
    Workspace::EditReport report;
    report.revision_before = "deadbeef";
    report.text = "base plus\n";
    report.caret = 0;
    const Json response = fixture.workspace.apply_user_edit(report);
    CHECK_EQ(response.str("status"), std::string("resynced"));
    CHECK_EQ(fixture.workspace.document().text(), std::string("base plus\n"));
}

TEST_CASE("workspace/user edit that changes nothing is a no-op") {
    Fixture fixture(false, "same\n");
    Workspace::EditReport report;
    report.revision_before = fixture.workspace.document().revision();
    report.text = "same\n";
    const Json response = fixture.workspace.apply_user_edit(report);
    CHECK_EQ(response.str("status"), std::string("unchanged"));
    CHECK_EQ(fixture.workspace.document().history().size(), size_t(0));
}

TEST_CASE("workspace/agent sees user changes since a revision") {
    Fixture fixture(false, "start\n");
    const std::string revision = fixture.workspace.document().revision();
    Workspace::EditReport report;
    report.revision_before = revision;
    report.text = "start and more\n";
    fixture.workspace.apply_user_edit(report);

    const Json changes = fixture.workspace.user_changes_since(revision, 10);
    CHECK_EQ(changes.integer("change_count"), 1);
    CHECK_EQ(changes["changes"][0].str("source"), std::string("user"));
    CHECK_EQ(changes["changes"][0].str("tool"), std::string("editor"));
    CHECK_EQ(changes.str("since"), revision);
}

TEST_CASE("workspace/poll tracks a session cursor") {
    Fixture fixture(false, "one\n");
    const Json first = fixture.workspace.poll_user_changes("agent-1", "", 10);
    CHECK_EQ(first.str("session"), std::string("agent-1"));
    CHECK_EQ(first.integer("change_count"), 0);

    const std::string cursor = first.str("cursor_revision");
    Workspace::EditReport report;
    report.revision_before = fixture.workspace.document().revision();
    report.text = "one two\n";
    fixture.workspace.apply_user_edit(report);

    // With no explicit revision the session resumes from its own cursor.
    const Json second = fixture.workspace.poll_user_changes("agent-1", "", 10);
    CHECK_EQ(second.integer("change_count"), 1);

    // A second session with the same cursor sees the same change.
    const Json other = fixture.workspace.poll_user_changes("agent-2", cursor, 10);
    CHECK_EQ(other.integer("change_count"), 1);
}

TEST_CASE("workspace/caret control respects the policy") {
    Fixture fixture(false, "hello world\n");
    CHECK(fixture.workspace.set_caret(0, 5, "s").ok);
    CHECK_EQ(fixture.workspace.document().selection().end(), size_t(5));
    CHECK(message_count(fixture.bridge, "selection_set") >= size_t(1));

    std::string error;
    CHECK(!fixture.workspace.set_caret(0, 99999, "s", &error).ok);
    CHECK_STR_CONTAINS(error, "past the end");

    fixture.workspace.policy().allow_selection_control = false;
    CHECK(!fixture.workspace.set_caret(0, 1, "s", &error).ok);
    CHECK_STR_CONTAINS(error, "disabled");
}

TEST_CASE("workspace/rollback last agent change undoes it and is audited") {
    Fixture fixture(false, "base\n");
    CHECK(fixture.workspace
              .apply_agent_patches({Patch::append_section("agent section")}, "append_section", "s",
                                   "append")
              .ok);
    CHECK_STR_CONTAINS(fixture.workspace.document().text(), "agent section");

    const Json result = fixture.workspace.rollback_last(EditSource::Agent, "rollback_agent_changes");
    CHECK_EQ(result.str("status"), std::string("applied"));
    CHECK_EQ(result.integer("reverted_edits"), 1);
    CHECK(fixture.workspace.document().text().find("agent section") == std::string::npos);
    CHECK(message_count(fixture.bridge, "document_changed") >= size_t(2));

    AuditQuery query;
    query.status = AuditStatus::RolledBack;
    CHECK(fixture.workspace.audit().query(query).size() >= size_t(1));
}

TEST_CASE("workspace/rollback is refused when the policy disables it") {
    Fixture fixture;
    fixture.workspace.policy().allow_rollback = false;
    const Json result = fixture.workspace.rollback_last(EditSource::Agent, "rollback");
    CHECK_EQ(result.str("status"), std::string("error"));
    CHECK_STR_CONTAINS(result.str("error"), "disabled");
}

TEST_CASE("workspace/rollback by revision") {
    Fixture fixture(false, "v0\n");
    CHECK(fixture.workspace
              .apply_agent_patches({Patch::append_section("v1")}, "append_section", "s", "v1")
              .ok);
    const std::string revision_v1 = fixture.workspace.document().revision();
    CHECK(fixture.workspace
              .apply_agent_patches({Patch::append_section("v2")}, "append_section", "s", "v2")
              .ok);
    CHECK_STR_CONTAINS(fixture.workspace.document().text(), "v2");

    const Json result = fixture.workspace.rollback_to_revision(revision_v1, "rollback_revision");
    CHECK_EQ(result.str("status"), std::string("applied"));
    CHECK_EQ(fixture.workspace.document().revision(), revision_v1);
    CHECK(fixture.workspace.document().text().find("v2") == std::string::npos);

    const Json missing = fixture.workspace.rollback_to_revision("not-a-revision", "rollback");
    CHECK_EQ(missing.str("status"), std::string("error"));
}

TEST_CASE("workspace/status and policy json") {
    Fixture fixture;
    const Json status = fixture.workspace.status_json();
    CHECK_EQ(status["document"].str("name"), std::string("test.md"));
    CHECK(status.boolean("bridge_ready"));
    CHECK(!status["selection"].is_null());
    CHECK_EQ(status["policy"].boolean("require_token"), true);

    Json patch = Json::object();
    patch.set("require_confirmation", Json(true));
    patch.set("nonsense", Json(true));
    const Json updated = fixture.workspace.set_policy(patch);
    CHECK(updated.boolean("require_confirmation"));
    CHECK(fixture.workspace.policy().require_confirmation);
    CHECK_EQ(updated["warnings"].size(), size_t(1));
}

TEST_CASE("workspace/audit json query") {
    Fixture fixture;
    CHECK(fixture.workspace
              .apply_agent_patches({Patch::append_section("x")}, "append_section", "sess-a", "one")
              .ok);
    CHECK(fixture.workspace
              .apply_agent_patches({Patch::append_section("y")}, "append_section", "sess-b", "two")
              .ok);
    AuditQuery query;
    query.limit = 10;
    query.session = "sess-a";
    const Json json = fixture.workspace.audit_json(query);
    CHECK_EQ(json.integer("count"), 1);
    CHECK_EQ(json["entries"][0].str("session"), std::string("sess-a"));

    // Without a filter both applied changes are visible.
    AuditQuery all;
    all.limit = 10;
    CHECK_EQ(fixture.workspace.audit_json(all).integer("count"), 2);}

TEST_CASE("workspace/save writes the file with the original line endings") {
    Fixture fixture(false, "line one\nline two\n");
    const std::string path = temp_path("save.md");
    ::DeleteFileA(path.c_str());

    fixture.workspace.document().set_line_ending("\r\n");
    std::string error;
    CHECK(fixture.workspace.save_file_as(path, &error).ok);
    CHECK_EQ(fixture.workspace.document().path(), path);
    CHECK(!fixture.workspace.document().dirty());

    std::ifstream stream(path, std::ios::binary);
    std::stringstream buffer;
    buffer << stream.rdbuf();
    const std::string written = buffer.str();
    CHECK_EQ(written, std::string("line one\r\nline two\r\n"));

    AuditQuery query;
    query.status = AuditStatus::Saved;
    CHECK(fixture.workspace.audit().query(query).size() >= size_t(1));
    ::DeleteFileA(path.c_str());
}

TEST_CASE("workspace/open reads a file and normalises to lf") {
    const std::string path = temp_path("open.md");
    {
        std::ofstream stream(path, std::ios::binary);
        stream << "# From disk\r\n\r\nbody text\r\n";
    }
    Fixture fixture;
    std::string error;
    CHECK(fixture.workspace.open_file(path, &error).ok);
    CHECK_EQ(fixture.workspace.document().text(), std::string("# From disk\n\nbody text\n"));
    CHECK_EQ(fixture.workspace.document().line_ending(), std::string("\r\n"));
    CHECK(!fixture.workspace.document().dirty());
    // The text is normalised to LF in memory and converted back on save.
    CHECK(fixture.workspace.document().text().find('\r') == std::string::npos);
    CHECK(!fixture.workspace.document().dirty());
    CHECK_EQ(fixture.workspace.document().display_name(), std::string("colaimdation_ws_open.md"));
    CHECK(fixture.bridge.reloads >= 1);
    ::DeleteFileA(path.c_str());
}

TEST_CASE("workspace/open strips a utf-8 bom") {
    const std::string path = temp_path("bom.md");
    {
        std::ofstream stream(path, std::ios::binary);
        stream << "\xEF\xBB\xBF# BOM heading\n";
    }
    Fixture fixture;
    std::string error;
    CHECK(fixture.workspace.open_file(path, &error).ok);
    CHECK_EQ(fixture.workspace.document().text(), std::string("# BOM heading\n"));
    ::DeleteFileA(path.c_str());
}

TEST_CASE("workspace/open a missing file reports not_found") {
    Fixture fixture;
    std::string error;
    const Status status = fixture.workspace.open_file(temp_path("does_not_exist.md"), &error);
    CHECK(!status.ok);
    CHECK_STR_CONTAINS(error, "not found");
}

TEST_CASE("workspace/allowed directory is enforced") {
    Fixture fixture;
    fixture.workspace.policy().allowed_directory = "C:\\vault";

    std::string reason;
    CHECK(fixture.workspace.path_allowed("C:\\vault\\note.md", &reason));
    CHECK(fixture.workspace.path_allowed("C:\\vault\\sub\\note.md", &reason));
    CHECK(!fixture.workspace.path_allowed("C:\\other\\note.md", &reason));
    CHECK_STR_CONTAINS(reason, "outside");
    // A sibling directory sharing a prefix must not pass.
    CHECK(!fixture.workspace.path_allowed("C:\\vaultx\\note.md", &reason));

    std::string error;
    const Status status = fixture.workspace.open_file("C:\\other\\secret.md", &error);
    CHECK(!status.ok);
    CHECK_EQ(status.code, std::string("permission_denied"));
}

TEST_CASE("workspace/history json exposes edits") {
    Fixture fixture(false, "start\n");
    CHECK(fixture.workspace
              .apply_agent_patches({Patch::append_section("added")}, "append_section", "s", "add")
              .ok);
    const Json history = fixture.workspace.history_json(10, false);
    CHECK(!history["edits"].is_null());
    CHECK_EQ(history["edits"].size(), size_t(1));
    const Json with_text = fixture.workspace.history_json(10, true);
    CHECK(!with_text["edits"][0].str("text_before").empty());
}

// ---------------------------------------------------------------------------
// Several documents open at once (tabs)
// ---------------------------------------------------------------------------

namespace {

// Writes a file with known content and returns its path.
std::string write_temp_document(const std::string& name, const std::string& text) {
    const std::string path = temp_path(name);
    std::ofstream stream(path, std::ios::binary);
    stream << text;
    return path;
}

std::string read_temp_document(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    std::ostringstream out;
    out << stream.rdbuf();
    return out.str();
}

} // namespace

TEST_CASE("workspace/open adds a tab and activates it") {
    Fixture fixture; // tab 0: the demo text at C:\docs\test.md
    const std::string first = write_temp_document("tab_one.md", "first file\n");
    const std::string second = write_temp_document("tab_two.md", "second file\n");

    std::string error;
    CHECK(fixture.workspace.open_file(first, &error).ok);
    CHECK_EQ(fixture.workspace.tab_count(), size_t(2));
    CHECK_EQ(fixture.workspace.active_tab(), size_t(1));
    CHECK_EQ(fixture.workspace.document().path(), first);
    CHECK_EQ(fixture.workspace.document().text(), std::string("first file\n"));

    CHECK(fixture.workspace.open_file(second, &error).ok);
    CHECK_EQ(fixture.workspace.tab_count(), size_t(3));
    CHECK_EQ(fixture.workspace.document().path(), second);

    // Re-opening a file that is already open brings its tab forward instead of
    // loading the same file twice (two tabs would fight over which one saves).
    CHECK(fixture.workspace.open_file(first, &error).ok);
    CHECK_EQ(fixture.workspace.tab_count(), size_t(3));
    CHECK_EQ(fixture.workspace.active_tab(), size_t(1));
    CHECK_EQ(fixture.workspace.document().path(), first);

    // A file that cannot be read must not leave a tab behind.
    CHECK(!fixture.workspace.open_file(temp_path("no_such_tab.md"), &error).ok);
    CHECK_EQ(fixture.workspace.tab_count(), size_t(3));

    ::DeleteFileA(first.c_str());
    ::DeleteFileA(second.c_str());
}

TEST_CASE("workspace/every tab keeps its own text and saves its own file") {
    Fixture fixture;
    const std::string first = write_temp_document("tab_a.md", "file a\n");
    const std::string second = write_temp_document("tab_b.md", "file b\n");
    std::string error;
    CHECK(fixture.workspace.open_file(first, &error).ok);
    CHECK(fixture.workspace.open_file(second, &error).ok);
    CHECK_EQ(fixture.workspace.active_tab(), size_t(2));

    // Edit the active tab and save: only that file changes.
    CHECK(fixture.workspace
              .apply_agent_patches({Patch::append_section("only in b")}, "append_section", "s", "b")
              .ok);
    CHECK(fixture.workspace.save_file(nullptr, &error).ok);
    CHECK_STR_CONTAINS(read_temp_document(second), "only in b");
    CHECK(read_temp_document(first).find("only in b") == std::string::npos);

    // Switching tabs restores the other document, untouched.
    CHECK(fixture.workspace.activate_tab(1, &error).ok);
    CHECK_EQ(fixture.workspace.document().text(), std::string("file a\n"));
    CHECK(!fixture.workspace.document().dirty());

    // The tab list describes every document and marks the active one.
    const Json tabs = fixture.workspace.tabs_json();
    CHECK_EQ(tabs.integer("count"), 3);
    CHECK_EQ(tabs.integer("active"), 1);
    CHECK_EQ(tabs["tabs"][1].str("path"), first);
    CHECK(tabs["tabs"][1].boolean("active"));
    CHECK(!tabs["tabs"][2].boolean("active"));
    CHECK_EQ(fixture.workspace.status_json().integer("tab_count"), 3);

    // Closing the active tab falls back to a neighbour.
    CHECK(fixture.workspace.close_tab(1, &error).ok);
    CHECK_EQ(fixture.workspace.tab_count(), size_t(2));
    CHECK(fixture.workspace.active_tab() < fixture.workspace.tab_count());

    ::DeleteFileA(first.c_str());
    ::DeleteFileA(second.c_str());
}

TEST_CASE("workspace/new documents are clean, path-less and individually identified") {
    Fixture fixture;
    const std::string demo_id = fixture.workspace.document().id();
    fixture.workspace.new_document();
    CHECK_EQ(fixture.workspace.tab_count(), size_t(2));
    CHECK_EQ(fixture.workspace.active_tab(), size_t(1));
    CHECK(fixture.workspace.document().path().empty());
    CHECK(!fixture.workspace.document().dirty());
    CHECK(fixture.workspace.document().text().empty());
    // Two untitled tabs must not share one identity: their audit entries and
    // staged changes would be indistinguishable.
    CHECK_NE(fixture.workspace.document().id(), demo_id);
    const std::string first_new_id = fixture.workspace.document().id();
    fixture.workspace.new_document();
    CHECK_NE(fixture.workspace.document().id(), first_new_id);

    // The editor is never left without a document to type into.
    std::string error;
    while (fixture.workspace.tab_count() > 1) {
        CHECK(fixture.workspace.close_tab(fixture.workspace.tab_count() - 1, &error).ok);
    }
    CHECK(fixture.workspace.close_tab(0, &error).ok);
    CHECK_EQ(fixture.workspace.tab_count(), size_t(1));
    CHECK(fixture.workspace.document().path().empty());
    CHECK(fixture.workspace.document().text().empty());
}

TEST_CASE("workspace/the first open fills the empty placeholder tab") {
    // Starting with no --demo and no file leaves one empty untitled document;
    // opening a file must fill it rather than leave "untitled" next to it.
    Workspace workspace;
    Workspace::Options options;
    options.policy = Policy::defaults();
    options.demo_content = false;
    std::string error;
    CHECK(workspace.initialise(std::move(options), &error));
    CHECK_EQ(workspace.tab_count(), size_t(1));

    const std::string path = write_temp_document("placeholder.md", "hello\n");
    CHECK(workspace.open_file(path, &error).ok);
    CHECK_EQ(workspace.tab_count(), size_t(1));
    CHECK_EQ(workspace.document().path(), path);
    CHECK_EQ(workspace.document().text(), std::string("hello\n"));
    ::DeleteFileA(path.c_str());
}

TEST_CASE("workspace/reload reads the file again instead of activating its tab") {
    Fixture fixture;
    const std::string path = write_temp_document("reload_tab.md", "version one\n");
    std::string error;
    CHECK(fixture.workspace.open_file(path, &error).ok);
    CHECK_EQ(fixture.workspace.active_tab(), size_t(1));

    { std::ofstream stream(path, std::ios::binary); stream << "version two\n"; }
    CHECK(fixture.workspace.reload_from_disk(&error).ok);
    CHECK_EQ(fixture.workspace.document().text(), std::string("version two\n"));
    CHECK_EQ(fixture.workspace.tab_count(), size_t(2));
    ::DeleteFileA(path.c_str());
}

TEST_CASE("workspace/a staged change is refused when the writer switched documents") {
    Fixture fixture;
    fixture.workspace.policy().require_confirmation = true;
    const std::string other = write_temp_document("switch.md", "other document\n");

    // The "user" switches tabs while the diff panel is waiting for an answer.
    fixture.bridge.decision_callback = [&](const Json&) {
        std::string error;
        fixture.workspace.open_file(other, &error);
        return true; // accepted, but for a document that is no longer active
    };

    Workspace::WriteOutcome outcome = fixture.workspace.apply_agent_patches(
        {Patch::append_section("## Late")}, "apply_patch", "s", "switch while staged");
    CHECK(!outcome.ok);
    CHECK_EQ(outcome.error_code, std::string("conflict"));
    // The other document was not touched by the patches meant for the first one.
    CHECK(fixture.workspace.document().text().find("## Late") == std::string::npos);
    CHECK_EQ(fixture.workspace.document().path(), other);
    ::DeleteFileA(other.c_str());
}
