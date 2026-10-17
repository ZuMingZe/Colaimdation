// Colaimdation - the editor shell window.
//
// Owns the Workspace (the single open document), the MCP server and the
// WebView2 bridge, and turns menu/shortcut commands into workspace actions.
// All shell text goes through core/i18n so the interface language can change at
// runtime without restarting.
#pragma once

#include <memory>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "core/policy.h"
#include "core/settings.h"
#include "core/workspace.h"
#include "mcp/mcp_server.h"

namespace colai {

class WebviewBridge;

class EditorWindow {
public:
    struct Options {
        Policy policy;
        std::string initial_file;
        std::string audit_path;
        std::string state_dir;
        std::string asset_directory;
        std::string settings_path;
        // Interface language tag; empty means "use the saved preference, else the
        // operating system locale".
        std::string language;
        uint16_t mcp_port = 0; // 0 = let the OS choose
        // --token was given, so it outranks the token saved in the settings file.
        bool token_pinned = false;
        bool demo_content = true;
        // --no-webview: skip creating the WebView2 front-end entirely. The MCP
        // server and the document engine still run, which is what makes this
        // useful for diagnosing a broken front-end.
        bool no_webview = false;
        double font_size = 16.0;
        std::string theme = "auto";
    };

    EditorWindow();
    ~EditorWindow();

    EditorWindow(const EditorWindow&) = delete;
    EditorWindow& operator=(const EditorWindow&) = delete;

    void set_options(Options options);

    bool create(HINSTANCE instance, int show_command, std::string* error = nullptr);
    int message_loop();
    void shutdown();

    HWND window() const { return window_; }
    Workspace& workspace() { return workspace_; }
    mcp::McpServer* mcp() { return mcp_.get(); }
    WebviewBridge* bridge() { return bridge_.get(); }
    SettingsStore& settings() { return settings_; }
    const std::string& last_mcp_error() const { return last_mcp_error_; }
    // Why the front-end is not running (missing WebView2 runtime, a broken
    // dispatcher window, ...). Empty when the bridge started or was skipped.
    const std::string& last_bridge_error() const { return last_bridge_error_; }
    // Why the document named on the command line did not load. Empty on success.
    const std::string& last_open_error() const { return last_open_error_; }

    static const wchar_t* window_class_name();

    // The window procedure for the class registered by window_class_name().
    // Public because main() has to hand it to win::register_window_classes():
    // registering the class with DefWindowProcW instead silently drops every
    // WM_COMMAND (dead menu bar), WM_SIZE (never resized WebView2) and WM_CLOSE
    // (the process would outlive its window).
    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);

private:
    LRESULT handle_message(UINT message, WPARAM wparam, LPARAM lparam);
    HMENU build_menu();
    // The payload the front-end asks for on every load: app info, MCP status,
    // policy and the appearance/language options. Rebuilt on demand so a reload
    // after changing them picks the new values up.
    Json build_bootstrap();
    // The saved endpoint settings, for the Agent panel's port/token controls.
    Json mcp_settings_json() const;
    // Rewrites the discovery file (%LOCALAPPDATA%\colaimdation\mcp.json) so the
    // stdio bridge and other tools pick up the current port/token.
    void refresh_discovery_file();
    void sync_menu_state();
    void update_window_title();

    // Commands (also reachable from the front-end toolbar).
    bool confirm_discard_changes();
    // Every unsaved document gets its chance to be saved before the window closes.
    bool confirm_discard_all();
    void command_new();
    void command_open();
    void command_save();
    void command_save_as();
    void command_reload();
    // Printing and exporting (File menu): the page prints, the engine writes the PDF
    // and the picture, and the Word package is built from the document itself.
    void command_print();
    void command_export_pdf();
    void command_export_word();
    void command_export_image();
    std::string export_file_name(const char* extension) const;
    // Tabs: close the active one (after the save question) and step through them.
    void command_close_tab();
    void command_step_tab(int delta);
    bool close_tab_with_prompt(size_t index);
    void command_undo();
    void command_redo();
    void command_copy_endpoint();
    void command_rollback_last_agent();
    void command_rollback_all_agent();
    void command_about();
    void command_help_mcp();
    // Runs the front-end's self-check and shows the report.
    void command_self_test();
    void on_frontend_command(const std::string& command);
    // Answers the front-end's status/audit/policy/confirmation messages.
    Json on_panel_query(const Json& message);

    // Language handling.
    void apply_language(const std::string& tag, bool announce);
    void remember_language(const std::string& tag);
    void persist_window_placement();

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    int show_command_ = SW_SHOWNORMAL;
    Options options_;
    Workspace workspace_;
    SettingsStore settings_;
    std::unique_ptr<mcp::McpServer> mcp_;
    std::unique_ptr<WebviewBridge> bridge_;
    std::string last_mcp_error_;
    // A non-fatal note about the endpoint (e.g. "that port was busy"), shown in the
    // Agent panel next to the port controls.
    std::string mcp_notice_;
    std::string last_bridge_error_;
    std::string last_open_error_;

    // Submenu handles reused when the language changes.
    HMENU language_menu_ = nullptr;
    std::vector<std::pair<std::string, UINT>> language_items_;
    UINT next_language_command_ = 1900;
};

} // namespace colai
