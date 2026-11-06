// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "Cli.h"
#include "Tty.h"

#include <algorithm>
#include <charconv>
#include <cmath>
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
    // Each line of the source text is a paragraph or a line of its own.
    std::string out;
    std::istringstream paragraphs { std::string(text) };
    std::string line;
    while (std::getline(paragraphs, line)) {
        // Lines that start with spaces are examples: kept as they are.
        if (line.empty() || line.starts_with("  ")) {
            out += (line.empty() ? std::string() : std::string(indent, ' ') + line) + "\n";
            continue;
        }
        std::string current(indent, ' ');
        bool empty = true;
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
        if (!empty) {
            out += current + "\n";
        }
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

namespace {

std::string& programName()
{
    static std::string name = "aglow";
    return name;
}

// A bound as the user types it: an Int bound in full (4294967295, not
// 4.29497e+09), a Float one in its shortest form, with "." in any locale.
std::string formatNumber(const Option& option, double value)
{
    if (option.kind == Option::Kind::Int) {
        return std::to_string(std::llround(value));
    }
    char text[32];
    const auto [end, ec] = std::to_chars(text, text + sizeof text, value);
    return ec == std::errc() ? std::string(text, end) : std::string();
}

// Checks @p value against the kind and the range of @p option.
bool checkValue(const Option& option, const std::string& name, const std::string& value, std::string& error)
{
    if (option.kind == Option::Kind::Choice
        && std::find(option.choices.begin(), option.choices.end(), value) == option.choices.end()) {
        std::string allowed;
        for (const auto& c : option.choices) {
            allowed += (allowed.empty() ? "" : ", ") + c;
        }
        error = "The value of " + name + " must be one of " + allowed;
        return false;
    }
    double number = 0;
    if (option.kind == Option::Kind::Int) {
        long long integer = 0;
        const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), integer);
        if (ec != std::errc() || end != value.data() + value.size()) {
            error = "Not an integer for " + name + ": " + value;
            return false;
        }
        number = double(integer);
    } else if (option.kind == Option::Kind::Float) {
        // from_chars, not strtod: the locale cannot change the decimal point,
        // and "inf", "nan" and hex floats are no numbers here.
        const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), number, std::chars_format::fixed);
        if (ec != std::errc() || end != value.data() + value.size() || !std::isfinite(number)) {
            error = "Not a number for " + name + ": " + value;
            return false;
        }
    } else {
        return true;
    }
    if (number < option.min || number > option.max) {
        error = "The value of " + name + " must be from " + formatNumber(option, option.min) + " to "
            + formatNumber(option, option.max) + ", not " + value;
        return false;
    }
    return true;
}

} // namespace

void setProgram(std::string_view argv0)
{
    const auto slash = argv0.rfind('/');
    const auto base = slash == std::string_view::npos ? argv0 : argv0.substr(slash + 1);
    // printable(): the name goes into the help and the messages, and a link
    // or a script can give argv[0] any bytes.
    programName() = base.empty() ? std::string("aglow") : printable(base);
}

const std::string& program() { return programName(); }

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
    // The value of @p option, given as @p name: inline, or the next argument.
    // A next argument that is an option means that the value was left out.
    auto valueOf = [&](const Option& option, const std::string& name, const std::optional<std::string>& inlineValue,
                       int& i) -> bool {
        std::string value;
        if (inlineValue) {
            value = *inlineValue;
        } else if (i + 1 < argc && !std::string_view(argv[i + 1]).starts_with("--")) {
            value = argv[++i];
        } else {
            // -d=VALUE would give the short option the value "=VALUE".
            error = "The option " + name + " needs a value. A value that starts with -- goes as " + name
                + (name.starts_with("--") ? "=VALUE" : "VALUE");
            return false;
        }
        if (!checkValue(option, name, value, error)) {
            return false;
        }
        parsed.m_values[key(option)].push_back(value);
        return true;
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
        if (arg.starts_with("--")) {
            std::string name(arg);
            std::optional<std::string> inlineValue;
            if (const auto eq = arg.find('='); eq != std::string_view::npos) {
                name = std::string(arg.substr(0, eq));
                inlineValue = std::string(arg.substr(eq + 1));
            }
            const Option* option = lookup(name);
            if (!option) {
                error = "Unknown option: " + name;
                return std::nullopt;
            }
            if (option->kind != Option::Kind::Flag) {
                if (!valueOf(*option, name, inlineValue, i)) {
                    return std::nullopt;
                }
                continue;
            }
            const auto v = inlineValue.value_or("y");
            if (v != "y" && v != "yes" && v != "true" && v != "n" && v != "no" && v != "false") {
                error = "Invalid value for " + name + ": " + v;
                return std::nullopt;
            }
            parsed.m_values[key(*option)].push_back(v == "y" || v == "yes" || v == "true" ? "y" : "n");
            continue;
        }
        // Short options: -x, -xy (flags together), -dvalue, and -xd value
        // (flags, then an option whose value is the rest or the next argument).
        for (std::size_t at = 1; at < arg.size(); ++at) {
            const std::string name = std::string("-") + arg[at];
            const Option* option = lookup(name);
            if (!option) {
                error = "Unknown option: " + (at == 1 ? std::string(arg) : name + " in " + std::string(arg));
                return std::nullopt;
            }
            if (option->kind == Option::Kind::Flag) {
                parsed.m_values[key(*option)].push_back("y");
                continue;
            }
            std::optional<std::string> inlineValue;
            if (at + 1 < arg.size()) {
                inlineValue = std::string(arg.substr(at + 1));
            }
            if (!valueOf(*option, name, inlineValue, i)) {
                return std::nullopt;
            }
            break;
        }
    }
    return parsed;
}

std::string helpText(const Command& command)
{
    std::string out = "Usage: " + program() + " " + command.name + " [options] " + command.usage + "\n\n";
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
        const bool numeric = option.kind == Option::Kind::Int || option.kind == Option::Kind::Float;
        if (numeric && (option.min != Option {}.min || option.max != Option {}.max)) {
            text += "\nFrom " + formatNumber(option, option.min) + " to " + formatNumber(option, option.max) + ".";
        }
        if (!option.fallback.empty() && option.kind != Option::Kind::Flag) {
            text += "\nDefault: " + option.fallback;
        }
        out += wrap(text, 6) + "\n";
    }
    out += "  -h, --help\n      Show this help.\n";
    return out;
}

} // namespace Aglow::Cli
