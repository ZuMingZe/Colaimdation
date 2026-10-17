// UTF-8 helpers and text edit primitives.
#include "test_framework.h"

#include "core/text_edit.h"
#include "core/utf8.h"

using namespace colai;

TEST_CASE("utf8/length counts code points not bytes") {
    CHECK_EQ(utf8::length("abc"), size_t(3));
    CHECK_EQ(utf8::length(""), size_t(0));
    CHECK_EQ(utf8::length("中文"), size_t(2));
    CHECK_EQ(utf8::length("a中b"), size_t(3));
    CHECK_EQ(utf8::length("😀x"), size_t(2));
    CHECK_EQ(utf8::length("\xe4\xb8\xad"), size_t(1));
}

TEST_CASE("utf8/validity rejects malformed sequences") {
    CHECK(utf8::is_valid("hello"));
    CHECK(utf8::is_valid("中文 😀"));
    CHECK(!utf8::is_valid("\xff"));
    CHECK(!utf8::is_valid("\xe4\xb8"));             // truncated
    CHECK(!utf8::is_valid("\xe4\xb8\x28"));         // bad continuation
    CHECK(!utf8::is_valid("\xc0\x80"));             // overlong NUL
    CHECK(!utf8::is_valid("\xed\xa0\x80"));         // surrogate
    CHECK(!utf8::is_valid("\xf8\x88\x80\x80\x80")); // 5-byte form
}

TEST_CASE("utf8/line and column math") {
    const std::string text = "alpha\nbeta\nγδ line\nlast";
    CHECK_EQ(utf8::line_number_at(text, 0), size_t(1));
    CHECK_EQ(utf8::line_number_at(text, 5), size_t(1));
    CHECK_EQ(utf8::line_number_at(text, 6), size_t(2));
    CHECK_EQ(utf8::line_number_at(text, 11), size_t(3));
    CHECK_EQ(utf8::column_at(text, 0), size_t(1));
    CHECK_EQ(utf8::column_at(text, 4), size_t(5));
    CHECK_EQ(utf8::column_at(text, 6), size_t(1));

    const size_t gamma = text.find("γ");
    CHECK_EQ(utf8::line_start(text, gamma), text.find("γδ"));
    CHECK_EQ(utf8::column_at(text, gamma), size_t(1));
    // The column counts characters, so after the two Greek letters it is 3.
    CHECK_EQ(utf8::column_at(text, text.find(" ")), size_t(3));
}

TEST_CASE("utf8/advance respects sequence boundaries") {
    const std::string text = "a中b😀c";
    CHECK_EQ(utf8::advance(text, 0, 0), size_t(0));
    CHECK_EQ(utf8::advance(text, 0, 1), size_t(1));
    CHECK_EQ(utf8::advance(text, 0, 2), size_t(4));
    CHECK_EQ(utf8::advance(text, 0, 3), size_t(5));
    CHECK_EQ(utf8::advance(text, 0, 4), size_t(9));
    CHECK_EQ(utf8::advance(text, 0, 99), text.size());
    // Stepping back from the end: "c" -> emoji -> "b" -> CJK -> "a".
    CHECK_EQ(utf8::advance(text, text.size(), -1), size_t(9));
    CHECK_EQ(utf8::advance(text, text.size(), -4), size_t(1));
    CHECK_EQ(utf8::advance(text, text.size(), -5), size_t(0));
    CHECK_EQ(utf8::advance(text, text.size(), -99), size_t(0));
}

TEST_CASE("utf8/clamp_to_boundary never splits a character") {
    const std::string text = "中x";
    CHECK_EQ(utf8::clamp_to_boundary(text, 0), size_t(0));
    CHECK_EQ(utf8::clamp_to_boundary(text, 1), size_t(0));
    CHECK_EQ(utf8::clamp_to_boundary(text, 2), size_t(0));
    CHECK_EQ(utf8::clamp_to_boundary(text, 3), size_t(3));
    CHECK_EQ(utf8::clamp_to_boundary(text, 1000), text.size());
}

TEST_CASE("utf8/wide round trip") {
    const std::string source = "混合 mixed 中文 😀";
    const std::wstring wide = utf8::to_wide(source);
    CHECK(!wide.empty());
    CHECK_EQ(utf8::from_wide(wide), source);
    CHECK_EQ(utf8::from_wide(L""), std::string());
    CHECK_EQ(utf8::to_wide(""), std::wstring());
}

TEST_CASE("utf8/hashes are stable and distinct") {
    const std::string a = utf8::sha256_hex("hello");
    const std::string b = utf8::sha256_hex("hello");
    const std::string c = utf8::sha256_hex("hellp");
    CHECK_EQ(a.size(), size_t(64));
    CHECK_EQ(a, b);
    CHECK_NE(a, c);
    // Known SHA-256 of "abc".
    CHECK_EQ(utf8::sha256_hex("abc"),
             std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));

    CHECK_EQ(utf8::fnv1a64_hex("x").size(), size_t(16));
    CHECK_NE(utf8::fnv1a64_hex("x"), utf8::fnv1a64_hex("y"));
}

TEST_CASE("utf8/random bytes differ") {
    const auto a = utf8::random_bytes(32);
    const auto b = utf8::random_bytes(32);
    CHECK_EQ(a.size(), size_t(32));
    CHECK(a != b);
}

TEST_CASE("utf8/base64 round trip") {
    const std::string data = "colaimdation \xe4\xb8\xad\xe6\x96\x87";
    const std::string encoded = utf8::base64_encode(data);
    std::string decoded;
    CHECK(utf8::base64_decode(encoded, decoded));
    CHECK_EQ(decoded, data);
    CHECK_EQ(utf8::base64_encode("ab"), std::string("YWI="));
    CHECK(!utf8::base64_decode("!!!!", decoded));
}

TEST_CASE("utf8/json escaping") {
    CHECK_EQ(utf8::json_escape("a\"b"), std::string("a\\\"b"));
    CHECK_EQ(utf8::json_escape("tab\there"), std::string("tab\\there"));
    CHECK_EQ(utf8::json_escape("nl\n"), std::string("nl\\n"));
    CHECK_EQ(utf8::json_escape(std::string("\x01")), std::string("\\u0001"));
    // Non-ASCII passes through untouched (the buffer is already UTF-8).
    CHECK_EQ(utf8::json_escape("中文"), std::string("中文"));
}

TEST_CASE("utf8/url encoding") {
    CHECK_EQ(utf8::url_encode("a b"), std::string("a%20b"));
    CHECK_EQ(utf8::url_decode("a%20b"), std::string("a b"));
    CHECK_EQ(utf8::url_decode("a+b"), std::string("a b"));
    CHECK_EQ(utf8::url_decode(utf8::url_encode("中文/x?y=1")), std::string("中文/x?y=1"));
}

TEST_CASE("text_edit/minimal change detection") {
    TextChange change;
    CHECK(!compute_minimal_change("abc", "abc", change));

    CHECK(compute_minimal_change("abc", "abXc", change));
    CHECK_EQ(change.begin, size_t(2));
    CHECK_EQ(change.end, size_t(2));
    CHECK_EQ(change.inserted, std::string("X"));

    CHECK(compute_minimal_change("abc", "ac", change));
    CHECK_EQ(change.begin, size_t(1));
    CHECK_EQ(change.end, size_t(2));
    CHECK_EQ(change.inserted, std::string());

    CHECK(compute_minimal_change("", "hello", change));
    CHECK_EQ(change.begin, size_t(0));
    CHECK_EQ(change.end, size_t(0));
    CHECK_EQ(change.inserted, std::string("hello"));

    CHECK(compute_minimal_change("hello", "", change));
    CHECK_EQ(change.end, size_t(5));
    CHECK(change.inserted.empty());
}

TEST_CASE("text_edit/apply change clamps and snaps") {
    std::string text = "中ab";
    TextChange change;
    change.begin = 1; // inside the 3-byte CJK sequence
    change.end = 1;
    change.inserted = "X";
    CHECK(apply_text_change(text, change));
    // The offset snaps to 0, so the insertion lands before the CJK character.
    CHECK_EQ(text, std::string("X中ab"));

    text = "abc";
    change.begin = 100;
    change.end = 200;
    change.inserted = "Z";
    CHECK(apply_text_change(text, change));
    CHECK_EQ(text, std::string("abcZ"));
}

TEST_CASE("text_edit/multiple changes apply back to front and detect overlap") {
    std::string text = "0123456789";
    std::vector<TextChange> changes = {
        {0, 2, "AA"},  // 01 -> AA
        {8, 10, "BB"}, // 89 -> BB
    };
    CHECK(apply_text_changes(text, changes));
    CHECK_EQ(text, std::string("AA234567BB"));

    std::string overlapping = "0123456789";
    CHECK(!apply_text_changes(overlapping, {{0, 5, "x"}, {3, 7, "y"}}));
    CHECK_EQ(overlapping, std::string("0123456789"));
}

TEST_CASE("text_edit/line endings") {
    std::string crlf = "a\r\nb\r\n";
    CHECK(normalize_newlines(crlf));
    CHECK_EQ(crlf, std::string("a\nb\n"));
    std::string lf = "a\nb\n";
    CHECK(!normalize_newlines(lf));

    CHECK_EQ(detect_line_ending("a\r\nb\r\n"), std::string("\r\n"));
    CHECK_EQ(detect_line_ending("a\nb\n"), std::string("\n"));
    CHECK_EQ(detect_line_ending("a\rb\r"), std::string("\r"));
    CHECK_EQ(detect_line_ending("no newlines"), std::string("\n"));

    CHECK_EQ(apply_line_ending("a\nb\n", "\r\n"), std::string("a\r\nb\r\n"));
    CHECK_EQ(apply_line_ending("a\nb\n", "\n"), std::string("a\nb\n"));
}

TEST_CASE("text_edit/line column conversion") {
    const std::string text = "one\ntwo\nthree";
    CHECK_EQ(offset_at_line_column(text, 1, 1), size_t(0));
    CHECK_EQ(offset_at_line_column(text, 2, 1), size_t(4));
    CHECK_EQ(offset_at_line_column(text, 2, 3), size_t(6));
    CHECK_EQ(offset_at_line_column(text, 3, 6), text.size());
    CHECK_EQ(offset_at_line_column(text, 99, 1), text.size());

    size_t line = 0;
    size_t column = 0;
    line_column_at_offset(text, 6, line, column);
    CHECK_EQ(line, size_t(2));
    CHECK_EQ(column, size_t(3));
}
