// colaimdation_mcp_bridge - speak MCP over stdio and forward everything to the
// running editor's loopback endpoint.
//
// Configure an Agent with:
//   command: colaimdation_mcp_bridge.exe
//   args:    []            (reads %LOCALAPPDATA%\colaimdation\mcp.json)
// or explicitly:
//   args:    ["--port", "41777", "--token", "<token from the editor>"]
#include "core/utf8.h"
#include "mcp/stdio_server.h"

#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// shellapi.h needs the base Windows types, so windows.h comes first.
#include <windows.h>
#include <shellapi.h>

namespace {

void print_usage() {
    std::fputs("colaimdation_mcp_bridge - MCP stdio bridge for the Colaimdation editor\n"
               "\n"
               "usage: colaimdation_mcp_bridge [options]\n"
               "\n"
               "options:\n"
               "  --port <n>            MCP port of a running editor\n"
               "  --token <value>       access token shown by the editor\n"
               "  --discovery <path>    read the endpoint from this file\n"
               "                        (default %LOCALAPPDATA%\\colaimdation\\mcp.json)\n"
               "  --quiet               do not relay editor notifications to stderr\n"
               "  --verbose             log every forwarded message to stderr\n"
               "  --help                show this message\n"
               "\n"
               "The bridge never reads or writes document files itself: it forwards JSON-RPC to\n"
               "the editor, which owns the document and enforces the security policy.\n",
               stderr);
}

std::vector<std::string> arguments_from(int argc, char** argv) {
    std::vector<std::string> arguments;
    arguments.reserve(static_cast<size_t>(argc));
    int wide_count = 0;
    LPWSTR* wide = ::CommandLineToArgvW(::GetCommandLineW(), &wide_count);
    if (wide) {
        for (int i = 1; i < wide_count; ++i) arguments.push_back(colai::utf8::from_wide(wide[i]));
        ::LocalFree(wide);
    } else {
        for (int i = 1; i < argc; ++i) arguments.push_back(argv[i]);
    }
    return arguments;
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> arguments = arguments_from(argc, argv);

    colai::mcp::StdioOptions options;
    for (size_t i = 0; i < arguments.size(); ++i) {
        const std::string& argument = arguments[i];
        auto next = [&](const char* name) -> std::string {
            if (i + 1 >= arguments.size()) {
                std::fprintf(stderr, "colaimdation_mcp_bridge: missing value for %s\n", name);
                std::exit(2);
            }
            return arguments[++i];
        };
        if (argument == "--help" || argument == "-h") {
            print_usage();
            return 0;
        }
        if (argument == "--port") {
            const std::string text = next("--port");
            // std::stoi throws on a non-numeric value and a value above 65535
            // used to wrap silently (70000 became 4464), so the bridge then
            // connected to the wrong port with a confusing error.
            int port = 0;
            try {
                size_t used = 0;
                port = std::stoi(text, &used);
                if (used != text.size()) port = -1;
            } catch (const std::exception&) {
                port = -1;
            }
            if (port < 0 || port > 65535) {
                std::fprintf(stderr,
                             "colaimdation_mcp_bridge: --port must be an integer between 0 and "
                             "65535 (got '%s')\n",
                             text.c_str());
                return 2;
            }
            options.port = static_cast<uint16_t>(port);
        } else if (argument == "--token") {
            options.token = next("--token");
        } else if (argument == "--discovery") {
            options.discovery_path = next("--discovery");
        } else if (argument == "--quiet") {
            options.forward_notifications = false;
        } else if (argument == "--verbose") {
            options.verbose = true;
        } else {
            std::fprintf(stderr, "colaimdation_mcp_bridge: unknown option '%s'\n", argument.c_str());
            print_usage();
            return 2;
        }
    }

    colai::mcp::StdioServer server;
    std::string error;
    if (!server.connect_endpoint(std::move(options), &error)) {
        std::fprintf(stderr, "colaimdation_mcp_bridge: %s\n", error.c_str());
        return 3;
    }
    if (server.options().verbose) {
        std::fprintf(stderr, "colaimdation_mcp_bridge: connected to 127.0.0.1:%u\n",
                     static_cast<unsigned>(server.options().port));
    }
    return server.run();
}
