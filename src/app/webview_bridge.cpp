#include "app/webview_bridge.h"

#include "app/win_util.h"
#include "core/docx.h"
#include "core/i18n.h"
#include "core/json.h"
#include "core/url_util.h"
#include "core/utf8.h"
#include "mcp/tool_registry.h" // colai::mcp::kServerName

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <vector>

#include <objbase.h>
#include <wrl.h>

#ifdef COLAIMDATION_HAVE_WEBVIEW2
#include <WebView2.h>
#endif

using namespace Microsoft::WRL;

namespace colai {
namespace {

#ifdef COLAIMDATION_HAVE_WEBVIEW2

// Minimal COM callback helper.
//
// The WebView2 runtime keeps one reference (taken by add_* / the Create* call)
// and the caller holds one; the object deletes itself when both are gone. Each
// adapter forwards to the stored callable, so one implementation serves every
// handler interface. One class per interface keeps the Invoke signatures exact.
template <typename Invoker>
class ComHandler {
public:
    explicit ComHandler(Invoker invoker) : invoker_(std::move(invoker)) {}
    virtual ~ComHandler() = default;

    unsigned long add_ref() { return ++references_; }
    unsigned long release() {
        const unsigned long remaining = --references_;
        if (remaining == 0) delete this;
        return remaining;
    }

    Invoker& invoker() { return invoker_; }

private:
    std::atomic<unsigned long> references_{1};
    Invoker invoker_;
};

// Environment options carrying extra Chromium switches (used only for the
// opt-in diagnostics port). Same reference-counting pattern as the handlers.
class EnvironmentOptions : public ICoreWebView2EnvironmentOptions {
public:
    explicit EnvironmentOptions(std::wstring arguments) : arguments_(std::move(arguments)) {}

    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (remaining == 0) delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
        if (!object) return E_POINTER;
        if (riid == __uuidof(ICoreWebView2EnvironmentOptions) || riid == __uuidof(IUnknown)) {
            *object = static_cast<ICoreWebView2EnvironmentOptions*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE get_AdditionalBrowserArguments(LPWSTR* value) override {
        if (!value) return E_POINTER;
        *value = static_cast<LPWSTR>(::CoTaskMemAlloc((arguments_.size() + 1) * sizeof(wchar_t)));
        if (!*value) return E_OUTOFMEMORY;
        ::wcscpy_s(*value, arguments_.size() + 1, arguments_.c_str());
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE put_AdditionalBrowserArguments(LPCWSTR value) override {
        arguments_ = value ? value : L"";
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_Language(LPWSTR* value) override {
        if (value) *value = nullptr;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE put_Language(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE get_TargetCompatibleBrowserVersion(LPWSTR* value) override {
        if (value) *value = nullptr;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE put_TargetCompatibleBrowserVersion(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE get_AllowSingleSignOnUsingOSPrimaryAccount(BOOL* value) override {
        if (value) *value = FALSE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE put_AllowSingleSignOnUsingOSPrimaryAccount(BOOL) override {
        return S_OK;
    }

private:
    std::atomic<ULONG> references_{1};
    std::wstring arguments_;
};

class EnvironmentHandler
    : public ComHandler<std::function<HRESULT(HRESULT, ICoreWebView2Environment*)>>,
      public ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler {
public:
    using ComHandler::ComHandler;
    ULONG STDMETHODCALLTYPE AddRef() override { return add_ref(); }
    ULONG STDMETHODCALLTYPE Release() override { return release(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
        if (!object) return E_POINTER;
        if (riid == __uuidof(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler) ||
            riid == __uuidof(IUnknown)) {
            *object = static_cast<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT result,
                                     ICoreWebView2Environment* environment) override {
        return invoker()(result, environment);
    }
};

class ControllerHandler
    : public ComHandler<std::function<HRESULT(HRESULT, ICoreWebView2Controller*)>>,
      public ICoreWebView2CreateCoreWebView2ControllerCompletedHandler {
public:
    using ComHandler::ComHandler;
    ULONG STDMETHODCALLTYPE AddRef() override { return add_ref(); }
    ULONG STDMETHODCALLTYPE Release() override { return release(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
        if (!object) return E_POINTER;
        if (riid == __uuidof(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler) ||
            riid == __uuidof(IUnknown)) {
            *object = static_cast<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, ICoreWebView2Controller* controller) override {
        return invoker()(result, controller);
    }
};

class WebMessageHandler
    : public ComHandler<std::function<HRESULT(ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs*)>>,
      public ICoreWebView2WebMessageReceivedEventHandler {
public:
    using ComHandler::ComHandler;
    ULONG STDMETHODCALLTYPE AddRef() override { return add_ref(); }
    ULONG STDMETHODCALLTYPE Release() override { return release(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
        if (!object) return E_POINTER;
        if (riid == __uuidof(ICoreWebView2WebMessageReceivedEventHandler) ||
            riid == __uuidof(IUnknown)) {
            *object = static_cast<ICoreWebView2WebMessageReceivedEventHandler*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2* sender,
                                     ICoreWebView2WebMessageReceivedEventArgs* args) override {
        return invoker()(sender, args);
    }
};

class TitleChangedHandler : public ComHandler<std::function<HRESULT(ICoreWebView2*, IUnknown*)>>,
                            public ICoreWebView2DocumentTitleChangedEventHandler {
public:
    using ComHandler::ComHandler;
    ULONG STDMETHODCALLTYPE AddRef() override { return add_ref(); }
    ULONG STDMETHODCALLTYPE Release() override { return release(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
        if (!object) return E_POINTER;
        if (riid == __uuidof(ICoreWebView2DocumentTitleChangedEventHandler) ||
            riid == __uuidof(IUnknown)) {
            *object = static_cast<ICoreWebView2DocumentTitleChangedEventHandler*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2* sender, IUnknown* args) override {
        return invoker()(sender, args);
    }
};

class NavigationHandler
    : public ComHandler<
          std::function<HRESULT(ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*)>>,
      public ICoreWebView2NavigationCompletedEventHandler {
public:
    using ComHandler::ComHandler;
    ULONG STDMETHODCALLTYPE AddRef() override { return add_ref(); }
    ULONG STDMETHODCALLTYPE Release() override { return release(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
        if (!object) return E_POINTER;
        if (riid == __uuidof(ICoreWebView2NavigationCompletedEventHandler) ||
            riid == __uuidof(IUnknown)) {
            *object = static_cast<ICoreWebView2NavigationCompletedEventHandler*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2* sender,
                                     ICoreWebView2NavigationCompletedEventArgs* args) override {
        return invoker()(sender, args);
    }
};

// A link click would otherwise *replace the editor* with the web page: the page
// has no back button and the shell has no navigation UI, so the whole interface
// disappears with no way out. Every navigation that is not the editor's own
// virtual host is cancelled here and handed to the system browser instead.
class NavigationStartingHandler
    : public ComHandler<
          std::function<HRESULT(ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs*)>>,
      public ICoreWebView2NavigationStartingEventHandler {
public:
    using ComHandler::ComHandler;
    ULONG STDMETHODCALLTYPE AddRef() override { return add_ref(); }
    ULONG STDMETHODCALLTYPE Release() override { return release(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
        if (!object) return E_POINTER;
        if (riid == __uuidof(ICoreWebView2NavigationStartingEventHandler) ||
            riid == __uuidof(IUnknown)) {
            *object = static_cast<ICoreWebView2NavigationStartingEventHandler*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2* sender,
                                     ICoreWebView2NavigationStartingEventArgs* args) override {
        return invoker()(sender, args);
    }
};

// target="_blank" / window.open(): no second WebView2 window exists, so the URL
// goes to the system browser as well.
class NewWindowRequestedHandler
    : public ComHandler<
          std::function<HRESULT(ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs*)>>,
      public ICoreWebView2NewWindowRequestedEventHandler {
public:
    using ComHandler::ComHandler;
    ULONG STDMETHODCALLTYPE AddRef() override { return add_ref(); }
    ULONG STDMETHODCALLTYPE Release() override { return release(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
        if (!object) return E_POINTER;
        if (riid == __uuidof(ICoreWebView2NewWindowRequestedEventHandler) ||
            riid == __uuidof(IUnknown)) {
            *object = static_cast<ICoreWebView2NewWindowRequestedEventHandler*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2* sender,
                                     ICoreWebView2NewWindowRequestedEventArgs* args) override {
        return invoker()(sender, args);
    }
};

// True for the editor's own document, which is served over the loopback virtual
// host. Everything else is a link - see core/url_util.h for why only that one page
// counts: a relative link ("other.md") resolves onto the same host, and treating it
// as "the editor" navigated the window to a page that does not exist.
bool is_editor_url(const std::string& uri, const std::string& virtual_host) {
    return url::is_editor_page(uri, virtual_host);
}

// The page was asked for its paper layout (app.js exportPrepare): the script is
// asynchronous, so whatever needs the prepared layout runs from here.
class ScriptCompletedHandler
    : public ComHandler<std::function<HRESULT(HRESULT, const std::string&)>>,
      public ICoreWebView2ExecuteScriptCompletedHandler {
public:
    using ComHandler::ComHandler;
    ULONG STDMETHODCALLTYPE AddRef() override { return add_ref(); }
    ULONG STDMETHODCALLTYPE Release() override { return release(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
        if (!object) return E_POINTER;
        if (riid == __uuidof(ICoreWebView2ExecuteScriptCompletedHandler) ||
            riid == __uuidof(IUnknown)) {
            *object = static_cast<ICoreWebView2ExecuteScriptCompletedHandler*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT error_code, LPCWSTR result) override {
        return invoker()(error_code, result ? win::narrow(result) : std::string());
    }
};

// PrintToPdf is asynchronous as well; the export is only done when this fires.
class PrintToPdfHandler
    : public ComHandler<std::function<HRESULT(HRESULT, BOOL)>>,
      public ICoreWebView2PrintToPdfCompletedHandler {
public:
    using ComHandler::ComHandler;
    ULONG STDMETHODCALLTYPE AddRef() override { return add_ref(); }
    ULONG STDMETHODCALLTYPE Release() override { return release(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
        if (!object) return E_POINTER;
        if (riid == __uuidof(ICoreWebView2PrintToPdfCompletedHandler) ||
            riid == __uuidof(IUnknown)) {
            *object = static_cast<ICoreWebView2PrintToPdfCompletedHandler*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT error_code, BOOL result) override {
        return invoker()(error_code, result);
    }
};

// The picture export captures the rendered surface, which is asynchronous too.
class CapturePreviewHandler
    : public ComHandler<std::function<HRESULT(HRESULT)>>,
      public ICoreWebView2CapturePreviewCompletedHandler {
public:
    using ComHandler::ComHandler;
    ULONG STDMETHODCALLTYPE AddRef() override { return add_ref(); }
    ULONG STDMETHODCALLTYPE Release() override { return release(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
        if (!object) return E_POINTER;
        if (riid == __uuidof(ICoreWebView2CapturePreviewCompletedHandler) ||
            riid == __uuidof(IUnknown)) {
            *object = static_cast<ICoreWebView2CapturePreviewCompletedHandler*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT error_code) override {
        return invoker()(error_code);
    }
};

// Reads the memory stream CapturePreview wrote into and saves it. Returns false when
// the bytes could not be written.
bool write_stream_to_file(IStream* stream, const std::string& path) {
    if (!stream) return false;
    STATSTG status{};
    if (FAILED(stream->Stat(&status, STATFLAG_NONAME))) return false;
    const ULONGLONG size = status.cbSize.QuadPart;
    if (size == 0) return false;

    HGLOBAL memory = nullptr;
    if (FAILED(::GetHGlobalFromStream(stream, &memory)) || !memory) return false;
    const void* bytes = ::GlobalLock(memory);
    if (!bytes) return false;

    std::ofstream file(win::wide(path).c_str(), std::ios::binary | std::ios::trunc);
    bool ok = false;
    if (file) {
        file.write(static_cast<const char*>(bytes), static_cast<std::streamsize>(size));
        ok = file.good();
    }
    ::GlobalUnlock(memory);
    return ok;
}


#endif // COLAIMDATION_HAVE_WEBVIEW2

} // namespace

// ---------------------------------------------------------------------------
// Implementation state (WebView2 objects live here so the header stays free of
// the SDK).
// ---------------------------------------------------------------------------
struct WebviewBridge::Impl {
#ifdef COLAIMDATION_HAVE_WEBVIEW2
    ComPtr<ICoreWebView2Environment> environment;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webview;
#endif
    std::string virtual_host = "colaimdation.local";
    // The URL of the navigation that is in flight, and how often the editor page has
    // been put back after a failed one (so a broken install cannot spin here).
    std::string last_navigation_uri;
    int recovery_attempts = 0;
    // The bounds the window last asked for, so a picture export can put the surface
    // back the way the writer had it.
    RECT last_bounds{};
    bool have_bounds = false;
    bool sdk_available = false;
    bool failed = false;
};

WebviewBridge::WebviewBridge() : impl_(std::make_unique<Impl>()) {}

WebviewBridge::~WebviewBridge() {
#ifdef COLAIMDATION_HAVE_WEBVIEW2
    if (impl_) {
        if (impl_->controller) impl_->controller->Close();
        impl_->controller.Reset();
        impl_->webview.Reset();
        impl_->environment.Reset();
    }
#endif
    if (dispatcher_) {
        ::DestroyWindow(dispatcher_);
        dispatcher_ = nullptr;
    }
}

bool WebviewBridge::initialise(HWND parent, HINSTANCE instance, const std::string& asset_directory,
                               const Json& bootstrap, std::string* error) {
    parent_ = parent;
    instance_ = instance;
    asset_directory_ = asset_directory;
    bootstrap_ = bootstrap;
    ui_thread_id_ = ::GetCurrentThreadId();

    // Hidden window used to marshal work onto the UI thread from MCP threads.
    dispatcher_ = ::CreateWindowExW(0, L"ColaimdationDispatcher", L"Colaimdation dispatcher", 0, 0,
                                    0, 0, 0, HWND_MESSAGE, nullptr, instance, this);
    if (!dispatcher_) {
        if (error) *error = "cannot create the bridge message window: " + win::format_error(::GetLastError());
        return false;
    }

    std::string webview_error;
    create_webview(&webview_error);
    if (!webview_error.empty()) last_error_ = webview_error;
    return true;
}

// ---------------------------------------------------------------------------
// WebView2 creation
// ---------------------------------------------------------------------------
void WebviewBridge::create_webview(std::string* error) {
#ifdef COLAIMDATION_HAVE_WEBVIEW2
    impl_->sdk_available = true;
    const std::string user_data = win::app_data_directory() + "\\WebView2";
    ::CreateDirectoryW(win::wide(user_data).c_str(), nullptr);

    auto* handler = new EnvironmentHandler(
        [this](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
            if (FAILED(result) || !environment) {
                impl_->failed = true;
                last_error_ = "WebView2 environment creation failed (0x" +
                              [&] {
                                  char buffer[16];
                                  std::snprintf(buffer, sizeof(buffer), "%08lX",
                                                static_cast<unsigned long>(result));
                                  return std::string(buffer);
                              }() +
                              "). Install the Microsoft Edge WebView2 Runtime.";
                // Tell the front-end (which will not load) via the window title.
                ::SetWindowTextW(parent_, L"Colaimdation - WebView2 runtime missing");
                return S_OK;
            }
            impl_->environment = environment;
            // Serve web/ over a loopback virtual host so the page has a normal
            // https origin (needed for localStorage and relative asset paths).
            auto* controller_handler = new ControllerHandler(
                [this](HRESULT controller_result, ICoreWebView2Controller* controller) -> HRESULT {
                        if (FAILED(controller_result) || !controller) {
                            impl_->failed = true;
                            last_error_ = "WebView2 controller creation failed";
                            return S_OK;
                        }
                        impl_->controller = controller;
                        controller->get_CoreWebView2(&impl_->webview);
                        if (!impl_->webview) {
                            impl_->failed = true;
                            last_error_ = "WebView2 core interface unavailable";
                            return S_OK;
                        }

                        RECT bounds{};
                        ::GetClientRect(parent_, &bounds);
                        impl_->controller->put_Bounds(bounds);
                        impl_->controller->put_IsVisible(TRUE);

                        // Bridge: page -> host. The page posts JavaScript
                        // *objects* (host.postMessage({type:...})), and for those
                        // TryGetWebMessageAsString fails with E_INVALIDARG - it
                        // only ever succeeds for postMessage('a string'). Using it
                        // silently dropped every message the front-end sent
                        // (ready, edit, command, status_query...), which is why
                        // the page never bootstrapped and typing never reached the
                        // document. get_WebMessageAsJson works for both shapes.
                        auto* message_handler = new WebMessageHandler(
                            [this](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args)
                                -> HRESULT {
                                    LPWSTR raw = nullptr;
                                    if (FAILED(args->get_WebMessageAsJson(&raw)) || !raw) {
                                        last_error_ = "the front-end sent a message that could not "
                                                      "be read as JSON";
                                        return S_OK;
                                    }
                                    const std::string utf8 = win::narrow(raw);
                                    ::CoTaskMemFree(raw);
                                    JsonParseError parse_error;
                                    auto parsed = json_parse(utf8, &parse_error);
                                    if (parsed) {
                                        on_web_message(*parsed);
                                    } else {
                                        last_error_ = "bad message from front-end: " +
                                                      parse_error.message;
                                    }
                                    return S_OK;
                                });
                        impl_->webview->add_WebMessageReceived(message_handler, nullptr);
                        message_handler->Release();

                        // Keep the window title in sync with the document name.
                        // The page's own <title> is the constant "Colaimdation"
                        // (index.html never changes it), so taking it verbatim
                        // erased the file name and dirty marker that
                        // EditorWindow::update_window_title() maintains.
                        auto* title_handler = new TitleChangedHandler(
                            [this](ICoreWebView2* sender, IUnknown*) -> HRESULT {
                                    LPWSTR title = nullptr;
                                    if (SUCCEEDED(sender->get_DocumentTitle(&title)) && title) {
                                        const std::string utf8 = win::narrow(title);
                                        ::CoTaskMemFree(title);
                                        if (utf8.empty() || utf8 == mcp::kServerName) {
                                            // Not a document title: leave the
                                            // shell's title alone.
                                            return S_OK;
                                        }
                                        const std::string window_title =
                                            std::string(mcp::kServerName) + " - " + utf8;
                                        ::SetWindowTextW(parent_, win::wide(window_title).c_str());
                                    }
                                    return S_OK;
                                });
                        impl_->webview->add_DocumentTitleChanged(title_handler, nullptr);
                        title_handler->Release();

                        auto* navigation_handler = new NavigationHandler(
                            [this](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args)
                                -> HRESULT {
                                    BOOL success = FALSE;
                                    args->get_IsSuccess(&success);
                                    if (success) {
                                        impl_->recovery_attempts = 0;
                                        return S_OK;
                                    }
                                    COREWEBVIEW2_WEB_ERROR_STATUS status =
                                        COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
                                    args->get_WebErrorStatus(&status);
                                    // A cancelled navigation (a link we refused, for one)
                                    // reports itself as a failure; that is not an error to
                                    // recover from.
                                    if (status == COREWEBVIEW2_WEB_ERROR_STATUS_OPERATION_CANCELED) {
                                        return S_OK;
                                    }
                                    last_error_ =
                                        "the editor front-end failed to load (status " +
                                        std::to_string(static_cast<int>(status)) + ")";
                                    // This window has no back button, so a navigation that
                                    // ended on the browser's error page would leave the
                                    // writer without an interface at all. Put the editor
                                    // back - unless the editor page itself is what failed
                                    // (which would only fail again), and only twice.
                                    const std::string page = url::editor_page(impl_->virtual_host);
                                    if (impl_->last_navigation_uri != page &&
                                        impl_->recovery_attempts < 2) {
                                        ++impl_->recovery_attempts;
                                        impl_->webview->Navigate(win::wide(page).c_str());
                                    }
                                    return S_OK;
                                });
                        impl_->webview->add_NavigationCompleted(navigation_handler, nullptr);
                        navigation_handler->Release();

                        // The editor must not be able to navigate away from itself.
                        // A link in the document used to load in place, which hides
                        // the entire UI (toolbar, tabs, panels) and cannot be undone
                        // from inside the app; send it to the default browser. Only the
                        // editor's own page may load here: a link that resolves onto the
                        // virtual host (a relative "other.md") left the window showing
                        // the browser's "file not found" page.
                        auto* starting_handler = new NavigationStartingHandler(
                            [this](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args)
                                -> HRESULT {
                                    LPWSTR raw = nullptr;
                                    if (FAILED(args->get_Uri(&raw)) || !raw) return S_OK;
                                    const std::string uri = win::narrow(raw);
                                    ::CoTaskMemFree(raw);
                                    impl_->last_navigation_uri = uri;
                                    if (is_editor_url(uri, impl_->virtual_host)) return S_OK;
                                    args->put_Cancel(TRUE);
                                    // URLs on our own virtual host exist only inside this
                                    // window, so there is nothing to hand to a browser.
                                    if (!url::is_editor_host(uri, impl_->virtual_host) &&
                                        !url::is_script_url(uri) && url::opens_in_browser(uri)) {
                                        win::open_in_browser(uri);
                                    }
                                    return S_OK;
                                });
                        impl_->webview->add_NavigationStarting(starting_handler, nullptr);
                        starting_handler->Release();

                        auto* new_window_handler = new NewWindowRequestedHandler(
                            [this](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args)
                                -> HRESULT {
                                    LPWSTR raw = nullptr;
                                    if (SUCCEEDED(args->get_Uri(&raw)) && raw) {
                                        const std::string uri = win::narrow(raw);
                                        ::CoTaskMemFree(raw);
                                        args->put_Handled(TRUE);
                                        // A new window is always a link: it goes to the
                                        // default browser, never into another WebView2
                                        // window of our own.
                                        if (!url::is_editor_host(uri, impl_->virtual_host) &&
                                            !url::is_script_url(uri) &&
                                            url::opens_in_browser(uri)) {
                                            win::open_in_browser(uri);
                                        }
                                    } else {
                                        args->put_Handled(TRUE);
                                    }
                                    return S_OK;
                                });
                        impl_->webview->add_NewWindowRequested(new_window_handler, nullptr);
                        new_window_handler->Release();

                        // Virtual host mapping needs the ICoreWebView2_3 interface.
                        ComPtr<ICoreWebView2_3> webview3;
                        if (SUCCEEDED(impl_->webview.As(&webview3)) && webview3) {
                            webview3->SetVirtualHostNameToFolderMapping(
                                win::wide(impl_->virtual_host).c_str(),
                                win::wide(asset_directory_).c_str(),
                                COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
                        } else {
                            last_error_ =
                                "this WebView2 runtime does not expose "
                                "SetVirtualHostNameToFolderMapping (needs 1.0.1020+); the front-end "
                                "assets cannot be served";
                        }

                        webview_ready_ = true;

                        const std::string url = url::editor_page(impl_->virtual_host);
                        impl_->webview->Navigate(win::wide(url).c_str());
                        return S_OK;
                    });
            environment->CreateCoreWebView2Controller(parent_, controller_handler);
            controller_handler->Release();
            return S_OK;
        });
    const std::wstring data_folder = win::wide(user_data);
    const HRESULT result = ::CreateCoreWebView2EnvironmentWithOptions(
        nullptr, data_folder.c_str(), nullptr, handler);
    handler->Release();
    if (FAILED(result)) {
        impl_->failed = true;
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "0x%08lX", static_cast<unsigned long>(result));
        if (error) {
            *error = std::string("CreateCoreWebView2EnvironmentWithOptions failed (") + buffer +
                     "). Install the Microsoft Edge WebView2 Runtime, or run with --no-webview.";
        }
        impl_->sdk_available = false;
    }
#else
    (void)error;
    impl_->sdk_available = false;
    last_error_ =
        "this build was compiled without the WebView2 SDK, so the editor front-end cannot load. "
        "Run tools/fetch_webview2.ps1 and rebuild.";
#endif
}

// ---------------------------------------------------------------------------
// IEditorBridge
// ---------------------------------------------------------------------------
std::optional<Json> WebviewBridge::post(std::function<Json()> job, int timeout_ms) {
    if (!job) return Json();
    // Already on the UI thread: run inline to avoid a self-deadlock.
    if (parent_ && ::GetCurrentThreadId() == ui_thread_id_) {
        return job();
    }
    if (!parent_) return std::nullopt;

    auto state = std::make_shared<Job>();
    state->body = std::move(job);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        jobs_[next_job_] = state;
        const uint64_t id = next_job_++;
        // The *editor* window is the target: EditorWindow::handle_message answers
        // kMsgRunJob by calling WebviewBridge::handle_message. Posting these to
        // the hidden dispatcher window (whose procedure is DefWindowProcW) threw
        // every job away, so each MCP write timed out with "editor_unavailable".
        ::PostMessageW(parent_, kMsgRunJob, static_cast<WPARAM>(id), 0);
    }

    std::unique_lock<std::mutex> lock(mutex_);
    const bool finished = condition_.wait_for(
        lock, std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 15000),
        [&] { return state->done; });
    if (!finished) {
        // Remove our slot so a late completion does not touch freed state.
        for (auto it = jobs_.begin(); it != jobs_.end(); ++it) {
            if (it->second == state) {
                jobs_.erase(it);
                break;
            }
        }
        return std::nullopt;
    }
    return state->result;
}

void WebviewBridge::notify(const Json& message) {
    if (!parent_) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_notifications_.push_back(message);
    }
    // Same reasoning as post(): only the editor window's procedure routes
    // kMsgNotify to deliver_notifications(). Sending it to the dispatcher window
    // meant the page never heard about document_loaded / document_saved, so an
    // opened file never appeared in the editor.
    ::PostMessageW(parent_, kMsgNotify, 0, 0);
}

bool WebviewBridge::request_confirmation(const Json& pending, int64_t timeout_ms) {
    const std::string id = pending.str("id");
    auto state = std::make_shared<ConfirmationWait>();
    {
        std::lock_guard<std::mutex> lock(confirmation_mutex_);
        confirmations_[id] = state;
    }
    // Ask the window to show the diff panel; the decision comes back through
    // resolve_confirmation() on the UI thread.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_notifications_.push_back([&] {
            Json message = Json::object();
            message.set("type", Json("confirmation_required"));
            message.set("pending", pending);
            return message;
        }());
    }
    if (!parent_) {
        // No UI to ask: never leave the tool waiting for an answer that cannot
        // arrive.
        resolve_confirmation(id, false, "no editor UI to confirm the change");
        return false;
    }
    ::PostMessageW(parent_, kMsgNotify, 0, 0);

    std::unique_lock<std::mutex> lock(confirmation_mutex_);
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 300000);
    confirmation_condition_.wait_until(lock, deadline, [&] { return state->decided; });
    const bool decided = state->decided;
    const bool accepted = state->accepted;
    confirmations_.erase(id);
    return decided && accepted;
}

void WebviewBridge::resolve_confirmation(const std::string& pending_id, bool accepted,
                                         const std::string& reason) {
    std::lock_guard<std::mutex> lock(confirmation_mutex_);
    const auto found = confirmations_.find(pending_id);
    if (found == confirmations_.end()) return;
    found->second->decided = true;
    found->second->accepted = accepted;
    found->second->reason = reason;
    confirmation_condition_.notify_all();
}

void WebviewBridge::reload_document(const Document& document) {
    Json message = Json::object();
    message.set("type", Json("document_loaded"));
    message.set("document", document.to_json(true));
    message.set("structure", document.structure_json(0));
    message.set("outline", document.outline_json());
    notify(message);
}

bool WebviewBridge::has_ui() const { return true; }

void WebviewBridge::open_devtools() {
#ifdef COLAIMDATION_HAVE_WEBVIEW2
    if (impl_ && impl_->webview) impl_->webview->OpenDevToolsWindow();
#endif
}

void WebviewBridge::restore_interface() {
#ifdef COLAIMDATION_HAVE_WEBVIEW2
    if (!impl_ || !impl_->webview) return;
    // Navigate, not Reload: reloading an error page just shows it again.
    impl_->recovery_attempts = 0;
    frontend_ready_ = false;
    const std::string page = url::editor_page(impl_->virtual_host);
    impl_->last_navigation_uri = page;
    impl_->webview->Navigate(win::wide(page).c_str());
#endif
}

// ---------------------------------------------------------------------------
// Printing and exporting
// ---------------------------------------------------------------------------

void WebviewBridge::print_document() {
#ifdef COLAIMDATION_HAVE_WEBVIEW2
    if (!impl_ || !impl_->webview) return;
    // Prepare the paper layout first (that script is asynchronous), then show the
    // engine's own print dialog. window.print() is only a fallback: in this runtime it
    // returned without a dialog, which is exactly why printing looked wrong - the page
    // printed as it was on screen, chrome and all.
    //
    // The dialog is asynchronous, so the editor is put back by the page's `afterprint`
    // listener; the fallback below only exists for an engine that never fires it.
    auto* prepared = new ScriptCompletedHandler([this](HRESULT, const std::string&) -> HRESULT {
        bool shown = false;
        ComPtr<ICoreWebView2_16> webview16;
        if (SUCCEEDED(impl_->webview.As(&webview16)) && webview16) {
            shown = SUCCEEDED(webview16->ShowPrintUI(COREWEBVIEW2_PRINT_DIALOG_KIND_SYSTEM));
        }
        if (!shown) {
            impl_->webview->ExecuteScript(L"window.colaimdation.printDocument();", nullptr);
        }
        return S_OK;
    });
    impl_->webview->ExecuteScript(L"window.colaimdation.exportPrepare('print');", prepared);
    prepared->Release();
#endif
}

void WebviewBridge::export_pdf(const std::string& path) {
#ifdef COLAIMDATION_HAVE_WEBVIEW2
    if (!impl_ || !impl_->webview) return;
    ComPtr<ICoreWebView2_7> webview7;
    if (FAILED(impl_->webview.As(&webview7)) || !webview7) {
        win::message_box(parent_, i18n::tr("export.title"), i18n::tr("export.unsupported"),
                         MB_ICONWARNING);
        return;
    }
    const std::string target = path;
    // Prepare first: the caret's block shows Markdown while writing, the chrome would
    // be printed along with the text, and the editor column scrolls. The script is
    // asynchronous, so the print starts from its completion handler.
    auto* prepared = new ScriptCompletedHandler(
        [this, webview7, target](HRESULT, const std::string&) -> HRESULT {
            auto* printed = new PrintToPdfHandler(
                [this, target](HRESULT, BOOL ok) -> HRESULT {
                    // Put the editor back whichever way the export went.
                    if (impl_ && impl_->webview) {
                        impl_->webview->ExecuteScript(L"window.colaimdation.exportFinish();",
                                                      nullptr);
                    }
                    win::message_box(parent_, i18n::tr("export.title"),
                                     ok ? i18n::tr("export.pdf.done", {target})
                                        : i18n::tr("export.failed", {target}),
                                     ok ? MB_ICONINFORMATION : MB_ICONERROR);
                    return S_OK;
                });
            webview7->PrintToPdf(win::wide(target).c_str(), nullptr, printed);
            printed->Release();
            return S_OK;
        });
    impl_->webview->ExecuteScript(L"window.colaimdation.exportPrepare('pdf');", prepared);
    prepared->Release();
#else
    (void)path;
#endif
}

void WebviewBridge::export_image(const std::string& path) {
#ifdef COLAIMDATION_HAVE_WEBVIEW2
    if (!impl_ || !impl_->webview || !impl_->controller) return;
    const std::string target = path;
    // The page reports the size the document needs once the chrome is out of the way;
    // the surface is then grown to that height (the window clips it, the engine still
    // renders all of it) and the whole thing is captured as one picture.
    auto* prepared = new ScriptCompletedHandler(
        [this, target](HRESULT, const std::string& result) -> HRESULT {
            const std::optional<Json> parsed = json_parse(result);
            const Json size = parsed ? *parsed : Json::object();
            const int width = static_cast<int>(size.integer("width", 900));
            const int height = static_cast<int>(size.integer("height", 1200));
            if (width <= 0 || height <= 0) {
                win::message_box(parent_, i18n::tr("export.title"),
                                 i18n::tr("export.failed", {target}), MB_ICONERROR);
                return S_OK;
            }
            // A capture has to fit in memory on the engine's side; cap the height and
            // let the picture be that tall rather than fail outright.
            constexpr int kMaxHeight = 16000;
            const int capture_height = height > kMaxHeight ? kMaxHeight : height;
            RECT grown{};
            grown.left = 0;
            grown.top = 0;
            grown.right = width;
            grown.bottom = capture_height;
            impl_->controller->put_Bounds(grown);
            // The resize has to be laid out and painted before the capture.
            ::Sleep(180);

            ComPtr<IStream> stream;
            if (FAILED(::CreateStreamOnHGlobal(nullptr, TRUE, &stream))) return S_OK;
            auto* captured = new CapturePreviewHandler(
                [this, target, stream](HRESULT status) -> HRESULT {
                    const bool saved = SUCCEEDED(status) && write_stream_to_file(stream.Get(), target);
                    // Put the surface back before the editor may repaint into it.
                    if (impl_ && impl_->controller) {
                        RECT restore{};
                        if (impl_->have_bounds) restore = impl_->last_bounds;
                        impl_->controller->put_Bounds(restore);
                    }
                    if (impl_ && impl_->webview) {
                        impl_->webview->ExecuteScript(L"window.colaimdation.exportFinish();",
                                                      nullptr);
                    }
                    win::message_box(parent_, i18n::tr("export.title"),
                                     saved ? i18n::tr("export.image.done", {target})
                                           : i18n::tr("export.failed", {target}),
                                     saved ? MB_ICONINFORMATION : MB_ICONERROR);
                    return S_OK;
                });
            impl_->webview->CapturePreview(
                COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG, stream.Get(), captured);
            captured->Release();
            return S_OK;
        });
    // No JSON.stringify: ExecuteScript already reports the result as JSON, and
    // stringifying it would arrive as a quoted string rather than an object.
    impl_->webview->ExecuteScript(L"window.colaimdation.exportPrepare('image')", prepared);
    prepared->Release();
#else
    (void)path;
#endif
}

// ---------------------------------------------------------------------------
// UI-thread work
// ---------------------------------------------------------------------------
void WebviewBridge::run_jobs() {
    std::vector<std::shared_ptr<Job>> ready;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ready.reserve(jobs_.size());
        for (auto& entry : jobs_) ready.push_back(entry.second);
        jobs_.clear();
    }
    for (const auto& job : ready) {
        try {
            job->result = job->body ? job->body() : Json();
        } catch (const std::exception& error) {
            job->result = Json::object();
            job->result.set("error", Json(error.what()));
        } catch (...) {
            job->result = Json::object();
            job->result.set("error", Json("unknown error while running a job on the UI thread"));
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            job->done = true;
        }
        condition_.notify_all();
    }
}

void WebviewBridge::deliver_notifications() {
    std::vector<Json> batch;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        batch.swap(pending_notifications_);
    }
    if (batch.empty()) return;
#ifdef COLAIMDATION_HAVE_WEBVIEW2
    if (impl_ && impl_->webview) {
        for (const Json& message : batch) {
            if (message.str("type") == "confirmation_required") {
                show_confirmation(message["pending"]);
                continue;
            }
            if (message.str("type") == "command") {
                send_command(message.str("command"), message);
                continue;
            }
            impl_->webview->PostWebMessageAsJson(win::wide(message.dump(false)).c_str());
        }
    }
#else
    (void)batch;
#endif
}

void WebviewBridge::show_confirmation(const Json& pending) {
    Json message = Json::object();
    message.set("type", Json("confirmation_required"));
    message.set("pending", pending);
#ifdef COLAIMDATION_HAVE_WEBVIEW2
    if (impl_ && impl_->webview) {
        impl_->webview->PostWebMessageAsJson(win::wide(message.dump(false)).c_str());
        // Bring the window forward: the writer has to make a decision.
        if (::IsIconic(parent_)) ::ShowWindow(parent_, SW_RESTORE);
        ::SetForegroundWindow(parent_);
        return;
    }
#endif
    // No UI available: never silently apply an unconfirmed change.
    resolve_confirmation(pending.str("id"), false, "no editor UI to confirm the change");
}

bool WebviewBridge::handle_message(HWND, UINT message, WPARAM wparam, LPARAM, LRESULT& result) {
    switch (message) {
    case kMsgRunJob: {
        const uint64_t id = static_cast<uint64_t>(wparam);
        std::shared_ptr<Job> job;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto found = jobs_.find(id);
            if (found != jobs_.end()) {
                job = found->second;
                jobs_.erase(found);
            }
        }
        if (!job) {
            result = 0;
            return true;
        }
        Json value;
        try {
            value = job->body ? job->body() : Json();
        } catch (const std::exception& error) {
            value = Json::object();
            value.set("error", Json(error.what()));
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            job->result = std::move(value);
            job->done = true;
        }
        condition_.notify_all();
        result = 0;
        return true;
    }
    case kMsgNotify:
        deliver_notifications();
        result = 0;
        return true;
    default: return false;
    }
}

// ---------------------------------------------------------------------------
// editor -> C++
// ---------------------------------------------------------------------------
void WebviewBridge::on_web_message(const Json& message) {
    const std::string type = message.str("type");
    if (type == "ready") {
        frontend_ready_ = true;
        Json bootstrap = bootstrap_;
        bootstrap.set("type", Json("bootstrap"));
        send_command("bootstrap", bootstrap);
        // The page is listening now: hand it the document it is supposed to show.
        if (ready_sink_) ready_sink_();
        return;
    }
    if (type == "command") {
        if (command_sink_) command_sink_(message.str("command"));
        return;
    }
    if (type == "error") {
        last_error_ = message.str("message");
        return;
    }
    // Anything else is a panel query (status, audit, ...) answered by the shell.
    if (panel_query_sink_) {
        const Json reply = panel_query_sink_(message);
        if (!reply.is_null()) {
#ifdef COLAIMDATION_HAVE_WEBVIEW2
            if (impl_ && impl_->webview) {
                impl_->webview->PostWebMessageAsJson(win::wide(reply.dump(false)).c_str());
            }
#endif
        }
    }
}

void WebviewBridge::execute_script(const std::string& script) {
#ifdef COLAIMDATION_HAVE_WEBVIEW2
    if (impl_ && impl_->webview) {
        impl_->webview->ExecuteScript(win::wide(script).c_str(), nullptr);
    }
#else
    (void)script;
#endif
}

void WebviewBridge::put_bounds(int width, int height) {
#ifdef COLAIMDATION_HAVE_WEBVIEW2
    if (impl_ && impl_->controller) {
        RECT bounds{};
        bounds.right = width;
        bounds.bottom = height;
        impl_->controller->put_Bounds(bounds);
        impl_->last_bounds = bounds;
        impl_->have_bounds = true;
    }
#else
    (void)width;
    (void)height;
#endif
}

void WebviewBridge::send_command(const std::string& command, Json extra) {
    Json message = std::move(extra);
    if (!message.is_object()) message = Json::object();
    message.set("type", Json("command"));
    message.set("command", Json(command));
#ifdef COLAIMDATION_HAVE_WEBVIEW2
    if (impl_ && impl_->webview) {
        impl_->webview->PostWebMessageAsJson(win::wide(message.dump(false)).c_str());
    }
#else
    (void)message;
#endif
}

} // namespace colai
