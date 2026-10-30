// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <AfterglowVt/Screen.h>
#include <AfterglowVt/Selection.h>
#include <QString>
#include <QVector>

namespace Afterglow {
/// Bounded, read-only UTF-16 view of a terminal viewport (or the live cursor row).
/// Every UTF-16 unit maps to its grapheme's cell, including surrogate pairs.
struct TerminalText {
    QString text;
    QVector<AfterglowVt::SelectionPoint> cells;
    int cursor = -1;
    int selectionStart = -1, selectionEnd = -1;
    static TerminalText capture(const AfterglowVt::Screen&, const AfterglowVt::Selection* = nullptr, bool cursorRow = false,
                                std::u32string_view wordBoundaries = AfterglowVt::DefaultWordBoundaries);
    int offset(AfterglowVt::SelectionPoint point) const;
};
}
