#include "core/policy.h"

#include "core/utf8.h"

#include <algorithm>

namespace colai {
namespace {

bool json_bool(const Json& value, std::string_view key, bool fallback) {
    const Json* found = value.find(key);
    return found ? found->as_bool(fallback) : fallback;
}

int64_t json_int(const Json& value, std::string_view key, int64_t fallback) {
    const Json* found = value.find(key);
    return found ? found->as_int(fallback) : fallback;
}

std::string json_str(const Json& value, std::string_view key, std::string fallback) {
    const Json* found = value.find(key);
    return found ? found->as_string(std::move(fallback)) : fallback;
}

} // namespace

Policy Policy::defaults() {
    Policy policy;
    policy.token = generate_token();
    return policy;
}

std::string Policy::generate_token() {
    const std::vector<unsigned char> bytes = utf8::random_bytes(32);
    return utf8::hex_encode(bytes.data(), bytes.size());
}

Json Policy::to_json() const {
    Json out = Json::object();
    out.set("require_token", Json(require_token));
    out.set("token_present", Json(!token.empty()));
    out.set("allow_only_loopback_host_header", Json(allow_only_loopback_host_header));
    out.set("allow_token_in_query", Json(allow_token_in_query));
    out.set("session_ttl_ms", Json(session_ttl_ms));
    out.set("allow_read", Json(allow_read));
    out.set("allow_write", Json(allow_write));
    out.set("allow_selection_control", Json(allow_selection_control));
    out.set("allow_undo", Json(allow_undo));
    out.set("allow_save", Json(allow_save));
    out.set("allow_rollback", Json(allow_rollback));
    out.set("allow_multi_patch", Json(allow_multi_patch));
    out.set("max_patch_bytes", Json(static_cast<int64_t>(max_patch_bytes)));
    out.set("max_patches_per_call", Json(static_cast<int64_t>(max_patches_per_call)));
    out.set("max_read_bytes", Json(static_cast<int64_t>(max_read_bytes)));
    out.set("protect_front_matter", Json(protect_front_matter));
    out.set("require_confirmation", Json(require_confirmation));
    out.set("confirmation_timeout_ms", Json(confirmation_timeout_ms));
    out.set("allow_confirmation_bypass", Json(allow_confirmation_bypass));
    out.set("audit_enabled", Json(audit_enabled));
    out.set("allow_agent_rollback", Json(allow_agent_rollback));
    out.set("audit_history_limit", Json(static_cast<int64_t>(audit_history_limit)));
    out.set("audit_byte_budget", Json(static_cast<int64_t>(audit_byte_budget)));
    out.set("allowed_directory", Json(allowed_directory));
    Json kinds = Json::array();
    for (const std::string& kind : allowed_block_kinds) kinds.push(Json(kind));
    out.set("allowed_block_kinds", std::move(kinds));
    Json ranges = Json::array();
    for (const auto& range : allowed_line_ranges) {
        Json entry = Json::array();
        entry.push(Json(static_cast<int64_t>(range.first)));
        entry.push(Json(static_cast<int64_t>(range.second)));
        ranges.push(std::move(entry));
    }
    out.set("allowed_line_ranges", std::move(ranges));
    return out;
}

void Policy::apply_json(const Json& patch, std::vector<std::string>* warnings) {
    if (!patch.is_object()) {
        if (warnings) warnings->push_back("policy payload is not an object; ignored");
        return;
    }
    auto warn_unknown = [&](std::string_view key) {
        if (warnings) warnings->push_back("unknown policy key '" + std::string(key) + "'");
    };

    for (const auto& entry : patch.members().entries()) {
        const std::string& key = entry.first;
        const Json& value = entry.second;
        if (key == "token") token = value.as_string(token);
        else if (key == "require_token") require_token = value.as_bool(require_token);
        else if (key == "allow_only_loopback_host_header")
            allow_only_loopback_host_header = value.as_bool(allow_only_loopback_host_header);
        else if (key == "allow_token_in_query") allow_token_in_query = value.as_bool(allow_token_in_query);
        else if (key == "session_ttl_ms") session_ttl_ms = value.as_int(session_ttl_ms);
        else if (key == "allow_read") allow_read = value.as_bool(allow_read);
        else if (key == "allow_write") allow_write = value.as_bool(allow_write);
        else if (key == "allow_selection_control")
            allow_selection_control = value.as_bool(allow_selection_control);
        else if (key == "allow_undo") allow_undo = value.as_bool(allow_undo);
        else if (key == "allow_save") allow_save = value.as_bool(allow_save);
        else if (key == "allow_rollback") allow_rollback = value.as_bool(allow_rollback);
        else if (key == "allow_agent_rollback") allow_agent_rollback = value.as_bool(allow_agent_rollback);
        else if (key == "allow_multi_patch") allow_multi_patch = value.as_bool(allow_multi_patch);
        else if (key == "max_patch_bytes") max_patch_bytes = static_cast<size_t>(std::max<int64_t>(0, value.as_int(static_cast<int64_t>(max_patch_bytes))));
        else if (key == "max_patches_per_call") max_patches_per_call = static_cast<size_t>(std::max<int64_t>(1, value.as_int(static_cast<int64_t>(max_patches_per_call))));
        else if (key == "max_read_bytes") max_read_bytes = static_cast<size_t>(std::max<int64_t>(1024, value.as_int(static_cast<int64_t>(max_read_bytes))));
        else if (key == "protect_front_matter") protect_front_matter = value.as_bool(protect_front_matter);
        else if (key == "require_confirmation") require_confirmation = value.as_bool(require_confirmation);
        else if (key == "confirmation_timeout_ms") confirmation_timeout_ms = value.as_int(confirmation_timeout_ms);
        else if (key == "allow_confirmation_bypass") allow_confirmation_bypass = value.as_bool(allow_confirmation_bypass);
        else if (key == "audit_enabled") audit_enabled = value.as_bool(audit_enabled);
        else if (key == "audit_history_limit") audit_history_limit = static_cast<size_t>(std::max<int64_t>(16, value.as_int(static_cast<int64_t>(audit_history_limit))));
        else if (key == "audit_byte_budget") audit_byte_budget = static_cast<size_t>(std::max<int64_t>(1024 * 1024, value.as_int(static_cast<int64_t>(audit_byte_budget))));
        else if (key == "allowed_directory") allowed_directory = value.as_string(allowed_directory);
        else if (key == "allowed_block_kinds") {
            allowed_block_kinds.clear();
            if (value.is_array()) {
                for (const Json& item : value.items()) allowed_block_kinds.push_back(item.as_string());
            }
        } else if (key == "allowed_line_ranges") {
            allowed_line_ranges.clear();
            if (value.is_array()) {
                for (const Json& item : value.items()) {
                    if (item.is_array() && item.size() >= 2) {
                        allowed_line_ranges.emplace_back(
                            static_cast<size_t>(std::max<int64_t>(1, item[0].as_int(1))),
                            static_cast<size_t>(std::max<int64_t>(1, item[1].as_int(1))));
                    }
                }
            }
        } else {
            warn_unknown(key);
        }
    }
}

Policy Policy::from_json(const Json& value, std::vector<std::string>* warnings) {
    Policy policy = defaults();
    policy.token.clear(); // never inherit a generated token from a config file
    policy.apply_json(value, warnings);
    if (policy.token.empty()) policy.token = generate_token();
    return policy;
}

bool secure_token_equals(std::string_view a, std::string_view b) {
    // Length is not secret, but compare in constant time over the shared length
    // plus a length difference accumulator.
    const size_t max_len = std::max(a.size(), b.size());
    unsigned char diff = static_cast<unsigned char>(a.size() ^ b.size());
    for (size_t i = 0; i < max_len; ++i) {
        const unsigned char ca = i < a.size() ? static_cast<unsigned char>(a[i]) : 0;
        const unsigned char cb = i < b.size() ? static_cast<unsigned char>(b[i]) : 0;
        diff = static_cast<unsigned char>(diff | (ca ^ cb));
    }
    return diff == 0 && !a.empty();
}

} // namespace colai
