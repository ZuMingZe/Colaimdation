#include "mcp/stdio_server.h"

#include "core/utf8.h"
#include "mcp/http_server.h"
#include "mcp/mcp_server.h"

#include <cstdio>
#include <fstream>
#include <sstream>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

namespace colai::mcp {
namespace {

std::string read_file(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return {};
    std::stringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

// Writes a whole line to stdout and flushes: MCP hosts read line by line and
// will hang if we buffer.
void write_stdout_line(const std::string& line) {
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

void write_stderr_line(const std::string& line) {
    std::fwrite(line.data(), 1, line.size(), stderr);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

// Minimal SSE client: connects to /notifications and parses `event:`/`data:`
// frames, forwarding `message` events to stderr.
class SseClient {
public:
    bool open(const StdioOptions& options, std::string* error) {
        if (!http_global_init()) {
            if (error) *error = "Winsock failed to initialise";
            return false;
        }
        socket_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket_ == INVALID_SOCKET) {
            if (error) *error = "socket() failed";
            return false;
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = ::htons(options.port);
        ::inet_pton(AF_INET, options.host.c_str(), &address.sin_addr);
        if (::connect(socket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) ==
            SOCKET_ERROR) {
            if (error) *error = "cannot connect the notification stream";
            ::closesocket(socket_);
            socket_ = INVALID_SOCKET;
            return false;
        }
        std::string request;
        request += "GET /notifications HTTP/1.1\r\n";
        request += "Host: " + options.host + ":" + std::to_string(options.port) + "\r\n";
        request += "Accept: text/event-stream\r\n";
        if (!options.token.empty()) request += "Authorization: Bearer " + options.token + "\r\n";
        request += "Connection: close\r\n\r\n";
        if (::send(socket_, request.data(), static_cast<int>(request.size()), 0) <= 0) {
            if (error) *error = "cannot start the notification stream";
            ::closesocket(socket_);
            socket_ = INVALID_SOCKET;
            return false;
        }
        return true;
    }

    // Blocks until an event arrives, the timeout expires, or the peer closes.
    // Returns false when the stream ended.
    bool next_event(std::string& event, std::string& data, int timeout_ms) {
        if (socket_ == INVALID_SOCKET) return false;
        const DWORD timeout = static_cast<DWORD>(timeout_ms);
        ::setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
                     sizeof(timeout));
        for (;;) {
            std::string frame;
            // Read until a blank line, which terminates an SSE frame.
            for (;;) {
                const size_t separator = buffer_.find("\n\n");
                if (separator != std::string::npos) {
                    frame = buffer_.substr(0, separator);
                    buffer_.erase(0, separator + 2);
                    break;
                }
                char chunk[4096];
                const int read = ::recv(socket_, chunk, sizeof(chunk), 0);
                if (read <= 0) {
                    const int code = ::WSAGetLastError();
                    if (code == WSAETIMEDOUT) {
                        if (!frame.empty()) break;
                        return true; // keep the connection, nothing pending
                    }
                    return false;
                }
                buffer_.append(chunk, static_cast<size_t>(read));
                if (buffer_.size() > 4u * 1024 * 1024) buffer_.clear();
            }
            event.clear();
            data.clear();
            size_t position = 0;
            while (position < frame.size()) {
                size_t end = frame.find('\n', position);
                if (end == std::string::npos) end = frame.size();
                std::string line = frame.substr(position, end - position);
                position = end + 1;
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty() || line[0] == ':') continue;
                const size_t colon = line.find(':');
                const std::string field = colon == std::string::npos ? line : line.substr(0, colon);
                std::string value = colon == std::string::npos ? std::string{} : line.substr(colon + 1);
                if (!value.empty() && value[0] == ' ') value.erase(0, 1);
                if (field == "event") event = value;
                else if (field == "data") data += value;
            }
            if (!event.empty() || !data.empty()) return true;
        }
    }

    void close() {
        if (socket_ != INVALID_SOCKET) {
            ::closesocket(socket_);
            socket_ = INVALID_SOCKET;
        }
    }

private:
    SOCKET socket_ = INVALID_SOCKET;
    std::string buffer_;
};

} // namespace

StdioServer::~StdioServer() { stop(); }

bool StdioServer::connect_endpoint(StdioOptions options, std::string* error) {
    options_ = std::move(options);

    if (options_.port == 0) {
        if (options_.discovery_path.empty()) {
            // Default location, matching what the editor writes on startup.
            const char* local_app_data = std::getenv("LOCALAPPDATA");
            options_.discovery_path =
                std::string(local_app_data ? local_app_data : ".") + "\\colaimdation\\mcp.json";
        }
        std::string contents = read_file(options_.discovery_path);
        if (contents.empty()) {
            if (error) {
                *error = "no MCP discovery file at '" + options_.discovery_path +
                         "'; start the Colaimdation editor first, or pass --port and --token";
            }
            return false;
        }
        JsonParseError parse_error;
        auto parsed = json_parse(contents, &parse_error);
        if (!parsed) {
            if (error) *error = "the MCP discovery file is not valid JSON: " + parse_error.message;
            return false;
        }
        const std::string url = parsed->str("url");
        if (url.empty()) {
            if (error) *error = "the MCP discovery file has no 'url' field";
            return false;
        }
        // Parse http://host:port
        const size_t scheme = url.find("://");
        const std::string authority = scheme == std::string::npos ? url : url.substr(scheme + 3);
        const size_t colon = authority.rfind(':');
        if (colon == std::string::npos) {
            if (error) *error = "cannot parse the MCP endpoint URL '" + url + "'";
            return false;
        }
        options_.host = authority.substr(0, colon);
        try {
            options_.port = static_cast<uint16_t>(std::stoi(authority.substr(colon + 1)));
        } catch (...) {
            if (error) *error = "cannot parse the port in '" + url + "'";
            return false;
        }
        if (options_.token.empty()) options_.token = parsed->str("token");
    }

    if (options_.port == 0) {
        if (error) *error = "no MCP port was provided and none could be discovered";
        return false;
    }

    // Verify the endpoint before reporting success, so a stdio host sees a clear
    // failure instead of a hung request later.
    const HttpResponseRaw health = http_post(options_.host, options_.port, "/health", "{}", {},
                                             "application/json", 5000);
    if (!health.ok) {
        if (error) {
            *error = "the Colaimdation MCP endpoint at " + options_.host + ":" +
                     std::to_string(options_.port) + " is not reachable: " + health.error;
        }
        return false;
    }
    session_id_ = "stdio-" + utf8::fnv1a64_hex(std::to_string(::GetCurrentProcessId())).substr(0, 8);
    return true;
}

bool StdioServer::forward(const std::string& line) {
    HttpResponseRaw response = http_post(options_.host, options_.port, "/mcp", line, options_.token,
                                         "application/json", 120000);
    if (!response.ok) {
        // Answer with a JSON-RPC transport error so the host is not left waiting.
        Json failure = rpc_failure(Json(), -32000,
                                   "Colaimdation is not reachable: " + response.error);
        // Recover the request id when possible so the host can match it up.
        JsonParseError parse_error;
        if (auto parsed = json_parse(line, &parse_error); parsed && parsed->is_object()) {
            if (parsed->find("id")) failure.set("id", (*parsed)["id"]);
        }
        write_stdout_line(failure.dump(false));
        return false;
    }
    if (response.body.empty()) return true; // notification: no response expected
    // The editor answers with a JSON-RPC message (or an empty body for
    // notifications); pass it through unchanged.
    std::string body = response.body;
    while (!body.empty() && (body.back() == '\n' || body.back() == '\r')) body.pop_back();
    if (body.empty()) return true;
    write_stdout_line(body);
    return true;
}

int StdioServer::run() {
    running_.store(true);
    if (options_.forward_notifications) {
        notification_thread_ = std::thread([this] { notification_loop(); });
    }

    std::string pending;
    char chunk[65536];
    for (;;) {
        const size_t read = std::fread(chunk, 1, sizeof(chunk), stdin);
        if (read == 0) break;
        pending.append(chunk, read);
        for (;;) {
            const size_t newline = pending.find('\n');
            if (newline == std::string::npos) break;
            std::string line = pending.substr(0, newline);
            pending.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            if (options_.verbose) write_stderr_line("[Colaimdation] -> " + line);
            forward(line);
        }
        if (pending.size() > 16u * 1024 * 1024) pending.clear();
    }
    // Handle a final message without a trailing newline.
    if (!pending.empty()) forward(pending);

    stop();
    return 0;
}

void StdioServer::stop() {
    if (!running_.exchange(false)) return;
    if (notification_thread_.joinable()) notification_thread_.join();
    if (stop_event_) {
        ::CloseHandle(static_cast<HANDLE>(stop_event_));
        stop_event_ = nullptr;
    }
}

void StdioServer::notification_loop() {
    if (!options_.forward_notifications) return;
    // Announce the session so the editor can label tool calls with it.
    Json announce = Json::object();
    announce.set("jsonrpc", Json("2.0"));
    announce.set("method", Json("notifications/initialized"));
    Json params = Json::object();
    params.set("session_id", Json(session_id_));
    announce.set("params", std::move(params));
    forward(announce.dump(false));

    while (running_.load()) {
        SseClient client;
        std::string error;
        if (!client.open(options_, &error)) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }
        std::string event;
        std::string data;
        while (running_.load()) {
            if (!client.next_event(event, data, 1000)) break;
            if (data.empty()) continue;
            if (event == "ping") continue;
            if (options_.verbose) write_stderr_line("[Colaimdation] <- " + event + " " + data);
            // Relay editor-side events to stderr: the MCP host's log, not the
            // protocol stream, is the right place for them.
            if (event == "message") {
                JsonParseError parse_error;
                auto parsed = json_parse(data, &parse_error);
                if (parsed && parsed->is_object()) {
                    const std::string method = parsed->str("method");
                    if (!method.empty() && method != "notifications/resources/updated") {
                        write_stderr_line("[colaimdation] editor event: " + method);
                    }
                }
            }
        }
        client.close();
    }
}

} // namespace colai::mcp
