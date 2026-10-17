// Audit log and security policy.
#include "test_framework.h"

#include "core/audit.h"
#include "core/policy.h"
#include "core/utf8.h"

#include <cstdio>
#include <fstream>
#include <sstream>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

using namespace colai;

namespace {

std::string temp_path(const std::string& suffix) {
    char buffer[MAX_PATH] = {};
    ::GetTempPathA(MAX_PATH, buffer);
    return std::string(buffer) + "colaimdation_test_" + utf8::fnv1a64_hex(suffix) + suffix;
}

std::string read_all(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    std::stringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

AuditRecord make_record(const std::string& tool, EditSource source, const std::string& session) {
    AuditRecord record;
    record.tool = tool;
    record.source = source;
    record.session = session;
    record.summary = "did " + tool;
    record.status = AuditStatus::Applied;
    record.document_id = "doc-1";
    record.revision_before = "rev-a";
    record.revision_after = "rev-b";
    record.bytes_before = 10;
    record.bytes_after = 20;
    record.changes.push_back({0, 0, "hello"});
    return record;
}

} // namespace

TEST_CASE("audit/assigns ids and timestamps") {
    AuditLog log;
    const int64_t first = log.record(make_record("read", EditSource::Agent, "s1"));
    const int64_t second = log.record(make_record("write", EditSource::Agent, "s1"));
    CHECK_EQ(first, int64_t(1));
    CHECK_EQ(second, int64_t(2));
    CHECK_EQ(log.last_id(), int64_t(2));
    CHECK_EQ(log.size(), size_t(2));

    const AuditRecord* found = log.find(second);
    CHECK(found != nullptr);
    CHECK_EQ(found->tool, std::string("write"));
    CHECK(found->timestamp_ms > 0);
    CHECK(!iso8601(found->timestamp_ms).empty());
    CHECK_EQ(iso8601(0), std::string("1970-01-01T00:00:00.000Z"));
}

TEST_CASE("audit/query filters by tool, session, source and status") {
    AuditLog log;
    log.record(make_record("read_document", EditSource::Agent, "alpha"));
    log.record(make_record("apply_patch", EditSource::Agent, "alpha"));
    log.record(make_record("apply_patch", EditSource::User, "beta"));
    AuditRecord rejected = make_record("apply_patch", EditSource::Agent, "beta");
    rejected.status = AuditStatus::Rejected;
    log.record(std::move(rejected));

    AuditQuery by_tool;
    by_tool.tool = "apply_patch";
    CHECK_EQ(log.query(by_tool).size(), size_t(3));

    AuditQuery by_session;
    by_session.session = "alpha";
    CHECK_EQ(log.query(by_session).size(), size_t(2));

    AuditQuery by_source;
    by_source.source = EditSource::User;
    CHECK_EQ(log.query(by_source).size(), size_t(1));

    AuditQuery by_status;
    by_status.status = AuditStatus::Rejected;
    CHECK_EQ(log.query(by_status).size(), size_t(1));

    AuditQuery limited;
    limited.limit = 2;
    const auto newest = log.query(limited);
    CHECK_EQ(newest.size(), size_t(2));
    // Newest first by default.
    CHECK(newest[0].id > newest[1].id);
    const auto oldest = log.query(limited, false);
    CHECK(oldest[0].id < oldest[1].id);

    AuditQuery by_document;
    by_document.document_id = "other";
    CHECK(log.query(by_document).empty());
}

TEST_CASE("audit/retention capacity evicts the oldest records") {
    AuditLog log;
    log.set_capacity(16, 1024 * 1024);
    for (int i = 0; i < 40; ++i) log.record(make_record("t" + std::to_string(i), EditSource::Agent, "s"));
    CHECK_EQ(log.size(), size_t(16));
    CHECK_EQ(log.last_id(), int64_t(40));
    // The oldest surviving record is #25 (ids 1..24 were evicted).
    AuditQuery query;
    query.limit = 100;
    const auto all = log.query(query, false);
    CHECK_EQ(all.front().id, int64_t(25));
}

TEST_CASE("audit/jsonl file is written and flushed") {
    AuditLog log;
    const std::string path = temp_path("_audit.jsonl");
    ::DeleteFileA(path.c_str());
    std::string error;
    CHECK(log.open_file(path, &error));
    CHECK(log.file_enabled());
    CHECK_EQ(log.file_path(), path);

    log.record(make_record("read_document", EditSource::Agent, "alpha"));
    log.record(make_record("apply_patch", EditSource::Agent, "alpha"));
    log.flush();

    const std::string contents = read_all(path);
    CHECK(!contents.empty());
    CHECK_EQ(contents.back(), '\n');
    // One JSON object per line, each parseable.
    std::stringstream stream(contents);
    std::string line;
    int lines = 0;
    while (std::getline(stream, line)) {
        if (line.empty()) continue;
        ++lines;
        JsonParseError parse_error;
        auto parsed = json_parse(line, &parse_error);
        CHECK(parsed.has_value());
        CHECK_EQ(parsed->str("tool"), lines == 1 ? std::string("read_document")
                                                 : std::string("apply_patch"));
        CHECK_EQ(parsed->str("status"), std::string("applied"));
        CHECK(!parsed->str("timestamp").empty());
    }
    CHECK_EQ(lines, 2);

    log.close_file();
    CHECK(!log.file_enabled());
    ::DeleteFileA(path.c_str());
}

TEST_CASE("audit/rejected entries carry the error") {
    AuditLog log;
    AuditRecord record = make_record("apply_patch", EditSource::Agent, "alpha");
    record.status = AuditStatus::Rejected;
    record.error_code = "conflict";
    record.error_message = "revision mismatch";
    log.record(std::move(record));
    const Json json = log.find(1)->to_json(true);
    CHECK_EQ(json.str("status"), std::string("rejected"));
    CHECK_EQ(json["error"].str("code"), std::string("conflict"));
    CHECK_EQ(json["error"].str("message"), std::string("revision mismatch"));
}

TEST_CASE("audit/record json exposes changes and byte delta") {
    AuditLog log;
    log.record(make_record("apply_patch", EditSource::Agent, "alpha"));
    const Json json = log.find(1)->to_json(false);
    CHECK_EQ(json.str("source"), std::string("agent"));
    CHECK_EQ(json.integer("byte_delta"), 10);
    CHECK(json.boolean("reversible") == false);
    CHECK_EQ(json["changes"].size(), size_t(1));
    CHECK_EQ(json["changes"][0].str("inserted"), std::string("hello"));
}

TEST_CASE("policy/defaults generate a strong token and are readable as json") {
    Policy policy = Policy::defaults();
    CHECK_EQ(policy.token.size(), size_t(64));
    CHECK(policy.require_token);
    CHECK(policy.allow_write);
    CHECK(!policy.require_confirmation);
    CHECK(policy.protect_front_matter == false);

    const Json json = policy.to_json();
    CHECK(json.boolean("require_token"));
    CHECK(json.boolean("token_present"));
    CHECK_EQ(json.integer("max_patches_per_call"), 64);
    // The token value itself is never serialised into status output.
    CHECK(json.find("token") == nullptr);
}

TEST_CASE("policy/tokens are unique") {
    const std::string a = Policy::generate_token();
    const std::string b = Policy::generate_token();
    CHECK_NE(a, b);
    CHECK_EQ(a.size(), size_t(64));
}

TEST_CASE("policy/secure comparison") {
    CHECK(secure_token_equals("abc", "abc"));
    CHECK(!secure_token_equals("abc", "abd"));
    CHECK(!secure_token_equals("abc", "ab"));
    CHECK(!secure_token_equals("", ""));
    CHECK(!secure_token_equals("abc", ""));
}

TEST_CASE("policy/apply json updates fields and reports unknown keys") {
    Policy policy = Policy::defaults();
    Json patch = Json::object();
    patch.set("allow_write", Json(false));
    patch.set("require_confirmation", Json(true));
    patch.set("max_patches_per_call", Json(7));
    patch.set("allowed_directory", Json("C:\\notes"));
    patch.set("nonsense_key", Json(1));
    Json kinds = Json::array();
    kinds.push(Json("paragraph"));
    kinds.push(Json("heading"));
    patch.set("allowed_block_kinds", std::move(kinds));

    std::vector<std::string> warnings;
    policy.apply_json(patch, &warnings);
    CHECK(!policy.allow_write);
    CHECK(policy.require_confirmation);
    CHECK_EQ(policy.max_patches_per_call, size_t(7));
    CHECK_EQ(policy.allowed_directory, std::string("C:\\notes"));
    CHECK_EQ(policy.allowed_block_kinds.size(), size_t(2));
    CHECK_EQ(warnings.size(), size_t(1));
    CHECK_STR_CONTAINS(warnings[0], "nonsense_key");
}

TEST_CASE("policy/from_json never inherits a token and clamps values") {
    Json config = Json::object();
    config.set("token", Json(""));
    config.set("max_patches_per_call", Json(0));
    config.set("max_read_bytes", Json(1));
    config.set("audit_history_limit", Json(-5));
    config.set("require_token", Json(false));

    Policy policy = Policy::from_json(config, nullptr);
    CHECK_EQ(policy.token.size(), size_t(64)); // regenerated
    CHECK_EQ(policy.max_patches_per_call, size_t(1));
    CHECK(policy.max_read_bytes >= size_t(1024));
    CHECK(policy.audit_history_limit >= size_t(16));
    CHECK(!policy.require_token);
}

TEST_CASE("policy/round trip through json") {
    Policy original = Policy::defaults();
    original.allow_write = false;
    original.require_confirmation = true;
    original.allowed_directory = "D:\\vault";
    original.allowed_line_ranges = {{1, 10}, {20, 30}};
    const Json json = original.to_json();

    Policy restored = Policy::from_json(json, nullptr);
    CHECK_EQ(restored.require_confirmation, true);
    CHECK_EQ(restored.allowed_directory, std::string("D:\\vault"));
    CHECK_EQ(restored.allowed_line_ranges.size(), size_t(2));
    CHECK_EQ(restored.allowed_line_ranges[1].first, size_t(20));
    CHECK_EQ(restored.allowed_line_ranges[1].second, size_t(30));
}
