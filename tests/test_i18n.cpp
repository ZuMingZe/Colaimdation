// Interface localisation: catalog completeness, lookups, and the language
// preference file. Also cross-checks the front-end language packs.
#include "test_framework.h"

#include "core/i18n.h"
#include "core/settings.h"
#include "core/utf8.h"
#include "core/workspace.h"

#include <algorithm>
#include <fstream>
#include <sstream>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

using namespace colai;

namespace {

std::string temp_path(const std::string& name) {
    char buffer[MAX_PATH] = {};
    ::GetTempPathA(MAX_PATH, buffer);
    return std::string(buffer) + "colaimdation_i18n_" + name;
}

std::string read_file(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return {};
    std::stringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

} // namespace

TEST_CASE("i18n/ships at least three languages with native names") {
    const auto& languages = i18n::languages();
    CHECK(languages.size() >= size_t(3));

    bool saw_english = false, saw_chinese = false, saw_japanese = false;
    for (const i18n::LanguageInfo& language : languages) {
        CHECK(!language.tag.empty());
        CHECK(!language.native_name.empty());
        CHECK(!language.english_name.empty());
        if (language.tag == i18n::kEnglish) saw_english = true;
        if (language.tag == i18n::kChineseSimplified) {
            saw_chinese = true;
            // 简体中文 as UTF-8, not as escaped bytes.
            CHECK_EQ(language.native_name, std::string("简体中文"));
        }
        if (language.tag == i18n::kJapanese) {
            saw_japanese = true;
            CHECK_EQ(language.native_name, std::string("日本語"));
        }
    }
    CHECK(saw_english);
    CHECK(saw_chinese);
    CHECK(saw_japanese);
}

TEST_CASE("i18n/every language defines every key") {
    for (const i18n::LanguageInfo& language : i18n::languages()) {
        const std::vector<std::string> missing = i18n::untranslated_keys_for(language.tag);
        if (missing.empty()) continue;
        std::string detail = "missing in " + language.tag + ": ";
        for (size_t i = 0; i < missing.size() && i < 8; ++i) {
            if (i) detail += ", ";
            detail += missing[i];
        }
        ::colai::test::fail(__FILE__, __LINE__, "all keys translated", detail);
    }
}

TEST_CASE("i18n/catalog is not trivially small") {
    const std::vector<std::string> keys = i18n::keys_for("en");
    CHECK(keys.size() >= size_t(50));
}

TEST_CASE("i18n/lookups follow the active language") {
    i18n::set_language(i18n::kEnglish);
    CHECK_EQ(i18n::language(), std::string("en"));
    const std::string english_save = i18n::tr("menu.file.save");
    CHECK_STR_CONTAINS(english_save, "Save");

    i18n::set_language(i18n::kChineseSimplified);
    CHECK_EQ(i18n::language(), std::string("zh-Hans"));
    const std::string chinese_save = i18n::tr("menu.file.save");
    CHECK_STR_CONTAINS(chinese_save, "保存");
    CHECK_NE(chinese_save, english_save);

    i18n::set_language(i18n::kJapanese);
    CHECK_EQ(i18n::language(), std::string("ja"));
    const std::string japanese_save = i18n::tr("menu.file.save");
    CHECK_STR_CONTAINS(japanese_save, "保存");
    CHECK_NE(japanese_save, english_save);

    i18n::set_language(i18n::kEnglish);
}

TEST_CASE("i18n/access keys and accelerators survive translation") {
    // Menu captions carry the Win32 accelerator marker and the shortcut hint;
    // losing either would silently break the keyboard UI.
    for (const i18n::LanguageInfo& language : i18n::languages()) {
        i18n::set_language(language.tag);
        for (const char* key : {"menu.file.save", "menu.file.open", "menu.edit.undo",
                                "menu.agent.panel", "menu.help.about"}) {
            const std::string caption = i18n::tr(key);
            CHECK_STR_CONTAINS(caption, "&");
        }
        CHECK_STR_CONTAINS(i18n::tr("menu.file.save"), "Ctrl+S");
        CHECK_STR_CONTAINS(i18n::tr("menu.file.open"), "Ctrl+O");
        CHECK_STR_CONTAINS(i18n::tr("menu.edit.undo"), "Ctrl+Z");
    }
    i18n::set_language(i18n::kEnglish);
}

TEST_CASE("i18n/placeholder substitution") {
    i18n::set_language(i18n::kEnglish);
    CHECK_EQ(i18n::tr("cli.unknown_option", {"--nope"}), std::string("unknown option: --nope"));
    CHECK_EQ(i18n::tr("cli.missing_value", {"--file"}), std::string("missing value for --file"));

    i18n::set_language(i18n::kChineseSimplified);
    CHECK_EQ(i18n::tr("cli.unknown_option", {"--nope"}), std::string("未知的选项：--nope"));

    // A placeholder that is not supplied stays visible instead of vanishing.
    const std::string partial = i18n::tr("dlg.save_failed.text");
    CHECK_STR_CONTAINS(partial, "{0}");
    i18n::set_language(i18n::kEnglish);
}

TEST_CASE("i18n/substitution handles several placeholders") {
    i18n::set_language(i18n::kEnglish);
    CHECK_EQ(i18n::tr("cli.port.number", {"x"}), std::string("--port needs a number, got 'x'"));

    const std::string text = i18n::tr("cli.config.malformed", {"f.json", "bad token", "7"});
    CHECK_STR_CONTAINS(text, "f.json");
    CHECK_STR_CONTAINS(text, "bad token");
    CHECK_STR_CONTAINS(text, "7");
    CHECK(text.find('{') == std::string::npos);
}

TEST_CASE("i18n/normalisation accepts common spellings") {
    CHECK_EQ(i18n::normalize_tag("en"), std::string("en"));
    CHECK_EQ(i18n::normalize_tag("en-US"), std::string("en"));
    CHECK_EQ(i18n::normalize_tag("EN_us"), std::string("en"));
    CHECK_EQ(i18n::normalize_tag("zh"), std::string("zh-Hans"));
    CHECK_EQ(i18n::normalize_tag("zh-CN"), std::string("zh-Hans"));
    CHECK_EQ(i18n::normalize_tag("zh_Hans_CN"), std::string("zh-Hans"));
    CHECK_EQ(i18n::normalize_tag("zh-Hans"), std::string("zh-Hans"));
    CHECK_EQ(i18n::normalize_tag("ja"), std::string("ja"));
    CHECK_EQ(i18n::normalize_tag("ja-JP"), std::string("ja"));
    // Unknown tags fall back to English rather than breaking the UI.
    CHECK_EQ(i18n::normalize_tag("fr"), std::string("en"));
    CHECK_EQ(i18n::normalize_tag(""), std::string("en"));
    CHECK_EQ(i18n::normalize_tag("klingon"), std::string("en"));
}

TEST_CASE("i18n/support detection") {
    CHECK(i18n::is_supported("en"));
    CHECK(i18n::is_supported("zh-Hans"));
    CHECK(i18n::is_supported("zh-CN"));
    CHECK(i18n::is_supported("ja"));
    CHECK(i18n::is_supported("ja-JP"));
    CHECK(!i18n::is_supported("fr"));
    CHECK(!i18n::is_supported(""));
    CHECK(!i18n::is_supported("english"));
}

TEST_CASE("i18n/unsupported language falls back to english") {
    i18n::set_language("fr");
    CHECK_EQ(i18n::language(), std::string("en"));
    CHECK_STR_CONTAINS(i18n::tr("menu.file.save"), "Save");
}

TEST_CASE("i18n/unknown key returns the key rather than an empty string") {
    const std::string value = i18n::tr("no.such.key.exists");
    CHECK_EQ(value, std::string("no.such.key.exists"));
}

TEST_CASE("i18n/system locale maps onto a shipped language") {
    const std::string detected = i18n::from_system_locale();
    CHECK(i18n::is_supported(detected) || detected == "en");
}

TEST_CASE("i18n/native names are usable in a menu") {
    CHECK_EQ(i18n::native_name("zh-Hans"), std::string("简体中文"));
    CHECK_EQ(i18n::native_name("ja-JP"), std::string("日本語"));
    CHECK_EQ(i18n::native_name("en-GB"), std::string("English"));
    CHECK_EQ(i18n::native_name("nope"), std::string("English"));
}

TEST_CASE("i18n/usage and help text exist in every language") {
    for (const i18n::LanguageInfo& language : i18n::languages()) {
        i18n::set_language(language.tag);
        const std::string usage = i18n::tr("cli.usage", {"en, zh-Hans, ja"});
        CHECK_STR_CONTAINS(usage, "--file");
        CHECK_STR_CONTAINS(usage, "--lang");
        CHECK_STR_CONTAINS(usage, "--port");
        CHECK_STR_CONTAINS(usage, "en, zh-Hans, ja");
        CHECK_STR_CONTAINS(i18n::tr("help.mcp.text", {"url", "bridge", "path"}), "url");
        CHECK(!i18n::tr("help.about.text", {"0.1.0", "endpoint"}).empty());
    }
    i18n::set_language(i18n::kEnglish);
}

TEST_CASE("i18n/native keys the shell needs are all present") {
    const std::vector<std::string> keys = i18n::keys_for("en");
    for (const char* required : {"app.title", "app.untitled", "menu.file", "menu.file.save",
                                 "menu.view.language", "menu.help.mcp", "menu.help.about",
                                 "dlg.unsaved.text", "dlg.endpoint_copied.text",
                                 "dlg.language.changed", "help.about.text", "help.mcp.text",
                                 "fd.open.title", "fd.save.title", "cli.usage",
                                 "cli.unknown_option", "cli.lang.invalid"}) {
        const bool found = std::find(keys.begin(), keys.end(), std::string(required)) != keys.end();
        if (!found) {
            ::colai::test::fail(__FILE__, __LINE__, "required native key",
                                std::string("missing: ") + required);
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Front-end packs                                                            */
/* -------------------------------------------------------------------------- */

TEST_CASE("i18n/front-end packs and the embedded runtime agree") {
    // The web UI keeps its own key namespace (toolbar.*, panel.*) because those
    // strings never reach the shell. Every shipped pack must define the web keys
    // and the runtime bundle the page loads must embed all three languages.
    const std::string root = ::colai::test::source_directory();
    CHECK(!root.empty());

    const char* web_keys[] = {"toolbar.new",   "toolbar.open",     "toolbar.save",
                              "toolbar.source", "toolbar.language", "panel.agent",
                              "panel.audit",    "panel.diff",       "diff.apply",
                              "diff.reject",    "toast.copied",     "status.unsaved",
                              "editor.empty",   "mcp.off",          "agent.permissions",
                              "audit.refresh",  "status.mode"};

    struct Pack {
        const char* file;
        const char* tag;
    };
    const Pack packs[] = {
        {"web/assets/lang/en.json", "en"},
        {"web/assets/lang/zh-Hans.json", "zh-Hans"},
        {"web/assets/lang/ja.json", "ja"},
    };
    for (const Pack& pack : packs) {
        const std::string contents = read_file(root + "/" + pack.file);
        if (contents.empty()) {
            ::colai::test::fail(__FILE__, __LINE__, pack.file, "file missing or empty");
            continue;
        }
        for (const char* key : web_keys) {
            if (contents.find(std::string("\"") + key + "\"") == std::string::npos) {
                ::colai::test::fail(__FILE__, __LINE__, pack.file,
                                    std::string("no entry for ") + key);
            }
        }
        if (contents.find(pack.tag) == std::string::npos) {
            ::colai::test::fail(__FILE__, __LINE__, pack.file, "pack does not name its own tag");
        }
    }

    const std::string bundle = read_file(root + "/web/assets/i18n.js");
    CHECK(!bundle.empty());
    for (const char* key : web_keys) {
        CHECK_STR_CONTAINS(bundle, std::string("'") + key + "'");
    }
    // One pack object per language, matching the tags in the native catalog.
    for (const char* tag : {"en: {", "'zh-Hans': {", "ja: {"}) {
        CHECK_STR_CONTAINS(bundle, tag);
    }
}

TEST_CASE("i18n/front-end packs are valid UTF-8 with real translations") {
    const std::string root = ::colai::test::source_directory();
    CHECK(!root.empty());
    for (const char* file : {"web/assets/lang/en.json", "web/assets/lang/zh-Hans.json",
                             "web/assets/lang/ja.json"}) {
        const std::string contents = read_file(root + "/" + file);
        CHECK(!contents.empty());
        CHECK(utf8::is_valid(contents));
        CHECK(contents.size() > 1000);
    }

    // The CJK packs must really contain CJK, and the two must differ: copying the
    // Chinese pack as "Japanese" would otherwise pass unnoticed.
    const std::string chinese = read_file(root + "/web/assets/lang/zh-Hans.json");
    const std::string japanese = read_file(root + "/web/assets/lang/ja.json");
    CHECK_NE(chinese, japanese);

    auto count_cjk = [](const std::string& text) {
        size_t count = 0;
        for (size_t i = 0; i + 2 < text.size();) {
            const unsigned char lead = static_cast<unsigned char>(text[i]);
            if (lead >= 0xE0 && lead <= 0xEF) {
                ++count;
                i += 3;
            } else {
                i += 1;
            }
        }
        return count;
    };
    CHECK(count_cjk(chinese) > size_t(200));
    CHECK(count_cjk(japanese) > size_t(200));
}

/* -------------------------------------------------------------------------- */
/* Settings                                                                   */
/* -------------------------------------------------------------------------- */

TEST_CASE("settings/round trip through disk") {
    const std::string path = temp_path("settings.json");
    ::DeleteFileA(path.c_str());

    SettingsStore store;
    store.set_path(path);
    std::string error;
    CHECK(store.load(&error)); // a missing file is not an error
    CHECK(store.get().language.empty());
    CHECK_EQ(store.get().theme, std::string("auto"));

    Settings& settings = store.get();
    settings.language = "zh-Hans";
    settings.theme = "dark";
    settings.font_size = 19.5;
    settings.window_x = 120;
    settings.window_y = 64;
    settings.window_width = 1100;
    settings.window_height = 700;
    settings.window_maximized = true;
    // The MCP endpoint is part of the saved preferences: Agents are configured with
    // one URL, so the port and (when pinned) the token have to survive a restart.
    settings.mcp_port = 41777;
    settings.mcp_token = std::string(64, 'a');
    CHECK(store.save(&error));

    SettingsStore reopened;
    reopened.set_path(path);
    CHECK(reopened.load(&error));
    CHECK_EQ(reopened.get().language, std::string("zh-Hans"));
    CHECK_EQ(reopened.get().theme, std::string("dark"));
    CHECK_EQ(reopened.get().font_size, 19.5);
    CHECK_EQ(reopened.get().window_x, 120);
    CHECK_EQ(reopened.get().window_width, 1100);
    CHECK(reopened.get().window_maximized);
    CHECK_EQ(reopened.get().mcp_port, 41777);
    CHECK_EQ(reopened.get().mcp_token, std::string(64, 'a'));
    ::DeleteFileA(path.c_str());
}

TEST_CASE("settings/mcp port and token are clamped or rejected") {
    Settings settings = Settings::defaults();
    CHECK_EQ(settings.mcp_port, 0);
    CHECK(settings.mcp_token.empty());

    Json patch = Json::object();
    patch.set("mcp_port", Json(static_cast<int64_t>(70000))); // not a port
    patch.set("mcp_token", Json("short"));                    // not a token
    settings.apply_json(patch);
    CHECK_EQ(settings.mcp_port, 0);
    CHECK(settings.mcp_token.empty());

    Json ok = Json::object();
    ok.set("mcp_port", Json(static_cast<int64_t>(41777)));
    ok.set("mcp_token", Json(std::string(64, 'f')));
    settings.apply_json(ok);
    CHECK_EQ(settings.mcp_port, 41777);
    CHECK_EQ(settings.mcp_token, std::string(64, 'f'));

    // 0 means "let the editor choose and remember": writing it must survive.
    Json auto_port = Json::object();
    auto_port.set("mcp_port", Json(static_cast<int64_t>(0)));
    settings.apply_json(auto_port);
    CHECK_EQ(settings.mcp_port, 0);
}

TEST_CASE("settings/a broken file falls back to defaults and reports why") {
    const std::string path = temp_path("broken.json");
    {
        std::ofstream stream(path, std::ios::binary);
        stream << "{ not json at all ";
    }
    SettingsStore store;
    store.set_path(path);
    std::string error;
    CHECK(!store.load(&error));
    CHECK_STR_CONTAINS(error, "not valid JSON");
    CHECK_EQ(store.get().theme, std::string("auto"));
    CHECK(store.get().language.empty());
    ::DeleteFileA(path.c_str());
}

TEST_CASE("settings/invalid values are clamped or replaced") {
    Settings settings = Settings::defaults();
    Json patch = Json::object();
    patch.set("theme", Json("psychedelic"));
    patch.set("font_size", Json(500.0));
    Json window = Json::object();
    window.set("width", Json(10));
    window.set("height", Json(999999));
    patch.set("window", std::move(window));
    settings.apply_json(patch);

    CHECK_EQ(settings.theme, std::string("auto"));
    CHECK_EQ(settings.font_size, 48.0);
    CHECK_EQ(settings.window_width, 640);
    CHECK_EQ(settings.window_height, 20000);
}

TEST_CASE("settings/set_language persists immediately") {
    const std::string path = temp_path("language.json");
    ::DeleteFileA(path.c_str());
    SettingsStore store;
    store.set_path(path);
    CHECK(store.set_language("ja"));

    SettingsStore reopened;
    reopened.set_path(path);
    CHECK(reopened.load(nullptr));
    CHECK_EQ(reopened.get().language, std::string("ja"));
    ::DeleteFileA(path.c_str());
}

TEST_CASE("settings/an empty path is a no-op instead of an error") {
    SettingsStore store;
    std::string error;
    CHECK(store.load(&error));
    CHECK(store.save(&error));
    CHECK(store.set_language("en"));
}
