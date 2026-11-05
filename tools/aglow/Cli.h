// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/// Command lines in the form that kitten uses: --name=value, --name value,
/// -n value, -nvalue, flags that take no value (also several in one word, as
/// -xy), options given more than once, and -- before arguments that start
/// with a dash.
namespace Aglow::Cli {

struct Option
{
    enum class Kind { String, Flag, List, Choice, Int, Float };
    /// The long name and an optional short one, as "--direction -d".
    std::string names;
    Kind kind = Kind::String;
    std::string fallback; ///< the default value
    std::vector<std::string> choices;
    std::string help;
    /// The values that an Int or a Float option takes. The default bounds
    /// keep a value times 1000 (seconds to milliseconds) inside long long.
    double min = -1e15;
    double max = 1e15;
};

struct Command
{
    std::string name; ///< "transfer"
    std::string usage; ///< "source_files_or_directories destination_path"
    std::string summary;
    std::string help;
    std::vector<Option> options;
};

class Parsed
{
public:
    /// The value of a String, Choice, Int or Float option, or its default.
    /// parse() checked an Int or Float value against its kind and range.
    std::string value(std::string_view name) const;
    bool flag(std::string_view name) const;
    std::vector<std::string> list(std::string_view name) const;
    long long integer(std::string_view name) const;
    double real(std::string_view name) const;
    std::vector<std::string> args;

private:
    friend std::optional<Parsed> parse(const Command&, int, char**, std::string&, bool&);
    const Command* m_command = nullptr;
    std::map<std::string, std::vector<std::string>, std::less<>> m_values;
    const Option* find(std::string_view name) const;
};

/// The name that aglow runs under, for its usage and its messages: the last
/// component of @p argv0, as "afterglow" when `afterglow icat` runs it.
/// "aglow" until it is set, or when argv[0] names nothing.
void setProgram(std::string_view argv0);
const std::string& program();

/// Reads argv after the command's name. nullopt with @p error set for a bad
/// command line, or with @p help set for --help.
std::optional<Parsed> parse(const Command& command, int argc, char** argv, std::string& error, bool& help);
/// The help text: usage, description and options, wrapped at 80 columns.
std::string helpText(const Command& command);

} // namespace Aglow::Cli
