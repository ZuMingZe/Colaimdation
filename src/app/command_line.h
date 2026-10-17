// Colaimdation - command line parsing.
//
// Split out of main.cpp so the CLI contract is covered by unit tests: an option
// that silently falls into the wrong branch (or an unknown flag that is
// accepted) is a real usability bug, and this is where it gets caught.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/policy.h"

namespace colai {

struct CommandLineOptions {
    // Security policy after the command line has been applied. The token is
    // always populated (generated when not supplied).
    Policy policy;
    std::string initial_file;
    std::string config_path;
    std::string state_dir;
    std::string asset_directory;
    std::string theme = "auto";
    // Interface language tag; empty means "use the saved preference, else the OS".
    std::string language;
    double font_size = 16.0;
    uint16_t mcp_port = 0; // 0 = let the OS choose
    // A token was pinned on the command line. It outranks the one saved in the
    // settings file, which in turn outranks generating a fresh one per run.
    bool token_pinned = false;
    bool show_help = false;
    bool demo_content = true;
    bool no_webview = false;
    bool log_to_file = false;
    bool force_read_only = false;
    bool force_confirmation = false;
    // Config-file parse notes, surfaced to the user at startup.
    std::vector<std::string> warnings;
};

// Throws std::runtime_error for unknown options and missing values.
CommandLineOptions parse_command_line_options(const std::vector<std::string>& arguments);

// Loads policy overrides from a JSON file (silently skipped when empty).
void load_policy_file(CommandLineOptions& options);

// The --help text.
const char* command_line_usage();

} // namespace colai
