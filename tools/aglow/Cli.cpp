// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "Cli.h"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <sstream>

namespace Aglow::Cli {

namespace {

std::vector<std::string> split(std::string_view names)
{
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start < names.size()) {
        const auto end = std::min(names.find(' ', start), names.size());
        if (end > start) {
            out.emplace_back(names.substr(start, end - start));
        }
        start = end + 1;
    }
    return out;
}

// The key of an option: its long name without the dashes.
std::string key(const Option& option) { return split(option.names).front().substr(2); }

std::string wrap(std::string_view text, std::size_t indent, std::size_t width = 80)
{
    std::string out;
    std::istringstream paragraphs { std::string(text) };
    std::string line;
    std::string current(indent, ' ');
    bool empty = true;
    while (std::getline(paragraphs, line)) {
        if (line.empty()) {
            if (!empty) {
                out += current + "\n";
            }
            out += "\n";
            current.assign(indent, ' ');
            empty = true;
            continue;
        }
        // Lines that start with spaces are examples: kept as they are.
        if (line.starts_with("  ")) {
            if (!empty) {
                out += current + "\n";
            }
            out += std::string(indent, ' ') + line + "\n";
            current.assign(indent, ' ');
            empty = true;
            continue;
        }
        std::istringstream words(line);
        std::string word;
        while (words >> word) {
            if (!empty && current.size() + 1 + word.size() > width) {
                out += current + "\n";
                current.assign(indent, ' ');
                empty = true;
            }
            current += (empty ? "" : " ") + word;
            empty = false;
        }
        // Each line of the source text is a paragraph or a line of its own.
        if (!empty) {
            out += current + "\n";
            current.assign(indent, ' ');
            empty = true;
        }
    }
    if (!empty) {
        out += current + "\n";
    }
    return out;
}

} // namespace

const Option* Parsed::find(std::string_view name) const
{
    for (const auto& option : m_command->options) {
        if (key(option) == name) {
            return &option;
        }
    }
    return nullptr;
}

std::string Parsed::value(std::string_view name) const
{
    const auto it = m_values.find(name);
    if (it != m_values.end() && !it->second.empty()) {
        return it->second.back();
    }
    const auto* option = find(name);
    return option ? option->fallback : std::string();
}

bool Parsed::flag(std::string_view name) const
{
    const auto it = m_values.find(name);
    return it != m_values.end() && !it->second.empty() && it->second.back() == "y";
}

std::vector<std::string> Parsed::list(std::string_view name) const
{
    const auto it = m_values.find(name);
    return it == m_values.end() ? std::vector<std::string>() : it->second;
}

long long Parsed::integer(std::string_view name) const { return std::strtoll(value(name).c_str(), nullptr, 10); }

double Parsed::real(std::string_view name) const { return std::strtod(value(name).c_str(), nullptr); }

std::optional<Parsed> parse(const Command& command, int argc, char** argv, std::string& error, bool& help)
{
    Parsed parsed;
    parsed.m_command = &command;
    help = false;
    auto lookup = [&](std::string_view name) -> const Option* {
        for (const auto& option : command.options) {
            for (const auto& n : split(option.names)) {
                if (n == name) {
                    return &option;
                }
            }
        }
        return nullptr;
    };
    bool optionsDone = false;
    for (int i = 0; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (optionsDone || arg == "-" || !arg.starts_with('-')) {
            parsed.args.emplace_back(arg);
            continue;
        }
        if (arg == "--") {
            optionsDone = true;
            continue;
        }
        if (arg == "--help" || arg == "-h") {
            help = true;
            return std::nullopt;
        }
        std::string name(arg);
        std::optional<std::string> inlineValue;
        if (arg.starts_with("--")) {
            if (const auto eq = arg.find('='); eq != std::string_view::npos) {
                name = std::string(arg.substr(0, eq));
                inlineValue = std::string(arg.substr(eq + 1));
            }
        } else if (arg.size() > 2) {
            // -dsend: a short option with its value attached.
            name = std::string(arg.substr(0, 2));
            inlineValue = std::string(arg.substr(2));
        }
        const Option* option = lookup(name);
        if (!option) {
            error = "Unknown option: " + name;
            return std::nullopt;
        }
        const auto k = key(*option);
        if (option->kind == Option::Kind::Flag) {
            if (inlineValue && name.starts_with("--")) {
                const auto& v = *inlineValue;
                if (v != "y" && v != "yes" && v != "true" && v != "n" && v != "no" && v != "false") {
                    error = "Invalid value for " + name + ": " + v;
                    return std::nullopt;
                }
                parsed.m_values[k].push_back(v == "y" || v == "yes" || v == "true" ? "y" : "n");
            } else if (inlineValue) {
                // -xc: several short flags together.
                parsed.m_values[k].push_back("y");
                for (const char c : *inlineValue) {
                    const Option* next = lookup(std::string("-") + c);
                    if (!next || next->kind != Option::Kind::Flag) {
                        error = std::string("Unknown option: -") + c;
                        return std::nullopt;
                    }
                    parsed.m_values[key(*next)].push_back("y");
                }
            } else {
                parsed.m_values[k].push_back("y");
            }
            continue;
        }
        std::string value;
        if (inlineValue) {
            value = *inlineValue;
        } else if (i + 1 < argc) {
            value = argv[++i];
        } else {
            error = "The option " + name + " needs a value";
            return std::nullopt;
        }
        if (option->kind == Option::Kind::Choice
            && std::find(option->choices.begin(), option->choices.end(), value) == option->choices.end()) {
            std::string allowed;
            for (const auto& c : option->choices) {
                allowed += (allowed.empty() ? "" : ", ") + c;
            }
            error = name + " must be one of: " + allowed;
            return std::nullopt;
        }
        if (option->kind == Option::Kind::Int) {
            long long number = 0;
            const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), number);
            if (ec != std::errc() || end != value.data() + value.size()) {
                error = "Not an integer for " + name + ": " + value;
                return std::nullopt;
            }
        }
        if (option->kind == Option::Kind::Float) {
            char* end = nullptr;
            std::strtod(value.c_str(), &end);
            if (value.empty() || *end) {
                error = "Not a number for " + name + ": " + value;
                return std::nullopt;
            }
        }
        parsed.m_values[k].push_back(value);
    }
    return parsed;
}

std::string helpText(const Command& command)
{
    std::string out = "Usage: aglow " + command.name + " [options] " + command.usage + "\n\n";
    out += wrap(command.summary, 0) + "\n";
    if (!command.help.empty()) {
        out += wrap(command.help, 0) + "\n";
    }
    out += "Options:\n";
    for (const auto& option : command.options) {
        auto names = split(option.names);
        std::string head = "  ";
        for (std::size_t i = 0; i < names.size(); ++i) {
            head += (i ? ", " : "") + names[i];
        }
        if (option.kind != Option::Kind::Flag) {
            head += option.kind == Option::Kind::Choice ? " {" : " ";
            if (option.kind == Option::Kind::Choice) {
                for (std::size_t i = 0; i < option.choices.size(); ++i) {
                    head += (i ? "," : "") + option.choices[i];
                }
                head += "}";
            } else {
                head += "VALUE";
            }
        }
        out += head + "\n";
        std::string text = option.help;
        if (!option.fallback.empty() && option.kind != Option::Kind::Flag) {
            text += "\nDefault: " + option.fallback;
        }
        out += wrap(text, 6) + "\n";
    }
    out += "  -h, --help\n      Show this help.\n";
    return out;
}

} // namespace Aglow::Cli
