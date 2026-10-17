// Colaimdation - UTF-8 helpers.
//
// The whole engine works on UTF-8 (std::string) because it is the natural
// encoding for both the Markdown document and the JSON-RPC / MCP transports.
// Windows API calls need UTF-16, so conversions live here.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace colai::utf8 {

// ---------------------------------------------------------------------------
// Character classification / decoding
// ---------------------------------------------------------------------------

// Number of bytes in the UTF-8 sequence starting with `lead` (1..4).
// Returns 1 for invalid lead bytes so callers can always make progress.
int sequence_length(unsigned char lead);

// Length in Unicode scalar values (UTF-16-independent code points).
size_t length(std::string_view text);

// True when the byte string is well-formed UTF-8 (also rejects overlong
// encodings, surrogates and values above U+10FFFF).
bool is_valid(std::string_view text);

// True when `path` is drive-absolute ("C:\x"), UNC ("\\server\share") or rooted
// ("\x"). The shell resolves relative document paths against the executable's
// directory, so a bare "notes.md" opens the same file however the process was
// started (shortcut, file association or another program).
bool is_absolute_path(std::string_view path);

// Number of code points in [byte_offset, end_of_line) - used for column math.
size_t length_until(std::string_view text, size_t byte_offset);

// Byte offset of the start of the line containing `byte_offset`.
size_t line_start(std::string_view text, size_t byte_offset);

// Byte offset of the '\n' ending the line containing `byte_offset`
// (or text.size() when the line is the last one and unterminated).
size_t line_end(std::string_view text, size_t byte_offset);

// Advance by `count` code points from `byte_offset`, clamped to [0, size].
size_t advance(std::string_view text, size_t byte_offset, int count);

// Clamp an arbitrary byte offset back to a UTF-8 sequence boundary.
size_t clamp_to_boundary(std::string_view text, size_t byte_offset);

// 1-based line number of `byte_offset`.
size_t line_number_at(std::string_view text, size_t byte_offset);

// 1-based column (in code points) of `byte_offset` within its line.
size_t column_at(std::string_view text, size_t byte_offset);

// Split into lines. `keep_terminators` keeps the trailing '\n'.
std::vector<std::string_view> split_lines(std::string_view text, bool keep_terminators = false);

// ---------------------------------------------------------------------------
// Conversions
// ---------------------------------------------------------------------------
std::wstring to_wide(std::string_view text);
std::string from_wide(std::wstring_view text);

// Replace bytes that cannot be represented safely in a Windows dialog / log
// line (used for titles and console output only - never for documents).
std::string to_console(std::string_view text);

// Hex-encode arbitrary bytes; the digest helper below reuses it for the token.
std::string hex_encode(const unsigned char* data, size_t size);

// ---------------------------------------------------------------------------
// Small self-contained hash: FNV-1a 64 over the raw bytes, rendered as hex.
// Used for document revisions, tokens and cache keys. Not a cryptographic
// primitive, but enough to fingerprint content and to generate local tokens
// (see policy.cpp for the clarification).
// ---------------------------------------------------------------------------
std::string fnv1a64_hex(std::string_view text);

// SHA-256, implemented locally so the security layer can use a real digest for
// the local auth token without pulling in a crypto dependency.
std::string sha256_hex(std::string_view text);

// Cryptographically-strong random bytes from the OS CSPRNG.
std::vector<unsigned char> random_bytes(size_t count);

// Base64 (standard alphabet, padded). Used for HTTP basic auth fallback.
std::string base64_encode(std::string_view data);
bool base64_decode(std::string_view text, std::string& out);

// Lowercase ASCII copy.
std::string to_lower(std::string_view text);
bool iequals_ascii(std::string_view a, std::string_view b);
std::string trim(std::string_view text);

// Escape a string for embedding in JSON (without the surrounding quotes).
std::string json_escape(std::string_view text);

// URL-decode `%XX` sequences and '+' as space (query strings only).
std::string url_decode(std::string_view text);
std::string url_encode(std::string_view text);

} // namespace colai::utf8
