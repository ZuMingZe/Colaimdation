#include "app/win_util.h"

#include "app/app_icon.h"
#include "core/i18n.h"
#include "core/utf8.h"

#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

namespace colai::win {
namespace {

constexpr const wchar_t* kWindowClassName = L"ColaimdationWindow";
constexpr const wchar_t* kDispatcherClassName = L"ColaimdationDispatcher";

// Builds a Win32 double-NUL-terminated filter string from localised labels.
std::wstring build_filter(const std::vector<std::pair<std::string, std::string>>& entries) {
    std::wstring out;
    for (const auto& entry : entries) {
        out += wide(entry.first);
        out.push_back(L'\0');
        out += wide(entry.second);
        out.push_back(L'\0');
    }
    out.push_back(L'\0');
    return out;
}

} // namespace

std::wstring wide(std::string_view text) { return utf8::to_wide(text); }
std::string narrow(std::wstring_view text) { return utf8::from_wide(text); }

bool register_window_classes(HINSTANCE instance, WNDPROC main_window_proc) {
    static bool registered = false;
    if (registered) return true;

    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_STANDARD_CLASSES | ICC_BAR_CLASSES;
    ::InitCommonControlsEx(&controls);

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    // The editor classes the shell's own window procedure. Registering
    // DefWindowProcW here (as this used to) made every WM_COMMAND, WM_CLOSE and
    // WM_SIZE vanish: the menu bar did nothing and closing the window left the
    // process running.
    window_class.lpfnWndProc = main_window_proc ? main_window_proc : ::DefWindowProcW;
    window_class.hInstance = instance;
    window_class.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    window_class.lpszClassName = kWindowClassName;
    // Title bar / taskbar / Alt+Tab icon, taken from the icon resource compiled into
    // the executable (see src/app/app_icon.rc). LoadImage picks the size Windows asks
    // for out of the multi-size .ico instead of stretching one bitmap.
    window_class.hIcon = static_cast<HICON>(
        ::LoadImageW(instance, MAKEINTRESOURCEW(IDI_COLAIMDATION), IMAGE_ICON,
                     ::GetSystemMetrics(SM_CXICON), ::GetSystemMetrics(SM_CYICON),
                     LR_DEFAULTCOLOR));
    window_class.hIconSm = static_cast<HICON>(
        ::LoadImageW(instance, MAKEINTRESOURCEW(IDI_COLAIMDATION), IMAGE_ICON,
                     ::GetSystemMetrics(SM_CXSMICON), ::GetSystemMetrics(SM_CYSMICON),
                     LR_DEFAULTCOLOR));
    if (!::RegisterClassExW(&window_class) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    WNDCLASSEXW dispatcher_class{};
    dispatcher_class.cbSize = sizeof(dispatcher_class);
    // DefWindowProcW is correct here: WebviewBridge::handle_message answers the
    // dispatcher's messages (kMsgRunJob / kMsgNotify) from the editor window's
    // own procedure, which is where the messages are actually posted.
    dispatcher_class.lpfnWndProc = ::DefWindowProcW;
    dispatcher_class.hInstance = instance;
    dispatcher_class.lpszClassName = kDispatcherClassName;
    if (!::RegisterClassExW(&dispatcher_class) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    registered = true;
    return true;
}

std::string open_file_dialog(HWND owner, const std::string& initial_directory) {
    wchar_t buffer[MAX_PATH * 4] = {};
    if (!initial_directory.empty()) {
        const std::wstring directory = wide(initial_directory);
        ::wcsncpy_s(buffer, directory.c_str(), _TRUNCATE);
    }
    const std::wstring title = wide(i18n::tr("fd.open.title"));
    const std::wstring filter = build_filter({
        {i18n::tr("fd.filter.markdown"), "*.md;*.markdown;*.mdown"},
        {i18n::tr("fd.filter.text"), "*.txt"},
        {i18n::tr("fd.filter.all"), "*.*"},
    });

    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = owner;
    dialog.lpstrFilter = filter.c_str();
    dialog.lpstrFile = buffer;
    dialog.nMaxFile = static_cast<DWORD>(std::size(buffer));
    dialog.lpstrTitle = title.c_str();
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    dialog.lpstrDefExt = L"md";
    if (!::GetOpenFileNameW(&dialog)) return {};
    return narrow(buffer);
}

std::string save_file_dialog(HWND owner, const std::string& suggested_name) {
    wchar_t buffer[MAX_PATH * 4] = {};
    const std::wstring suggestion = wide(suggested_name.empty() ? "untitled.md" : suggested_name);
    ::wcsncpy_s(buffer, suggestion.c_str(), _TRUNCATE);

    const std::wstring title = wide(i18n::tr("fd.save.title"));
    const std::wstring filter = build_filter({
        {i18n::tr("fd.filter.markdown"), "*.md"},
        {i18n::tr("fd.filter.text"), "*.txt"},
        {i18n::tr("fd.filter.all"), "*.*"},
    });

    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = owner;
    dialog.lpstrFilter = filter.c_str();
    dialog.lpstrFile = buffer;
    dialog.nMaxFile = static_cast<DWORD>(std::size(buffer));
    dialog.lpstrTitle = title.c_str();
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    dialog.lpstrDefExt = L"md";
    if (!::GetSaveFileNameW(&dialog)) return {};
    return narrow(buffer);
}

std::string save_file_dialog_as(HWND owner, const std::string& suggested_name,
                                const std::string& title, const std::string& extension) {
    wchar_t buffer[MAX_PATH * 4] = {};
    const std::wstring suggestion = wide(suggested_name);
    ::wcsncpy_s(buffer, suggestion.c_str(), _TRUNCATE);

    // One filter, the format being asked for: a Save dialog for a PDF has no business
    // offering Markdown.
    const std::wstring pattern = L"*." + wide(extension);
    const std::wstring filter = build_filter({
        {i18n::tr("fd.filter.all"), narrow(pattern)},
    });
    const std::wstring wide_title = wide(title);

    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = owner;
    dialog.lpstrFilter = filter.c_str();
    dialog.lpstrFile = buffer;
    dialog.nMaxFile = static_cast<DWORD>(std::size(buffer));
    dialog.lpstrTitle = wide_title.c_str();
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    const std::wstring default_extension = wide(extension);
    dialog.lpstrDefExt = default_extension.c_str();
    if (!::GetSaveFileNameW(&dialog)) return {};
    return narrow(buffer);
}

void message_box(HWND owner, std::string_view title, std::string_view text, UINT flags) {
    ::MessageBoxW(owner, wide(text).c_str(), wide(title).c_str(), flags | MB_SETFOREGROUND);
}

bool confirm(HWND owner, std::string_view title, std::string_view text) {
    return ::MessageBoxW(owner, wide(text).c_str(), wide(title).c_str(),
                         MB_OKCANCEL | MB_ICONQUESTION | MB_SETFOREGROUND) == IDOK;
}

std::string app_data_directory() {
    wchar_t path[MAX_PATH] = {};
    if (::SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, path) != S_OK) {
        return ".";
    }
    std::string result = narrow(path) + "\\colaimdation";
    ::CreateDirectoryW(wide(result).c_str(), nullptr);
    return result;
}

std::string executable_directory() {
    wchar_t path[MAX_PATH] = {};
    const DWORD length = ::GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    if (length == 0) return ".";
    std::string result = narrow(std::wstring_view(path, length));
    const size_t slash = result.find_last_of("\\/");
    return slash == std::string::npos ? std::string(".") : result.substr(0, slash);
}

void enable_dpi_awareness() {
    using SetContextFn = BOOL(WINAPI*)(void*);
    if (HMODULE user32 = ::LoadLibraryW(L"user32.dll")) {
        auto set_context = reinterpret_cast<SetContextFn>(
            ::GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        if (set_context) {
            // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 is the pseudo-handle -4.
            void* per_monitor_v2 = reinterpret_cast<void*>(static_cast<INT_PTR>(-4));
            if (set_context(per_monitor_v2)) {
                ::FreeLibrary(user32);
                return;
            }
        }
        ::FreeLibrary(user32);
    }
    ::SetProcessDPIAware();
}

std::string format_error(DWORD code) {
    LPWSTR buffer = nullptr;
    const DWORD length = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), reinterpret_cast<LPWSTR>(&buffer),
        0, nullptr);
    std::string result;
    if (length && buffer) {
        result = narrow(std::wstring_view(buffer, length));
        while (!result.empty() && (result.back() == '\r' || result.back() == '\n')) result.pop_back();
    }
    if (buffer) ::LocalFree(buffer);
    if (result.empty()) result = "error " + std::to_string(code);
    return result;
}

bool open_in_browser(const std::string& url) {
    const HINSTANCE result =
        ::ShellExecuteW(nullptr, L"open", wide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
}

bool set_clipboard_text(HWND owner, std::string_view text) {
    const std::wstring payload = wide(text);
    if (!::OpenClipboard(owner)) return false;
    ::EmptyClipboard();
    const size_t bytes = (payload.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) {
        ::CloseClipboard();
        return false;
    }
    if (void* target = ::GlobalLock(memory)) {
        std::memcpy(target, payload.c_str(), bytes);
        ::GlobalUnlock(memory);
        ::SetClipboardData(CF_UNICODETEXT, memory);
    }
    ::CloseClipboard();
    return true;
}

std::vector<std::string> parse_command_line(const std::wstring& command_line) {
    std::vector<std::string> arguments;
    int count = 0;
    LPWSTR* argv = ::CommandLineToArgvW(command_line.c_str(), &count);
    if (!argv) return arguments;
    for (int i = 1; i < count; ++i) arguments.push_back(narrow(argv[i]));
    ::LocalFree(argv);
    return arguments;
}

std::string environment_variable(const char* name) {
    if (!name || !*name) return {};
    const std::wstring wide_name = wide(name);
    const DWORD needed = ::GetEnvironmentVariableW(wide_name.c_str(), nullptr, 0);
    if (needed == 0) return {};
    std::wstring buffer(static_cast<size_t>(needed), L'\0');
    const DWORD written =
        ::GetEnvironmentVariableW(wide_name.c_str(), buffer.data(), static_cast<DWORD>(buffer.size()));
    if (written == 0 || written >= buffer.size()) return {};
    buffer.resize(written);
    return narrow(buffer);
}

} // namespace colai::win
