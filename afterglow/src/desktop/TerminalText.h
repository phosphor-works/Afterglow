// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <AfterglowVt/Screen.h>
#include <AfterglowVt/Selection.h>
#include <QList>
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
};
/// One accessible text change: at `position`, remove `removed`, then insert `inserted`.
struct TextEdit {
    int position = 0;
    QString removed, inserted;
};
/// Edits that turn `before` into `after`, applied in order. A viewport that scrolled by
/// whole lines gives a removal of the top lines and an insertion of the new text only.
QList<TextEdit> textEdits(const QString& before, const QString& after);
}
