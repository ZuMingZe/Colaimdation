// Colaimdation - persisted user preferences.
//
// Stored as a small JSON file next to the audit log so a user can inspect or
// edit it. Reading never fails hard: a missing or broken file falls back to
// defaults and the next save rewrites it.
#pragma once

#include <string>
#include <vector>

#include "core/json.h"

namespace colai {

struct Settings {
    // Interface language tag ("en", "zh-Hans", "ja"); empty means "follow the
    // operating system".
    std::string language;
    // "auto" | "light" | "dark".
    std::string theme = "auto";
    double font_size = 16.0;
    // Window placement, persisted so the editor reopens where you left it.
    int window_x = -1;
    int window_y = -1;
    int window_width = 1280;
    int window_height = 860;
    bool window_maximized = false;

    // --- MCP endpoint ------------------------------------------------------
    // The stable endpoint Agents are configured with. The port is remembered after
    // the first successful start (0 means "not chosen yet": pick a free one and
    // store it), so the URL does not change on every restart.
    int mcp_port = 0;
    // Empty means "generate a fresh token for every run" (the safer default).
    // A pinned token keeps the configured endpoint valid across restarts.
    std::string mcp_token;

    Json to_json() const;
    void apply_json(const Json& value);

    static Settings defaults();
};

class SettingsStore {
public:
    SettingsStore() = default;

    // `path` is the JSON file to use. Call load() before get().
    void set_path(std::string path) { path_ = std::move(path); }
    const std::string& path() const { return path_; }

    // Returns false and fills `error` only for an unreadable-but-present file.
    bool load(std::string* error = nullptr);
    bool save(std::string* error = nullptr) const;

    Settings& get() { return settings_; }
    const Settings& get() const { return settings_; }

    // Updates one language tag and persists it.
    bool set_language(std::string tag);
    bool set_theme(std::string theme);
    // MCP endpoint: 0 means "let the editor pick a free port" (it stores whichever
    // port it ends up with), and an empty token means "generate one per run".
    bool set_mcp_port(int port);
    bool set_mcp_token(std::string token);

private:
    std::string path_;
    Settings settings_ = Settings::defaults();
};

} // namespace colai
