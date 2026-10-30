// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QStringList>
#include <QVariantMap>
namespace Afterglow {
struct PaneLaunchOptions {
    QString cwd;
    QStringList command;
    QVariantMap environment; ///< String values override; false removes inherited entries.
    double fraction = 0.5; ///< New pane's fraction of the split.
};
}
