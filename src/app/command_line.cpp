#include "app/command_line.h"

#include "core/i18n.h"
#include "core/json.h"
#include "core/utf8.h"
#include "core/workspace.h"

#include <algorithm>
#include <stdexcept>

namespace colai {
namespace {


std::string language_tag_list() {
    std::string out;
    for (const i18n::LanguageInfo& language : i18n::languages()) {
        if (!out.empty()) out += ", ";
        out += language.tag;
    }
    return out;
}

} 

CommandLineOptions parse_command_line_options(const std::vector<std::string>& arguments) {
    CommandLineOptions options;
    options.policy = Policy::defaults();

    for (size_t i = 0; i < arguments.size(); ++i) {
        const std::string argument = arguments[i];
        
        auto next = [&](const char* name) -> std::string {
            if (i + 1 >= arguments.size()) {
                throw std::runtime_error(
                    i18n::tr("cli.missing_value", {std::string(name)}));
            }
            return arguments[++i];
        };

        if (argument == "--help" || argument == "-h" || argument == "/?") {
            options.show_help = true;
        } else if (argument == "--file") {
            options.initial_file = next("--file");
        } else if (argument == "--port") {
            const std::string value = next("--port");
            try {
                const long long port = std::stoll(value);
                if (port < 0 || port > 65535) throw std::runtime_error(i18n::tr("cli.port.range"));
                options.mcp_port = static_cast<uint16_t>(port);
            } catch (const std::invalid_argument&) {
                throw std::runtime_error(i18n::tr("cli.port.number", {value}));
            } catch (const std::out_of_range&) {
                throw std::runtime_error(i18n::tr("cli.port.out_of_range", {value}));
            }
        } else if (argument == "--token") {
            options.policy.token = next("--token");
            options.token_pinned = true;
        } else if (argument == "--config") {
            options.config_path = next("--config");
        } else if (argument == "--read-only") {
            options.force_read_only = true;
        } else if (argument == "--confirm") {
            options.force_confirmation = true;
        } else if (argument == "--allow-dir") {
            options.policy.allowed_directory = next("--allow-dir");
        } else if (argument == "--demo") {
            options.demo_content = true;
        } else if (argument == "--no-demo") {
            options.demo_content = false;
        } else if (argument == "--theme") {
            const std::string value = next("--theme");
            options.theme = (value == "light" || value == "dark") ? value : "auto";
        } else if (argument == "--lang" || argument == "--language") {
            const std::string value = next("--lang");
            if (i18n::is_supported(value)) {
                options.language = i18n::normalize_tag(value);
            } else {
                options.language = i18n::kEnglish;
                options.warnings.push_back(
                    i18n::tr("cli.lang.unknown", {value, language_tag_list()}));
            }
        } else if (argument == "--font-size") {
            const std::string value = next("--font-size");
            try {
                options.font_size = std::stod(value);
            } catch (const std::exception&) {
                throw std::runtime_error(i18n::tr("cli.font.number", {value}));
            }
            if (options.font_size < 8.0 || options.font_size > 48.0) {
                throw std::runtime_error(i18n::tr("cli.font.range"));
            }
        } else if (argument == "--no-webview") {
            options.no_webview = true;
        } else if (argument == "--log") {
            options.log_to_file = true;
        } else if (argument.size() > 1 && argument[0] == '-') {
            throw std::runtime_error(i18n::tr("cli.unknown_option", {argument}));
        } else {
            
            options.initial_file = argument;
        }
    }

    
    if (options.policy.token.empty()) options.policy.token = Policy::generate_token();
    return options;
}

void load_policy_file(CommandLineOptions& options) {
    if (options.config_path.empty()) return;
    std::string contents;
    std::string error;
    if (!Workspace::read_text_file(options.config_path, &contents, &error).ok) {
        options.warnings.push_back(
            i18n::tr("cli.config.unreadable", {options.config_path, error}));
        return;
    }
    JsonParseError parse_error;
    auto parsed = json_parse(contents, &parse_error);
    if (!parsed) {
        options.warnings.push_back(i18n::tr("cli.config.malformed",
                                            {options.config_path, parse_error.message,
                                             std::to_string(parse_error.line)}));
        return;
    }
    options.policy.apply_json(*parsed, &options.warnings);
    if (options.policy.token.empty()) options.policy.token = Policy::generate_token();
}

const char* command_line_usage() {
    
    static std::string usage;
    usage = i18n::tr("cli.usage", {language_tag_list()});
    return usage.c_str();
}

} 
