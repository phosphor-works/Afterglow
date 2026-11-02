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
/// ~/.local/share on), then the bundled ones, then other terminals' themes:
/// Ghostty's (~/.config/ghostty/themes, then ghostty/themes in each XDG data
/// directory), Kitty's (~/.config/kitty/themes) and Konsole's (konsole in
/// each XDG data directory). A build-tree binary puts the checkout's themes
/// before the XDG data directories.
QStringList searchDirs();

/// Every file that could hold theme @p name, in search order: name.json,
/// name.conf, name.colorscheme and name in each of @p dirs. A name with a
/// slash is one path (~ for the home directory); a relative one is resolved
/// against @p base, the config file's directory.
QStringList candidates(const QString& name, const QStringList& dirs, const QString& base);

/// The first of candidates() that is a file. Empty if none is.
QString locate(const QString& name, const QStringList& dirs, const QString& base = {});

/// Whether @p name is a colour that a setting or theme can hold: a Qt colour
/// name or hex form, but not eight hex digits. Qt reads those as #AARRGGBB,
/// and these colours have no alpha, so "#1e1e2eff" would come out a
/// different colour with no warning.
bool isColorName(const QString& name);

/// Whether @p data is an Afterglow JSON theme rather than a Ghostty, kitty or
/// Konsole one: parse() decides the format by the content, not the file name.
bool isJson(const QByteArray& data);

/// Whether @p data is a Konsole color scheme: INI sections such as [Color0]
/// and [Foreground], each with a Color=r,g,b line.
bool isKonsole(const QByteArray& data);

/// Reads a theme: afterglow's JSON (one object with the colour keys
/// foreground, background, cursor, cursorText, selectionForeground,
/// selectionBackground, divider, the highlight colours and palette), a
/// Ghostty theme ("palette = 0=#1d1f21", "cursor-color = ..."), a kitty
/// one ("color0 #1d1f21", "cursor ...") or a Konsole color scheme ([Color0]
/// to [Color7], their Intense forms for 8 to 15, [Foreground] and
/// [Background]; Konsole's faint and intense text colours, wallpaper and
/// opacity have no Afterglow counterpart). nullopt, with @p error set, if it
/// has no colours. Keys it does not use go to @p ignored; keys it uses whose
/// value is not a colour (or, for visualBellIntensity, not 0-1; for palette,
/// not an array) go to @p invalid, palette entries as "palette[3]".
std::optional<Theme> parse(const QByteArray& data, QString* error = nullptr, QStringList* ignored = nullptr,
                           QStringList* invalid = nullptr);

/// Whether theme name @p name is "plasma" or "plasma:NAME": colours from the
/// desktop's colour scheme over theme NAME. @p base gets NAME, empty for
/// "plasma" alone.
bool isPlasma(const QString& name, QString* base = nullptr);

/// The kdeglobals files that hold the desktop's colour scheme, the system's
/// first and the user's last, which overrides them. The user's is listed even
/// when it does not exist yet, so that a watcher sees it appear.
QStringList plasmaSources();

/// The desktop colour scheme's background, foreground, selection colours and
/// cursor (its accent colour, with the background for the text under it):
/// [Colors:View] BackgroundNormal, ForegroundNormal and DecorationFocus and
/// [Colors:Selection] BackgroundNormal and ForegroundNormal from @p sources,
/// later files over earlier ones, as KDE reads them. A colour that no file
/// sets comes from the application palette (Base, Text, Highlight,
/// HighlightedText, Accent), so other desktops get their own colours too.
Theme plasmaColors(const QStringList& sources);

} // namespace Themes

} // namespace Afterglow
