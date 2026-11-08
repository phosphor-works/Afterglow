// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>

#include <ostream>

// A failed check shows a QString as its text, not as its bytes. Found by
// argument-dependent lookup, so in the namespace of QString. Every test file
// of an executable that prints a QString includes this: a file without it
// would print QString another way, and two meanings of gtest's printer for
// one type break the one definition rule.
QT_BEGIN_NAMESPACE
inline void PrintTo(const QString& text, std::ostream* out)
{
    *out << '"' << text.toStdString() << '"';
}
QT_END_NAMESPACE
