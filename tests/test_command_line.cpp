// Command line contract. Regression coverage for a real bug: "--demo" was once
// accepted by the "--allow-dir" branch because the comparison matched a prefix.
#include "test_framework.h"

#include "app/command_line.h"
#include "core/utf8.h"

#include <fstream>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

using namespace colai;

namespace {

CommandLineOptions parse(std::vector<std::string> arguments) {
    return parse_command_line_options(arguments);
}

std::string temp_path(const std::string& name) {
    char buffer[MAX_PATH] = {};
    ::GetTempPathA(MAX_PATH, buffer);
    return std::string(buffer) + "colaimdation_cli_" + name;
}

} // namespace

TEST_CASE("cli/defaults are safe and usable") {
    const CommandLineOptions options = parse({});
    CHECK(!options.show_help);
    CHECK(options.initial_file.empty());
    CHECK(options.demo_content); // welcome document by default
    CHECK_EQ(options.mcp_port, uint16_t(0));
    CHECK(options.theme == "auto");
    CHECK_EQ(options.policy.token.size(), size_t(64));
    CHECK(options.policy.require_token);
    CHECK(options.policy.allow_write);
    CHECK(!options.policy.require_confirmation);
    CHECK(options.warnings.empty());
}

TEST_CASE("cli/--demo and --no-demo are exact matches") {
    // The bug this guards against: prefix matching made "--demo" fall into the
    // "--allow-dir" branch and throw "missing value for --allow-dir".
    const CommandLineOptions demo = parse({"--demo"});
    CHECK(demo.demo_content);
    CHECK(demo.policy.allowed_directory.empty());

    const CommandLineOptions no_demo = parse({"--no-demo"});
    CHECK(!no_demo.demo_content);
    CHECK(no_demo.policy.allowed_directory.empty());
}

TEST_CASE("cli/prefix lookalikes are rejected instead of misparsed") {
    CHECK_THROWS(parse({"--dem"}));
    CHECK_THROWS(parse({"--demo-content"}));
    CHECK_THROWS(parse({"--allow-di"}));
    CHECK_THROWS(parse({"--nonsense"}));
    CHECK_THROWS(parse({"-x"}));
}

TEST_CASE("cli/options that take a value") {
    const CommandLineOptions options =
        parse({"--file", "notes.md", "--port", "41777", "--token", "abc123",
               "--allow-dir", "C:\\vault", "--theme", "dark", "--font-size", "18"});
    CHECK(options.initial_file == "notes.md");
    CHECK_EQ(options.mcp_port, uint16_t(41777));
    CHECK(options.policy.token == "abc123");
    CHECK(options.policy.allowed_directory == "C:\\vault");
    CHECK(options.theme == "dark");
    CHECK_EQ(options.font_size, 18.0);
}

TEST_CASE("cli/switches") {
    const CommandLineOptions options =
        parse({"--read-only", "--confirm", "--no-webview", "--log", "--help"});
    CHECK(options.force_read_only);
    CHECK(options.force_confirmation);
    CHECK(options.no_webview);
    CHECK(options.log_to_file);
    CHECK(options.show_help);
}

TEST_CASE("cli/a positional argument is the document") {
    const CommandLineOptions options = parse({"C:\\docs\\paper.md"});
    CHECK(options.initial_file == "C:\\docs\\paper.md");
    CHECK(options.demo_content);

    // Later positional arguments win, matching "last one opens".
    const CommandLineOptions last = parse({"a.md", "b.md"});
    CHECK(last.initial_file == "b.md");
}

TEST_CASE("cli/relative document paths are recognised as relative") {
    // A relative path has to be resolved against the executable's directory:
    // launched from a shortcut or another program the working directory is
    // elsewhere, and a bare "notes.md" then silently opened nothing.
    CHECK(!colai::utf8::is_absolute_path("notes.md"));
    CHECK(!colai::utf8::is_absolute_path("sub\\notes.md"));
    CHECK(!colai::utf8::is_absolute_path("sub/notes.md"));
    CHECK(!colai::utf8::is_absolute_path("notes with spaces.md"));

    CHECK(colai::utf8::is_absolute_path("C:\\docs\\notes.md"));
    CHECK(colai::utf8::is_absolute_path("c:/docs/notes.md"));
    CHECK(colai::utf8::is_absolute_path("\\\\server\\share\\notes.md"));
    CHECK(colai::utf8::is_absolute_path("\\rooted\\notes.md"));
}

TEST_CASE("cli/missing values are reported") {
    CHECK_THROWS(parse({"--file"}));
    CHECK_THROWS(parse({"--port"}));
    CHECK_THROWS(parse({"--token"}));
    CHECK_THROWS(parse({"--allow-dir"}));
    CHECK_THROWS(parse({"--config"}));
    CHECK_THROWS(parse({"--font-size"}));
}

TEST_CASE("cli/numeric validation") {
    CHECK_THROWS(parse({"--port", "abc"}));
    CHECK_THROWS(parse({"--port", "-1"}));
    CHECK_THROWS(parse({"--port", "70000"}));
    CHECK_THROWS(parse({"--port", "99999999999999999999"}));
    CHECK_EQ(parse({"--port", "0"}).mcp_port, uint16_t(0));

    CHECK_THROWS(parse({"--font-size", "huge"}));
    CHECK_THROWS(parse({"--font-size", "2"}));
    CHECK_THROWS(parse({"--font-size", "100"}));
    CHECK_EQ(parse({"--font-size", "12.5"}).font_size, 12.5);
}

TEST_CASE("cli/--help variants") {
    CHECK(parse({"--help"}).show_help);
    CHECK(parse({"-h"}).show_help);
    CHECK(parse({"/?"}).show_help);
    CHECK(parse({"--help", "--demo"}).show_help);
}

TEST_CASE("cli/usage text mentions every option") {
    const std::string usage = command_line_usage();
    for (const char* option : {"--file", "--port", "--token", "--config", "--read-only", "--confirm",
                               "--allow-dir", "--demo", "--no-demo", "--theme", "--font-size",
                               "--no-webview", "--log", "--help"}) {
        CHECK_STR_CONTAINS(usage, option);
    }
}

TEST_CASE("cli/config file overrides the policy") {
    const std::string path = temp_path("config.json");
    {
        std::ofstream stream(path, std::ios::binary);
        stream << R"({
            "require_confirmation": true,
            "protect_front_matter": true,
            "allowed_directory": "D:\\notes",
            "max_patch_bytes": 4096,
            "mystery_key": 1
        })";
    }
    CommandLineOptions options = parse({"--config", path});
    load_policy_file(options);

    CHECK(options.policy.require_confirmation);
    CHECK(options.policy.protect_front_matter);
    CHECK(options.policy.allowed_directory == "D:\\notes");
    CHECK_EQ(options.policy.max_patch_bytes, size_t(4096));
    CHECK_EQ(options.warnings.size(), size_t(1));
    CHECK_STR_CONTAINS(options.warnings[0], "mystery_key");
    ::DeleteFileA(path.c_str());
}

TEST_CASE("cli/a missing config file warns but does not fail") {
    CommandLineOptions options = parse({"--config", temp_path("does_not_exist.json")});
    load_policy_file(options);
    CHECK_EQ(options.warnings.size(), size_t(1));
    CHECK_STR_CONTAINS(options.warnings[0], "cannot read");
    CHECK(!options.policy.token.empty());
}

TEST_CASE("cli/an invalid config file warns and keeps the defaults") {
    const std::string path = temp_path("broken.json");
    {
        std::ofstream stream(path, std::ios::binary);
        stream << "{ this is not json }";
    }
    CommandLineOptions options = parse({"--config", path});
    load_policy_file(options);
    CHECK_EQ(options.warnings.size(), size_t(1));
    CHECK_STR_CONTAINS(options.warnings[0], "cannot parse");
    CHECK(options.policy.allow_write);
    ::DeleteFileA(path.c_str());
}

TEST_CASE("cli/command line flags survive a config load") {
    const std::string path = temp_path("policy.json");
    {
        std::ofstream stream(path, std::ios::binary);
        stream << R"({"allow_write": false, "require_confirmation": true})";
    }
    CommandLineOptions options = parse({"--config", path, "--read-only"});
    load_policy_file(options);
    CHECK(!options.policy.allow_write);
    CHECK(options.policy.require_confirmation);
    ::DeleteFileA(path.c_str());
}
