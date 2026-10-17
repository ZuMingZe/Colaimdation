#include "core/settings.h"

#include "core/workspace.h"

#include <algorithm>

namespace colai {

Json Settings::to_json() const {
    Json out = Json::object();
    out.set("language", Json(language));
    out.set("theme", Json(theme));
    out.set("font_size", Json(font_size));
    Json window = Json::object();
    window.set("x", Json(static_cast<int64_t>(window_x)));
    window.set("y", Json(static_cast<int64_t>(window_y)));
    window.set("width", Json(static_cast<int64_t>(window_width)));
    window.set("height", Json(static_cast<int64_t>(window_height)));
    window.set("maximized", Json(window_maximized));
    out.set("window", std::move(window));
    // The MCP endpoint Agents connect to. Written so the same URL keeps working
    // after a restart (see EditorWindow::create for the precedence rules).
    out.set("mcp_port", Json(static_cast<int64_t>(mcp_port)));
    out.set("mcp_token", Json(mcp_token));
    return out;
}

void Settings::apply_json(const Json& value) {
    if (!value.is_object()) return;
    if (value.find("language")) language = value.str("language", language);
    if (value.find("theme")) {
        const std::string candidate = value.str("theme", theme);
        theme = (candidate == "light" || candidate == "dark") ? candidate : "auto";
    }
    if (value.find("font_size")) {
        font_size = std::clamp(value.number("font_size", font_size), 8.0, 48.0);
    }
    if (value.find("mcp_port")) {
        const int64_t port = value.integer("mcp_port", mcp_port);
        // 0 = "pick one and remember it"; anything else must be a real port.
        mcp_port = (port > 0 && port <= 65535) ? static_cast<int>(port) : 0;
    }
    if (value.find("mcp_token")) {
        const std::string token = value.str("mcp_token", mcp_token);
        // A pinned token is hex; anything else would silently break auth.
        mcp_token = (token.size() >= 16 && token.size() <= 256) ? token : std::string();
    }
    const Json* window = value.find("window");
    if (window && window->is_object()) {
        window_x = static_cast<int>(window->integer("x", window_x));
        window_y = static_cast<int>(window->integer("y", window_y));
        window_width = std::clamp(static_cast<int>(window->integer("width", window_width)), 640, 20000);
        window_height = std::clamp(static_cast<int>(window->integer("height", window_height)), 420, 20000);
        window_maximized = window->boolean("maximized", window_maximized);
    }
}

Settings Settings::defaults() { return Settings{}; }

bool SettingsStore::load(std::string* error) {
    settings_ = Settings::defaults();
    if (path_.empty()) return true;

    std::string contents;
    std::string read_error;
    if (!Workspace::read_text_file(path_, &contents, &read_error).ok) {
        // No file yet is the normal first-run case, not a failure.
        return true;
    }
    JsonParseError parse_error;
    auto parsed = json_parse(contents, &parse_error);
    if (!parsed) {
        if (error) {
            *error = "settings file '" + path_ + "' is not valid JSON (" + parse_error.message +
                     "); using defaults";
        }
        return false;
    }
    settings_.apply_json(*parsed);
    return true;
}

bool SettingsStore::save(std::string* error) const {
    if (path_.empty()) return true;
    // UTF-8 BOM so Windows PowerShell 5.1 (and older editors) read the file with
    // the right code page; Workspace::read_text_file strips it again on load.
    return Workspace::write_text_file(path_, settings_.to_json().dump(true) + "\n", error, true).ok;
}

bool SettingsStore::set_language(std::string tag) {
    settings_.language = std::move(tag);
    return save(nullptr);
}

bool SettingsStore::set_theme(std::string theme) {
    settings_.theme = std::move(theme);
    return save(nullptr);
}

bool SettingsStore::set_mcp_port(int port) {
    settings_.mcp_port = (port > 0 && port <= 65535) ? port : 0;
    return save(nullptr);
}

bool SettingsStore::set_mcp_token(std::string token) {
    settings_.mcp_token = std::move(token);
    return save(nullptr);
}

} // namespace colai
