// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>
#include <QStringList>

namespace Afterglow {

/// Shader references for each chain (see AfterglowRender::ShaderChains): pack
/// ids (directory names), or "<package>.<pack>" for a script package's packs.
/// Kept apart from ShaderLibrary so the settings need no renderer types.
struct ShaderRefs
{
    QStringList background;
    QStringList text;
    QStringList cursor;
    QStringList pane;
    QStringList window;
    QString overview; ///< the pane overview; empty for none
    QString tabTransition; ///< tab switches; empty for none
    QString paneEffects; ///< panes opening and closing; empty for none
    bool operator==(const ShaderRefs&) const = default;
};

} // namespace Afterglow
