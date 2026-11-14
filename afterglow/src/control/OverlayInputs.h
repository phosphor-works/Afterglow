// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "../scripting/OverlaySpec.h"

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QVariantMap>

namespace Afterglow {

/// Standard input for overlays that comes in parts before the overlay call
/// (overlayInput), kept for each owner: a client connection of the control
/// socket, or a client of a session that the mux daemon passes on.
class OverlayInputs
{
public:
    /// Inputs that wait for their overlay, for one owner.
    static constexpr int MaxWaiting = 4;
    /// The bytes of all the inputs that wait, of every owner.
    static constexpr qsizetype MaxWaitingBytes = 16 * OverlaySpec::MaxInput;

    /// Whether @p name can name an input: 1 to 64 letters, digits, _ and -.
    static bool validName(const QString& name);
    /// Adds @p chunk to input @p name of @p owner, which ends with @p eof:
    /// empty, else why not. It fails for a name that is not 1 to 64
    /// letters, digits, _ and -, for an input past MaxWaiting, for an input
    /// that has ended, and past MaxWaitingBytes. Past OverlaySpec::MaxInput
    /// bytes it fails too, and drops that input.
    QString add(const QString& owner, const QString& name, const QString& chunk, bool eof);
    /// Makes the input that @p options names ("input") its "stdin": empty,
    /// else why not. That input goes, also on a failure. Options without
    /// "input" stay as they are.
    QString resolve(const QString& owner, QVariantMap& options);
    /// Drops the inputs of @p owner.
    void drop(const QString& owner);
    /// Drops the inputs of every owner whose name starts with @p prefix.
    void dropPrefix(const QString& prefix);
    void clear();

private:
    struct Waiting
    {
        QByteArray bytes; ///< UTF-8
        bool eof = false;
    };
    void drop(const QString& owner, const QString& name);

    QHash<QString, QHash<QString, Waiting>> m_inputs;
    qsizetype m_bytes = 0;
};

} // namespace Afterglow
