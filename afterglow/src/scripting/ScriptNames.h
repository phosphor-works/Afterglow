// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QRegularExpression>
#include <QString>

namespace Afterglow {
/// Module, asset, pulse, shader pack and project names: 1 to 64 letters,
/// digits, _ and -. A dock id has the same characters, and at most 32
/// (ScriptHost checks the length).
inline bool validName(const QString& name)
{
    static const QRegularExpression re(QStringLiteral("\\A[A-Za-z0-9_-]{1,64}\\z"));
    return re.match(name).hasMatch();
}

/// Script and package ids, dependency names (the ids of packages) and
/// command names: names that do not start with -, which `aglow run`
/// would read as an option.
inline bool validScriptId(const QString& id)
{
    return validName(id) && !id.startsWith(QLatin1Char('-'));
}

/// The rule of validScriptId, for error messages.
inline constexpr QLatin1StringView ScriptIdRule{"1 to 64 letters, digits, _ and -, and must not start with -"};

/// What a command's text and an overlay's title must not hold: controls,
/// the line and paragraph separators, and the bidirectional controls. They
/// could break or reorder a help line, a palette entry, a popup's border or
/// the terminal line that shows them.
inline bool unprintable(QChar c)
{
    const char16_t u = c.unicode();
    return u < 0x20 || (u >= 0x7f && u < 0xa0) || u == 0x061c || u == 0x200e || u == 0x200f || u == 0x2028 || u == 0x2029
        || (u >= 0x202a && u <= 0x202e) || (u >= 0x2066 && u <= 0x2069);
}

/// A directory of a package's shaders directory that is a shader pack:
/// "shared" holds the packs' includes, and "none" names no pack. The
/// package loader and the shader library both decide by it.
inline bool packageShaderPack(const QString& directory)
{
    return validName(directory) && directory != QLatin1String("shared") && directory != QLatin1String("none");
}
}
