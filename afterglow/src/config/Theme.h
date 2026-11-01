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
/// keeps the value from the config, except the divider, which a theme without
/// one derives from its own background and foreground.
struct Theme
{
    std::optional<AfterglowVt::Rgb> foreground;
    std::optional<AfterglowVt::Rgb> background;
    std::optional<AfterglowVt::Rgb> cursor;
    std::optional<AfterglowVt::Rgb> cursorText;
    std::optional<AfterglowVt::Rgb> selectionForeground;
    std::optional<AfterglowVt::Rgb> selectionBackground;
    std::optional<AfterglowVt::Rgb> divider;
    std::optional<AfterglowVt::Rgb> searchMatch;
    std::optional<AfterglowVt::Rgb> searchCurrentMatch;
    std::optional<AfterglowVt::Rgb> searchMatchText;
    std::optional<AfterglowVt::Rgb> visualBell;
    std::optional<double> visualBellIntensity; ///< 0-1
    std::array<std::optional<AfterglowVt::Rgb>, 16> palette;
};

namespace Themes {

/// The theme name to use from @p spec: "name", or "light:a,dark:b" to follow
/// the desktop's colour scheme (either half may be left out, and either may
/// be a path). A spec without a light: or dark: part is one name or path.
QString pick(const QString& spec, bool dark);

/// Where themes live, most important first: the user's
/// ~/.config/afterglow/themes, then the XDG data directories' (from
/// ~/.local/share on), then the bundled ones. A build-tree binary puts the
/// checkout's themes before the XDG data directories.
QStringList searchDirs();

/// Every file that could hold theme @p name, in search order: name.json,
/// name.conf and name in each of @p dirs. A name with a slash is one path
/// (~ for the home directory); a relative one is resolved against @p base,
/// the config file's directory.
QStringList candidates(const QString& name, const QStringList& dirs, const QString& base);

/// The first of candidates() that is a file. Empty if none is.
QString locate(const QString& name, const QStringList& dirs, const QString& base = {});

/// Whether @p name is a colour that a setting or theme can hold: a Qt colour
/// name or hex form, but not eight hex digits. Qt reads those as #AARRGGBB,
/// and these colours have no alpha, so "#1e1e2eff" would come out a
/// different colour with no warning.
bool isColorName(const QString& name);

/// Reads a theme: afterglow's JSON (one object with the colour keys
/// foreground, background, cursor, cursorText, selectionForeground,
/// selectionBackground, divider, the highlight colours and palette), a
/// Ghostty theme ("palette = 0=#1d1f21", "cursor-color = ...") or a kitty
/// one ("color0 #1d1f21", "cursor ..."). nullopt, with @p error set, if it
/// has no colours. Keys it does not use go to @p ignored; keys it uses whose
/// value is not a colour (or, for visualBellIntensity, not 0-1) go to
/// @p invalid, palette entries as "palette[3]".
std::optional<Theme> parse(const QByteArray& data, QString* error = nullptr, QStringList* ignored = nullptr,
                           QStringList* invalid = nullptr);

} // namespace Themes

} // namespace Afterglow
