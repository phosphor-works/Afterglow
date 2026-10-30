// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "ShaderLibrary.h"
#include <PhosphorAnimation/AnimatedValue.h>

namespace Afterglow {
/// GUI-thread scene state. Rebuilding invalidates IDs and retires all overrides.
class ShaderControls
{
public:
    explicit ShaderControls(ShaderLibrary& library) : m_library(library) {}
    void setClock(PhosphorAnimation::IMotionClock* clock) { m_clock = clock; }
    bool advance(bool finish = false);
    void rebuild(AfterglowRender::ShaderChains& chains);
    QVariantList describe(quint32 pane) const;
    QString update(const QString& owner, const QList<quint32>& panes, const QString& instance,
                   const QString& scope, const QVariantMap& values, const QVariantMap& transition = {});
    AfterglowRender::ShaderParameterMap values(quint32 pane) const;
    void clearOwner(const QString& owner);
    void removePane(quint32 pane);
private:
    struct Instance { AfterglowRender::ShaderProgramPtr program; QString layer; QVariantList parameters; };
    struct Transition {
        AfterglowRender::ShaderParameterValues from, target;
        std::uint32_t discrete = 0;
        PhosphorAnimation::AnimatedValue<qreal> progress;
    };
    struct Override {
        QString owner; quint32 pane; QString instance;
        AfterglowRender::ShaderParameterValues packed;
        std::shared_ptr<Transition> transition;
    };
    PhosphorAnimation::IMotionClock* m_clock = nullptr;
    ShaderLibrary& m_library;
    QList<Instance> m_instances;
    QList<Override> m_overrides; // last write wins per parameter, across owners
};
}
