// End-to-end MCP transport test: a real loopback HTTP server, real JSON-RPC,
// real tool calls against a real Workspace.
#include "test_framework.h"

#include "core/utf8.h"
#include "core/workspace.h"
#include "mcp/http_server.h"
#include "mcp/mcp_server.h"

#include <cstdio>
#include <map>
#include <mutex>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

using namespace colai;

namespace {

// A bridge that behaves like the real editor: the job runs inline (the UI
// thread in production) and confirmations resolve immediately with the
// scripted decision.
class ScriptedBridge : public IEditorBridge {
public:
    std::optional<Json> post(std::function<Json()> job, int) override { return job(); }
    void notify(const Json& message) override { messages.push_back(message); }

    bool request_confirmation(const Json& pending, int64_t) override {
        ++confirmations;
        last_pending = pending;
        // Register the wait, decide, then let the engine observe the answer -
        // exactly the handshake the WebView2 bridge performs with the UI.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_decisions[pending.str("id")].push_back(accept_confirmation);
        }
        return accept_confirmation;
    }

    void reload_document(const Document&) override { ++reloads; }
    bool has_ui() const override { return true; }

    std::vector<Json> messages;
    std::map<std::string, std::vector<bool>> pending_decisions;
    std::mutex mutex_;
    bool accept_confirmation = true;
    int reloads = 0;
    int confirmations = 0;
    Json last_pending;
};

struct Fixture {
    Workspace workspace;
    ScriptedBridge bridge;
    std::unique_ptr<mcp::McpServer> server;

    Fixture() {
        Workspace::Options options;
        options.policy = Policy::defaults();
        std::string error;
        workspace.initialise(std::move(options), &error);
        workspace.set_bridge(&bridge);
        workspace.set_demo_content("C:\\docs\\mcp-test.md");
        workspace.document().set_text("# Title\n\nbody\n", EditSource::System, "setup", "setup", true);
        workspace.document().clear_history();
        workspace.audit().clear();
        workspace.adopt_document_text();
        server = std::make_unique<mcp::McpServer>();
        if (!server->initialise(workspace, 0, &error)) {
            throw std::runtime_error("cannot start the MCP server: " + error);
        }
    }

    ~Fixture() { server->stop(); }

    // Sends one raw JSON-RPC document to the loopback endpoint.
    Json rpc(const std::string& body, const std::string& path = "/mcp") {
        const auto response =
            http_post("127.0.0.1", server->status().port, path, body, workspace.policy().token);
        if (!response.ok) throw std::runtime_error("HTTP request failed: " + response.error);
        if (response.body.empty()) return Json();
        JsonParseError parse_error;
        auto parsed = json_parse(response.body, &parse_error);
        if (!parsed) throw std::runtime_error("bad JSON response: " + parse_error.message);
        return *parsed;
    }

    Json call_request(const std::string& method, Json params, int id) {
        Json request = Json::object();
        request.set("jsonrpc", Json("2.0"));
        request.set("id", Json(static_cast<int64_t>(id)));
        request.set("method", Json(method));
        request.set("params", std::move(params));
        return request;
    }

    Json call_tool(const std::string& name, Json arguments, int id = 7) {
        Json params = Json::object();
        params.set("name", Json(name));
        params.set("arguments", std::move(arguments));
        const Json response = rpc(call_request("tools/call", std::move(params), id).dump(false));
        if (!response.contains("result")) {
            throw std::runtime_error("tool call failed: " + response.dump(false));
        }
        return response["result"];
    }

    static Json structured(const Json& call_result) {
        const Json* value = call_result.find("structuredContent");
        if (!value) throw std::runtime_error("tool result has no structuredContent");
        return *value;
    }
};

// Builds a JSON array from patch objects: `patch_array(a, b)`.
Json patch_array() { return Json::array(); }

template <typename... Rest> Json patch_array(Json first, Rest... rest) {
    Json list = Json::array();
    list.push(std::move(first));
    const Json tail = patch_array(std::move(rest)...);
    for (const Json& entry : tail.items()) list.push(entry);
    return list;
}

size_t http_status(const mcp::McpServer& server, const std::string& path, const std::string& body,
                   const std::string& token) {
    const auto response = http_post("127.0.0.1", server.status().port, path, body, token);
    return response.ok ? static_cast<size_t>(response.status) : 0;
}

} // namespace

TEST_CASE("mcp/initialize advertises the server and its capabilities") {
    Fixture fixture;
    Json params = Json::object();
    params.set("protocolVersion", Json("2025-06-18"));
    params.set("clientInfo", Json(Json::object().set("name", Json("test-agent")).set("version", Json("1.0"))));
    const Json response = fixture.rpc(fixture.call_request("initialize", params, 1).dump(false));
    CHECK(response.contains("result"));
    const Json result = response["result"];
    CHECK_EQ(result.str("protocolVersion"), std::string("2025-06-18"));
    CHECK_EQ(result["serverInfo"].str("name"), std::string("Colaimdation"));
    CHECK(result["capabilities"].contains("tools"));
    CHECK(result["capabilities"].contains("resources"));
    CHECK_STR_CONTAINS(result.str("instructions"), "never by reading or writing files");
}

TEST_CASE("mcp/tools/list describes every built-in tool with a schema") {
    Fixture fixture;
    const Json response =
        fixture.rpc(fixture.call_request("tools/list", Json::object(), 2).dump(false));
    CHECK(response.contains("result"));
    const Json tools = response["result"]["tools"];
    CHECK(tools.items().size() >= size_t(15));

    bool saw_patch = false, saw_structure = false, saw_rollback = false, saw_audit = false;
    for (const Json& tool : tools.items()) {
        CHECK(!tool.str("name").empty());
        CHECK(!tool.str("description").empty());
        CHECK_EQ(tool["inputSchema"].str("type"), std::string("object"));
        const std::string name = tool.str("name");
        CHECK_EQ(name.rfind("colaimdation_", 0), size_t(0));
        if (name == "colaimdation_apply_patch") saw_patch = true;
        if (name == "colaimdation_get_structure") saw_structure = true;
        if (name == "colaimdation_rollback") saw_rollback = true;
        if (name == "colaimdation_get_audit_log") saw_audit = true;
    }
    CHECK(saw_patch);
    CHECK(saw_structure);
    CHECK(saw_rollback);
    CHECK(saw_audit);
}

TEST_CASE("mcp/authentication rejects a missing or wrong token") {
    Fixture fixture;
    const std::string body =
        fixture.call_request("tools/list", Json::object(), 1).dump(false);
    CHECK_EQ(http_status(*fixture.server, "/mcp", body, ""), size_t(401));
    CHECK_EQ(http_status(*fixture.server, "/mcp", body, "not-the-token"), size_t(401));
    CHECK_EQ(http_status(*fixture.server, "/mcp", body, fixture.workspace.policy().token), size_t(200));
}

TEST_CASE("mcp/health endpoint answers without a token") {
    Fixture fixture;
    const auto response =
        http_post("127.0.0.1", fixture.server->status().port, "/health", "{}", "");
    CHECK(response.ok);
    CHECK_EQ(response.status, 200);
    auto parsed = json_parse(response.body);
    CHECK(parsed.has_value());
    CHECK_EQ(parsed->str("name"), std::string("Colaimdation"));
    CHECK_EQ(parsed->str("status"), std::string("ok"));
}

TEST_CASE("mcp/unknown paths and methods are refused") {
    Fixture fixture;
    const std::string body = fixture.call_request("tools/list", Json::object(), 1).dump(false);
    CHECK_EQ(http_status(*fixture.server, "/nope", body, fixture.workspace.policy().token), size_t(404));

    Json response = fixture.rpc(
        fixture.call_request("does/not/exist", Json::object(), 3).dump(false));
    CHECK(response.contains("error"));
    CHECK_EQ(response["error"].integer("code"), int64_t(-32601));
}

TEST_CASE("mcp/malformed JSON produces a parse error") {
    Fixture fixture;
    const Json response = fixture.rpc("{not json");
    CHECK(response.contains("error"));
    CHECK_EQ(response["error"].integer("code"), int64_t(-32700));
}

TEST_CASE("mcp/unknown tool name is an MCP tool error, not a transport error") {
    Fixture fixture;
    const Json result = fixture.call_tool("colaimdation_does_not_exist", Json::object());
    CHECK(result.boolean("isError"));
    CHECK_EQ(result["structuredContent"].str("code"), std::string("unknown_tool"));
}

TEST_CASE("mcp/read_document returns content, metadata and the caret") {
    Fixture fixture;
    fixture.workspace.document().set_selection(0, 5, EditSource::User);
    const Json out = Fixture::structured(fixture.call_tool("colaimdation_read_document", Json::object()));
    CHECK_EQ(out.str("content"), std::string("# Title\n\nbody\n"));
    CHECK_EQ(out["document"].integer("blocks"), 2);
    CHECK_EQ(out["document"].str("name"), std::string("mcp-test.md"));
    CHECK_EQ(out["selection"].integer("end"), 5);
    CHECK_EQ(out["range"].integer("byte_start"), 0);
}

TEST_CASE("mcp/get_structure lists addressable blocks") {
    Fixture fixture;
    const Json out = Fixture::structured(
        fixture.call_tool("colaimdation_get_structure", Json::object()));
    CHECK_EQ(out.integer("blocks_total"), 2);
    CHECK_EQ(out["blocks"].items().size(), size_t(2));
    CHECK_EQ(out["blocks"][0].str("block_id"), std::string("b0"));
    CHECK_EQ(out["blocks"][0].str("kind"), std::string("heading"));
    CHECK_EQ(out["blocks"][1].str("kind"), std::string("paragraph"));
    CHECK(!out["fingerprint"].as_string().empty());
}

TEST_CASE("mcp/apply_patch edits the document and reports the diff") {
    Fixture fixture;
    const std::string revision = fixture.workspace.document().revision();

    Json patch = Json::object();
    patch.set("op", Json("replace_block"));
    patch.set("block_id", Json("b1"));
    patch.set("text", Json("the body written by an agent"));
    Json arguments = Json::object();
    arguments.set("patches", patch_array(patch));
    arguments.set("expected_revision", Json(revision));
    arguments.set("summary", Json("rewrite the body"));

    const Json out = Fixture::structured(fixture.call_tool("colaimdation_apply_patch", arguments));
    CHECK_EQ(out.str("status"), std::string("applied"));
    CHECK_EQ(out.integer("applied_patches"), 1);
    CHECK_EQ(out.str("revision_before"), revision);
    CHECK_NE(out.str("revision_after"), revision);
    CHECK_STR_CONTAINS(out.str("diff"), "the body written by an agent");
    CHECK_STR_CONTAINS(fixture.workspace.document().text(), "the body written by an agent");
    CHECK_STR_CONTAINS(fixture.workspace.document().text(), "# Title");

    // The write is attributed to the session in the audit log.
    AuditQuery query;
    query.tool = "apply_patch";
    const auto entries = fixture.workspace.audit().query(query);
    CHECK_EQ(entries.size(), size_t(1));
    CHECK_EQ(entries[0].source, EditSource::Agent);
    CHECK(entries[0].reversible);
}

TEST_CASE("mcp/apply_patch refuses a stale expected_revision") {
    Fixture fixture;
    Json patch = Json::object();
    patch.set("op", Json("append_section"));
    patch.set("text", Json("## Extra"));
    Json arguments = Json::object();
    arguments.set("patches", patch_array(patch));
    arguments.set("expected_revision", Json("00000000000000000000000000000000"));

    const Json result = fixture.call_tool("colaimdation_apply_patch", arguments);
    CHECK(result.boolean("isError"));
    CHECK_EQ(result["structuredContent"].str("code"), std::string("conflict"));
    CHECK(fixture.workspace.document().text().find("Extra") == std::string::npos);
}

TEST_CASE("mcp/apply_patch preview does not change the document") {
    Fixture fixture;
    const std::string before = fixture.workspace.document().text();
    Json patch = Json::object();
    patch.set("op", Json("append_section"));
    patch.set("text", Json("## Previewed"));
    Json arguments = Json::object();
    arguments.set("patches", patch_array(patch));
    arguments.set("preview", Json(true));

    const Json out = Fixture::structured(fixture.call_tool("colaimdation_apply_patch", arguments));
    CHECK_EQ(out.str("status"), std::string("preview"));
    CHECK_STR_CONTAINS(out.str("diff"), "## Previewed");
    CHECK_EQ(fixture.workspace.document().text(), before);
}

TEST_CASE("mcp/apply_patch rejects an overlapping batch atomically") {
    Fixture fixture;
    Json first = Json::object();
    first.set("op", Json("replace_range"));
    first.set("start", Json(0));
    first.set("end", Json(5));
    first.set("text", Json("XXXXX"));
    Json second = Json::object();
    second.set("op", Json("replace_range"));
    second.set("start", Json(3));
    second.set("end", Json(8));
    second.set("text", Json("YYYYY"));
    Json arguments = Json::object();
    arguments.set("patches", patch_array(first, second));

    const Json result = fixture.call_tool("colaimdation_apply_patch", arguments);
    CHECK(result.boolean("isError"));
    CHECK_STR_CONTAINS(result["structuredContent"].str("message"), "overlap");
    CHECK_STR_CONTAINS(fixture.workspace.document().text(), "# Title");
}

TEST_CASE("mcp/confirmation policy stages the change until the user accepts") {
    Fixture fixture;
    fixture.workspace.policy().require_confirmation = true;
    fixture.bridge.accept_confirmation = false;

    Json patch = Json::object();
    patch.set("op", Json("append_section"));
    patch.set("text", Json("## Pending"));
    Json arguments = Json::object();
    arguments.set("patches", patch_array(patch));

    const Json result = fixture.call_tool("colaimdation_apply_patch", arguments);
    CHECK(result.boolean("isError"));
    CHECK_EQ(result["structuredContent"].str("code"), std::string("rejected_by_user"));
    CHECK(fixture.workspace.document().text().find("Pending") == std::string::npos);

    fixture.bridge.accept_confirmation = true;
    const Json accepted = Fixture::structured(fixture.call_tool("colaimdation_apply_patch", arguments));
    CHECK_EQ(accepted.str("status"), std::string("applied"));
    CHECK_STR_CONTAINS(fixture.workspace.document().text(), "## Pending");
}

TEST_CASE("mcp/blocked writes are refused with permission_denied") {
    Fixture fixture;
    fixture.workspace.policy().allow_write = false;
    Json patch = Json::object();
    patch.set("op", Json("append_section"));
    patch.set("text", Json("nope"));
    Json arguments = Json::object();
    arguments.set("patches", patch_array(patch));
    const Json result = fixture.call_tool("colaimdation_apply_patch", arguments);
    CHECK(result.boolean("isError"));
    CHECK_EQ(result["structuredContent"].str("code"), std::string("permission_denied"));
}

TEST_CASE("mcp/set_selection moves the caret and get_selection reports it") {
    Fixture fixture;
    Json arguments = Json::object();
    arguments.set("start", Json(0));
    arguments.set("end", Json(7));
    const Json out = Fixture::structured(
        fixture.call_tool("colaimdation_set_selection", arguments));
    CHECK_EQ(out.str("status"), std::string("ok"));
    CHECK_EQ(fixture.workspace.document().selection().end(), size_t(7));
    CHECK(!fixture.workspace.document().selection().collapsed());

    const Json selection = Fixture::structured(
        fixture.call_tool("colaimdation_get_selection", Json::object()));
    CHECK_EQ(selection.integer("start"), 0);
    CHECK_EQ(selection.integer("end"), 7);
    CHECK_EQ(selection.str("text"), std::string("# Title"));
}

TEST_CASE("mcp/user edits are visible through get_user_changes") {
    Fixture fixture;
    const std::string revision = fixture.workspace.document().revision();
    Workspace::EditReport report;
    report.revision_before = revision;
    report.text = "# Title\n\nbody plus a human sentence\n";
    fixture.workspace.apply_user_edit(report);

    Json arguments = Json::object();
    arguments.set("since_revision", Json(revision));
    const Json out = Fixture::structured(
        fixture.call_tool("colaimdation_get_user_changes", arguments));
    CHECK_EQ(out.integer("change_count"), 1);
    CHECK_EQ(out["changes"][0].str("source"), std::string("user"));
}

TEST_CASE("mcp/rollback reverts the last agent change") {
    Fixture fixture;
    Json patch = Json::object();
    patch.set("op", Json("append_section"));
    patch.set("text", Json("## Agent section"));
    Json arguments = Json::object();
    arguments.set("patches", patch_array(patch));
    CHECK_EQ(Fixture::structured(fixture.call_tool("colaimdation_apply_patch", arguments)).str("status"),
             std::string("applied"));
    CHECK_STR_CONTAINS(fixture.workspace.document().text(), "Agent section");

    Json rollback_arguments = Json::object();
    rollback_arguments.set("last", Json(true));
    rollback_arguments.set("source", Json("agent"));
    const Json out = Fixture::structured(
        fixture.call_tool("colaimdation_rollback", rollback_arguments));
    CHECK_EQ(out.str("status"), std::string("applied"));
    CHECK_EQ(out.integer("reverted_edits"), 1);
    CHECK(fixture.workspace.document().text().find("Agent section") == std::string::npos);
    CHECK_STR_CONTAINS(fixture.workspace.document().text(), "# Title");
}

TEST_CASE("mcp/audit log is queryable over the transport") {
    Fixture fixture;
    Json patch = Json::object();
    patch.set("op", Json("append_section"));
    patch.set("text", Json("## Audited"));
    Json arguments = Json::object();
    arguments.set("patches", patch_array(patch));
    arguments.set("summary", Json("audited change"));
    fixture.call_tool("colaimdation_apply_patch", arguments);

    Json query = Json::object();
    query.set("limit", Json(10));
    query.set("source", Json("agent"));
    query.set("include_diff", Json(true));
    const Json out = Fixture::structured(fixture.call_tool("colaimdation_get_audit_log", query));
    CHECK(out.integer("count") >= 1);
    CHECK_EQ(out["entries"][0].str("summary"), std::string("audited change"));
    CHECK_EQ(out["entries"][0].str("source"), std::string("agent"));
    CHECK_STR_CONTAINS(out["entries"][0].str("diff"), "## Audited");
}

TEST_CASE("mcp/set_policy requires acknowledgement and updates the rules") {
    Fixture fixture;
    Json arguments = Json::object();
    arguments.set("allow_write", Json(false));
    Json refused = fixture.call_tool("colaimdation_set_policy", arguments);
    CHECK(refused.boolean("isError"));
    CHECK_STR_CONTAINS(refused["structuredContent"].str("message"), "confirm");

    arguments.set("confirm", Json("yes"));
    const Json out = Fixture::structured(fixture.call_tool("colaimdation_set_policy", arguments));
    CHECK(!out.boolean("allow_write"));
    CHECK(!fixture.workspace.policy().allow_write);
}

TEST_CASE("mcp/resources expose the document and its structure") {
    Fixture fixture;
    const Json list = fixture.rpc(fixture.call_request("resources/list", Json::object(), 9).dump(false));
    CHECK(list["result"]["resources"].items().size() >= size_t(2));

    Json params = Json::object();
    params.set("uri", Json("colaimdation://document"));
    const Json read = fixture.rpc(fixture.call_request("resources/read", params, 10).dump(false));
    CHECK_EQ(read["result"]["contents"][0].str("text"), std::string("# Title\n\nbody\n"));
}

TEST_CASE("mcp/batch requests are answered together") {
    Fixture fixture;
    Json batch = Json::array();
    batch.push(fixture.call_request("tools/list", Json::object(), 1));
    batch.push(fixture.call_request("ping", Json::object(), 2));
    const Json response = fixture.rpc(batch.dump(false));
    CHECK(response.is_array());
    CHECK(response.items().size() >= size_t(1));
}

TEST_CASE("mcp/notification-shaped calls produce no response body") {
    Fixture fixture;
    Json request = Json::object();
    request.set("jsonrpc", Json("2.0"));
    request.set("method", Json("notifications/initialized"));
    request.set("params", Json(Json::object().set("session_id", Json("stdio-test"))));
    const Json response = fixture.rpc(request.dump(false));
    CHECK(response.is_null());
}

TEST_CASE("mcp/discovery file describes the endpoint") {
    Fixture fixture;
    const std::string path =
        std::string(std::getenv("TEMP") ? std::getenv("TEMP") : ".") + "\\colaimdation_mcp_test.json";
    ::DeleteFileA(path.c_str());
    std::string error;
    CHECK(fixture.server->write_discovery_file(path, &error));
    CHECK_EQ(fixture.server->discovery_path(), path);

    std::string contents;
    CHECK(Workspace::read_text_file(path, &contents, &error).ok);
    auto parsed = json_parse(contents);
    CHECK(parsed.has_value());
    CHECK_EQ(parsed->str("name"), std::string("Colaimdation"));
    CHECK_STR_CONTAINS(parsed->str("url"), "127.0.0.1");
    CHECK_EQ(parsed->str("token"), fixture.workspace.policy().token);
    CHECK(parsed->contains("tools"));
    ::DeleteFileA(path.c_str());
}
