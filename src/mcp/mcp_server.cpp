#include "mcp/mcp_server.h"

#include "core/i18n.h"
#include "core/policy.h"
#include "core/utf8.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace colai::mcp {
namespace {

std::string make_session_id() {
    const std::vector<unsigned char> bytes = utf8::random_bytes(8);
    return "sess-" + utf8::hex_encode(bytes.data(), bytes.size());
}

// Accepts "Bearer <token>", a bare token, or Basic base64("user:token").
std::string extract_token(const std::string& authorization) {
    const std::string trimmed = utf8::trim(authorization);
    if (trimmed.empty()) return {};
    const std::string lower = utf8::to_lower(trimmed);
    if (lower.rfind("bearer ", 0) == 0) return utf8::trim(trimmed.substr(7));
    if (lower.rfind("basic ", 0) == 0) {
        std::string decoded;
        if (!utf8::base64_decode(utf8::trim(trimmed.substr(6)), decoded)) return {};
        const size_t colon = decoded.find(':');
        // Accept either "user:token" or "token:" (token as the username, which
        // is what several MCP clients do).
        if (colon == std::string::npos) return utf8::trim(decoded);
        const std::string user = decoded.substr(0, colon);
        const std::string password = decoded.substr(colon + 1);
        return utf8::trim(password.empty() ? user : password);
    }
    return trimmed;
}

std::string hostname_of(const HttpRequest& request) {
    std::string host = request.header("host");
    const size_t colon = host.find(':');
    if (colon != std::string::npos) host = host.substr(0, colon);
    return utf8::to_lower(host);
}

bool is_loopback_host(const std::string& host) {
    return host.empty() || host == "127.0.0.1" || host == "localhost" || host == "[::1]" ||
           host == "::1";
}

std::string to_iso8601(int64_t unix_ms) { return iso8601(unix_ms); }

} // namespace

// ---------------------------------------------------------------------------
// Envelope helpers
// ---------------------------------------------------------------------------
Json rpc_response(const Json& id, Json result) {
    Json out = Json::object();
    out.set("jsonrpc", Json("2.0"));
    out.set("id", id.is_null() ? Json(0) : id);
    out.set("result", std::move(result));
    return out;
}

Json rpc_failure(const Json& id, int code, std::string message, Json data) {
    Json error = Json::object();
    error.set("code", Json(static_cast<int64_t>(code)));
    error.set("message", Json(std::move(message)));
    if (!data.is_null()) error.set("data", std::move(data));
    Json out = Json::object();
    out.set("jsonrpc", Json("2.0"));
    out.set("id", id.is_null() ? Json(0) : id);
    out.set("error", std::move(error));
    return out;
}

Json ServerStatus::to_json() const {
    Json out = Json::object();
    out.set("listening", Json(listening));
    out.set("port", Json(static_cast<int64_t>(port)));
    out.set("base_url", Json(base_url));
    out.set("endpoint", Json(endpoint));
    out.set("require_token", Json(require_token));
    out.set("discovery_path", Json(discovery_path));
    out.set("tools", Json(static_cast<int64_t>(tools)));
    out.set("sessions", Json(static_cast<int64_t>(sessions)));
    out.set("requests", Json(requests));
    out.set("tool_calls", Json(tool_calls));
    out.set("started", Json(to_iso8601(started_ms)));
    return out;
}

// ---------------------------------------------------------------------------
// McpServer
// ---------------------------------------------------------------------------
McpServer::McpServer() = default;

McpServer::~McpServer() { stop(); }

bool McpServer::initialise(Workspace& workspace, uint16_t port, std::string* error) {
    workspace_ = &workspace;
    registry_ = ToolRegistry{};
    register_builtin_tools(registry_);

    status_.require_token = workspace.policy().require_token;
    status_.token = workspace.policy().token;
    status_.tools = registry_.size();
    status_.started_ms = now_ms();

    http_ = std::make_unique<HttpServer>();
    http_->set_handler([this](const HttpRequest& request, EventSink* sink) {
        return http_dispatch(request, sink);
    });

    std::string listen_error;
    if (!http_->listen(port, &listen_error)) {
        if (error) *error = listen_error;
        status_.listening = false;
        return false;
    }

    status_.listening = true;
    status_.port = http_->port();
    status_.base_url = http_->base_url();
    status_.endpoint = status_.base_url + "/mcp";
    if (status_.require_token && !status_.token.empty()) {
        status_.endpoint += "?token=" + status_.token;
    }
    return true;
}

void McpServer::stop() {
    if (http_) http_->stop();
    status_.listening = false;
}

void McpServer::set_token(std::string token) {
    status_.token = std::move(token);
    status_.endpoint = status_.base_url + "/mcp";
    if (status_.require_token && !status_.token.empty()) {
        status_.endpoint += "?token=" + status_.token;
    }
    // Keep the policy in step: the tools report it and the stdio bridge reads it
    // from the discovery file this instance rewrites.
    if (workspace_) {
        Json patch = Json::object();
        patch.set("token", Json(status_.token));
        workspace_->set_policy(patch);
    }
}

bool McpServer::write_discovery_file(const std::string& path, std::string* error) {
    Json document = Json::object();
    document.set("name", Json(kServerName));
    document.set("title", Json(kServerTitle));
    document.set("version", Json("0.1.0"));
    document.set("protocol_version", Json(kProtocolVersion));
    document.set("transport", Json("http"));
    document.set("url", Json(status_.base_url + "/mcp"));
    if (status_.require_token && !status_.token.empty()) {
        document.set("url_with_token", Json(status_.endpoint));
        document.set("token", Json(status_.token));
        Json header = Json::object();
        header.set("Authorization", Json("Bearer " + status_.token));
        document.set("headers", std::move(header));
    }
    document.set("document", Json(workspace_ ? workspace_->document().path() : std::string{}));
    Json tools = Json::array();
    for (const ToolDescriptor& tool : registry_.tools()) tools.push(Json(tool.name));
    document.set("tools", std::move(tools));
    document.set("generated_at", Json(to_iso8601(now_ms())));

    if (path.empty()) {
        if (error) *error = "no discovery path configured";
        return false;
    }
    // Workspace::write_text_file creates the directory and truncates the file.
    const std::string payload = document.dump(true) + "\n";
    // Written with a UTF-8 BOM: this file is read by the *other* tools on the
    // machine (the stdio bridge, and whatever scripting language the writer
    // uses). Without a BOM, Windows PowerShell 5.1 decodes it with the ANSI code
    // page and fails on any non-ASCII path - which is exactly what happens when
    // the project or document lives in a directory with Chinese characters.
    std::string write_error;
    if (!Workspace::write_text_file(path, payload, &write_error, true).ok) {
        if (error) {
            *error = "cannot write the MCP discovery file '" + path + "': " + write_error;
        }
        return false;
    }
    status_.discovery_path = path;
    return true;
}

// ---------------------------------------------------------------------------
// Authentication
// ---------------------------------------------------------------------------
bool McpServer::authorise(const HttpRequest& request, std::string* error) const {
    // The endpoint is bound to loopback, so a direct connection can only come
    // from this machine; the token protects against other local programs.
    if (!is_loopback_host(hostname_of(request))) {
        if (error) *error = "requests must use a loopback Host header";
        return false;
    }
    if (!status_.require_token) return true;
    if (status_.token.empty()) return true;

    const std::string bearer = extract_token(request.header("authorization"));
    if (!bearer.empty() && secure_token_equals(bearer, status_.token)) return true;

    const std::string query_token = request.query_param("token");
    if (!query_token.empty() && secure_token_equals(query_token, status_.token)) return true;

    const std::string header_token = request.header("x-colaimdation-token");
    if (!header_token.empty() && secure_token_equals(header_token, status_.token)) return true;

    if (error) {
        *error =
            "missing or invalid token; send it as 'Authorization: Bearer <token>' or ?token=<token> "
            "(the editor shows the endpoint URL in its MCP panel)";
    }
    return false;
}

// ---------------------------------------------------------------------------
// HTTP dispatch
// ---------------------------------------------------------------------------
HttpResponse McpServer::http_dispatch(const HttpRequest& request, EventSink* sink) {
    requests_.fetch_add(1);

    // Liveness probe, deliberately unauthenticated so the editor's own
    // self-check and port scanners can confirm the server is up.
    if (request.path == "/health" || request.path == "/") {
        Json out = Json::object();
        out.set("name", Json(kServerName));
        out.set("status", Json("ok"));
        out.set("mcp_endpoint", Json("/mcp"));
        out.set("tools", Json(static_cast<int64_t>(registry_.size())));
        out.set("protocol_version", Json(kProtocolVersion));
        out.set("language", Json(i18n::language()));
        out.set("document", Json(workspace_ ? workspace_->document().display_name() : std::string{}));
        return HttpResponse::json(out.dump(false));
    }

    if (request.path == "/notifications" || request.path == "/sse") {
        std::string auth_error;
        if (!authorise(request, &auth_error)) {
            return HttpResponse::json(rpc_failure(Json(), -32001, auth_error).dump(false), 401);
        }
        if (!sink) {
            return HttpResponse::json(
                rpc_failure(Json(), rpc::kInvalidRequest,
                            "this endpoint requires an SSE client (Accept: text/event-stream)")
                    .dump(false),
                406);
        }
        add_sink(sink);
        {
            Json hello = Json::object();
            hello.set("server", Json(kServerName));
            hello.set("protocol_version", Json(kProtocolVersion));
            hello.set("revision", Json(workspace_ ? workspace_->document().revision() : std::string{}));
            if (!sink->send("hello", hello.dump(false))) {
                remove_sink(sink);
                return HttpResponse::no_content();
            }
        }
        // Hold the connection open until the peer disappears. The periodic ping
        // both keeps the connection alive and detects a dead client.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::hours(12);
        int64_t tick = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::seconds(15));
            Json ping = Json::object();
            ping.set("server", Json(kServerName));
            ping.set("revision",
                     Json(workspace_ ? workspace_->document().revision() : std::string{}));
            ping.set("character_count",
                     Json(workspace_ ? static_cast<int64_t>(workspace_->document().character_count())
                                     : int64_t(0)));
            ping.set("tick", Json(++tick));
            if (!sink->send("ping", ping.dump(false))) break;
        }
        remove_sink(sink);
        // Status 0 tells the transport the handler already wrote the stream.
        HttpResponse streaming;
        streaming.status = 0;
        return streaming;
    }

    if (request.path != "/mcp" && request.path != "/mcp/") {
        return HttpResponse::json(
            rpc_failure(Json(), rpc::kInvalidRequest,
                        "unknown path '" + request.path + "'; the MCP endpoint is /mcp")
                .dump(false),
            404);
    }

    if (request.method != "POST") {
        HttpResponse response = HttpResponse::json(
            rpc_failure(Json(), rpc::kInvalidRequest,
                        "the MCP endpoint accepts POST with a JSON-RPC body (GET /notifications "
                        "streams events)")
                .dump(false),
            405);
        response.headers.emplace_back("Allow", "POST, GET");
        return response;
    }

    std::string auth_error;
    if (!authorise(request, &auth_error)) {
        return HttpResponse::json(rpc_failure(Json(), -32001, auth_error).dump(false), 401);
    }

    JsonParseError parse_error;
    auto parsed = json_parse(request.body, &parse_error);
    if (!parsed) {
        return HttpResponse::json(
            rpc_failure(Json(), rpc::kParseError,
                        "invalid JSON: " + parse_error.message + " (line " +
                            std::to_string(parse_error.line) + ")")
                .dump(false),
            400);
    }

    Json response;
    if (parsed->is_array()) {
        Json batch = Json::array();
        for (const Json& item : parsed->items()) {
            Json item_response = handle(item, "http", request.header("authorization"));
            if (!item_response.is_null()) batch.push(std::move(item_response));
        }
        // A batch of pure notifications gets no body at all.
        if (batch.items().empty()) return HttpResponse::no_content();
        response = std::move(batch);
    } else {
        response = handle(*parsed, "http", request.header("authorization"));
    }
    if (response.is_null()) return HttpResponse::no_content();
    return HttpResponse::json(response.dump(false));
}

// ---------------------------------------------------------------------------
// JSON-RPC / MCP handling
// ---------------------------------------------------------------------------
Session& McpServer::session_for(const std::string& key, const Json& params) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    for (auto& entry : sessions_) {
        if (entry.first == key) {
            entry.second.last_seen_ms = now_ms();
            ++entry.second.calls;
            if (entry.second.client_name.empty()) {
                const Json* info = params.find("clientInfo");
                if (info) {
                    entry.second.client_name = info->str("name");
                    entry.second.client_version = info->str("version");
                }
            }
            return entry.second;
        }
    }
    Session session;
    session.id = key.empty() ? make_session_id() : key;
    session.connected_ms = now_ms();
    session.last_seen_ms = session.connected_ms;
    session.calls = 1;
    const Json* info = params.find("clientInfo");
    if (info) {
        session.client_name = info->str("name");
        session.client_version = info->str("version");
    }
    sessions_.emplace_back(session.id, session);
    status_.sessions = sessions_.size();
    return sessions_.back().second;
}

Json McpServer::rpc_result(const Json& id, Json result) const {
    return rpc_response(id, std::move(result));
}

Json McpServer::rpc_error(const Json& id, int code, std::string message, Json data) const {
    return rpc_failure(id, code, std::move(message), std::move(data));
}

Json McpServer::mcp_method(const std::string& method, const Json& params, Session& session,
                           const std::string& transport) {
    if (!workspace_) {
        return rpc_failure(Json(), rpc::kInternalError, "the MCP server has no workspace attached");
    }

    if (method == "initialize") {
        Json result = Json::object();
        const std::string requested = params.str("protocolVersion");
        result.set("protocolVersion",
                   Json(requested.empty() ? std::string(kProtocolVersion) : requested));
        Json capabilities = Json::object();
        Json tools_capability = Json::object();
        tools_capability.set("listChanged", Json(false));
        capabilities.set("tools", std::move(tools_capability));
        Json resources_capability = Json::object();
        resources_capability.set("subscribe", Json(true));
        resources_capability.set("listChanged", Json(false));
        capabilities.set("resources", std::move(resources_capability));
        Json prompts_capability = Json::object();
        capabilities.set("prompts", std::move(prompts_capability));
        Json logging_capability = Json::object();
        capabilities.set("logging", std::move(logging_capability));
        result.set("capabilities", std::move(capabilities));
        Json info = Json::object();
        info.set("name", Json(kServerName));
        info.set("title", Json(kServerTitle));
        info.set("version", Json("0.1.0"));
        result.set("serverInfo", std::move(info));
        result.set("instructions", Json(
            "You are connected to Colaimdation, a local Markdown editor with a human writer at the "
            "keyboard. Work on the document through these tools - never by reading or writing files "
            "on disk, because the writer may have unsaved changes that only exist in the editor. "
            "Read first (colaimdation_read_document / colaimdation_get_structure), then make small "
            "incremental edits with colaimdation_apply_patch and pass the revision you read as "
            "expected_revision so a concurrent edit by the writer becomes a clean conflict instead "
            "of a lost change. Use colaimdation_get_selection to see where the writer is working, "
            "and colaimdation_get_user_changes to notice what they have written since you last "
            "looked. Every change you make is shown live in the editor and recorded in an audit log "
            "the writer can roll back."));
        return rpc_result(Json(), std::move(result));
    }

    if (method == "notifications/initialized" || method == "initialized" || method == "ping") {
        return Json(); // notifications and pings need no response body
    }

    if (method == "tools/list") {
        Json tools = Json::array();
        for (const ToolDescriptor& tool : registry_.tools()) tools.push(tool.to_mcp_json());
        Json result = Json::object();
        result.set("tools", std::move(tools));
        return rpc_result(Json(), std::move(result));
    }

    if (method == "tools/call") {
        const std::string name = params.str("name");
        if (name.empty()) {
            return rpc_failure(Json(), rpc::kInvalidParams, "tools/call requires a 'name'");
        }
        Json arguments = params.contains("arguments") ? params["arguments"] : Json::object();
        if (arguments.is_null()) arguments = Json::object();

        ToolContext context;
        context.workspace = workspace_;
        context.session = &session;
        context.emit = [this](const Json& message) { broadcast(message); };
        context.log = [this](const std::string& level, const std::string& message) {
            if (activity_sink_) {
                Json entry = Json::object();
                entry.set("type", Json("activity"));
                entry.set("level", Json(level));
                entry.set("message", Json(message));
                entry.set("timestamp", Json(to_iso8601(now_ms())));
                activity_sink_(entry);
            }
        };

        tool_calls_.fetch_add(1);
        status_.tool_calls = tool_calls_.load();
        Json result = registry_.call(name, arguments, context);
        result.set("serverInfo", Json([&] {
            Json info = Json::object();
            info.set("name", Json(kServerName));
            info.set("version", Json("0.1.0"));
            return info;
        }()));

        if (activity_sink_) {
            Json entry = Json::object();
            entry.set("type", Json("tool_call"));
            entry.set("tool", Json(name));
            entry.set("session", Json(session.id));
            entry.set("client", Json(session.client_name));
            entry.set("transport", Json(transport));
            entry.set("is_error", Json(result.boolean("isError", false)));
            entry.set("timestamp", Json(to_iso8601(now_ms())));
            activity_sink_(entry);
        }

        // Tell every other connected Agent (and the editor) that the document
        // may have moved on.
        Json notification = Json::object();
        notification.set("method", Json("notifications/resources/updated"));
        Json notification_params = Json::object();
        notification_params.set("uri", Json("colaimdation://document"));
        notification_params.set("tool", Json(name));
        notification_params.set("session", Json(session.id));
        notification_params.set("is_error", Json(result.boolean("isError", false)));
        notification_params.set("revision", Json(workspace_->document().revision()));
        notification_params.set("character_count",
                                Json(static_cast<int64_t>(workspace_->document().character_count())));
        notification_params.set("block_count",
                                Json(static_cast<int64_t>(workspace_->document().block_count())));
        notification.set("params", std::move(notification_params));
        broadcast(notification);

        return rpc_result(Json(), std::move(result));
    }

    if (method == "resources/list") {
        Json resources = Json::array();
        Json document = Json::object();
        document.set("uri", Json("colaimdation://document"));
        document.set("name", Json("Open Markdown document"));
        document.set("description", Json("The document currently open in the Colaimdation editor"));
        document.set("mimeType", Json("text/markdown"));
        resources.push(std::move(document));
        Json structure = Json::object();
        structure.set("uri", Json("colaimdation://document/structure"));
        structure.set("name", Json("Document block structure"));
        structure.set("description", Json("Blocks with ids, kinds and byte ranges"));
        structure.set("mimeType", Json("application/json"));
        resources.push(std::move(structure));
        Json result = Json::object();
        result.set("resources", std::move(resources));
        return rpc_result(Json(), std::move(result));
    }

    if (method == "resources/read") {
        const std::string uri = params.str("uri");
        if (uri == "colaimdation://document") {
            Json contents = Json::array();
            Json entry = Json::object();
            entry.set("uri", Json(uri));
            entry.set("mimeType", Json("text/markdown"));
            entry.set("text", Json(workspace_->document().text()));
            contents.push(std::move(entry));
            Json result = Json::object();
            result.set("contents", std::move(contents));
            return rpc_result(Json(), std::move(result));
        }
        if (uri == "colaimdation://document/structure") {
            Json contents = Json::array();
            Json entry = Json::object();
            entry.set("uri", Json(uri));
            entry.set("mimeType", Json("application/json"));
            entry.set("text", Json(workspace_->document().structure_json(0).dump(false)));
            contents.push(std::move(entry));
            Json result = Json::object();
            result.set("contents", std::move(contents));
            return rpc_result(Json(), std::move(result));
        }
        return rpc_failure(Json(), rpc::kInvalidParams, "unknown resource uri '" + uri + "'");
    }

    if (method == "prompts/list") {
        Json result = Json::object();
        result.set("prompts", Json::array());
        return rpc_result(Json(), std::move(result));
    }

    if (method == "logging/setLevel" || method == "notifications/cancelled" ||
        method == "notifications/roots/list_changed") {
        return Json();
    }

    return rpc_failure(Json(), rpc::kMethodNotFound, "unknown method '" + method + "'");
}

Json McpServer::handle(const Json& request, const std::string& transport,
                       const std::string& authorization) {
    (void)authorization;
    if (!request.is_object()) {
        return rpc_failure(Json(), rpc::kInvalidRequest, "a JSON-RPC message must be an object");
    }
    const std::string version = request.str("jsonrpc", "2.0");
    if (version != "2.0") {
        return rpc_failure(request.find("id") ? request["id"] : Json(), rpc::kInvalidRequest,
                           "jsonrpc must be \"2.0\"");
    }
    const Json id = request.find("id") ? request["id"] : Json();
    const bool is_notification = request.find("id") == nullptr;
    const std::string method = request.str("method");
    if (method.empty()) {
        return rpc_failure(id, rpc::kInvalidRequest, "missing 'method'");
    }
    Json params = request.contains("params") ? request["params"] : Json::object();
    if (params.is_null()) params = Json::object();

    // Session identity: the client name reported during initialize is enough to
    // attribute edits in the audit log; a transport-provided session id (see
    // stdio_server) takes precedence.
    const std::string session_key = params.str("session_id");
    Session& session = session_for(session_key, params);

    Json response = mcp_method(method, params, session, transport);
    if (is_notification) return Json();
    if (response.is_null()) {
        // Notification-shaped method called with an id: answer with an empty
        // result so the client is not left hanging.
        return rpc_result(id, Json::object());
    }
    if (response.contains("result") && response.find("id") && response["id"].is_null()) {
        response.set("id", id);
    }
    if (response.contains("error") && response.find("id") && response["id"].is_null()) {
        response.set("id", id);
    }
    return response;
}

Json McpServer::handle_raw(const std::string& body, const std::string& transport,
                           const std::string& authorization) {
    JsonParseError parse_error;
    auto parsed = json_parse(body, &parse_error);
    if (!parsed) {
        return rpc_failure(Json(), rpc::kParseError,
                           "invalid JSON: " + parse_error.message + " (line " +
                               std::to_string(parse_error.line) + ", column " +
                               std::to_string(parse_error.column) + ")");
    }
    if (parsed->is_array()) {
        Json batch = Json::array();
        for (const Json& item : parsed->items()) {
            Json response = handle(item, transport, authorization);
            if (!response.is_null()) batch.push(std::move(response));
        }
        return batch;
    }
    return handle(*parsed, transport, authorization);
}

// ---------------------------------------------------------------------------
// SSE fan-out
// ---------------------------------------------------------------------------
void McpServer::add_sink(EventSink* sink) {
    std::lock_guard<std::mutex> lock(sinks_mutex_);
    if (std::find(sinks_.begin(), sinks_.end(), sink) == sinks_.end()) sinks_.push_back(sink);
}

void McpServer::remove_sink(EventSink* sink) {
    std::lock_guard<std::mutex> lock(sinks_mutex_);
    sinks_.erase(std::remove(sinks_.begin(), sinks_.end(), sink), sinks_.end());
}

void McpServer::broadcast(const Json& message) {
    const std::string payload = message.dump(false);
    std::lock_guard<std::mutex> lock(sinks_mutex_);
    for (EventSink* sink : sinks_) {
        if (!sink) continue;
        sink->send("message", payload);
    }
}

} // namespace colai::mcp
