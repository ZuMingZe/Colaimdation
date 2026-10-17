// Colaimdation - shell translations (menus, dialogs, window titles).
//
// The editor front-end has its own catalog under web/assets/lang/; this one
// covers the strings the native shell owns. Keys are shared by name so a
// translator can work from one list, and both catalogs are validated by tests
// that check every key exists in every language.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace colai::i18n {

// Language tags follow BCP-47 as far as the UI menu needs: "en", "zh-Hans", "ja".
inline constexpr const char* kEnglish = "en";
inline constexpr const char* kChineseSimplified = "zh-Hans";
inline constexpr const char* kJapanese = "ja";

struct LanguageInfo {
    std::string tag;         // "zh-Hans"
    std::string english_name; // "Chinese (Simplified)"
    std::string native_name;  // "简体中文"
};

// Every language this build ships with, in menu order.
const std::vector<LanguageInfo>& languages();

// True when the tag names a language we ship (case-insensitive, tolerant of
// "zh_CN", "zh-Hans-CN", "en-US" style inputs).
bool is_supported(std::string_view tag);

// Maps an arbitrary tag onto a shipped language, or "en" when nothing matches.
std::string normalize_tag(std::string_view tag);

// Best language for a Windows locale name such as "zh-CN" / "ja-JP".
std::string from_system_locale();

// Native (UTF-16) name shown in the language menu, e.g. "简体中文".
std::string native_name(std::string_view tag);

// Sets the active language; unsupported tags fall back to English.
void set_language(std::string_view tag);
const std::string& language();

// Looks up `key` in the active language, falling back to English, then to the
// key itself so a missing translation is visible instead of blank.
std::string tr(std::string_view key);
// Same, with {0}..{9} placeholders replaced in order.
std::string tr(std::string_view key, const std::vector<std::string>& arguments);

// Exposed for tests: every key present in the catalog for `tag`.
std::vector<std::string> keys_for(std::string_view tag);

// Keys whose translation is missing or empty for `tag`. Used by the tests to
// guarantee every shipped language is complete.
std::vector<std::string> untranslated_keys_for(std::string_view tag);

} // namespace colai::i18n
