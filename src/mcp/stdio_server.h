// Colaimdation - MCP over stdio.
//
// Some Agents can only launch a stdio MCP server rather than speak HTTP. This
// front-end does not own a document: it forwards every JSON-RPC message to the
// running editor's loopback endpoint, so a stdio client controls the same live
// document the writer has open. That is the whole point of the bridge.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

#include "core/json.h"

namespace colai::mcp {

struct StdioOptions {
    std::string host = "127.0.0.1";
    uint16_t port = 0;
    std::string token;
    // Path to the discovery file written by the editor (used when port is 0).
    std::string discovery_path;
    // Forward editor notifications to stderr so the Agent host can log them.
    bool forward_notifications = true;
    bool verbose = false;
};

class StdioServer {
public:
    StdioServer() = default;
    ~StdioServer();

    StdioServer(const StdioServer&) = delete;
    StdioServer& operator=(const StdioServer&) = delete;

    // Resolves the endpoint (from the discovery file if needed) and verifies it
    // answers a ping. Returns false with a message on `error`.
    bool connect_endpoint(StdioOptions options, std::string* error = nullptr);

    // Reads newline-delimited JSON-RPC messages from stdin until EOF.
    int run();

    void stop();

    const StdioOptions& options() const { return options_; }

private:
    void notification_loop();
    bool forward(const std::string& line);

    StdioOptions options_;
    std::string session_id_;
    std::atomic<bool> running_{false};
    std::thread notification_thread_;
    void* stop_event_ = nullptr; // HANDLE
};

} // namespace colai::mcp
