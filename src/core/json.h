// Colaimdation - tiny dependency-free JSON value / parser / serializer.
//
// The MCP transports speak JSON-RPC 2.0, so a compact, allocation-light JSON
// implementation lives in the engine. Values keep object member order, which
// makes tool responses and audit entries stable and diff-friendly, and UTF-8
// strings are handed through untouched except for the escapes JSON requires.
#pragma once

#include <cstdint>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace colai {

class Json;
using JsonArray = std::vector<Json>;

// Ordered object storage: insertion order is preserved by the vector, lookups
// stay O(n) which is irrelevant at MCP payload sizes.
class JsonObject {
public:
    using Entry = std::pair<std::string, Json>;
    using iterator = std::vector<Entry>::iterator;
    using const_iterator = std::vector<Entry>::const_iterator;

    JsonObject() = default;
    JsonObject(std::initializer_list<Entry> init) : entries_(init) {}

    bool empty() const { return entries_.empty(); }
    size_t size() const { return entries_.size(); }
    iterator begin() { return entries_.begin(); }
    iterator end() { return entries_.end(); }
    const_iterator begin() const { return entries_.begin(); }
    const_iterator end() const { return entries_.end(); }

    bool contains(std::string_view key) const { return find(key) != nullptr; }
    const Json* find(std::string_view key) const;
    Json* find(std::string_view key);

    // Insert or overwrite, keeping the original position on overwrite.
    Json& set(std::string key, Json value);
    bool erase(std::string_view key);

    const std::vector<Entry>& entries() const { return entries_; }
    std::vector<Entry>& entries() { return entries_; }

private:
    std::vector<Entry> entries_;
};

class Json {
public:
    enum class Kind { Null, Bool, Int, Double, String, Array, Object };

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool value) : value_(value) {}
    Json(int value) : value_(static_cast<int64_t>(value)) {}
    Json(int64_t value) : value_(value) {}
    Json(uint32_t value) : value_(static_cast<int64_t>(value)) {}
    Json(uint64_t value) : value_(static_cast<int64_t>(value)) {}
    Json(double value) : value_(value) {}
    Json(const char* value) : value_(std::string(value ? value : "")) {}
    Json(std::string value) : value_(std::move(value)) {}
    Json(std::string_view value) : value_(std::string(value)) {}
    Json(JsonArray value) : value_(std::move(value)) {}
    Json(JsonObject value) : value_(std::move(value)) {}
    Json(std::initializer_list<JsonObject::Entry> init) : value_(JsonObject(init)) {}

    static Json array() { return Json(JsonArray{}); }
    static Json array(JsonArray items) { return Json(std::move(items)); }
    static Json object() { return Json(JsonObject{}); }

    Kind kind() const { return static_cast<Kind>(value_.index()); }
    bool is_null() const { return std::holds_alternative<std::monostate>(value_); }
    bool is_bool() const { return std::holds_alternative<bool>(value_); }
    bool is_int() const { return std::holds_alternative<int64_t>(value_); }
    bool is_double() const { return std::holds_alternative<double>(value_); }
    bool is_number() const { return is_int() || is_double(); }
    bool is_string() const { return std::holds_alternative<std::string>(value_); }
    bool is_array() const { return std::holds_alternative<JsonArray>(value_); }
    bool is_object() const { return std::holds_alternative<JsonObject>(value_); }

    // Typed access with sane defaults - reads never throw.
    bool as_bool(bool fallback = false) const;
    int64_t as_int(int64_t fallback = 0) const;
    double as_double(double fallback = 0.0) const;
    std::string as_string(std::string fallback = {}) const;
    // String for text nodes, compact JSON for everything else.
    std::string as_text() const;

    JsonArray& items();
    const JsonArray& items() const;
    JsonObject& members();
    const JsonObject& members() const;

    size_t size() const;
    bool empty() const;

    // Object helpers (no-ops / nullopt when the value is not an object).
    const Json* find(std::string_view key) const;
    Json* find(std::string_view key);
    // Three explicit key overloads. Without the `const char*` one, a string
    // literal would silently bind to operator[](size_t) through pointer-to-
    // integer conversion instead of being treated as an object key.
    Json& operator[](std::string_view key);
    const Json& operator[](std::string_view key) const;
    Json& operator[](const char* key) { return (*this)[std::string_view(key ? key : "")]; }
    const Json& operator[](const char* key) const {
        return (*this)[std::string_view(key ? key : "")];
    }
    Json& operator[](const std::string& key) { return (*this)[std::string_view(key)]; }
    const Json& operator[](const std::string& key) const { return (*this)[std::string_view(key)]; }
    Json& operator[](size_t index);
    const Json& operator[](size_t index) const;
    Json& operator[](int index) { return (*this)[static_cast<size_t>(index)]; }
    const Json& operator[](int index) const { return (*this)[static_cast<size_t>(index)]; }
    bool contains(std::string_view key) const { return find(key) != nullptr; }
    Json& push(Json value);
    Json& set(std::string key, Json value);

    // Convenience readers for object members.
    std::string str(std::string_view key, std::string fallback = {}) const;
    int64_t integer(std::string_view key, int64_t fallback = 0) const;
    double number(std::string_view key, double fallback = 0.0) const;
    bool boolean(std::string_view key, bool fallback = false) const;

    // Serialization: compact by default, `pretty` uses two-space indentation.
    std::string dump(bool pretty = false) const;
    void dump_to(std::string& out, bool pretty, int indent) const;

private:
    std::variant<std::monostate, bool, int64_t, double, std::string, JsonArray, JsonObject> value_;
};

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------
struct JsonParseError {
    std::string message;
    size_t offset = 0;
    size_t line = 1;
    size_t column = 1;
};

// Returns nullopt on failure and fills `error` when provided.
std::optional<Json> json_parse(std::string_view text, JsonParseError* error = nullptr);

// Throwing variant for tests and internal use where failure is a bug.
Json json_parse_or_throw(std::string_view text);

// ---------------------------------------------------------------------------
// Shared result type: the engine never throws across the MCP boundary.
// ---------------------------------------------------------------------------
template <typename T> struct Result {
    bool ok = false;
    T value{};
    std::string code;
    std::string message;
    Json data;

    static Result success(T v) {
        Result r;
        r.ok = true;
        r.value = std::move(v);
        return r;
    }
    static Result failure(std::string code_, std::string message_, Json data_ = Json()) {
        Result r;
        r.ok = false;
        r.code = std::move(code_);
        r.message = std::move(message_);
        r.data = std::move(data_);
        return r;
    }
    explicit operator bool() const { return ok; }
};

using Status = Result<std::monostate>;

inline Status status_ok() { return Status::success(std::monostate{}); }
inline Status status_error(std::string code, std::string message, Json data = Json()) {
    return Status::failure(std::move(code), std::move(message), std::move(data));
}

} // namespace colai
