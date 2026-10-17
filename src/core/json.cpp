#include "core/json.h"

#include "core/utf8.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace colai {
namespace {

inline void append_utf8(std::string& out, uint32_t cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

class Parser {
public:
    Parser(std::string_view text, JsonParseError* error) : text_(text), error_(error) {}

    std::optional<Json> parse() {
        skip_whitespace();
        Json value;
        if (!parse_value(value, 0)) return std::nullopt;
        skip_whitespace();
        if (pos_ != text_.size()) {
            fail("unexpected trailing content");
            return std::nullopt;
        }
        return value;
    }

private:
    static constexpr int kMaxDepth = 200;

    std::string_view text_;
    size_t pos_ = 0;
    JsonParseError* error_ = nullptr;

    bool fail(std::string message) {
        if (error_) {
            error_->message = std::move(message);
            error_->offset = pos_;
            error_->line = utf8::line_number_at(text_, pos_);
            error_->column = utf8::column_at(text_, pos_);
        }
        return false;
    }

    bool eof() const { return pos_ >= text_.size(); }
    char peek() const { return eof() ? '\0' : text_[pos_]; }

    void skip_whitespace() {
        while (!eof()) {
            const char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    bool literal(std::string_view word) {
        if (text_.compare(pos_, word.size(), word) != 0) return fail("invalid literal");
        pos_ += word.size();
        return true;
    }

    bool parse_value(Json& out, int depth) {
        if (depth > kMaxDepth) return fail("maximum nesting depth exceeded");
        skip_whitespace();
        if (eof()) return fail("unexpected end of input");
        switch (peek()) {
        case '{': return parse_object(out, depth);
        case '[': return parse_array(out, depth);
        case '"': {
            std::string s;
            if (!parse_string(s)) return false;
            out = Json(std::move(s));
            return true;
        }
        case 't':
            if (!literal("true")) return false;
            out = Json(true);
            return true;
        case 'f':
            if (!literal("false")) return false;
            out = Json(false);
            return true;
        case 'n':
            if (!literal("null")) return false;
            out = Json(nullptr);
            return true;
        default: return parse_number(out);
        }
    }

    bool parse_object(Json& out, int depth) {
        ++pos_; // '{'
        JsonObject object;
        skip_whitespace();
        if (peek() == '}') {
            ++pos_;
            out = Json(std::move(object));
            return true;
        }
        while (true) {
            skip_whitespace();
            if (peek() != '"') return fail("expected object key string");
            std::string key;
            if (!parse_string(key)) return false;
            skip_whitespace();
            if (peek() != ':') return fail("expected ':' after object key");
            ++pos_;
            Json value;
            if (!parse_value(value, depth + 1)) return false;
            object.set(std::move(key), std::move(value));
            skip_whitespace();
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            if (peek() == '}') {
                ++pos_;
                break;
            }
            return fail("expected ',' or '}' in object");
        }
        out = Json(std::move(object));
        return true;
    }

    bool parse_array(Json& out, int depth) {
        ++pos_; // '['
        JsonArray array;
        skip_whitespace();
        if (peek() == ']') {
            ++pos_;
            out = Json(std::move(array));
            return true;
        }
        while (true) {
            Json value;
            if (!parse_value(value, depth + 1)) return false;
            array.push_back(std::move(value));
            skip_whitespace();
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            if (peek() == ']') {
                ++pos_;
                break;
            }
            return fail("expected ',' or ']' in array");
        }
        out = Json(std::move(array));
        return true;
    }

    bool parse_hex4(uint32_t& value) {
        if (pos_ + 4 > text_.size()) return fail("truncated \\u escape");
        value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[pos_ + static_cast<size_t>(i)];
            uint32_t digit = 0;
            if (c >= '0' && c <= '9') digit = static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') digit = static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') digit = static_cast<uint32_t>(c - 'A' + 10);
            else return fail("invalid hex digit in \\u escape");
            value = (value << 4) | digit;
        }
        pos_ += 4;
        return true;
    }

    bool parse_string(std::string& out) {
        ++pos_; // opening quote
        out.clear();
        while (true) {
            if (eof()) return fail("unterminated string");
            const unsigned char c = static_cast<unsigned char>(text_[pos_]);
            if (c == '"') {
                ++pos_;
                return true;
            }
            if (c == '\\') {
                ++pos_;
                if (eof()) return fail("unterminated escape");
                const char e = text_[pos_++];
                switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    uint32_t cp = 0;
                    if (!parse_hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        // High surrogate: try to pair with a low surrogate.
                        if (pos_ + 1 < text_.size() && text_[pos_] == '\\' &&
                            text_[pos_ + 1] == 'u') {
                            const size_t save = pos_;
                            pos_ += 2;
                            uint32_t low = 0;
                            if (parse_hex4(low) && low >= 0xDC00 && low <= 0xDFFF) {
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                            } else {
                                pos_ = save;
                                cp = 0xFFFD;
                            }
                        } else {
                            cp = 0xFFFD;
                        }
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        cp = 0xFFFD; // lone low surrogate
                    }
                    append_utf8(out, cp);
                    break;
                }
                default: return fail("invalid escape sequence");
                }
                continue;
            }
            if (c < 0x20) return fail("unescaped control character in string");
            out.push_back(static_cast<char>(c));
            ++pos_;
        }
    }

    bool parse_number(Json& out) {
        const size_t start = pos_;
        if (peek() == '-') ++pos_;
        if (eof()) return fail("truncated number");
        if (peek() == '0') {
            ++pos_;
        } else if (peek() >= '1' && peek() <= '9') {
            while (!eof() && peek() >= '0' && peek() <= '9') ++pos_;
        } else {
            return fail("invalid number");
        }
        bool is_double = false;
        if (peek() == '.') {
            is_double = true;
            ++pos_;
            if (eof() || peek() < '0' || peek() > '9') return fail("invalid fraction");
            while (!eof() && peek() >= '0' && peek() <= '9') ++pos_;
        }
        if (peek() == 'e' || peek() == 'E') {
            is_double = true;
            ++pos_;
            if (peek() == '+' || peek() == '-') ++pos_;
            if (eof() || peek() < '0' || peek() > '9') return fail("invalid exponent");
            while (!eof() && peek() >= '0' && peek() <= '9') ++pos_;
        }
        const std::string literal(text_.substr(start, pos_ - start));
        if (!is_double) {
            errno = 0;
            char* end = nullptr;
            const long long v = std::strtoll(literal.c_str(), &end, 10);
            if (errno == 0 && end && *end == '\0') {
                out = Json(static_cast<int64_t>(v));
                return true;
            }
        }
        out = Json(std::strtod(literal.c_str(), nullptr));
        return true;
    }
};

void dump_escaped(std::string& out, std::string_view text) {
    out.push_back('"');
    out += utf8::json_escape(text);
    out.push_back('"');
}

void dump_number(std::string& out, const Json& value) {
    if (value.is_int()) {
        out += std::to_string(value.as_int());
        return;
    }
    const double d = value.as_double();
    if (std::isfinite(d)) {
        char buffer[40];
        // %.17g round-trips IEEE-754 doubles; trim a trailing ".0" style noise
        // by trying the shorter representation first.
        std::snprintf(buffer, sizeof(buffer), "%.15g", d);
        const double check = std::strtod(buffer, nullptr);
        if (check != d) std::snprintf(buffer, sizeof(buffer), "%.17g", d);
        out += buffer;
    } else {
        out += "null"; // JSON has no NaN/Infinity
    }
}

} // namespace

// ---------------------------------------------------------------------------
// JsonObject
// ---------------------------------------------------------------------------
const Json* JsonObject::find(std::string_view key) const {
    for (const auto& entry : entries_) {
        if (entry.first == key) return &entry.second;
    }
    return nullptr;
}

Json* JsonObject::find(std::string_view key) {
    for (auto& entry : entries_) {
        if (entry.first == key) return &entry.second;
    }
    return nullptr;
}

Json& JsonObject::set(std::string key, Json value) {
    if (Json* existing = find(key)) {
        *existing = std::move(value);
        return *existing;
    }
    entries_.emplace_back(std::move(key), std::move(value));
    return entries_.back().second;
}

bool JsonObject::erase(std::string_view key) {
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (it->first == key) {
            entries_.erase(it);
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Json accessors
// ---------------------------------------------------------------------------
bool Json::as_bool(bool fallback) const {
    if (std::holds_alternative<bool>(value_)) return std::get<bool>(value_);
    if (std::holds_alternative<int64_t>(value_)) return std::get<int64_t>(value_) != 0;
    return fallback;
}

int64_t Json::as_int(int64_t fallback) const {
    if (std::holds_alternative<int64_t>(value_)) return std::get<int64_t>(value_);
    if (std::holds_alternative<double>(value_)) return static_cast<int64_t>(std::get<double>(value_));
    if (std::holds_alternative<bool>(value_)) return std::get<bool>(value_) ? 1 : 0;
    if (std::holds_alternative<std::string>(value_)) {
        const auto& s = std::get<std::string>(value_);
        try {
            size_t used = 0;
            const long long v = std::stoll(s, &used);
            if (used == s.size()) return v;
        } catch (...) {
        }
    }
    return fallback;
}

double Json::as_double(double fallback) const {
    if (std::holds_alternative<double>(value_)) return std::get<double>(value_);
    if (std::holds_alternative<int64_t>(value_)) return static_cast<double>(std::get<int64_t>(value_));
    return fallback;
}

std::string Json::as_string(std::string fallback) const {
    if (std::holds_alternative<std::string>(value_)) return std::get<std::string>(value_);
    if (std::holds_alternative<bool>(value_)) return std::get<bool>(value_) ? "true" : "false";
    if (std::holds_alternative<int64_t>(value_)) return std::to_string(std::get<int64_t>(value_));
    if (std::holds_alternative<double>(value_)) {
        std::string out;
        dump_number(out, *this);
        return out;
    }
    return fallback;
}

std::string Json::as_text() const {
    if (is_string()) return std::get<std::string>(value_);
    if (is_null()) return {};
    return dump(false);
}

JsonArray& Json::items() {
    if (!is_array()) value_ = JsonArray{};
    return std::get<JsonArray>(value_);
}

const JsonArray& Json::items() const {
    static const JsonArray kEmpty;
    return is_array() ? std::get<JsonArray>(value_) : kEmpty;
}

JsonObject& Json::members() {
    if (!is_object()) value_ = JsonObject{};
    return std::get<JsonObject>(value_);
}

const JsonObject& Json::members() const {
    static const JsonObject kEmpty;
    return is_object() ? std::get<JsonObject>(value_) : kEmpty;
}

size_t Json::size() const {
    if (is_array()) return std::get<JsonArray>(value_).size();
    if (is_object()) return std::get<JsonObject>(value_).size();
    if (is_string()) return utf8::length(std::get<std::string>(value_));
    return 0;
}

bool Json::empty() const { return size() == 0; }

const Json* Json::find(std::string_view key) const { return members().find(key); }

Json* Json::find(std::string_view key) { return members().find(key); }

Json& Json::operator[](std::string_view key) {
    JsonObject& object = members();
    if (Json* existing = object.find(key)) return *existing;
    return object.set(std::string(key), Json());
}

const Json& Json::operator[](std::string_view key) const {
    static const Json kNull;
    const Json* found = find(key);
    return found ? *found : kNull;
}

Json& Json::operator[](size_t index) {
    JsonArray& array = items();
    if (index >= array.size()) array.resize(index + 1);
    return array[index];
}

const Json& Json::operator[](size_t index) const {
    static const Json kNull;
    const JsonArray& array = items();
    return index < array.size() ? array[index] : kNull;
}

Json& Json::push(Json value) {
    JsonArray& array = items();
    array.push_back(std::move(value));
    return array.back();
}

Json& Json::set(std::string key, Json value) {
    return members().set(std::move(key), std::move(value));
}

std::string Json::str(std::string_view key, std::string fallback) const {
    const Json* found = find(key);
    if (!found || found->is_null()) return fallback;
    return found->as_string(std::move(fallback));
}

int64_t Json::integer(std::string_view key, int64_t fallback) const {
    const Json* found = find(key);
    return found ? found->as_int(fallback) : fallback;
}

double Json::number(std::string_view key, double fallback) const {
    const Json* found = find(key);
    return found ? found->as_double(fallback) : fallback;
}

bool Json::boolean(std::string_view key, bool fallback) const {
    const Json* found = find(key);
    return found ? found->as_bool(fallback) : fallback;
}

void Json::dump_to(std::string& out, bool pretty, int indent) const {
    switch (kind()) {
    case Kind::Null: out += "null"; return;
    case Kind::Bool: out += as_bool() ? "true" : "false"; return;
    case Kind::Int:
    case Kind::Double: dump_number(out, *this); return;
    case Kind::String: dump_escaped(out, std::get<std::string>(value_)); return;
    case Kind::Array: {
        const JsonArray& array = std::get<JsonArray>(value_);
        if (array.empty()) {
            out += "[]";
            return;
        }
        out.push_back('[');
        for (size_t i = 0; i < array.size(); ++i) {
            if (i) out.push_back(',');
            if (pretty) {
                out.push_back('\n');
                out.append(static_cast<size_t>(indent + 1) * 2, ' ');
            }
            array[i].dump_to(out, pretty, indent + 1);
        }
        if (pretty) {
            out.push_back('\n');
            out.append(static_cast<size_t>(indent) * 2, ' ');
        }
        out.push_back(']');
        return;
    }
    case Kind::Object: {
        const JsonObject& object = std::get<JsonObject>(value_);
        if (object.empty()) {
            out += "{}";
            return;
        }
        out.push_back('{');
        bool first = true;
        for (const auto& entry : object.entries()) {
            if (!first) out.push_back(',');
            first = false;
            if (pretty) {
                out.push_back('\n');
                out.append(static_cast<size_t>(indent + 1) * 2, ' ');
            }
            dump_escaped(out, entry.first);
            out.push_back(':');
            if (pretty) out.push_back(' ');
            entry.second.dump_to(out, pretty, indent + 1);
        }
        if (pretty) {
            out.push_back('\n');
            out.append(static_cast<size_t>(indent) * 2, ' ');
        }
        out.push_back('}');
        return;
    }
    }
}

std::string Json::dump(bool pretty) const {
    std::string out;
    out.reserve(256);
    dump_to(out, pretty, 0);
    return out;
}

// ---------------------------------------------------------------------------
// Parsing entry points
// ---------------------------------------------------------------------------
std::optional<Json> json_parse(std::string_view text, JsonParseError* error) {
    // Tolerate a UTF-8 BOM, which some Windows tools prepend.
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        text.remove_prefix(3);
    }
    Parser parser(text, error);
    return parser.parse();
}

Json json_parse_or_throw(std::string_view text) {
    JsonParseError error;
    auto parsed = json_parse(text, &error);
    if (!parsed) {
        throw std::runtime_error("JSON parse error at line " + std::to_string(error.line) + ", column " +
                                 std::to_string(error.column) + ": " + error.message);
    }
    return *parsed;
}

} // namespace colai
