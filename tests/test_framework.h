// Colaimdation - minimal test harness.
//
// Deliberately dependency-free: the engine is tested through the same public
// API the MCP tools use, and ctest only needs a process exit code.
#pragma once

#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace colai::test {

struct TestCase {
    std::string name;
    std::function<void()> body;
};

class Registry {
public:
    static Registry& instance() {
        static Registry registry;
        return registry;
    }
    void add(std::string name, std::function<void()> body) {
        cases_.push_back({std::move(name), std::move(body)});
    }
    const std::vector<TestCase>& cases() const { return cases_; }

private:
    std::vector<TestCase> cases_;
};

struct Registrar {
    Registrar(const char* name, std::function<void()> body) {
        Registry::instance().add(name, std::move(body));
    }
};

// Thrown by the assertion macros; caught by the runner per test case.
struct AssertionFailure {
    std::string message;
};

inline void fail(const std::string& file, int line, const std::string& expression,
                 const std::string& detail) {
    std::ostringstream stream;
    stream << file << ":" << line << ": " << expression;
    if (!detail.empty()) stream << "  [" << detail << "]";
    throw AssertionFailure{stream.str()};
}

// ---------------------------------------------------------------------------
// Value description for assertion messages.
//
// The generic form exists only when the type is streamable, so a CHECK_EQ on a
// non-streamable type still compiles (worst case the detail is generic). The
// std::string overload also makes CHECK_EQ(std::string, "literal") work without
// an ambiguous user-defined conversion.
// ---------------------------------------------------------------------------
template <typename T, typename = void> struct SafeDescribe {
    static constexpr bool supported = false;
    static std::string get(const T&) { return "<value>"; }
};

template <typename T>
struct SafeDescribe<T, std::void_t<decltype(std::declval<std::ostream&>() << std::declval<const T&>())>> {
    static constexpr bool supported = true;
    static std::string get(const T& value) {
        std::ostringstream stream;
        stream << value;
        return stream.str();
    }
};

inline std::string describe(const std::string& value) { return "\"" + value + "\""; }
inline std::string describe(const char* value) {
    return value ? std::string("\"") + value + "\"" : std::string("(null)");
}
inline std::string describe(bool value) { return value ? "true" : "false"; }

template <typename T> std::string describe(const T& value) {
    return SafeDescribe<T>::get(value);
}

int run_all(const std::string& filter);

// Directory holding the project sources. Tests that need to inspect shipped
// assets (language packs) ask for it here: the path contains characters that
// cannot survive a compile-time string literal, so it is passed at run time.
void set_source_directory(std::string path);
const std::string& source_directory();

} // namespace colai::test

#define COLAI_CONCAT_INNER(a, b) a##b
#define COLAI_CONCAT(a, b) COLAI_CONCAT_INNER(a, b)

#define TEST_CASE(name)                                                                      \
    static void COLAI_CONCAT(cola_test_body_, __LINE__)();                                   \
    static ::colai::test::Registrar COLAI_CONCAT(cola_test_reg_, __LINE__)(                  \
        name, COLAI_CONCAT(cola_test_body_, __LINE__));                                      \
    static void COLAI_CONCAT(cola_test_body_, __LINE__)()

#define CHECK(condition)                                                                     \
    do {                                                                                     \
        if (!(condition)) {                                                                  \
            ::colai::test::fail(__FILE__, __LINE__, #condition, "");                         \
        }                                                                                    \
    } while (false)

#define CHECK_EQ(actual, expected)                                                           \
    do {                                                                                     \
        auto cola_actual = (actual);                                                         \
        auto cola_expected = (expected);                                                     \
        if (!(cola_actual == cola_expected)) {                                               \
            ::colai::test::fail(__FILE__, __LINE__, #actual " == " #expected,                \
                                ::colai::test::describe(cola_actual) + " != " +              \
                                    ::colai::test::describe(cola_expected));                 \
        }                                                                                    \
    } while (false)

#define CHECK_NE(actual, unexpected)                                                         \
    do {                                                                                     \
        if ((actual) == (unexpected)) {                                                      \
            ::colai::test::fail(__FILE__, __LINE__, #actual " != " #unexpected, "");         \
        }                                                                                    \
    } while (false)

#define CHECK_STR_CONTAINS(haystack, needle)                                                 \
    do {                                                                                     \
        const std::string cola_haystack = (haystack);                                        \
        const std::string cola_needle = (needle);                                            \
        if (cola_haystack.find(cola_needle) == std::string::npos) {                          \
            ::colai::test::fail(__FILE__, __LINE__,                                          \
                                "string contains " #needle,                                  \
                                "not found in: " + cola_haystack);                           \
        }                                                                                    \
    } while (false)

#define CHECK_THROWS(expression)                                                             \
    do {                                                                                     \
        bool cola_threw = false;                                                             \
        try {                                                                                \
            (void)(expression);                                                              \
        } catch (...) {                                                                      \
            cola_threw = true;                                                               \
        }                                                                                    \
        if (!cola_threw) {                                                                   \
            ::colai::test::fail(__FILE__, __LINE__, #expression " throws", "did not throw"); \
        }                                                                                    \
    } while (false)
