// Colaimdation - security policy.
//
// The product promise is that an Agent can only touch the Markdown file the
// editor currently has open, and only through the editor. This file holds the
// tunable rules that enforce that promise; Workspace enforces them.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.h"

namespace colai {

struct Policy {
    // --- authentication ----------------------------------------------------
    // Local bearer token required on every MCP request. Generated at startup
    // when the app launches unless the user pinned one.
    std::string token;
    bool require_token = true;
    // Requests from hosts other than the loopback interface are always refused;
    // this flag additionally refuses requests whose Host header is not loopback.
    bool allow_only_loopback_host_header = true;
    // When true, a well-formed token is accepted without the "Authorization"
    // header being present (used only by COLAIMDATION_INSECURE_TOKEN_URL mode in
    // the dev harness).
    bool allow_token_in_query = true;
    // Idle time after which a session must re-authenticate. 0 disables it.
    int64_t session_ttl_ms = 12ll * 60 * 60 * 1000;

    // --- what agents may do ------------------------------------------------
    bool allow_write = true;             // any mutating tool at all
    bool allow_read = true;
    bool allow_selection_control = true; // move the caret / selection
    bool allow_undo = true;
    bool allow_save = true;
    bool allow_rollback = true;
    bool allow_multi_patch = true;       // apply several patches in one call
    size_t max_patch_bytes = 512 * 1024; // refuse absurd single edits
    size_t max_patches_per_call = 64;
    size_t max_read_bytes = 4 * 1024 * 1024;

    // Restrict writes to the block kinds listed here (empty = allow all).
    std::vector<std::string> allowed_block_kinds;
    // Restrict writes to these 1-based line ranges (empty = allow all).
    std::vector<std::pair<size_t, size_t>> allowed_line_ranges;
    // Never allow writes to the front matter block.
    bool protect_front_matter = false;

    // --- human in the loop -------------------------------------------------
    // When true every agent write is staged and must be confirmed by the user
    // in the diff preview panel before it reaches the document.
    bool require_confirmation = false;
    // Auto-confirm a staged change after this many milliseconds (0 = wait for
    // the user indefinitely).
    int64_t confirmation_timeout_ms = 0;
    bool allow_confirmation_bypass = false; // lets a tool opt out (off by default)

    // --- bookkeeping -------------------------------------------------------
    bool audit_enabled = true;
    bool allow_agent_rollback = true;
    size_t audit_history_limit = 400;
    size_t audit_byte_budget = 64ull * 1024 * 1024;
    // Refuse to open documents outside this directory tree when non-empty.
    std::string allowed_directory;

    Json to_json() const;
    // Applies the subset of fields present in `patch`; unknown keys are ignored
    // and reported through `warnings`.
    void apply_json(const Json& patch, std::vector<std::string>* warnings = nullptr);
    static Policy from_json(const Json& value, std::vector<std::string>* warnings = nullptr);
    static Policy defaults();

    // Generates a fresh 256-bit token rendered as hex.
    static std::string generate_token();
};

// Constant-time comparison for tokens (avoids trivial timing oracles).
bool secure_token_equals(std::string_view a, std::string_view b);

} // namespace colai
