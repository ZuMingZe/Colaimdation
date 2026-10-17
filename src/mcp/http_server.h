// Colaimdation - minimal loopback HTTP server for the MCP endpoint.
//
// Only ever bound to 127.0.0.1, only ever serving the editor's own MCP
// transport. Winsock is used directly so the whole MCP stack has no
// third-party dependency.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/json.h"

namespace colai {

// ---------------------------------------------------------------------------
// SSE sink: a handler writes events to it while it runs.
// ---------------------------------------------------------------------------
class EventSink {
public:
    virtual ~EventSink() = default;
    // Sends one Server-Sent Event. Returns false when the peer went away.
    virtual bool send(std::string_view event, std::string_view data) = 0;
};

struct HttpRequest {
    std::string method;
    std::string target; // raw request target
    std::string path;   // decoded path
    std::string query;  // raw query string
    std::string body;
    std::string content_type;
    std::map<std::string, std::string> headers; // keys lowercased
    std::string peer_address;
    bool keep_alive = true;

    std::string header(std::string_view name) const;
    bool has_header(std::string_view name) const;
    // Query parameter lookup, URL-decoded. Returns `fallback` when absent.
    std::string query_param(std::string_view name, std::string fallback = {}) const;
    bool has_query_param(std::string_view name) const;
};

struct HttpResponse {
    int status = 200;
    std::string reason = "OK";
    std::string content_type = "application/json; charset=utf-8";
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;

    static HttpResponse json(std::string body, int status = 200);
    static HttpResponse text(std::string body, int status = 200,
                             std::string content_type = "text/plain; charset=utf-8");
    static HttpResponse error(int status, std::string message);
    static HttpResponse no_content();
};

// A handler that returns status 0 has taken over the connection: it must have
// written a complete stream through `sink` (used by the SSE endpoint).
using RequestHandler = std::function<HttpResponse(const HttpRequest&, EventSink*)>;

class HttpServer {
public:
    HttpServer();
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // Binds to 127.0.0.1. Pass 0 to let the OS choose the port.
    bool listen(uint16_t port, std::string* error = nullptr);
    void stop();
    bool running() const { return running_.load(); }

    uint16_t port() const { return port_; }
    std::string base_url() const;

    void set_handler(RequestHandler handler) { handler_ = std::move(handler); }
    void set_max_connections(size_t count) { max_connections_ = count; }
    size_t connection_count() const { return connections_.load(); }

private:
    void accept_loop();
    void serve_client(uintptr_t socket_handle);

    RequestHandler handler_;
    std::atomic<bool> running_{false};
    std::atomic<size_t> connections_{0};
    uintptr_t listen_socket_ = ~uintptr_t(0);
    uint16_t port_ = 0;
    size_t max_connections_ = 64;
    std::thread accept_thread_;

    // Tracks in-flight client threads so stop() can join them instead of
    // leaving them running against objects the shell is about to destroy.
    std::mutex clients_mutex_;
    std::vector<std::thread> client_threads_;
};

// ---------------------------------------------------------------------------
// HTTP client (used by the stdio bridge to reach the running editor).
// ---------------------------------------------------------------------------
struct HttpResponseRaw {
    bool ok = false;
    int status = 0;
    std::string body;
    std::string error;
};

// Blocking POST. `timeout_ms` covers the whole request. Only http://127.0.0.1
// endpoints are supported, by design.
HttpResponseRaw http_post(const std::string& host, uint16_t port, const std::string& path,
                          const std::string& body, const std::string& bearer_token,
                          std::string_view content_type = "application/json",
                          int timeout_ms = 60000);

// Initialises Winsock once per process. Safe to call from any thread.
bool http_global_init();

} // namespace colai
