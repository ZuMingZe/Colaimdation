// JSON parser / serializer round-trip tests.
#include "test_framework.h"

#include "core/json.h"

using namespace colai;

TEST_CASE("json/parse scalars") {
    auto null_value = json_parse("null");
    CHECK(null_value.has_value());
    CHECK(null_value->is_null());

    CHECK(json_parse("true")->as_bool());
    CHECK(!json_parse("false")->as_bool());
    CHECK_EQ(json_parse("42")->as_int(), 42);
    CHECK_EQ(json_parse("-17")->as_int(), -17);
    CHECK_EQ(json_parse("3.5")->as_double(), 3.5);
    CHECK_EQ(json_parse("1e3")->as_double(), 1000.0);
    CHECK_EQ(json_parse("\"hi\"")->as_string(), std::string("hi"));
}

TEST_CASE("json/parse large integers stay exact") {
    auto value = json_parse("9007199254740993");
    CHECK(value.has_value());
    CHECK(value->is_int());
    CHECK_EQ(value->as_int(), 9007199254740993LL);
}

TEST_CASE("json/parse nested structures") {
    const char* text = R"({"a":[1,2,{"b":"x"}],"c":{"d":null},"e":true})";
    auto value = json_parse(text);
    CHECK(value.has_value());
    CHECK(value->is_object());
    CHECK_EQ(value->size(), size_t(3));
    CHECK_EQ((*value)["a"].size(), size_t(3));
    CHECK_EQ((*value)["a"][2]["b"].as_string(), std::string("x"));
    CHECK((*value)["c"]["d"].is_null());
    CHECK((*value)["e"].as_bool());
    // Member order is preserved.
    CHECK_EQ(value->members().entries().front().first, std::string("a"));
}

TEST_CASE("json/parse escapes and unicode") {
    auto value = json_parse(R"("line\nbreak\ttab\"quote\\slash")");
    CHECK(value.has_value());
    CHECK_EQ(value->as_string(), std::string("line\nbreak\ttab\"quote\\slash"));

    // \u escapes, including a surrogate pair for an emoji.
    auto cjk = json_parse(R"("\u4e2d\u6587")");
    CHECK(cjk.has_value());
    CHECK_EQ(cjk->as_string(), std::string("\xe4\xb8\xad\xe6\x96\x87"));
    CHECK_EQ(cjk->as_string().size(), size_t(6));

    auto emoji = json_parse(R"("\ud83d\ude00")");
    CHECK(emoji.has_value());
    CHECK_EQ(emoji->as_string(), std::string("\xf0\x9f\x98\x80"));
}

TEST_CASE("json/parse errors report position") {
    JsonParseError error;
    CHECK(!json_parse("{\"a\":1", &error).has_value());
    CHECK(!error.message.empty());
    CHECK_EQ(error.line, size_t(1));

    CHECK(!json_parse("[1,2,]", &error).has_value());
    CHECK(!json_parse("{\"a\" 1}", &error).has_value());
    CHECK(!json_parse("nul", &error).has_value());
    CHECK(!json_parse("01", &error).has_value());
    CHECK(!json_parse("\"unterminated", &error).has_value());
    CHECK(!json_parse("{} trailing", &error).has_value());
}

TEST_CASE("json/round trip") {
    const std::string original =
        R"({"s":"中文 with \"quotes\"\n","n":-12.5,"i":7,"b":false,"z":null,"a":[1,[2,3]],"o":{"k":"v"}})";
    auto parsed = json_parse(original);
    CHECK(parsed.has_value());
    const std::string dumped = parsed->dump(false);
    auto reparsed = json_parse(dumped);
    CHECK(reparsed.has_value());
    CHECK_EQ(reparsed->dump(false), dumped);
    CHECK_EQ((*reparsed)["s"].as_string(), std::string("中文 with \"quotes\"\n"));
}

TEST_CASE("json/pretty printing is stable") {
    Json value = Json::object();
    value.set("b", Json(2));
    value.set("a", Json("x"));
    const std::string pretty = value.dump(true);
    CHECK_STR_CONTAINS(pretty, "\n  \"b\": 2");
    CHECK_EQ(value.dump(false), std::string(R"({"b":2,"a":"x"})"));
}

TEST_CASE("json/typed accessors fall back safely") {
    auto value = json_parse(R"({"i":"12","b":1,"d":2.5})");
    CHECK(value.has_value());
    CHECK_EQ(value->integer("i"), 12);
    CHECK_EQ(value->integer("missing", -1), -1);
    CHECK(value->boolean("b"));
    CHECK_EQ(value->number("d"), 2.5);
    CHECK_EQ(value->str("nope", "fallback"), std::string("fallback"));
    CHECK((*value)["absent"].is_null());
}

TEST_CASE("json/array building") {
    Json array = Json::array();
    array.push(Json(1));
    array.push(Json("two"));
    array.push(Json::object());
    CHECK_EQ(array.size(), size_t(3));
    CHECK_EQ(array.dump(false), std::string(R"([1,"two",{}])"));
}

TEST_CASE("json/bom and whitespace tolerated") {
    std::string with_bom = "\xEF\xBB\xBF  {\"a\" : 1 }  ";
    auto value = json_parse(with_bom);
    CHECK(value.has_value());
    CHECK_EQ(value->integer("a"), 1);
}

TEST_CASE("json/depth limit is enforced") {
    std::string deep;
    for (int i = 0; i < 400; ++i) deep.push_back('[');
    JsonParseError error;
    CHECK(!json_parse(deep, &error).has_value());
    CHECK_STR_CONTAINS(error.message, "nesting");
}
