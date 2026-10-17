add_executable(colaimdation_tests
    test_main.cpp
    test_json.cpp
    test_utf8.cpp
    test_markdown.cpp
    test_document.cpp
    test_diff.cpp
    test_audit_policy.cpp
    test_workspace.cpp
    test_mcp.cpp
    test_command_line.cpp
    test_i18n.cpp
    test_regression.cpp
    test_export.cpp
)

target_include_directories(colaimdation_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src")

target_link_libraries(colaimdation_tests PRIVATE colaimdation_core)

if(MSVC)
    target_compile_options(colaimdation_tests PRIVATE /wd4996)
endif()

# The i18n tests read the shipped language packs at run time. The project path
# contains non-ASCII characters, which cannot round-trip through a compile-time
# string literal, so it is passed as an argument instead.
set(COLAI_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")

add_test(NAME colaimdation_tests
         COMMAND colaimdation_tests "--source-dir=${COLAI_SOURCE_DIR}")

# Grouped shorthands so a single area can be exercised from the command line.
foreach(area json utf8 markdown document diff audit policy workspace mcp cli i18n regression export)
    add_test(NAME colaimdation_${area}
             COMMAND colaimdation_tests "--source-dir=${COLAI_SOURCE_DIR}" "--filter=${area}")
endforeach()

# Front-end checks. The web assets have no build step, so these are the only
# things that catch a broken render.js/editor.js before it reaches WebView2.
# Skipped (not failed) when Node.js is unavailable.
find_program(COLAI_NODE_EXECUTABLE NAMES node)
if(COLAI_NODE_EXECUTABLE)
    add_test(NAME colaimdation_web
             COMMAND "${COLAI_NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/web_probe.js")
    add_test(NAME colaimdation_web_typing
             COMMAND "${COLAI_NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/web_typing.js")
    # The formatting toolbar: every button has to write valid Markdown and leave
    # the caret somewhere the writer can carry on typing.
    add_test(NAME colaimdation_web_format
             COMMAND "${COLAI_NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/web_format.js")
    # Drives the real DOM in headless Chrome/Edge: the only way to test how the
    # editor behaves with the line-break structure a browser actually produces
    # (Enter makes a <div>, not a '\n'). Skips itself when no browser is found.
    add_test(NAME colaimdation_web_browser
             COMMAND "${COLAI_NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/web_browser.js")
else()
    message(STATUS "Node.js not found: skipping the colaimdation_web front-end tests")
endif()
