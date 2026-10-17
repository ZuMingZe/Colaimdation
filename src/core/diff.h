// Colaimdation - diff helpers.
//
// The editor needs two different views of the same comparison:
//   * a unified diff for the confirmation UI and the audit log, and
//   * a structured line list for the in-app "Agent changes" preview panel.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "core/json.h"

namespace colai {

struct DiffLine {
    enum class Kind { Context, Insert, Delete };
    Kind kind = Kind::Context;
    std::string_view text;
    size_t old_line = 0; // 1-based, 0 when the line does not exist on that side
    size_t new_line = 0;
};

std::vector<DiffLine> diff_lines(std::string_view before, std::string_view after);

// Unified diff with `context` lines of context. Returns an empty string when
// the two inputs are identical.
std::string unified_diff(std::string_view before, std::string_view after,
                         std::string_view before_label = "before",
                         std::string_view after_label = "after", int context = 3);

// Structured diff for the UI / agents: counts plus per-line operations.
Json diff_to_json(std::string_view before, std::string_view after);

} // namespace colai
