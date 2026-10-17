// Colaimdation - MCP server (JSON-RPC 2.0 over the loopback HTTP endpoint).
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/json.h"
#include "core/workspace.h"
#include "mcp/http_server.h"
#include "mcp/tool_registry.h"

namespace colai::mcp {

// ---------------------------------------------------------------------------
// JSON-RPC error codes
// ---------------------------------------------------------------------------
namespace rpc {
inline constexpr int kParseError = -32700;
inline constexpr int kInvalidRequest = -32600;
inline constexpr int kMethodNotFound = -32601;
inline constexpr int kInvalidParams = -32602;
inline constexpr int kInternalError = -32603;
} // namespace rpc

struct ServerStatus {
    bool listening = false;
    uint16_t port = 0;
    std::string base_url;
    std::string endpoint;      // full MCP endpoint URL including the token query
    std::string token;
    bool require_token = true;
    std::string discovery_path; // where the endpoint was written for Agents
    size_t tools = 0;
    size_t sessions = 0;
    int64_t requests = 0;
    int64_t tool_calls = 0;
    int64_t started_ms = 0;

    Json to_json() const;
};

class McpServer {
public:
    McpServer();
    ~McpServer();

    McpServer(const McpServer&) = delete;
    McpServer& operator=(const McpServer&) = delete;

    // Builds the tool registry and prepares the transport. `workspace` must
    // outlive the server.
    bool initialise(Workspace& workspace, uint16_t port, std::string* error = nullptr);
    void stop();

    const ServerStatus& status() const { return status_; }
    std::string discovery_path() const { return status_.discovery_path; }

    // Replaces the token used for authentication (and the one in the endpoint URL).
    // Used by the Agent panel's "new token" button and by pinning; it takes effect
    // on the next request, the transport itself is untouched.
    void set_token(std::string token);

    // Writes the endpoint, token and tool list to `path` so Agents can find this
    // instance. Called by initialise; call again to refresh.
    bool write_discovery_file(const std::string& path, std::string* error = nullptr);

    // Optional hook so the editor UI can reflect MCP activity.
    void set_activity_sink(std::function<void(const Json&)> sink) { activity_sink_ = std::move(sink); }

    // JSON-RPC entry point used by the HTTP endpoint and by the stdio front-end.
    Json handle(const Json& request, const std::string& transport = "http",
                const std::string& authorization = {});
    // Parses and handles a raw message; returns a response, or null for
    // notifications.
    Json handle_raw(const std::string& body, const std::string& transport = "http",
                    const std::string& authorization = {});

    ToolRegistry& tools() { return registry_; }

    // Fan-out to every attached SSE client.
    void add_sink(EventSink* sink);
    void remove_sink(EventSink* sink);

private:
    bool authorise(const HttpRequest& request, std::string* error) const;
    HttpResponse http_dispatch(const HttpRequest& request, EventSink* sink);
    Json mcp_method(const std::string& method, const Json& params, Session& session,
                    const std::string& transport);
    Json rpc_result(const Json& id, Json result) const;
    Json rpc_error(const Json& id, int code, std::string message, Json data = Json()) const;

    Session& session_for(const std::string& key, const Json& params);
    void broadcast(const Json& message);

    Workspace* workspace_ = nullptr;
    std::unique_ptr<HttpServer> http_;
    ToolRegistry registry_;
    ServerStatus status_;
    std::function<void(const Json&)> activity_sink_;

    mutable std::mutex sessions_mutex_;
    std::vector<std::pair<std::string, Session>> sessions_;

    mutable std::mutex sinks_mutex_;
    std::vector<EventSink*> sinks_;

    std::atomic<int64_t> requests_{0};
    std::atomic<int64_t> tool_calls_{0};
};

// Builds the JSON-RPC response envelope.
Json rpc_response(const Json& id, Json result);
Json rpc_failure(const Json& id, int code, std::string message, Json data = Json());

} // namespace colai::mcp
