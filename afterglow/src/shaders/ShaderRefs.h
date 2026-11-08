// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>
#include <QStringList>

namespace Afterglow {

/// The keys of the Shaders group that hold a chain of packs, in the order the
/// chains apply.
inline const QStringList ShaderChainKeys{QStringLiteral("background"), QStringLiteral("text"), QStringLiteral("cursor"),
                                         QStringLiteral("pane"), QStringLiteral("window")};
/// The keys of the Shaders group that hold one event pack each.
inline const QStringList ShaderEventKeys{QStringLiteral("overview"), QStringLiteral("tabTransition"),
                                         QStringLiteral("paneEffects"), QStringLiteral("overlayTransition")};

/// Shader references for each chain (see AfterglowRender::ShaderChains): pack
/// ids (directory names), or "<package>.<pack>" for a script package's packs.
/// Kept apart from ShaderLibrary so the settings need no renderer types.
struct ShaderRefs
{
    QStringList background = {};
    QStringList text = {};
    QStringList cursor = {};
    QStringList pane = {};
    QStringList window = {};
    QString overview = {}; ///< the pane overview; empty for none
    QString tabTransition = {}; ///< tab switches; empty for none
    QString paneEffects = {}; ///< panes opening and closing; empty for none
    QString overlayTransition = {}; ///< overlays fading in and out over panes; empty for none
    bool operator==(const ShaderRefs&) const = default;
    /// The packs that the references name, as ShaderChains::size() counts
    /// the programs that load.
    qsizetype size() const
    {
        qsizetype count = background.size() + text.size() + cursor.size() + pane.size() + window.size();
        for (const QString* ref : {&overview, &tabTransition, &paneEffects, &overlayTransition}) {
            if (!ref->isEmpty()) ++count;
        }
        return count;
    }
};

} // namespace Afterglow
