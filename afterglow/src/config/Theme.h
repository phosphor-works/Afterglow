// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <AfterglowVt/Terminal.h>

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <array>
#include <optional>

namespace Afterglow {

/// Colours from a theme file. Each one is optional: what a theme leaves out
/// keeps the value from the config.
struct Theme
{
    std::optional<AfterglowVt::Rgb> foreground;
    std::optional<AfterglowVt::Rgb> background;
    std::optional<AfterglowVt::Rgb> cursor;
    std::optional<AfterglowVt::Rgb> cursorText;
    std::optional<AfterglowVt::Rgb> selectionForeground;
    std::optional<AfterglowVt::Rgb> selectionBackground;
    std::optional<AfterglowVt::Rgb> divider;
    std::array<std::optional<AfterglowVt::Rgb>, 16> palette;
};

namespace Themes {

/// The theme name to use from @p spec: "name", or "light:a,dark:b" to follow
/// the desktop's colour scheme (either half may be left out).
QString pick(const QString& spec, bool dark);

/// Where themes live, most important first: the user's
/// ~/.config/afterglow/themes, then the system's, then the bundled ones.
QStringList searchDirs();

/// The file for theme @p name in @p dirs: name.json, name.conf or name. A
/// name with a slash is a path (~ for the home directory). Empty if missing.
QString locate(const QString& name, const QStringList& dirs);

/// Reads a theme: afterglow's JSON (the Colors keys), a Ghostty theme
/// ("palette = 0=#1d1f21", "cursor-color = ...") or a kitty one ("color0
/// #1d1f21", "cursor ..."). nullopt, with @p error set, if it has no colours.
std::optional<Theme> parse(const QByteArray& data, QString* error = nullptr);

} // namespace Themes

} // namespace Afterglow
