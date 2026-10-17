// Colaimdation - classifying the URLs the shell hands out.
//
// The front end is served from a loopback virtual host, so a link inside a document
// resolves against that host: "other.md" becomes
// https://colaimdation.local/other.md. Accepting *anything* under that host (rather
// than the editor's own page) let one click navigate the editor window to a page that
// does not exist, which replaces the whole interface - toolbar, tabs, panels - with
// the browser's error page, and the window has no back button to undo it.
#pragma once

#include "core/utf8.h"

#include <string>
#include <string_view>

namespace colai::url {

// The page the editor shell loads, and the only URL it may navigate itself to.
inline std::string editor_page(std::string_view virtual_host, std::string_view page = "index.html") {
    return "https://" + std::string(virtual_host) + "/" + std::string(page);
}

// True when `uri` addresses that page, allowing a query or a fragment after it
// (`index.html#anchor`), and ignoring case in the scheme and host.
inline bool is_editor_page(std::string_view uri, std::string_view virtual_host,
                           std::string_view page = "index.html") {
    const std::string target = utf8::to_lower(editor_page(virtual_host, page));
    const std::string lower = utf8::to_lower(uri);
    if (lower == target) return true;
    if (lower.rfind(target, 0) != 0) return false;
    const char next = lower.size() > target.size() ? lower[target.size()] : '\0';
    return next == '?' || next == '#';
}

// True for any URL on the editor's own virtual host. Those only exist inside this
// window's WebView2, so they must never be handed to the default browser: a relative
// link lands here, and there is nothing for a browser to open.
inline bool is_editor_host(std::string_view uri, std::string_view virtual_host) {
    const std::string prefix = "https://" + utf8::to_lower(virtual_host);
    const std::string lower = utf8::to_lower(uri);
    if (lower.rfind(prefix, 0) != 0) return false;
    const char next = lower.size() > prefix.size() ? lower[prefix.size()] : '\0';
    return next == '\0' || next == '/' || next == ':' || next == '?' || next == '#';
}

// URIs a browser cannot do anything useful with; cancelling them is enough.
inline bool is_script_url(std::string_view uri) {
    const std::string lower = utf8::to_lower(uri);
    return lower.rfind("javascript:", 0) == 0 || lower.rfind("data:", 0) == 0 ||
           lower.rfind("about:", 0) == 0 || lower.rfind("blob:", 0) == 0;
}

// Schemes worth handing to the system's default browser.
inline bool opens_in_browser(std::string_view uri) {
    const std::string lower = utf8::to_lower(uri);
    return lower.rfind("http://", 0) == 0 || lower.rfind("https://", 0) == 0 ||
           lower.rfind("mailto:", 0) == 0 || lower.rfind("file:", 0) == 0;
}

} // namespace colai::url
