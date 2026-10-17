// Colaimdation - WebView2 host and the bridge between the document engine and
// the editor front-end.
//
// Threading contract: Workspace is owned by the UI thread. Tool calls arriving
// on MCP HTTP threads are marshalled here through `post()`, and any message the
// engine wants to show in the editor is marshalled through `notify()`.
#pragma once

#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "core/json.h"
#include "core/workspace.h"

namespace colai {

class EditorWindow;

// Custom window messages used for thread marshalling.
inline constexpr UINT kMsgRunJob = WM_APP + 1;
inline constexpr UINT kMsgNotify = WM_APP + 2;
inline constexpr UINT kMsgConfirmation = WM_APP + 3;

class WebviewBridge : public IEditorBridge {
public:
    WebviewBridge();
    ~WebviewBridge() override;

    WebviewBridge(const WebviewBridge&) = delete;
    WebviewBridge& operator=(const WebviewBridge&) = delete;

    // Creates the hidden dispatcher window and (when the SDK is available) the
    // WebView2 environment for `parent`.
    bool initialise(HWND parent, HINSTANCE instance, const std::string& asset_directory,
                    const Json& bootstrap, std::string* error = nullptr);

    // Pumps messages that belong to the bridge. Called from the window's own
    // message loop; returns true when the message was handled.
    bool handle_message(HWND window, UINT message, WPARAM wparam, LPARAM lparam, LRESULT& result);

    HWND dispatcher_window() const { return dispatcher_; }
    HWND parent_window() const { return parent_; }

    // --- IEditorBridge ------------------------------------------------------
    std::optional<Json> post(std::function<Json()> job, int timeout_ms) override;
    void notify(const Json& message) override;
    bool request_confirmation(const Json& pending, int64_t timeout_ms) override;
    void reload_document(const Document& document) override;
    bool has_ui() const override;

    // --- editor -> C++ ------------------------------------------------------
    // Called on the UI thread with a parsed message from the front-end.
    void on_web_message(const Json& message);

    // Records the user's answer to a staged change and wakes the waiting tool
    // thread. Called on the UI thread.
    void resolve_confirmation(const std::string& pending_id, bool accepted,
                              const std::string& reason);

    // Runs JavaScript in the page (used for menu commands and focus).
    void execute_script(const std::string& script);

    // Resizes the WebView2 control after the host window changed size.
    void put_bounds(int width, int height);

    // Pushes a command object to the page, e.g. {"type":"command","command":"undo"}.
    void send_command(const std::string& command, Json extra = Json());

    // True once the front-end reported that it finished loading.
    bool frontend_ready() const { return frontend_ready_; }
    const std::string& last_error() const { return last_error_; }
    // Set by EditorWindow so menu/accelerator commands reach the app.
    void set_command_sink(std::function<void(const std::string&)> sink) {
        command_sink_ = std::move(sink);
    }
    // Set by EditorWindow to answer the front-end's status/audit queries.
    void set_panel_query_sink(std::function<Json(const Json&)> sink) {
        panel_query_sink_ = std::move(sink);
    }
    // Set by EditorWindow: run as soon as the front-end reports that it is
    // listening. That is the first moment a document can reach the page - the
    // push done while the shell is still starting up either has no WebView2
    // object to go to or arrives before the page has registered its listener.
    void set_ready_sink(std::function<void()> sink) { ready_sink_ = std::move(sink); }

    // Path of the web assets the bridge serves.
    const std::string& asset_directory() const { return asset_directory_; }

    // Replaces the bootstrap payload the page receives when it announces itself.
    // The shell rebuilds it when a setting changes, so a front-end reload (F5)
    // starts from the current appearance/language instead of the startup values.
    void set_bootstrap(Json bootstrap) { bootstrap_ = std::move(bootstrap); }

    // Opens the WebView2 DevTools window (developer builds only).
    void open_devtools();

    // Loads the editor page again, whatever the window is showing now. The shell has no
    // back button, so this is the way out of a page the engine ended up on (an error
    // page from a navigation, or a front end that failed to start).
    void restore_interface();

    // Printing and exporting. Each one asks the page for its paper layout first (the
    // caret's block shows Markdown while writing, the chrome would otherwise end up on
    // paper or in the picture, and the editor column scrolls), then drives the engine.
    void print_document();
    void export_pdf(const std::string& path);
    void export_image(const std::string& path);

private:
    struct Job {
        std::function<Json()> body;
        Json result;
        bool done = false;
    };

    struct ConfirmationWait {
        bool decided = false;
        bool accepted = false;
        std::string reason;
    };

    void run_jobs();
    void deliver_notifications();
    void show_confirmation(const Json& pending);
    void create_webview(std::string* error);

    HWND parent_ = nullptr;
    HWND dispatcher_ = nullptr;
    HINSTANCE instance_ = nullptr;
    DWORD ui_thread_id_ = 0;
    std::string asset_directory_;
    Json bootstrap_ = Json::object();
    std::string last_error_;
    bool frontend_ready_ = false;
    bool webview_ready_ = false;
    std::function<void(const std::string&)> command_sink_;
    std::function<Json(const Json&)> panel_query_sink_;
    std::function<void()> ready_sink_;

    std::mutex mutex_;
    std::condition_variable condition_;
    std::map<uint64_t, std::shared_ptr<Job>> jobs_;
    uint64_t next_job_ = 1;
    std::vector<Json> pending_notifications_;

    std::mutex confirmation_mutex_;
    std::condition_variable confirmation_condition_;
    std::map<std::string, std::shared_ptr<ConfirmationWait>> confirmations_;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace colai
