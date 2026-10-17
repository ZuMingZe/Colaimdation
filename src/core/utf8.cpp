#include "core/utf8.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>

#pragma comment(lib, "bcrypt.lib")

namespace colai::utf8 {
namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

inline bool is_continuation(unsigned char c) { return (c & 0xC0) == 0x80; }

inline bool is_ascii_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

} // namespace

int sequence_length(unsigned char lead) {
    if (lead < 0x80) return 1;
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    if ((lead & 0xF8) == 0xF0) return 4;
    return 1;
}

bool is_valid(std::string_view text) {
    size_t i = 0;
    const size_t n = text.size();
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c < 0x80) {
            ++i;
            continue;
        }
        int len = 0;
        uint32_t cp = 0;
        if ((c & 0xE0) == 0xC0) {
            len = 2;
            cp = c & 0x1Fu;
        } else if ((c & 0xF0) == 0xE0) {
            len = 3;
            cp = c & 0x0Fu;
        } else if ((c & 0xF8) == 0xF0) {
            len = 4;
            cp = c & 0x07u;
        } else {
            return false; // stray continuation or 5/6-byte lead
        }
        if (i + static_cast<size_t>(len) > n) return false;
        for (int k = 1; k < len; ++k) {
            const unsigned char cc = static_cast<unsigned char>(text[i + k]);
            if (!is_continuation(cc)) return false;
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        // overlong / out of range / surrogate
        static const uint32_t kMin[5] = {0, 0, 0x80, 0x800, 0x10000};
        if (cp < kMin[len]) return false;
        if (cp > 0x10FFFF) return false;
        if (cp >= 0xD800 && cp <= 0xDFFF) return false;
        i += static_cast<size_t>(len);
    }
    return true;
}

size_t length(std::string_view text) {
    size_t count = 0;
    for (size_t i = 0; i < text.size();) {
        i += static_cast<size_t>(sequence_length(static_cast<unsigned char>(text[i])));
        ++count;
    }
    return count;
}

bool is_absolute_path(std::string_view path) {
    if (path.size() >= 2 && (path[1] == ':' || path[1] == '|')) {
        // Drive-absolute ("C:\..."); '|' is the form some shells pass through.
        const char drive = path[0];
        return (drive >= 'A' && drive <= 'Z') || (drive >= 'a' && drive <= 'z');
    }
    // UNC ("\\server\share") or rooted ("\folder").
    return !path.empty() && (path[0] == '\\' || path[0] == '/');
}

size_t line_start(std::string_view text, size_t byte_offset) {
    if (byte_offset > text.size()) byte_offset = text.size();
    size_t i = byte_offset;
    while (i > 0 && text[i - 1] != '\n') --i;
    return i;
}

size_t line_end(std::string_view text, size_t byte_offset) {
    if (byte_offset > text.size()) byte_offset = text.size();
    size_t i = text.find('\n', byte_offset);
    return i == std::string_view::npos ? text.size() : i;
}

size_t length_until(std::string_view text, size_t byte_offset) {
    return length(text.substr(0, std::min(byte_offset, text.size())));
}

size_t advance(std::string_view text, size_t byte_offset, int count) {
    if (byte_offset > text.size()) byte_offset = text.size();
    if (count == 0) return byte_offset;
    size_t i = byte_offset;
    if (count > 0) {
        while (count-- > 0 && i < text.size()) {
            i += static_cast<size_t>(sequence_length(static_cast<unsigned char>(text[i])));
        }
        return std::min(i, text.size());
    }
    // Backwards: impossible to derive the previous boundary from a single byte,
    // so walk forward from the start of the text instead.
    const int current = static_cast<int>(length(text.substr(0, i)));
    const int target = current + count; // count is negative
    if (target <= 0) return 0;
    size_t position = 0;
    for (int step = 0; step < target && position < text.size(); ++step) {
        position += static_cast<size_t>(sequence_length(static_cast<unsigned char>(text[position])));
    }
    return std::min(position, text.size());
}

size_t clamp_to_boundary(std::string_view text, size_t byte_offset) {
    if (byte_offset >= text.size()) return text.size();
    size_t i = byte_offset;
    while (i > 0 && is_continuation(static_cast<unsigned char>(text[i]))) --i;
    return i;
}

size_t line_number_at(std::string_view text, size_t byte_offset) {
    const size_t end = std::min(byte_offset, text.size());
    size_t line = 1;
    for (size_t i = 0; i < end; ++i) {
        if (text[i] == '\n') ++line;
    }
    return line;
}

size_t column_at(std::string_view text, size_t byte_offset) {
    const size_t start = line_start(text, byte_offset);
    return length(text.substr(start, std::min(byte_offset, text.size()) - start)) + 1;
}

std::vector<std::string_view> split_lines(std::string_view text, bool keep_terminators) {
    std::vector<std::string_view> out;
    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find('\n', start);
        if (nl == std::string_view::npos) {
            if (start < text.size()) out.push_back(text.substr(start));
            break;
        }
        size_t end = keep_terminators ? nl + 1 : nl;
        if (end > start || keep_terminators) out.push_back(text.substr(start, end - start));
        start = nl + 1;
    }
    return out;
}

std::wstring to_wide(std::string_view text) {
    if (text.empty()) return {};
    const int need = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0);
    if (need <= 0) {
        // Fall back to a lossy conversion so the UI never goes blank.
        const int lossy = ::MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                                static_cast<int>(text.size()), nullptr, 0);
        if (lossy <= 0) return {};
        std::wstring buffer(static_cast<size_t>(lossy), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                              buffer.data(), lossy);
        return buffer;
    }
    std::wstring buffer(static_cast<size_t>(need), L'\0');
    ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                          static_cast<int>(text.size()), buffer.data(), need);
    return buffer;
}

std::string from_wide(std::wstring_view text) {
    if (text.empty()) return {};
    const int need = ::WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0, nullptr,
                                           nullptr);
    if (need <= 0) return {};
    std::string buffer(static_cast<size_t>(need), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), buffer.data(),
                          need, nullptr, nullptr);
    return buffer;
}

std::string to_console(std::string_view text) {
    std::wstring wide = to_wide(text);
    if (wide.empty()) return std::string(text);
    const int need = ::WideCharToMultiByte(CP_ACP, 0, wide.data(), static_cast<int>(wide.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (need <= 0) return std::string(text);
    std::string buffer(static_cast<size_t>(need), '\0');
    ::WideCharToMultiByte(CP_ACP, 0, wide.data(), static_cast<int>(wide.size()), buffer.data(),
                          need, nullptr, nullptr);
    return buffer;
}

std::string hex_encode(const unsigned char* data, size_t size) {
    std::string out;
    out.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) {
        out.push_back(kHexDigits[data[i] >> 4]);
        out.push_back(kHexDigits[data[i] & 0x0F]);
    }
    return out;
}

std::string fnv1a64_hex(std::string_view text) {
    uint64_t hash = 1469598103934665603ULL;
    for (unsigned char c : text) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    unsigned char bytes[8];
    for (int i = 0; i < 8; ++i) bytes[i] = static_cast<unsigned char>((hash >> (i * 8)) & 0xFF);
    return hex_encode(bytes, 8);
}

std::string sha256_hex(std::string_view text) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (::BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
        // Extremely unlikely; degrade to the non-cryptographic digest.
        return fnv1a64_hex(text);
    }
    DWORD object_size = 0, result_size = 0, hash_size = 0;
    ::BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_size),
                        sizeof(object_size), &result_size, 0);
    ::BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hash_size),
                        sizeof(hash_size), &result_size, 0);

    std::vector<unsigned char> object(object_size);
    std::vector<unsigned char> hash(hash_size);
    BCRYPT_HASH_HANDLE h = nullptr;
    std::string out;
    if (::BCryptCreateHash(alg, &h, object.data(), object_size, nullptr, 0, 0) == 0) {
        ::BCryptHashData(h, reinterpret_cast<PUCHAR>(const_cast<char*>(text.data())),
                         static_cast<ULONG>(text.size()), 0);
        ::BCryptFinishHash(h, hash.data(), hash_size, 0);
        ::BCryptDestroyHash(h);
        out = hex_encode(hash.data(), hash.size());
    }
    ::BCryptCloseAlgorithmProvider(alg, 0);
    return out.empty() ? fnv1a64_hex(text) : out;
}

std::vector<unsigned char> random_bytes(size_t count) {
    std::vector<unsigned char> buffer(count);
    if (count > 0 && ::BCryptGenRandom(nullptr, buffer.data(), static_cast<ULONG>(count),
                                       BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        // Fall back to a time/pid mix - only reached if the CSPRNG is missing.
        uint64_t seed = static_cast<uint64_t>(::GetTickCount64()) ^
                        (static_cast<uint64_t>(::GetCurrentProcessId()) << 32);
        for (size_t i = 0; i < count; ++i) {
            seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
            buffer[i] = static_cast<unsigned char>((seed >> 33) & 0xFF);
        }
    }
    return buffer;
}

namespace {
constexpr char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
}

std::string base64_encode(std::string_view data) {
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    size_t i = 0;
    while (i + 2 < data.size()) {
        const uint32_t v = (static_cast<unsigned char>(data[i]) << 16) |
                           (static_cast<unsigned char>(data[i + 1]) << 8) |
                           static_cast<unsigned char>(data[i + 2]);
        out.push_back(kB64[(v >> 18) & 63]);
        out.push_back(kB64[(v >> 12) & 63]);
        out.push_back(kB64[(v >> 6) & 63]);
        out.push_back(kB64[v & 63]);
        i += 3;
    }
    if (i + 1 == data.size()) {
        const uint32_t v = static_cast<unsigned char>(data[i]) << 16;
        out.push_back(kB64[(v >> 18) & 63]);
        out.push_back(kB64[(v >> 12) & 63]);
        out.push_back('=');
        out.push_back('=');
    } else if (i + 2 == data.size()) {
        const uint32_t v = (static_cast<unsigned char>(data[i]) << 16) |
                           (static_cast<unsigned char>(data[i + 1]) << 8);
        out.push_back(kB64[(v >> 18) & 63]);
        out.push_back(kB64[(v >> 12) & 63]);
        out.push_back(kB64[(v >> 6) & 63]);
        out.push_back('=');
    }
    return out;
}

bool base64_decode(std::string_view text, std::string& out) {
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    out.clear();
    uint32_t acc = 0;
    int bits = 0;
    for (char c : text) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        const int v = value(c);
        if (v < 0) return false;
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((acc >> bits) & 0xFF));
        }
    }
    return true;
}

std::string to_lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    });
    return out;
}

bool iequals_ascii(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        unsigned char x = static_cast<unsigned char>(a[i]);
        unsigned char y = static_cast<unsigned char>(b[i]);
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return false;
    }
    return true;
}

std::string trim(std::string_view text) {
    size_t b = 0;
    size_t e = text.size();
    while (b < e && is_ascii_space(text[b])) ++b;
    while (e > b && is_ascii_space(text[e - 1])) --e;
    return std::string(text.substr(b, e - b));
}

std::string json_escape(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (size_t i = 0; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
    }
    return out;
}

std::string url_decode(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '%' && i + 2 < text.size()) {
            auto hex = [](char h) -> int {
                if (h >= '0' && h <= '9') return h - '0';
                if (h >= 'a' && h <= 'f') return h - 'a' + 10;
                if (h >= 'A' && h <= 'F') return h - 'A' + 10;
                return -1;
            };
            const int hi = hex(text[i + 1]);
            const int lo = hex(text[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(c == '+' ? ' ' : c);
    }
    return out;
}

std::string url_encode(std::string_view text) {
    std::string out;
    out.reserve(text.size() * 3);
    for (unsigned char c : text) {
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
                                c == '~';
        if (unreserved) {
            out.push_back(static_cast<char>(c));
        } else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

} // namespace colai::utf8
