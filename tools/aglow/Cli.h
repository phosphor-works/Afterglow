// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/// Command lines in the form that kitten uses: --name=value, --name value,
/// -n value, flags that take no value, options given more than once, and --
/// before arguments that start with a dash.
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

/// Reads argv after the command's name. nullopt with @p error set for a bad
/// command line, or with @p help set for --help.
std::optional<Parsed> parse(const Command& command, int argc, char** argv, std::string& error, bool& help);
/// The help text: usage, description and options, wrapped at 80 columns.
std::string helpText(const Command& command);

} // namespace Aglow::Cli
