// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "ShaderLibrary.h"
#include <QSet>

namespace Afterglow {
/// GUI-thread scene state. Rebuilding invalidates IDs and retires all overrides.
class ShaderControls
{
public:
    explicit ShaderControls(ShaderLibrary& library) : m_library(library) {}
    void rebuild(AfterglowRender::ShaderChains& chains);
    QVariantList describe(quint32 pane) const;
    QString update(const QString& owner, const QList<quint32>& panes, const QString& instance,
                   const QString& scope, const QVariantMap& values);
    AfterglowRender::ShaderParameterMap values(quint32 pane) const;
    void clearOwner(const QString& owner);
    void removePane(quint32 pane);
private:
    struct Instance { AfterglowRender::ShaderProgramPtr program; QString layer; QVariantList parameters; };
    struct Override { QString owner; quint32 pane; QString instance; AfterglowRender::ShaderParameterValues packed; };
    ShaderLibrary& m_library;
    QList<Instance> m_instances;
    QList<Override> m_overrides; // last write wins per parameter, across owners
};
}
