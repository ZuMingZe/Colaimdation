cmake_minimum_required(VERSION 3.24)

project(colaimdation
    VERSION 0.1.0
    DESCRIPTION "Local WYSIWYG Markdown editor with an embedded MCP server for Agent collaboration"
    LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Global settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# MSVC: UTF-8 sources, strict but practical warnings, fast debug info.
if(MSVC)
    add_compile_options(/utf-8 /W4 /permissive- /Zc:__cplusplus /MP /bigobj)
    add_compile_options(/wd4100) # unreferenced formal parameter
    add_compile_definitions(
        UNICODE _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS
    )
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_link_options(/DEBUG:FULL)
    endif()
else()
    add_compile_options(-Wall -Wextra -Wno-unused-parameter)
    add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

# ---------------------------------------------------------------------------
# WebView2 SDK (vendored under third_party/webview2, see tools/fetch_webview2.ps1)
# ---------------------------------------------------------------------------
option(COLAIMDATION_REQUIRE_WEBVIEW2 "Fail configuration when the WebView2 SDK is missing" OFF)

set(WEBVIEW2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/webview2" CACHE PATH "WebView2 SDK root")
set(WEBVIEW2_NATIVE "${WEBVIEW2_ROOT}/build/native")

find_path(WEBVIEW2_INCLUDE_DIR
    NAMES WebView2.h
    HINTS "${WEBVIEW2_NATIVE}/include"
    NO_DEFAULT_PATH)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(WEBVIEW2_ARCH_DIR "x64")
else()
    set(WEBVIEW2_ARCH_DIR "x86")
endif()

find_library(WEBVIEW2_LOADER_LIB
    NAMES WebView2Loader.dll
    HINTS "${WEBVIEW2_NATIVE}/${WEBVIEW2_ARCH_DIR}"
    NO_DEFAULT_PATH)

find_file(WEBVIEW2_LOADER_DLL
    NAMES WebView2Loader.dll
    HINTS "${WEBVIEW2_NATIVE}/${WEBVIEW2_ARCH_DIR}"
    NO_DEFAULT_PATH)

if(WEBVIEW2_INCLUDE_DIR AND WEBVIEW2_LOADER_LIB)
    set(COLAIMDATION_HAVE_WEBVIEW2 ON)
    message(STATUS "WebView2 SDK: ${WEBVIEW2_INCLUDE_DIR}")
    message(STATUS "WebView2 loader: ${WEBVIEW2_LOADER_LIB}")
else()
    set(COLAIMDATION_HAVE_WEBVIEW2 OFF)
    message(WARNING
        "WebView2 SDK not found under '${WEBVIEW2_NATIVE}'.\n"
        "  The native editor shell will be built WITHOUT the WebView2 front-end\n"
        "  (COLAIMDATION_NO_WEBVIEW2 is defined). Run tools/fetch_webview2.ps1 to vendor it.\n"
        "  The MCP server, document engine and tests build regardless.")
    if(COLAIMDATION_REQUIRE_WEBVIEW2)
        message(FATAL_ERROR "COLAIMDATION_REQUIRE_WEBVIEW2=ON but the WebView2 SDK is missing")
    endif()
endif()

# ---------------------------------------------------------------------------
# Core library: document engine, markdown AST, MCP server, security layer.
# Deliberately free of GUI dependencies so it can be unit tested headlessly.
# ---------------------------------------------------------------------------
add_library(colaimdation_core STATIC
    src/core/utf8.cpp
    src/core/json.cpp
    src/core/text_edit.cpp
    src/core/markdown.cpp
    src/core/document.cpp
    src/core/diff.cpp
    src/core/audit.cpp
    src/core/policy.cpp
    src/core/i18n.cpp
    src/core/docx.cpp
    src/core/settings.cpp
    src/core/workspace.cpp
    src/app/command_line.cpp
    src/mcp/mcp_server.cpp
    src/mcp/http_server.cpp
    src/mcp/stdio_server.cpp
    src/mcp/tool_registry.cpp
    src/mcp/tools_document.cpp
    src/mcp/tools_agent.cpp
)

target_include_directories(colaimdation_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_link_libraries(colaimdation_core PUBLIC ws2_32)

if(MSVC)
    target_compile_options(colaimdation_core PRIVATE /wd4996)
endif()

# ---------------------------------------------------------------------------
# Editor shell + MCP host executable
# ---------------------------------------------------------------------------
set(APP_SOURCES
    src/app/main.cpp
    src/app/editor_window.cpp
    src/app/webview_bridge.cpp
    src/app/win_util.cpp
)

# The application icon: ico/colaimdation.ico is compiled into both executables by a
# resource script, which is what gives them their icon in Explorer, the taskbar and
# the title bar (a runtime-only LoadImage cannot change the file's own icon).
# Regenerate the .ico from the artwork with `python tools/make_icon.py`.
if(WIN32)
    enable_language(RC)
    set(COLAI_APP_ICON_RC "${CMAKE_CURRENT_SOURCE_DIR}/src/app/app_icon.rc")
    set_source_files_properties("${COLAI_APP_ICON_RC}" PROPERTIES
        INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/ico;${CMAKE_CURRENT_SOURCE_DIR}/src/app")
else()
    set(COLAI_APP_ICON_RC "")
endif()

# The front-end assets are staged next to the executable.
#
# This used to be a POST_BUILD step on the executable alone. CMake cannot see that
# the copied files are inputs of that step, so editing app.js or editor.js did not
# make the executable dirty: it was not relinked, the POST_BUILD never ran, and
# build-core/bin/web silently kept serving an old front-end while the sources
# looked fixed. A stamp file is not enough either (a deleted or corrupted staged
# file does not re-run the copy), so the staging target always syncs - CMake's
# copy_directory skips files that are already identical, and tools/build.ps1
# verifies the result afterwards.
add_executable(colaimdation WIN32 ${APP_SOURCES} ${COLAI_APP_ICON_RC})
target_link_libraries(colaimdation PRIVATE
    colaimdation_core
    comctl32 comdlg32 ole32 oleaut32 shlwapi shell32 user32 gdi32 advapi32)

# End-user variant: no developer tools and no editor self-check in the menu (and no
# F12). Built into its own tree by `tools/build.ps1 -User`, into build-user/bin.
option(COLAI_USER_BUILD "Build the end-user variant (no developer tools, no self-test)" OFF)
if(COLAI_USER_BUILD)
    target_compile_definitions(colaimdation PRIVATE COLAI_USER_BUILD=1)
    message(STATUS "Colaimdation: building the end-user variant (no DevTools / self-test)")
endif()

add_custom_target(colaimdation_web_assets ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/web" "$<TARGET_FILE_DIR:colaimdation>/web"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/vendor" "$<TARGET_FILE_DIR:colaimdation>/web/vendor"
    COMMENT "Staging editor front-end assets")
add_dependencies(colaimdation colaimdation_web_assets)

if(COLAIMDATION_HAVE_WEBVIEW2)
    target_compile_definitions(colaimdation PRIVATE COLAIMDATION_HAVE_WEBVIEW2)
    target_include_directories(colaimdation PRIVATE "${WEBVIEW2_INCLUDE_DIR}")
    target_link_libraries(colaimdation PRIVATE "${WEBVIEW2_LOADER_LIB}")
    if(WEBVIEW2_LOADER_DLL)
        add_custom_command(TARGET colaimdation POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${WEBVIEW2_LOADER_DLL}" "$<TARGET_FILE_DIR:colaimdation>"
            COMMENT "Copying WebView2Loader.dll next to the editor executable")
    endif()
else()
    target_compile_definitions(colaimdation PRIVATE COLAIMDATION_NO_WEBVIEW2)
endif()

# ---------------------------------------------------------------------------
# MCP stdio bridge: lets a desktop Agent drive an already running editor.
# ---------------------------------------------------------------------------
add_executable(colaimdation_mcp_bridge src/app/mcp_bridge_main.cpp ${COLAI_APP_ICON_RC})
target_link_libraries(colaimdation_mcp_bridge PRIVATE colaimdation_core)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
option(COLAIMDATION_BUILD_TESTS "Build the unit test suite" ON)
if(COLAIMDATION_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

message(STATUS "colaimdation ${PROJECT_VERSION} | WebView2: ${COLAIMDATION_HAVE_WEBVIEW2} | tests: ${COLAIMDATION_BUILD_TESTS}")
