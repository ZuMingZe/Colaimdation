// Colaimdation - application entry point.
#include "app/command_line.h"
#include "app/editor_window.h"
#include "app/win_util.h"
#include "core/i18n.h"
#include "core/settings.h"
#include "core/utf8.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <objbase.h>
#include <shellapi.h>
#include <windows.h>

namespace {

// A windowed executable has no console, so attach to the launching shell when
// there is one to make --help and warnings visible.
void attach_parent_console() {
    if (!::AttachConsole(ATTACH_PARENT_PROCESS)) return;
    FILE* stream = nullptr;
    freopen_s(&stream, "CONOUT$", "w", stdout);
    freopen_s(&stream, "CONOUT$", "w", stderr);
}

void show_error(const std::string& message) {
    ::MessageBoxW(nullptr, colai::win::wide(message).c_str(), L"Colaimdation", MB_ICONERROR);
}

// Picks the interface language before the full parse so even argument errors are
// reported in the language the user asked for.
std::string prescan_language(const std::vector<std::string>& arguments) {
    for (size_t i = 0; i < arguments.size(); ++i) {
        if (arguments[i] != "--lang" && arguments[i] != "--language") continue;
        if (i + 1 < arguments.size() && colai::i18n::is_supported(arguments[i + 1])) {
            return colai::i18n::normalize_tag(arguments[i + 1]);
        }
    }
    return {};
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int show_command) {
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    attach_parent_console();

    const std::vector<std::string> arguments =
        colai::win::parse_command_line(::GetCommandLineW());

    // Loopback the language choice into the translator before anything is
    // printed; the saved preference (or the OS locale) is applied later, once
    // the settings file has been read.
    if (const std::string requested = prescan_language(arguments); !requested.empty()) {
        colai::i18n::set_language(requested);
    } else {
        colai::i18n::set_language(colai::i18n::from_system_locale());
    }

    colai::CommandLineOptions command_line;
    try {
        command_line = colai::parse_command_line_options(arguments);
        // Policy warnings come back already localised by the callee.
        colai::load_policy_file(command_line);
    } catch (const std::exception& error) {
        show_error(colai::i18n::tr("cli.error.prefix", {error.what()}));
        std::fputs(colai::command_line_usage(), stderr);
        return 2;
    }

    if (command_line.show_help) {
        std::fputs(colai::command_line_usage(), stdout);
        return 0;
    }

    const HRESULT com_result = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com_result)) {
        show_error(colai::i18n::tr("cli.com_failed"));
        return 3;
    }

    if (!colai::win::register_window_classes(instance, &colai::EditorWindow::window_proc)) {
        show_error(colai::i18n::tr(
            "cli.classes_failed", {colai::win::format_error(::GetLastError())}));
        ::CoUninitialize();
        return 3;
    }

    if (command_line.force_read_only) command_line.policy.allow_write = false;
    if (command_line.force_confirmation) command_line.policy.require_confirmation = true;

    colai::EditorWindow::Options options;
    options.policy = command_line.policy;
    options.initial_file = command_line.initial_file;
    options.token_pinned = command_line.token_pinned;
    options.mcp_port = command_line.mcp_port;
    // A relative document path is resolved against the executable's own folder,
    // not the process working directory. Launched from a shortcut, a file
    // association or another program, the working directory is often somewhere
    // else entirely, and a bare "SKILL.md" then silently failed to open while
    // the window looked perfectly normal.
    if (!options.initial_file.empty() && !colai::utf8::is_absolute_path(options.initial_file)) {
        options.initial_file = colai::win::executable_directory() + "\\" + options.initial_file;
    }
    options.demo_content = command_line.demo_content;
    options.no_webview = command_line.no_webview;
    options.theme = command_line.theme;
    options.language = command_line.language;
    options.font_size = command_line.font_size;
    options.state_dir = colai::win::app_data_directory();
    options.settings_path = options.state_dir + "\\settings.json";
    options.asset_directory = colai::win::executable_directory() + "\\web";
    options.audit_path = command_line.log_to_file
                             ? (colai::win::executable_directory() + "\\colaimdation-audit.jsonl")
                             : (options.state_dir + "\\audit.jsonl");

    colai::EditorWindow window;
    window.set_options(options);
    std::string error;
    if (!window.create(instance, show_command, &error)) {
        show_error(error);
        ::CoUninitialize();
        return 4;
    }

    for (const std::string& warning : command_line.warnings) {
        std::fprintf(stderr, "%s\n", colai::i18n::tr("cli.warning.prefix", {warning}).c_str());
    }
    // A document that failed to load must be visible somewhere: with
    // --no-webview the warning box may not be seen and the editor would look
    // empty for no apparent reason.
    if (!window.last_open_error().empty()) {
        std::fprintf(stderr, "%s\n",
                     colai::i18n::tr("dlg.open_failed.text", {window.last_open_error()}).c_str());
    }
    if (!window.last_bridge_error().empty()) {
        std::fprintf(stderr, "%s\n", window.last_bridge_error().c_str());
    }
    if (auto* mcp = window.mcp(); mcp && !mcp->status().listening) {
        std::fprintf(stderr, "%s\n",
                     colai::i18n::tr("cli.mcp_unavailable", {window.last_mcp_error()}).c_str());
    }

    const int exit_code = window.message_loop();
    window.shutdown();
    ::CoUninitialize();
    return exit_code;
}
