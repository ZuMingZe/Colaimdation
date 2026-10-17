#include "mcp/http_server.h"

#include "core/utf8.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <sstream>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

namespace colai {
namespace {

constexpr size_t kMaxRequestBytes = 8u * 1024 * 1024;
constexpr int kReadTimeoutMs = 15000;

std::string status_reason(int status) {
    switch (status) {
    case 200: return "OK";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 406: return "Not Acceptable";
    case 413: return "Payload Too Large";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default: return "OK";
    }
}

std::string lower(std::string_view text) { return utf8::to_lower(text); }

bool send_all(SOCKET socket, const char* data, size_t size) {
    size_t sent = 0;
    while (sent < size) {
        const int chunk = static_cast<int>(std::min<size_t>(size - sent, 1u << 18));
        const int written = ::send(socket, data + sent, chunk, 0);
        if (written <= 0) return false;
        sent += static_cast<size_t>(written);
    }
    return true;
}

bool parse_request_line(const std::string& line, HttpRequest& request) {
    const size_t first = line.find(' ');
    if (first == std::string::npos) return false;
    const size_t second = line.find(' ', first + 1);
    if (second == std::string::npos) return false;
    request.method = line.substr(0, first);
    request.target = line.substr(first + 1, second - first - 1);
    const size_t question = request.target.find('?');
    if (question == std::string::npos) {
        request.path = utf8::url_decode(request.target);
    } else {
        request.path = utf8::url_decode(request.target.substr(0, question));
        request.query = request.target.substr(question + 1);
    }
    const std::string version = line.substr(second + 1);
    request.keep_alive = version.find("1.0") == std::string::npos;
    return true;
}

class SocketSink : public EventSink {
public:
    explicit SocketSink(SOCKET socket) : socket_(socket) {}

    bool send(std::string_view event, std::string_view data) override {
        std::string frame;
        frame.reserve(event.size() + data.size() + 32);
        frame += "event: ";
        frame += std::string(event);
        frame += "\ndata: ";
        // SSE data must not contain raw newlines.
        for (char c : data) frame.push_back(c == '\n' ? ' ' : c);
        frame += "\n\n";
        return send_all(socket_, frame.data(), frame.size());
    }

    SOCKET socket_;
};

} // namespace

bool http_global_init() {
    static std::atomic<bool> ready{false};
    static std::atomic<bool> attempted{false};
    bool expected = false;
    if (attempted.compare_exchange_strong(expected, true)) {
        WSADATA data{};
        ready.store(::WSAStartup(MAKEWORD(2, 2), &data) == 0);
    }
    return ready.load();
}

// ---------------------------------------------------------------------------
// HttpRequest / HttpResponse
// ---------------------------------------------------------------------------
std::string HttpRequest::header(std::string_view name) const {
    const auto found = headers.find(lower(name));
    return found == headers.end() ? std::string{} : found->second;
}

bool HttpRequest::has_header(std::string_view name) const {
    return headers.find(lower(name)) != headers.end();
}

std::string HttpRequest::query_param(std::string_view name, std::string fallback) const {
    size_t position = 0;
    while (position <= query.size()) {
        size_t ampersand = query.find('&', position);
        if (ampersand == std::string::npos) ampersand = query.size();
        const std::string_view pair(query.data() + position, ampersand - position);
        const size_t equals = pair.find('=');
        const std::string_view key =
            equals == std::string_view::npos ? pair : pair.substr(0, equals);
        if (key == name) {
            const std::string_view value =
                equals == std::string_view::npos ? std::string_view{} : pair.substr(equals + 1);
            return utf8::url_decode(value);
        }
        position = ampersand + 1;
    }
    return fallback;
}

bool HttpRequest::has_query_param(std::string_view name) const {
    size_t position = 0;
    while (position <= query.size()) {
        size_t ampersand = query.find('&', position);
        if (ampersand == std::string::npos) ampersand = query.size();
        const std::string_view pair(query.data() + position, ampersand - position);
        const size_t equals = pair.find('=');
        if ((equals == std::string_view::npos ? pair : pair.substr(0, equals)) == name) return true;
        position = ampersand + 1;
    }
    return false;
}

HttpResponse HttpResponse::json(std::string body, int status) {
    HttpResponse response;
    response.status = status;
    response.reason = status_reason(status);
    response.content_type = "application/json; charset=utf-8";
    response.body = std::move(body);
    return response;
}

HttpResponse HttpResponse::text(std::string body, int status, std::string content_type) {
    HttpResponse response;
    response.status = status;
    response.reason = status_reason(status);
    response.content_type = std::move(content_type);
    response.body = std::move(body);
    return response;
}

HttpResponse HttpResponse::error(int status, std::string message) {
    Json body = Json::object();
    body.set("error", Json(message));
    body.set("status", Json(static_cast<int64_t>(status)));
    return json(body.dump(false), status);
}

HttpResponse HttpResponse::no_content() {
    HttpResponse response;
    response.status = 204;
    response.reason = status_reason(204);
    response.content_type = "text/plain; charset=utf-8";
    return response;
}

// ---------------------------------------------------------------------------
// HttpServer
// ---------------------------------------------------------------------------
HttpServer::HttpServer() = default;

HttpServer::~HttpServer() { stop(); }

bool HttpServer::listen(uint16_t port, std::string* error) {
    if (running_.load()) {
        if (error) *error = "the server is already listening";
        return false;
    }
    if (!http_global_init()) {
        if (error) *error = "Winsock failed to initialise";
        return false;
    }

    SOCKET socket_handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_handle == INVALID_SOCKET) {
        if (error) *error = "socket() failed with error " + std::to_string(::WSAGetLastError());
        return false;
    }

    // Loopback only: the editor's MCP endpoint must never be reachable from the
    // network.
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    address.sin_port = ::htons(port);

    if (::bind(socket_handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) ==
        SOCKET_ERROR) {
        const int code = ::WSAGetLastError();
        ::closesocket(socket_handle);
        if (error) {
            *error = "cannot bind 127.0.0.1:" + std::to_string(port) + " (error " +
                     std::to_string(code) + ")";
        }
        return false;
    }
    if (::listen(socket_handle, SOMAXCONN) == SOCKET_ERROR) {
        ::closesocket(socket_handle);
        if (error) *error = "listen() failed with error " + std::to_string(::WSAGetLastError());
        return false;
    }

    sockaddr_in actual{};
    int length = sizeof(actual);
    if (::getsockname(socket_handle, reinterpret_cast<sockaddr*>(&actual), &length) == 0) {
        port_ = ::ntohs(actual.sin_port);
    } else {
        port_ = port;
    }

    listen_socket_ = static_cast<uintptr_t>(socket_handle);
    running_.store(true);
    accept_thread_ = std::thread([this] { accept_loop(); });
    return true;
}

void HttpServer::stop() {
    if (!running_.exchange(false)) return;
    if (listen_socket_ != ~uintptr_t(0)) {
        ::closesocket(static_cast<SOCKET>(listen_socket_));
        listen_socket_ = ~uintptr_t(0);
    }
    // Joining the accept loop first means no new client thread is spawned while
    // we are collecting the existing ones.
    if (accept_thread_.joinable()) accept_thread_.join();

    // Client threads are joined rather than detached: the objects they touch
    // (Workspace, the editor bridge) are destroyed as soon as the shell tears the
    // server down, so a tool call still in flight must not outlive them. Each
    // connection has a 15s receive timeout, so a stuck peer delays shutdown by at
    // most that long instead of hanging it forever.
    std::vector<std::thread> clients;
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        clients.swap(client_threads_);
    }
    for (std::thread& thread : clients) {
        if (thread.joinable()) thread.join();
    }
}

std::string HttpServer::base_url() const {
    return "http://127.0.0.1:" + std::to_string(port_);
}

void HttpServer::accept_loop() {
    while (running_.load()) {
        SOCKET client = ::accept(static_cast<SOCKET>(listen_socket_), nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            if (!running_.load()) break;
            const int code = ::WSAGetLastError();
            if (code == WSAEINTR || code == WSAENOTSOCK) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        if (connections_.load() >= max_connections_) {
            const std::string busy = "HTTP/1.1 503 Service Unavailable\r\n"
                                     "Content-Length: 0\r\nConnection: close\r\n\r\n";
            send_all(client, busy.data(), busy.size());
            ::closesocket(client);
            continue;
        }
        connections_.fetch_add(1);
        std::thread worker([this, client] {
            serve_client(static_cast<uintptr_t>(client));
            connections_.fetch_sub(1);
        });
        std::lock_guard<std::mutex> lock(clients_mutex_);
        client_threads_.push_back(std::move(worker));
    }
}

void HttpServer::serve_client(uintptr_t socket_value) {
    SOCKET socket_handle = static_cast<SOCKET>(socket_value);
    DWORD timeout = kReadTimeoutMs;
    ::setsockopt(socket_handle, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
                 sizeof(timeout));
    BOOL nodelay = TRUE;
    ::setsockopt(socket_handle, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay),
                 sizeof(nodelay));

    std::string buffer;
    buffer.reserve(8192);
    char chunk[16384];

    for (;;) {
        // --- read headers ---------------------------------------------------
        size_t header_end = buffer.find("\r\n\r\n");
        while (header_end == std::string::npos) {
            const int read = ::recv(socket_handle, chunk, sizeof(chunk), 0);
            if (read <= 0) {
                ::closesocket(socket_handle);
                return;
            }
            buffer.append(chunk, static_cast<size_t>(read));
            if (buffer.size() > kMaxRequestBytes) {
                const std::string too_large = "HTTP/1.1 413 Payload Too Large\r\n"
                                              "Content-Length: 0\r\nConnection: close\r\n\r\n";
                send_all(socket_handle, too_large.data(), too_large.size());
                ::closesocket(socket_handle);
                return;
            }
            header_end = buffer.find("\r\n\r\n");
        }

        HttpRequest request;
        {
            const std::string head = buffer.substr(0, header_end);
            std::istringstream stream(head);
            std::string line;
            if (!std::getline(stream, line)) {
                ::closesocket(socket_handle);
                return;
            }
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!parse_request_line(line, request)) {
                const std::string bad = "HTTP/1.1 400 Bad Request\r\n"
                                        "Content-Length: 0\r\nConnection: close\r\n\r\n";
                send_all(socket_handle, bad.data(), bad.size());
                ::closesocket(socket_handle);
                return;
            }
            while (std::getline(stream, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty()) continue;
                const size_t colon = line.find(':');
                if (colon == std::string::npos) continue;
                request.headers[lower(utf8::trim(line.substr(0, colon)))] =
                    utf8::trim(line.substr(colon + 1));
            }
        }

        // An explicit "Connection: close" wins over the HTTP version default:
        // otherwise a client waiting for EOF would block until the read timeout.
        if (request.has_header("connection")) {
            request.keep_alive = lower(request.header("connection")).find("close") == std::string::npos;
        }

        // --- read body ------------------------------------------------------
        size_t content_length = 0;
        if (request.has_header("content-length")) {
            try {
                content_length = static_cast<size_t>(std::stoull(request.header("content-length")));
            } catch (...) {
                content_length = 0;
            }
        }
        if (content_length > kMaxRequestBytes) {
            const std::string too_large = "HTTP/1.1 413 Payload Too Large\r\n"
                                          "Content-Length: 0\r\nConnection: close\r\n\r\n";
            send_all(socket_handle, too_large.data(), too_large.size());
            ::closesocket(socket_handle);
            return;
        }
        size_t body_have = buffer.size() - (header_end + 4);
        while (body_have < content_length) {
            const int read = ::recv(socket_handle, chunk, sizeof(chunk), 0);
            if (read <= 0) break;
            buffer.append(chunk, static_cast<size_t>(read));
            body_have = buffer.size() - (header_end + 4);
        }
        const size_t consumed_body = std::min(content_length, body_have);
        request.body = buffer.substr(header_end + 4, consumed_body);
        request.content_type = request.header("content-type");
        request.peer_address = "127.0.0.1";

        // --- dispatch -------------------------------------------------------
        bool streamed = false;
        HttpResponse response;
        if (!handler_) {
            response = HttpResponse::error(503, "no request handler installed");
        } else {
            SocketSink sink(socket_handle);
            response = handler_(request, &sink);
            if (response.status == 0) {
                streamed = true;
                const std::string head = "HTTP/1.1 200 OK\r\n"
                                         "Content-Type: text/event-stream; charset=utf-8\r\n"
                                         "Cache-Control: no-cache\r\n"
                                         "Connection: close\r\n\r\n";
                send_all(socket_handle, head.data(), head.size());
            }
        }

        if (streamed) {
            // The handler already flushed its events; close the socket.
            ::closesocket(socket_handle);
            return;
        }

        std::string header;
        header += "HTTP/1.1 " + std::to_string(response.status) + " " + response.reason + "\r\n";
        header += "Content-Type: " + response.content_type + "\r\n";
        header += "Content-Length: " + std::to_string(response.body.size()) + "\r\n";
        header += "Cache-Control: no-store\r\n";
        for (const auto& extra : response.headers) {
            header += extra.first + ": " + extra.second + "\r\n";
        }
        header += request.keep_alive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
        header += "\r\n";
        if (!send_all(socket_handle, header.data(), header.size()) ||
            !send_all(socket_handle, response.body.data(), response.body.size())) {
            ::closesocket(socket_handle);
            return;
        }
        if (!request.keep_alive) {
            ::closesocket(socket_handle);
            return;
        }
        buffer.erase(0, header_end + 4 + consumed_body);
    }
}

// ---------------------------------------------------------------------------
// HTTP client
// ---------------------------------------------------------------------------
HttpResponseRaw http_post(const std::string& host, uint16_t port, const std::string& path,
                          const std::string& body, const std::string& bearer_token,
                          std::string_view content_type, int timeout_ms) {
    HttpResponseRaw result;
    if (!http_global_init()) {
        result.error = "Winsock failed to initialise";
        return result;
    }
    SOCKET socket_handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_handle == INVALID_SOCKET) {
        result.error = "socket() failed";
        return result;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = ::htons(port);
    ::inet_pton(AF_INET, host.c_str(), &address.sin_addr);
    if (::connect(socket_handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) ==
        SOCKET_ERROR) {
        result.error = "cannot connect to " + host + ":" + std::to_string(port) + " (error " +
                       std::to_string(::WSAGetLastError()) + ")";
        ::closesocket(socket_handle);
        return result;
    }
    DWORD timeout = static_cast<DWORD>(std::max(100, timeout_ms));
    ::setsockopt(socket_handle, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
                 sizeof(timeout));
    ::setsockopt(socket_handle, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout),
                 sizeof(timeout));

    std::string request;
    request.reserve(body.size() + 512);
    request += "POST " + path + " HTTP/1.1\r\n";
    request += "Host: " + host + ":" + std::to_string(port) + "\r\n";
    request += "Content-Type: " + std::string(content_type) + "\r\n";
    request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    request += "Accept: application/json\r\n";
    if (!bearer_token.empty()) request += "Authorization: Bearer " + bearer_token + "\r\n";
    request += "Connection: close\r\n\r\n";
    request += body;

    if (!send_all(socket_handle, request.data(), request.size())) {
        result.error = "send failed";
        ::closesocket(socket_handle);
        return result;
    }

    std::string response;
    char chunk[16384];
    for (;;) {
        const int read = ::recv(socket_handle, chunk, sizeof(chunk), 0);
        if (read <= 0) break;
        response.append(chunk, static_cast<size_t>(read));
    }
    ::closesocket(socket_handle);

    const size_t header_end = response.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        result.error = "malformed HTTP response";
        return result;
    }
    const std::string head = response.substr(0, header_end);
    {
        const size_t first_space = head.find(' ');
        if (first_space != std::string::npos) {
            try {
                result.status = std::stoi(head.substr(first_space + 1, 4));
            } catch (...) {
                result.status = 0;
            }
        }
    }
    std::string payload = response.substr(header_end + 4);
    if (lower(head).find("transfer-encoding: chunked") != std::string::npos) {
        std::string decoded;
        size_t position = 0;
        while (position < payload.size()) {
            const size_t line_end = payload.find("\r\n", position);
            if (line_end == std::string::npos) break;
            size_t size = 0;
            try {
                size = static_cast<size_t>(
                    std::stoull(payload.substr(position, line_end - position), nullptr, 16));
            } catch (...) {
                break;
            }
            if (size == 0) break;
            decoded.append(payload, line_end + 2, size);
            position = line_end + 2 + size + 2;
        }
        payload = std::move(decoded);
    }
    result.body = std::move(payload);
    result.ok = true;
    return result;
}

} // namespace colai
