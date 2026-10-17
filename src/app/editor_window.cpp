#include "app/editor_window.h"

#include "app/webview_bridge.h"
#include "app/win_util.h"
#include "core/diff.h"
#include "core/docx.h"
#include "core/i18n.h"
#include "core/utf8.h"
#include "mcp/mcp_server.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include <commctrl.h>
#include <shellapi.h>

namespace colai {
namespace {

namespace menu_ids {
constexpr int kFileNew = 1001;
constexpr int kFileOpen = 1002;
constexpr int kFileSave = 1003;
constexpr int kFileSaveAs = 1004;
constexpr int kFileReload = 1005;
constexpr int kFileExit = 1006;
constexpr int kFileCloseTab = 1007;
// Printing and exporting live in the File menu, next to Save: they are the same kind
// of action (the document leaves the editor, in another format).
constexpr int kFilePrint = 1008;
constexpr int kFileExportPdf = 1009;
constexpr int kFileExportWord = 1010;
constexpr int kFileExportImage = 1011;
constexpr int kEditUndo = 1101;
constexpr int kEditRedo = 1102;
constexpr int kEditCopyMcp = 1103;
constexpr int kAgentPanel = 1201;
constexpr int kAgentPreview = 1202;
constexpr int kAgentRollback = 1203;
constexpr int kAgentRollbackAll = 1204;
constexpr int kAgentAllowWrites = 1205;
constexpr int kAgentAuditPanel = 1206;
constexpr int kAgentEndpoint = 1207;
constexpr int kViewSource = 1301;
constexpr int kViewReload = 1302;
constexpr int kViewDevTools = 1303;
constexpr int kViewSelfTest = 1304;
constexpr int kViewNextTab = 1305;
constexpr int kViewPrevTab = 1306;
constexpr int kHelpAbout = 1401;
constexpr int kHelpMcp = 1402;
} // namespace menu_ids

// Appends a localised menu item.
void append_item(HMENU menu, UINT flags, UINT_PTR id, const char* key) {
    ::AppendMenuW(menu, flags, id, win::wide(i18n::tr(key)).c_str());
}

// Appends a localised submenu.
void append_submenu(HMENU parent, HMENU child, const char* key) {
    ::AppendMenuW(parent, MF_POPUP, reinterpret_cast<UINT_PTR>(child),
                  win::wide(i18n::tr(key)).c_str());
}

} // namespace

const wchar_t* EditorWindow::window_class_name() { return L"ColaimdationWindow"; }

EditorWindow::EditorWindow() = default;

EditorWindow::~EditorWindow() { shutdown(); }

void EditorWindow::set_options(Options options) { options_ = std::move(options); }

bool EditorWindow::create(HINSTANCE instance, int show_command, std::string* error) {
    instance_ = instance;
    show_command_ = show_command;

    // --- settings ----------------------------------------------------------
    if (!options_.settings_path.empty()) {
        settings_.set_path(options_.settings_path);
        std::string settings_error;
        if (!settings_.load(&settings_error) && error && error->empty()) {
            *error = settings_error;
        }
    }

    // Language precedence: command line, then the saved preference, then the
    // operating system locale.
    std::string language = options_.language;
    if (language.empty()) language = settings_.get().language;
    if (language.empty()) language = i18n::from_system_locale();
    i18n::set_language(language);
    settings_.get().language = i18n::language();

    // The MCP endpoint is meant to survive a restart: Agents are configured with one
    // URL and should keep working. Token precedence: --token, then the pinned token
    // from the settings file, then a fresh random one for this run.
    if (!options_.token_pinned) {
        const std::string pinned = settings_.get().mcp_token;
        if (!pinned.empty()) {
            options_.policy.token = pinned;
        } else if (options_.policy.token.empty()) {
            options_.policy.token = Policy::generate_token();
        }
    }

    // --- workspace ---------------------------------------------------------
    Workspace::Options workspace_options;
    workspace_options.policy = options_.policy;
    workspace_options.audit_path = options_.audit_path;
    workspace_options.state_dir = options_.state_dir;
    workspace_options.initial_file = options_.initial_file;
    workspace_options.demo_content = options_.demo_content;
    std::string workspace_error;
    workspace_.initialise(std::move(workspace_options), &workspace_error);

    // --- window ------------------------------------------------------------
    const Settings& saved = settings_.get();
    RECT work_area{};
    ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_area, 0);
    int width = saved.window_width > 0 ? saved.window_width : 1280;
    int height = saved.window_height > 0 ? saved.window_height : 860;
    width = std::min(width, static_cast<int>(work_area.right - work_area.left) - 40);
    height = std::min(height, static_cast<int>(work_area.bottom - work_area.top) - 40);
    const int x = saved.window_x >= 0 ? saved.window_x : CW_USEDEFAULT;
    const int y = saved.window_y >= 0 ? saved.window_y : CW_USEDEFAULT;

    window_ = ::CreateWindowExW(0, window_class_name(), L"Colaimdation",
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, std::max(640, width),
                                std::max(420, height), nullptr, build_menu(), instance, this);
    if (!window_) {
        if (error) *error = "cannot create the editor window: " + win::format_error(::GetLastError());
        return false;
    }
    update_window_title();
    if (saved.window_maximized) show_command_ = SW_SHOWMAXIMIZED;

    // --- MCP server --------------------------------------------------------
    // Port precedence: --port, then the remembered port, then let the OS choose.
    // Whichever port this instance actually gets is stored, so the next start
    // reuses it and the endpoint the Agent was configured with keeps working.
    mcp_ = std::make_unique<mcp::McpServer>();
    const int remembered_port = settings_.get().mcp_port;
    const uint16_t requested_port =
        options_.mcp_port != 0 ? options_.mcp_port
                               : static_cast<uint16_t>(remembered_port > 0 ? remembered_port : 0);
    std::string mcp_error;
    bool used_requested_port = true;
    if (!mcp_->initialise(workspace_, requested_port, &mcp_error)) {
        // The remembered port may have been taken by something else in the
        // meantime; fall back to any free port instead of running headless.
        if (requested_port != 0 && mcp_->initialise(workspace_, 0, &mcp_error)) {
            used_requested_port = false;
            mcp_notice_ = i18n::tr("dlg.mcp_port_busy", {std::to_string(requested_port)});
        } else {
            last_mcp_error_ = mcp_error;
        }
    }
    if (mcp_->status().listening) {
        const std::string discovery =
            options_.state_dir.empty() ? (win::app_data_directory() + "\\mcp.json")
                                       : (options_.state_dir + "\\mcp.json");
        std::string discovery_error;
        if (!mcp_->write_discovery_file(discovery, &discovery_error)) {
            last_mcp_error_ = discovery_error;
        }
        // Remember the port so the endpoint is the same next time - but only in
        // "let the editor choose" mode. A port the writer pinned must stay pinned
        // even when it was busy once, and --port must not rewrite the preference.
        Settings& settings = settings_.get();
        bool dirty = false;
        const bool auto_port = options_.mcp_port == 0 && remembered_port == 0;
        if (auto_port && used_requested_port &&
            settings.mcp_port != static_cast<int>(mcp_->status().port)) {
            settings.mcp_port = static_cast<int>(mcp_->status().port);
            dirty = true;
        }
        if (!settings.mcp_token.empty() && settings.mcp_token != mcp_->status().token) {
            settings.mcp_token = mcp_->status().token;
            dirty = true;
        }
        if (dirty) {
            std::string save_error;
            if (!settings_.save(&save_error)) {
                std::fprintf(stderr, "Colaimdation: %s\n", save_error.c_str());
            }
        }
    }
    mcp_->set_activity_sink([this](const Json& message) {
        if (bridge_) bridge_->notify(message);
    });

    // --- WebView2 ----------------------------------------------------------
    // --no-webview was parsed but never propagated, so the front-end always
    // started anyway.
    if (!options_.no_webview) {
        bridge_ = std::make_unique<WebviewBridge>();
    }
    Json bootstrap = build_bootstrap();
    std::string bridge_error;
    if (bridge_) {
        bridge_->initialise(window_, instance, options_.asset_directory, bootstrap, &bridge_error);
        if (!bridge_error.empty()) last_bridge_error_ = bridge_error;
        bridge_->set_command_sink([this](const std::string& command) { on_frontend_command(command); });
        bridge_->set_panel_query_sink([this](const Json& message) { return on_panel_query(message); });
        // The page can only receive a document once it has registered its message
        // listener, so the document (and the tab list) is (re-)pushed when it says
        // it is ready.
        bridge_->set_ready_sink([this] {
            workspace_.notify_document_loaded();
            workspace_.notify_tabs_changed();
        });
    }
    // Wire the workspace to the UI. Without this the workspace believes it is
    // headless: it never pushes document_loaded/document_changed to the page,
    // never asks the writer to confirm a staged change (so `--confirm` silently
    // applied agent writes immediately) and never marshals agent writes onto the
    // UI thread.
    workspace_.set_bridge(bridge_.get());

    // --- open the initial document / welcome ------------------------------
    // Workspace::initialise() already opened options_.initial_file, back when
    // the workspace was still headless. Re-opening it here duplicated the file
    // read and the audit entry, and the "welcome document" branch used to fire
    // even with --no-demo.
    if (!options_.initial_file.empty() && workspace_.document().path().empty()) {
        // open_file() leaves the path unset when it fails, so this is how a
        // failed startup open is detected (initialise() reports it soft). Say so
        // and fall back to the welcome document: a window that looks editable but
        // silently holds nothing is the worst possible outcome here.
        last_open_error_ = workspace_error.empty() ? options_.initial_file : workspace_error;
        win::message_box(window_, i18n::tr("app.title"),
                         i18n::tr("dlg.open_failed.text", {last_open_error_}), MB_ICONWARNING);
        if (options_.demo_content) workspace_.set_demo_content();
    } else if (options_.initial_file.empty() && workspace_.document().text().empty() &&
               options_.demo_content) {
        workspace_.set_demo_content();
    }
    // The page was never told what to display: every notification sent before
    // the bridge existed was dropped, so push the document now.
    workspace_.notify_document_loaded();
    update_window_title();

    ::ShowWindow(window_, show_command_);
    ::UpdateWindow(window_);
    sync_menu_state();
    return true;
}

// ---------------------------------------------------------------------------
// Menu
// ---------------------------------------------------------------------------
// The payload the page asks for on every load (and after every reload), so it is
// rebuilt from the current settings rather than cached from startup: changing the
// language or the appearance must survive an F5.
Json EditorWindow::build_bootstrap() {
    Json bootstrap = Json::object();
    bootstrap.set("app", Json("colaimdation"));
    bootstrap.set("version", Json("0.1.0"));
    bootstrap.set("virtual_host", Json("colaimdation.local"));
    bootstrap.set("mcp", mcp_ ? mcp_->status().to_json() : Json());
    bootstrap.set("mcp_settings", mcp_settings_json());
    bootstrap.set("policy", workspace_.policy_json());
    bootstrap.set("options", [&] {
        Json map = Json::object();
        map.set("font_size",
                Json(options_.font_size > 0 ? options_.font_size : settings_.get().font_size));
        // A command-line --theme wins over the saved preference; "auto" stays
        // "auto" and is resolved against the operating system by the page.
        const std::string saved = settings_.get().theme;
        const std::string theme = options_.theme != "auto" ? options_.theme
                                                           : (saved.empty() ? "auto" : saved);
        map.set("theme", Json(theme));
        map.set("language", Json(i18n::language()));
        return map;
    }());
    return bootstrap;
}

Json EditorWindow::mcp_settings_json() const {
    const Settings& settings = settings_.get();
    Json out = Json::object();
    // What will be used next time (0 = pick a free port).
    out.set("port", Json(static_cast<int64_t>(settings.mcp_port)));
    out.set("keep_token", Json(!settings.mcp_token.empty()));
    out.set("active_port", Json(static_cast<int64_t>(mcp_ ? mcp_->status().port : 0)));
    out.set("listening", Json(mcp_ ? mcp_->status().listening : false));
    if (!mcp_notice_.empty()) out.set("notice", Json(mcp_notice_));
    return out;
}

void EditorWindow::refresh_discovery_file() {
    if (!mcp_ || !mcp_->status().listening) return;
    const std::string discovery = options_.state_dir.empty()
                                      ? (win::app_data_directory() + "\\mcp.json")
                                      : (options_.state_dir + "\\mcp.json");
    std::string error;
    if (!mcp_->write_discovery_file(discovery, &error)) last_mcp_error_ = error;
}

HMENU EditorWindow::build_menu() {
    HMENU menu = ::CreateMenu();

    HMENU file = ::CreatePopupMenu();
    append_item(file, MF_STRING, menu_ids::kFileNew, "menu.file.new");
    append_item(file, MF_STRING, menu_ids::kFileOpen, "menu.file.open");
    append_item(file, MF_STRING, menu_ids::kFileSave, "menu.file.save");
    append_item(file, MF_STRING, menu_ids::kFileSaveAs, "menu.file.save_as");
    ::AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    append_item(file, MF_STRING, menu_ids::kFilePrint, "menu.file.print");
    append_item(file, MF_STRING, menu_ids::kFileExportPdf, "menu.file.export_pdf");
    append_item(file, MF_STRING, menu_ids::kFileExportWord, "menu.file.export_word");
    append_item(file, MF_STRING, menu_ids::kFileExportImage, "menu.file.export_image");
    ::AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    append_item(file, MF_STRING, menu_ids::kFileCloseTab, "menu.file.close_tab");
    append_item(file, MF_STRING, menu_ids::kFileReload, "menu.file.reload");
    ::AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    append_item(file, MF_STRING, menu_ids::kFileExit, "menu.file.exit");
    append_submenu(menu, file, "menu.file");

    HMENU edit = ::CreatePopupMenu();
    append_item(edit, MF_STRING, menu_ids::kEditUndo, "menu.edit.undo");
    append_item(edit, MF_STRING, menu_ids::kEditRedo, "menu.edit.redo");
    ::AppendMenuW(edit, MF_SEPARATOR, 0, nullptr);
    append_item(edit, MF_STRING, menu_ids::kEditCopyMcp, "menu.edit.copy_mcp");
    append_submenu(menu, edit, "menu.edit");

    HMENU agent = ::CreatePopupMenu();
    append_item(agent, MF_STRING, menu_ids::kAgentPanel, "menu.agent.panel");
    append_item(agent, MF_STRING, menu_ids::kAgentAuditPanel, "menu.agent.audit");
    ::AppendMenuW(agent, MF_SEPARATOR, 0, nullptr);
    append_item(agent, MF_STRING, menu_ids::kAgentAllowWrites, "menu.agent.allow_writes");
    append_item(agent, MF_STRING, menu_ids::kAgentPreview, "menu.agent.preview");
    ::AppendMenuW(agent, MF_SEPARATOR, 0, nullptr);
    append_item(agent, MF_STRING, menu_ids::kAgentRollback, "menu.agent.rollback");
    append_item(agent, MF_STRING, menu_ids::kAgentRollbackAll, "menu.agent.rollback_all");
    ::AppendMenuW(agent, MF_SEPARATOR, 0, nullptr);
    append_item(agent, MF_STRING, menu_ids::kAgentEndpoint, "menu.agent.endpoint");
    append_submenu(menu, agent, "menu.agent");

    HMENU view = ::CreatePopupMenu();
    append_item(view, MF_STRING, menu_ids::kViewSource, "menu.view.source");
    append_item(view, MF_STRING, menu_ids::kViewReload, "menu.view.reload");
#ifndef COLAI_USER_BUILD
    // Developer tools and the editor self-check are not part of the end-user build.
    append_item(view, MF_STRING, menu_ids::kViewDevTools, "menu.view.devtools");
#endif
    // Walking the open documents: the same commands the Ctrl+Tab accelerator and
    // the tab bar use, reachable without a keyboard shortcut.
    append_item(view, MF_STRING, menu_ids::kViewNextTab, "menu.view.next_tab");
    append_item(view, MF_STRING, menu_ids::kViewPrevTab, "menu.view.prev_tab");
#ifndef COLAI_USER_BUILD
    // Front-end self-check: runs the editing paths that only exist inside the
    // browser and reports the result. Ask for it when the rendering misbehaves.
    append_item(view, MF_STRING, menu_ids::kViewSelfTest, "menu.view.selftest");
#endif
    ::AppendMenuW(view, MF_SEPARATOR, 0, nullptr);

    // Language submenu: one radio item per shipped language.
    language_menu_ = ::CreatePopupMenu();
    language_items_.clear();
    for (const i18n::LanguageInfo& language : i18n::languages()) {
        const UINT id = next_language_command_++;
        language_items_.emplace_back(language.tag, id);
        ::AppendMenuW(language_menu_, MF_STRING, id, win::wide(language.native_name).c_str());
    }
    append_submenu(view, language_menu_, "menu.view.language");
    append_submenu(menu, view, "menu.view");

    HMENU help = ::CreatePopupMenu();
    append_item(help, MF_STRING, menu_ids::kHelpMcp, "menu.help.mcp");
    append_item(help, MF_STRING, menu_ids::kHelpAbout, "menu.help.about");
    append_submenu(menu, help, "menu.help");
    return menu;
}

void EditorWindow::sync_menu_state() {
    HMENU menu = ::GetMenu(window_);
    if (!menu) return;
    ::CheckMenuItem(menu, menu_ids::kAgentAllowWrites,
                    MF_BYCOMMAND |
                        (workspace_.policy().allow_write ? MF_CHECKED : MF_UNCHECKED));
    ::CheckMenuItem(menu, menu_ids::kAgentPreview,
                    MF_BYCOMMAND |
                        (workspace_.policy().require_confirmation ? MF_CHECKED : MF_UNCHECKED));
    ::EnableMenuItem(menu, menu_ids::kEditUndo,
                     MF_BYCOMMAND | (workspace_.document().can_undo() ? MF_ENABLED : MF_GRAYED));
    ::EnableMenuItem(menu, menu_ids::kEditRedo,
                     MF_BYCOMMAND | (workspace_.document().can_redo() ? MF_ENABLED : MF_GRAYED));
    ::EnableMenuItem(menu, menu_ids::kFileSave,
                     MF_BYCOMMAND | (workspace_.document().dirty() ? MF_ENABLED : MF_GRAYED));

    // Radio the active language.
    if (language_menu_) {
        const std::string active = i18n::language();
        const int count = ::GetMenuItemCount(language_menu_);
        for (int i = 0; i < count; ++i) {
            const UINT id = ::GetMenuItemID(language_menu_, static_cast<UINT>(i));
            ::CheckMenuItem(language_menu_, id,
                            MF_BYCOMMAND | (language_items_[static_cast<size_t>(i)].first == active
                                                ? MF_CHECKED
                                                : MF_UNCHECKED));
        }
    }
}

void EditorWindow::update_window_title() {
    std::string title = i18n::tr("app.title");
    const std::string name = workspace_.document().display_name();
    if (!name.empty() && name != i18n::tr("app.untitled")) {
        title += " - " + name;
    }
    // With several files open, say which one this window is showing.
    if (workspace_.tab_count() > 1) {
        title += " (" + std::to_string(workspace_.active_tab() + 1) + "/" +
                 std::to_string(workspace_.tab_count()) + ")";
    }
    if (workspace_.document().dirty()) title += " *";
    ::SetWindowTextW(window_, win::wide(title).c_str());
}

// ---------------------------------------------------------------------------
// Language
// ---------------------------------------------------------------------------
void EditorWindow::apply_language(const std::string& tag, bool announce) {
    i18n::set_language(tag);
    const std::string active = i18n::language();

    // Rebuild the whole menu bar so every caption picks up the new language.
    HMENU old = window_ ? ::GetMenu(window_) : nullptr;
    HMENU fresh = build_menu();
    if (window_) {
        ::SetMenu(window_, fresh);
        if (old) ::DestroyMenu(old);
        ::DrawMenuBar(window_);
    } else if (old) {
        ::DestroyMenu(old);
    }
    sync_menu_state();
    update_window_title();

    // The front-end owns its own strings; tell it which language is active. File
    // dialogs read the active language directly on their next use.
    if (bridge_) {
        // Keep the bootstrap in step: the page asks for it again after a reload.
        bridge_->set_bootstrap(build_bootstrap());
        if (announce) {
            Json payload = Json::object();
            payload.set("language", Json(active));
            bridge_->send_command("set_language", payload);
        }
    }

    if (announce) {
        win::message_box(window_, i18n::tr("app.title"),
                         i18n::tr("dlg.language.changed", {i18n::native_name(active)}),
                         MB_ICONINFORMATION);
    }
}

void EditorWindow::remember_language(const std::string& tag) {
    settings_.get().language = tag;
    std::string error;
    if (!settings_.save(&error)) {
        // Not fatal: the interface switched, it just will not be remembered.
        std::fprintf(stderr, "Colaimdation: %s\n", error.c_str());
    }
}

void EditorWindow::persist_window_placement() {
    if (!window_) return;
    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    if (!::GetWindowPlacement(window_, &placement)) return;
    const RECT bounds = placement.rcNormalPosition;
    Settings& settings = settings_.get();
    settings.window_maximized = placement.showCmd == SW_SHOWMAXIMIZED;
    settings.window_x = bounds.left;
    settings.window_y = bounds.top;
    settings.window_width = std::max<int>(640, bounds.right - bounds.left);
    settings.window_height = std::max<int>(420, bounds.bottom - bounds.top);
    std::string error;
    if (!settings_.save(&error)) {
        std::fprintf(stderr, "Colaimdation: %s\n", error.c_str());
    }
}

// ---------------------------------------------------------------------------
// Message handling
// ---------------------------------------------------------------------------
LRESULT CALLBACK EditorWindow::window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    EditorWindow* self = nullptr;
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = static_cast<EditorWindow*>(create->lpCreateParams);
        self->window_ = window;
        ::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<EditorWindow*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
    }
    if (!self) return ::DefWindowProcW(window, message, wparam, lparam);
    return self->handle_message(message, wparam, lparam);
}

LRESULT EditorWindow::handle_message(UINT message, WPARAM wparam, LPARAM lparam) {
    if (!window_) return ::DefWindowProcW(nullptr, message, wparam, lparam);
    if (bridge_) {
        LRESULT result = 0;
        if (bridge_->handle_message(window_, message, wparam, lparam, result)) return result;
    }

    switch (message) {
    case WM_SIZE: {
        RECT client{};
        ::GetClientRect(window_, &client);
        if (bridge_) bridge_->put_bounds(client.right - client.left, client.bottom - client.top);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
        info->ptMinTrackSize.x = 640;
        info->ptMinTrackSize.y = 420;
        return 0;
    }
    case WM_SETFOCUS: {
        if (bridge_) bridge_->execute_script("window.colaimdation && window.colaimdation.focus();");
        return 0;
    }
    case WM_COMMAND: {
        const int id = LOWORD(wparam);

        // Language radio items are allocated dynamically when the menu is built.
        for (const auto& entry : language_items_) {
            if (static_cast<int>(entry.second) != id) continue;
            apply_language(entry.first, true);
            remember_language(i18n::language());
            return 0;
        }

        switch (id) {
        case menu_ids::kFileNew: command_new(); return 0;
        case menu_ids::kFileOpen: command_open(); return 0;
        case menu_ids::kFileSave: command_save(); return 0;
        case menu_ids::kFileSaveAs: command_save_as(); return 0;
        case menu_ids::kFilePrint: command_print(); return 0;
        case menu_ids::kFileExportPdf: command_export_pdf(); return 0;
        case menu_ids::kFileExportWord: command_export_word(); return 0;
        case menu_ids::kFileExportImage: command_export_image(); return 0;
        case menu_ids::kFileCloseTab: command_close_tab(); return 0;
        case menu_ids::kFileReload: command_reload(); return 0;
        case menu_ids::kFileExit: ::PostMessageW(window_, WM_CLOSE, 0, 0); return 0;
        case menu_ids::kEditUndo: command_undo(); return 0;
        case menu_ids::kEditRedo: command_redo(); return 0;
        case menu_ids::kEditCopyMcp:
        case menu_ids::kAgentEndpoint: command_copy_endpoint(); return 0;
        case menu_ids::kAgentPanel:
            if (bridge_) bridge_->send_command("toggle_agent_panel");
            return 0;
        case menu_ids::kAgentAuditPanel:
            if (bridge_) bridge_->send_command("toggle_audit_panel");
            return 0;
        case menu_ids::kAgentPreview: {
            Json patch = Json::object();
            patch.set("require_confirmation", Json(!workspace_.policy().require_confirmation));
            workspace_.set_policy(patch);
            sync_menu_state();
            if (bridge_) bridge_->send_command("policy_changed", workspace_.policy_json());
            return 0;
        }
        case menu_ids::kAgentAllowWrites: {
            Json patch = Json::object();
            patch.set("allow_write", Json(!workspace_.policy().allow_write));
            workspace_.set_policy(patch);
            sync_menu_state();
            if (bridge_) bridge_->send_command("policy_changed", workspace_.policy_json());
            return 0;
        }
        case menu_ids::kAgentRollback: command_rollback_last_agent(); return 0;
        case menu_ids::kAgentRollbackAll: command_rollback_all_agent(); return 0;
        case menu_ids::kViewSource:
            if (bridge_) bridge_->send_command("toggle_source_mode");
            return 0;
        case menu_ids::kViewReload:
            // Load the editor page again rather than reloading whatever is showing:
            // after a navigation the engine ended up on (an error page), a plain
            // location.reload() just showed that page once more, and the shell has no
            // back button to leave it.
            if (bridge_) bridge_->restore_interface();
            return 0;
        case menu_ids::kViewDevTools:
#ifdef COLAI_USER_BUILD
            // Not offered in the end-user build: no developer tools window.
            return 0;
#else
            if (bridge_) bridge_->open_devtools();
            return 0;
#endif
        case menu_ids::kViewSelfTest:
#ifdef COLAI_USER_BUILD
            return 0;
#else
            command_self_test();
            return 0;
#endif
        case menu_ids::kViewNextTab: command_step_tab(1); return 0;
        case menu_ids::kViewPrevTab: command_step_tab(-1); return 0;
        case menu_ids::kHelpAbout: command_about(); return 0;
        case menu_ids::kHelpMcp: command_help_mcp(); return 0;
        default: break;
        }
        return 0;
    }
    case WM_CLOSE: {
        if (!confirm_discard_all()) return 0;
        persist_window_placement();
        ::DestroyWindow(window_);
        return 0;
    }
    case WM_DESTROY: {
        if (bridge_) {
            bridge_->execute_script("window.colaimdation && window.colaimdation.shutdown();");
        }
        // Detach the workspace before the bridge is destroyed: an in-flight MCP
        // tool call must never dereference a freed bridge.
        workspace_.set_bridge(nullptr);
        mcp_.reset();
        bridge_.reset();
        workspace_.shutdown();
        ::PostQuitMessage(0);
        return 0;
    }
    default: break;
    }
    return ::DefWindowProcW(window_, message, wparam, lparam);
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------
bool EditorWindow::confirm_discard_changes() {
    if (!workspace_.document().dirty()) return true;
    // "Yes" means "save first", "No" means "throw the changes away and carry on",
    // "Cancel" aborts. Saying so explicitly matters: the old prompt only asked
    // "Save before closing?" and answered with Save/Discard, so when it appeared
    // in front of File -> Open, choosing the save path and then cancelling the
    // Save As dialog silently aborted the open - which reads as "opening a file
    // does nothing".
    const int answer = ::MessageBoxW(
        window_, win::wide(i18n::tr("dlg.unsaved.text")).c_str(),
        win::wide(i18n::tr("app.title")).c_str(),
        MB_YESNOCANCEL | MB_ICONWARNING | MB_SETFOREGROUND);
    if (answer == IDCANCEL) return false;
    if (answer == IDYES) {
        std::string error;
        std::string saved_path;
        // Save As when the document has no real path yet; that is the only way a
        // fresh document can keep its changes.
        if (workspace_.document().path().empty()) {
            const std::string suggested = workspace_.document().display_name().empty()
                                              ? i18n::tr("app.untitled")
                                              : workspace_.document().display_name();
            const std::string path = win::save_file_dialog(window_, suggested);
            if (path.empty()) return false; // the writer cancelled the save
            if (!workspace_.save_file_as(path, &error).ok) {
                win::message_box(window_, i18n::tr("app.title"),
                                 i18n::tr("dlg.save_failed.text", {error}), MB_ICONERROR);
                return false;
            }
            update_window_title();
            return true;
        }
        if (!workspace_.save_file(&saved_path, &error).ok) {
            win::message_box(window_, i18n::tr("app.title"),
                             i18n::tr("dlg.save_failed.text", {error}), MB_ICONERROR);
            return false;
        }
        update_window_title();
    }
    return true;
}

void EditorWindow::command_new() {
    // A new tab, not a replacement: the document that is already open keeps its
    // content, its history and its unsaved changes, so there is nothing to ask
    // about here.
    workspace_.new_document();
    update_window_title();
    sync_menu_state();
}

// Closes one tab, asking about unsaved changes first. The tab is brought forward
// before the question so the writer sees which document they are deciding about
// (and so "Save" targets the right file).
bool EditorWindow::close_tab_with_prompt(size_t index) {
    if (index >= workspace_.tab_count()) return false;
    if (index != workspace_.active_tab()) {
        std::string error;
        workspace_.activate_tab(index, &error);
        update_window_title();
        sync_menu_state();
    }
    if (!confirm_discard_changes()) return false; // the writer cancelled
    std::string error;
    if (!workspace_.close_tab(index, &error).ok) {
        win::message_box(window_, i18n::tr("app.title"), error, MB_ICONERROR);
        return false;
    }
    update_window_title();
    sync_menu_state();
    return true;
}

void EditorWindow::command_close_tab() {
    close_tab_with_prompt(workspace_.active_tab());
}

void EditorWindow::command_step_tab(int delta) {
    const size_t count = workspace_.tab_count();
    if (count < 2) return;
    const int current = static_cast<int>(workspace_.active_tab());
    int next = (current + delta) % static_cast<int>(count);
    if (next < 0) next += static_cast<int>(count);
    std::string error;
    if (!workspace_.activate_tab(static_cast<size_t>(next), &error).ok) return;
    update_window_title();
    sync_menu_state();
}

// Closes the window only when every unsaved document has been dealt with.
bool EditorWindow::confirm_discard_all() {
    for (size_t i = 0; i < workspace_.tab_count();) {
        const size_t index = i;
        const bool dirty = workspace_.document_at(index).dirty();
        if (!dirty) {
            ++i;
            continue;
        }
        if (index != workspace_.active_tab()) {
            std::string error;
            workspace_.activate_tab(index, &error);
            update_window_title();
        }
        if (!confirm_discard_changes()) return false;
        // Saving does not change the tab list, so the index stays valid.
        ++i;
    }
    return true;
}

void EditorWindow::command_self_test() {
    // The front-end answers with a "self_test_result" panel query.
    if (bridge_) bridge_->send_command("self_test");
}

void EditorWindow::command_open() {
    // No "save first?" question: opening a file adds a tab and leaves the current
    // document (and its unsaved changes) exactly where it was.
    std::string initial = workspace_.document().path();
    const size_t slash = initial.find_last_of("\\/");
    initial = slash == std::string::npos ? std::string{} : initial.substr(0, slash);
    const std::string path = win::open_file_dialog(window_, initial);
    if (path.empty()) return;
    std::string error;
    if (!workspace_.open_file(path, &error).ok) {
        win::message_box(window_, i18n::tr("app.title"), i18n::tr("dlg.open_failed.text", {error}),
                         MB_ICONERROR);
        return;
    }
    update_window_title();
    sync_menu_state();
}

void EditorWindow::command_save() {
    // Decide *before* attempting the write. A document with no file behind it
    // (New, or the welcome text) needs Save As; the check used to sit in the
    // failure branch, where it could never be reached, because the placeholder
    // name was itself a valid relative path that the save happily wrote to.
    if (workspace_.document().path().empty()) {
        command_save_as();
        return;
    }
    std::string error;
    std::string saved;
    if (!workspace_.save_file(&saved, &error).ok) {
        win::message_box(window_, i18n::tr("app.title"), i18n::tr("dlg.save_failed.text", {error}),
                         MB_ICONERROR);
        return;
    }
    update_window_title();
    sync_menu_state();
}

void EditorWindow::command_save_as() {
    const std::string suggested = workspace_.document().display_name().empty()
                                      ? i18n::tr("app.untitled")
                                      : workspace_.document().display_name();
    const std::string path = win::save_file_dialog(window_, suggested);
    if (path.empty()) return;
    std::string error;
    if (!workspace_.save_file_as(path, &error).ok) {
        win::message_box(window_, i18n::tr("app.title"), i18n::tr("dlg.save_failed.text", {error}),
                         MB_ICONERROR);
        return;
    }
    update_window_title();
    sync_menu_state();
}

// ---------------------------------------------------------------------------
// Printing and exporting
// ---------------------------------------------------------------------------

// What an export should suggest: the document's own name with the new extension, or
// "untitled" for a document that has never been saved.
std::string EditorWindow::export_file_name(const char* extension) const {
    std::string base = workspace_.document().display_name();
    const size_t dot = base.find_last_of('.');
    if (dot != std::string::npos && dot > 0) base = base.substr(0, dot);
    if (base.empty()) base = i18n::tr("export.untitled");
    return base + extension;
}

void EditorWindow::command_print() {
    if (bridge_) bridge_->print_document();
}

void EditorWindow::command_export_pdf() {
    if (!bridge_) return;
    const std::string path =
        win::save_file_dialog_as(window_, export_file_name(".pdf"), i18n::tr("export.pdf.title"),
                                 "pdf");
    if (path.empty()) return;
    bridge_->export_pdf(path);
}

void EditorWindow::command_export_word() {
    const std::string path =
        win::save_file_dialog_as(window_, export_file_name(".docx"), i18n::tr("export.word.title"),
                                 "docx");
    if (path.empty()) return;
    // Built from the document itself (see core/docx.h): the same parsed blocks the
    // editor renders, so no second Markdown implementation is involved.
    const std::string title = export_file_name("");
    const std::string package = docx::from_markdown(workspace_.document().text(), title);
    std::ofstream file(win::wide(path).c_str(), std::ios::binary | std::ios::trunc);
    bool written = false;
    if (file) {
        file.write(package.data(), static_cast<std::streamsize>(package.size()));
        written = file.good();
    }
    win::message_box(window_, i18n::tr("export.title"),
                     written ? i18n::tr("export.word.done", {path})
                             : i18n::tr("export.failed", {path}),
                     written ? MB_ICONINFORMATION : MB_ICONERROR);
}

void EditorWindow::command_export_image() {
    if (!bridge_) return;
    const std::string path =
        win::save_file_dialog_as(window_, export_file_name(".png"), i18n::tr("export.image.title"),
                                 "png");
    if (path.empty()) return;
    bridge_->export_image(path);
}

void EditorWindow::command_reload() {
    if (workspace_.document().path().empty()) {
        win::message_box(window_, i18n::tr("app.title"), i18n::tr("dlg.not_saved_yet.text"),
                         MB_ICONINFORMATION);
        return;
    }
    if (workspace_.document().dirty() &&
        !win::confirm(window_, i18n::tr("app.title"), i18n::tr("dlg.discard_reload.text"))) {
        return;
    }
    std::string error;
    if (!workspace_.reload_from_disk(&error).ok) {
        win::message_box(window_, i18n::tr("app.title"), i18n::tr("dlg.reload_failed.text", {error}),
                         MB_ICONERROR);
        return;
    }
    update_window_title();
    sync_menu_state();
}

void EditorWindow::command_undo() {
    if (!workspace_.document().can_undo()) return;
    workspace_.document().undo(EditSource::User);
    if (bridge_) bridge_->reload_document(workspace_.document());
    update_window_title();
    sync_menu_state();
}

void EditorWindow::command_redo() {
    if (!workspace_.document().can_redo()) return;
    workspace_.document().redo(EditSource::User);
    if (bridge_) bridge_->reload_document(workspace_.document());
    update_window_title();
    sync_menu_state();
}

void EditorWindow::command_copy_endpoint() {
    if (!mcp_ || !mcp_->status().listening) {
        win::message_box(window_, i18n::tr("app.title"),
                         i18n::tr("dlg.mcp_not_running.text", {last_mcp_error_}), MB_ICONWARNING);
        return;
    }
    const std::string payload = mcp_->status().endpoint;
    if (win::set_clipboard_text(window_, payload)) {
        win::message_box(window_, i18n::tr("app.title"),
                         i18n::tr("dlg.endpoint_copied.text", {payload}), MB_ICONINFORMATION);
    }
}

void EditorWindow::command_rollback_last_agent() {
    const Json result = workspace_.rollback_last(EditSource::Agent, "menu_rollback");
    if (result.str("status") != "applied") {
        win::message_box(window_, i18n::tr("app.title"),
                         i18n::tr("dlg.nothing_to_rollback.text",
                                  {result.str("error", i18n::tr("dlg.no_agent_changes"))}),
                         MB_ICONINFORMATION);
    }
    sync_menu_state();
}

void EditorWindow::command_rollback_all_agent() {
    if (!win::confirm(window_, i18n::tr("app.title"), i18n::tr("dlg.rollback_all.text"))) return;
    int reverted = 0;
    for (int i = 0; i < 200; ++i) {
        const Json result = workspace_.rollback_last(EditSource::Agent, "menu_rollback");
        if (result.str("status") != "applied") break;
        ++reverted;
    }
    win::message_box(window_, i18n::tr("app.title"),
                     i18n::tr("dlg.rolled_back.text", {std::to_string(reverted)}),
                     MB_ICONINFORMATION);
    sync_menu_state();
}

void EditorWindow::command_about() {
    const std::string endpoint =
        (mcp_ && mcp_->status().listening) ? mcp_->status().base_url + "/mcp" : "(-)";
    win::message_box(window_, i18n::tr("app.title"),
                     i18n::tr("help.about.text", {"0.1.0", endpoint}), MB_ICONINFORMATION);
}

void EditorWindow::command_help_mcp() {
    const std::string endpoint =
        (mcp_ && mcp_->status().listening) ? mcp_->status().endpoint : std::string("(-)");
    const std::string discovery =
        (mcp_ && !mcp_->discovery_path().empty()) ? mcp_->discovery_path() : std::string("(-)");
    win::message_box(
        window_, i18n::tr("help.mcp.title"),
        i18n::tr("help.mcp.text", {endpoint, "colaimdation_mcp_bridge.exe", discovery}),
        MB_ICONINFORMATION);
}

void EditorWindow::on_frontend_command(const std::string& command) {
    if (command == "new") command_new();
    else if (command == "open") command_open();
    else if (command == "save") command_save();
    else if (command == "save_as") command_save_as();
    else if (command == "reload") command_reload();
    else if (command == "undo") command_undo();
    else if (command == "redo") command_redo();
    else if (command == "close_tab") command_close_tab();
    else if (command == "next_tab") command_step_tab(1);
    else if (command == "prev_tab") command_step_tab(-1);
    else if (command == "copy_endpoint") command_copy_endpoint();
    else if (command == "about") command_about();
    else if (command == "quit") ::PostMessageW(window_, WM_CLOSE, 0, 0);
    else if (command == "devtools") {
#ifdef COLAI_USER_BUILD
        // The end-user build has no developer tools at all.
        return;
#else
        // Opens the DevTools *window*: echoing this command back to the page made
        // the two sides bounce it between them forever (and DevTools never opened).
        if (bridge_) bridge_->open_devtools();
#endif
    }
    else if (command == "toggle_source_mode" || command == "toggle_agent_panel" ||
             command == "toggle_audit_panel" || command == "request_status") {
        // Commands the front-end owns: echo them back so its own handler runs.
        if (bridge_) bridge_->send_command(command);
    }
    sync_menu_state();
}

Json EditorWindow::on_panel_query(const Json& message) {
    const std::string type = message.str("type");
    if (type == "activate_tab") {
        const int64_t index = message.integer("index", -1);
        if (index < 0) return Json();
        std::string error;
        if (!workspace_.activate_tab(static_cast<size_t>(index), &error).ok) return Json();
        update_window_title();
        sync_menu_state();
        // The tab list travels back as the reply so the bar highlights the tab even
        // if the push notification is still in flight.
        Json reply = workspace_.tabs_json();
        reply.set("type", Json("tabs"));
        return reply;
    }
    if (type == "close_tab") {
        const int64_t index = message.integer("index", -1);
        if (index < 0 || static_cast<size_t>(index) >= workspace_.tab_count()) return Json();
        close_tab_with_prompt(static_cast<size_t>(index));
        return Json();
    }
    if (type == "tabs_query") {
        Json reply = workspace_.tabs_json();
        reply.set("type", Json("tabs"));
        return reply;
    }
    if (type == "theme_changed") {
        // The writer picked an appearance in the toolbar. Remember it (it is what
        // the next start uses; --theme on the command line still wins) and refresh
        // the bootstrap so a front-end reload keeps the new choice.
        const std::string theme = message.str("theme");
        if (theme == "light" || theme == "dark" || theme == "auto") {
            settings_.set_theme(theme);
            if (bridge_) bridge_->set_bootstrap(build_bootstrap());
        }
        return Json();
    }
    if (type == "mcp_settings") {
        // The Agent panel's endpoint controls. The port is remembered automatically
        // (see create()); these let the writer pin a specific port, pin the token so
        // the configured URL survives a restart, or roll a fresh token.
        Settings& settings = settings_.get();
        if (message.contains("port")) {
            settings_.set_mcp_port(static_cast<int>(message.integer("port", settings.mcp_port)));
        }
        if (message.boolean("regenerate_token", false) && mcp_) {
            mcp_->set_token(Policy::generate_token());
            // A pinned token follows the new value; otherwise it stays ephemeral.
            if (!settings.mcp_token.empty()) settings_.set_mcp_token(mcp_->status().token);
            refresh_discovery_file();
        }
        if (message.contains("keep_token") && mcp_) {
            settings_.set_mcp_token(message.boolean("keep_token", false) ? mcp_->status().token
                                                                        : std::string());
            refresh_discovery_file();
        }
        Json reply = Json::object();
        reply.set("type", Json("status_reply"));
        reply.set("mcp", mcp_ ? mcp_->status().to_json() : Json());
        reply.set("policy", workspace_.policy_json());
        reply.set("mcp_settings", mcp_settings_json());
        return reply;
    }
    if (type == "edit") {
        // The writer typed in the editor. Without this branch the front-end's
        // editing was silently discarded: the workspace kept the old text, so
        // File -> Save wrote a stale document and agents read stale content.
        Workspace::EditReport report;
        report.revision_before = message.str("revision_before");
        report.text = message.str("text");
        report.caret = static_cast<size_t>(std::max<int64_t>(0, message.integer("caret", 0)));
        report.selection_anchor =
            static_cast<size_t>(std::max<int64_t>(0, message.integer("selection_anchor", report.caret)));
        report.selection_head =
            static_cast<size_t>(std::max<int64_t>(0, message.integer("selection_head", report.caret)));
        report.hint = message.str("hint", "typing");
        Json reply = workspace_.apply_user_edit(report);
        // The reply goes straight back to the page, so it must not be mistaken
        // for one of the queries below.
        sync_menu_state();
        return reply;
    }
    if (type == "selection") {
        const int64_t anchor = std::max<int64_t>(0, message.integer("anchor", 0));
        const int64_t head = std::max<int64_t>(0, message.integer("head", anchor));
        return workspace_.update_selection(static_cast<size_t>(anchor), static_cast<size_t>(head));
    }
    if (type == "status_query") {
        Json reply = workspace_.status_json();
        reply.set("type", Json("status_reply"));
        reply.set("mcp", mcp_ ? mcp_->status().to_json() : Json());
        reply.set("mcp_settings", mcp_settings_json());
        const Document& document = workspace_.document();
        Json document_info = reply["document"];
        document_info.set("can_undo", Json(document.can_undo()));
        document_info.set("can_redo", Json(document.can_redo()));
        return reply;
    }
    if (type == "audit_query") {
        AuditQuery query;
        query.limit = static_cast<size_t>(std::max<int64_t>(1, message.integer("limit", 40)));
        query.with_diff = message.boolean("include_diff", false);
        Json reply = workspace_.audit_json(query);
        reply.set("type", Json("audit_reply"));
        return reply;
    }
    if (type == "language_changed") {
        // The writer switched language from the editor's own toolbar: follow it,
        // but do not echo a second message back (announce=false).
        const std::string tag = message.str("language");
        if (i18n::normalize_tag(tag) != i18n::language()) {
            apply_language(tag, false);
        }
        remember_language(i18n::language());
        return Json();
    }
    if (type == "ui_command") {
        // Language switching without the menu, for scripts and automation:
        // the page posts {type:"ui_command", command:"set_language", language:"ja"}.
        if (message.str("command") == "set_language") {
            const std::string tag = message.str("language");
            if (i18n::normalize_tag(tag) != i18n::language()) {
                apply_language(tag, false);
                remember_language(i18n::language());
            }
        }
        return Json();
    }
    if (type == "self_test_result") {
        // The front-end's own diagnostics: shown verbatim so a report can be
        // copied straight back to whoever is fixing the problem.
        const std::string report = message.str("report");
        win::message_box(window_, i18n::tr("dlg.selftest.title"),
                         report.empty() ? i18n::tr("dlg.selftest.empty") : report);
        return Json();
    }
    if (type == "set_policy") {
        Json updated =
            workspace_.set_policy(message.contains("policy") ? message["policy"] : message);
        sync_menu_state();
        updated.set("type", Json("policy_changed"));
        return updated;
    }
    if (type == "rollback_last") {
        Json result = workspace_.rollback_last(EditSource::Agent, "ui_rollback");
        sync_menu_state();
        result.set("type", Json("rollback_reply"));
        return result;
    }
    if (type == "confirm_change") {
        if (bridge_) {
            bridge_->resolve_confirmation(message.str("pending_id"),
                                          message.boolean("accepted", false), message.str("reason"));
        }
        return Json();
    }
    return Json();
}

int EditorWindow::message_loop() {
    MSG message{};
    while (::GetMessageW(&message, nullptr, 0, 0) > 0) {
        // Accelerators first so Ctrl+S works even when the WebView2 control has
        // focus (WM_COMMAND from the accelerator reaches us directly).
        if (message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN) {
            const bool ctrl = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
            const bool shift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if (ctrl) {
                switch (message.wParam) {
                case 'N': command_new(); continue;
                case 'O': command_open(); continue;
                case 'S': shift ? command_save_as() : command_save(); continue;
                // Ctrl+P is deliberately left alone: the engine's own print is the
                // accelerator that worked before, and the page prepares the paper
                // layout for it through beforeprint (File -> Print uses the engine's
                // ShowPrintUI instead).
                case 'W': command_close_tab(); continue;
                case 'Z': shift ? command_rollback_last_agent() : command_undo(); continue;
                case 'Y': command_redo(); continue;
                case VK_TAB: command_step_tab(shift ? -1 : 1); continue;
                case 'A':
                    // Only consume the key when the accelerator actually fires;
                    // swallowing plain Ctrl+A killed select-all in the editor.
                    if (shift && bridge_) {
                        bridge_->send_command("toggle_agent_panel");
                        continue;
                    }
                    break;
                case 'L':
                    if (shift && bridge_) {
                        bridge_->send_command("toggle_audit_panel");
                        continue;
                    }
                    break;
                default: break;
                }
            }
            if (message.wParam == VK_F5) {
                // Same as View -> Reload the front-end: it has to work when the window
                // is showing a page that is not the editor any more.
                if (bridge_) bridge_->restore_interface();
                continue;
            }
            if (message.wParam == VK_F12) {
#ifdef COLAI_USER_BUILD
                // Swallowed: the end-user build has no developer tools (letting the
                // key through would send it to the page, which does nothing with it).
                continue;
#else
                if (bridge_) bridge_->open_devtools();
                continue;
#endif
            }
        }
        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

void EditorWindow::shutdown() {
    workspace_.set_bridge(nullptr);
    mcp_.reset();
    bridge_.reset();
    workspace_.shutdown();
    window_ = nullptr;
}

} // namespace colai
