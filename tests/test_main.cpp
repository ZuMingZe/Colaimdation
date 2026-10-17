#include "test_framework.h"

#include <cstring>

namespace colai::test {
namespace {
std::string g_source_directory;
}

void set_source_directory(std::string path) { g_source_directory = std::move(path); }
const std::string& source_directory() { return g_source_directory; }

int run_all(const std::string& filter) {
    // Unbuffered: if a test crashes the process, the output still shows which one
    // got that far (a buffered stream loses everything on an access violation).
    std::cout << std::unitbuf;
    int failures = 0;
    int executed = 0;
    for (const TestCase& test : Registry::instance().cases()) {
        if (!filter.empty() && test.name.find(filter) == std::string::npos) continue;
        ++executed;
        try {
            test.body();
            std::cout << "[  ok  ] " << test.name << "\n";
        } catch (const AssertionFailure& failure) {
            ++failures;
            std::cout << "[ FAIL ] " << test.name << "\n         " << failure.message << "\n";
        } catch (const std::exception& error) {
            ++failures;
            std::cout << "[ FAIL ] " << test.name << "\n         unexpected exception: "
                      << error.what() << "\n";
        } catch (...) {
            ++failures;
            std::cout << "[ FAIL ] " << test.name << "\n         unknown exception\n";
        }
    }
    std::cout << "\n" << (executed - failures) << "/" << executed << " tests passed";
    if (failures > 0) std::cout << " (" << failures << " failed)";
    std::cout << std::endl;
    return failures == 0 ? 0 : 1;
}

} // namespace colai::test

int main(int argc, char** argv) {
    std::string filter;
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--filter=", 9) == 0) {
            filter = argv[i] + 9;
        } else if (std::strncmp(argv[i], "--source-dir=", 13) == 0) {
            colai::test::set_source_directory(argv[i] + 13);
        }
    }
    return colai::test::run_all(filter);
}
