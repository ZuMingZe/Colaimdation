// Colaimdation - Win32 helpers.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace colai::win {

// Registers the window classes and common controls. Safe to call repeatedly.
//
// `main_window_proc` is the procedure installed for the editor's own window
// class. It must not be null: the shell routes every menu command, WM_CLOSE and
// WM_SIZE through it, and a class registered with DefWindowProcW silently drops
// all of them. The dispatcher message window always uses DefWindowProcW because
// its messages are handled by WebviewBridge::handle_message.
bool register_window_classes(HINSTANCE instance, WNDPROC main_window_proc);

// UTF-8 -> UTF-16 shortcuts.
std::wstring wide(std::string_view text);
std::string narrow(std::wstring_view text);

// Native file dialogs (filtered to Markdown). Return an empty string when the
// user cancels. The title, filter labels and button captions come from `owner`
// so they follow the interface language.
std::string open_file_dialog(HWND owner, const std::string& initial_directory = {});
std::string save_file_dialog(HWND owner, const std::string& suggested_name);

// A Save dialog for one format only (PDF, Word, PNG), titled for the export being
// asked for, so it cannot offer a format the export would not write.
std::string save_file_dialog_as(HWND owner, const std::string& suggested_name,
                                const std::string& title, const std::string& extension);

void message_box(HWND owner, std::string_view title, std::string_view text, UINT flags = MB_OK);
bool confirm(HWND owner, std::string_view title, std::string_view text);

// %LOCALAPPDATA%\colaimdation, created on demand.
std::string app_data_directory();
// The directory holding the running executable.
std::string executable_directory();

// Sets the process DPI awareness to per-monitor v2 when available.
void enable_dpi_awareness();

// Formats a Windows error code as a readable string.
std::string format_error(DWORD code);

// Registry-free "open in browser" for the endpoint URL.
bool open_in_browser(const std::string& url);

// Copies UTF-8 text to the clipboard.
bool set_clipboard_text(HWND owner, std::string_view text);

// Splits a command line into arguments, honouring double quotes.
std::vector<std::string> parse_command_line(const std::wstring& command_line);

// Value of an environment variable as UTF-8, or an empty string when unset.
std::string environment_variable(const char* name);

} // namespace colai::win
