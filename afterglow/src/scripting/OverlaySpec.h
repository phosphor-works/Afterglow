// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <optional>

namespace Afterglow {

/// What an overlay runs over a pane: the overlay effect of scripts, and the
/// overlay call of the control socket, which share these rules.
struct OverlaySpec
{
    /// Where the program's answer comes from: its standard output, the file
    /// that AFTERGLOW_RESULT names, or nowhere.
    enum class Capture { Stdout, File, None };

    QStringList argv;
    /// Where it starts. Not a local directory (a remote shell's path): $HOME.
    QString cwd;
    /// A string sets the variable, false unsets it.
    QVariantMap env;
    /// Its standard input; unset reads the terminal.
    std::optional<QByteArray> input;
    Capture capture = Capture::Stdout;
    /// Shows as the pane's title while the overlay runs, and in the top
    /// edge of its border.
    QString title;

    /// A size along one side of a popup: cells, or a percentage of the host
    /// pane's grid. It counts the border.
    struct Extent
    {
        int value = 0;
        bool percent = false;
        bool operator==(const Extent&) const = default;
    };
    /// Where a popup sits in the host pane's grid.
    enum class Position { Center, Top, Bottom, Left, Right, TopLeft, TopRight, BottomLeft, BottomRight };
    /// Unset: the whole grid of the host pane.
    std::optional<Extent> width;
    std::optional<Extent> height;
    Position position = Position::Center;
    /// Unset: a border for a popup, none for a full-pane overlay.
    std::optional<bool> border;

    /// A width or a height makes the overlay a popup over its host pane.
    bool sized() const { return width || height; }
    bool framed() const { return border.value_or(sized()); }
    /// The host pane shows around it, and its border if it has one.
    bool popup() const { return sized() || framed(); }

    /// Where the overlay goes in a host grid of @p cols by @p rows: its
    /// program's grid, in the host's cells, and whether a border (one cell
    /// on each side) goes around it. The whole popup stays in the host grid.
    /// The program gets at least 2 columns by 1 row, and the border goes
    /// when the host has no room for it.
    struct Placement
    {
        QRect grid;
        bool border = false;
        bool operator==(const Placement&) const = default;
    };
    Placement place(int cols, int rows) const;

    /// Arguments, as for exec.
    static constexpr qsizetype MaxArguments = 64;
    static constexpr qsizetype MaxArgument = 4096;
    static constexpr qsizetype MaxInput = 16 * 1024 * 1024;
    /// Of the captured answer, as for exec.
    static constexpr qsizetype MaxOutput = 1024 * 1024;
    static constexpr qsizetype MaxTitle = 512;
    /// Of a width or a height in cells.
    static constexpr int MaxCells = 10000;

    /// @p argv with @p options (stdin, capture, cwd, env, title, width,
    /// height, position, border), or nothing
    /// with @p error set when one breaks a rule.
    static std::optional<OverlaySpec> read(const QStringList& argv, const QVariantMap& options, QString* error);
};

} // namespace Afterglow
