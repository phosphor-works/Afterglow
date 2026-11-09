// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QtGlobal>

namespace Afterglow::Test {

/// An environment variable as it was, put back at the end: a test that
/// sets XDG_RUNTIME_DIR or AFTERGLOW_SOCKET leaves the next one as it found it.
struct Saved
{
    explicit Saved(const char* name)
        : name(name)
        , wasSet(qEnvironmentVariableIsSet(name))
        , value(qgetenv(name))
    {
    }
    /// Saves @p name, then sets it to @p set until the guard goes.
    Saved(const char* name, const QByteArray& set)
        : Saved(name)
    {
        qputenv(name, set);
    }
    ~Saved()
    {
        if (wasSet) qputenv(name, value);
        else qunsetenv(name);
    }
    Saved(const Saved&) = delete;
    Saved& operator=(const Saved&) = delete;

    const char* name;
    bool wasSet;
    QByteArray value;
};

} // namespace Afterglow::Test
