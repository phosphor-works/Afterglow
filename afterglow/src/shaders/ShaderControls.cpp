// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ShaderControls.h"
#include <QColor>
#include <QUuid>
#include <algorithm>
#include <cstring>

namespace Afterglow {
void ShaderControls::rebuild(AfterglowRender::ShaderChains& chains)
{
    m_instances.clear();
    m_overrides.clear();
    const auto generation = QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto add = [&](auto& chain, const QString& layer) {
        int index = 0;
        for (auto& program : chain) {
            program = program->instance(generation + QLatin1Char(':') + layer + QLatin1Char(':') + QString::number(++index));
            m_instances.append({program, layer, m_library.parameters(program->id())});
        }
    };
    add(chains.background, QStringLiteral("background")); add(chains.text, QStringLiteral("text"));
    add(chains.cursor, QStringLiteral("cursor")); add(chains.pane, QStringLiteral("pane")); add(chains.window, QStringLiteral("window"));
}

AfterglowRender::ShaderParameterMap ShaderControls::values(quint32 pane) const
{
    AfterglowRender::ShaderParameterMap result;
    for (const auto& item : m_overrides)
        if (item.pane == pane || item.pane == 0) result[item.instance].merge(item.packed);
    return result;
}

QVariantList ShaderControls::describe(quint32 pane) const
{
    QVariantList result;
    const auto overrides = values(pane);
    for (const auto& instance : m_instances) {
        const auto& program = *instance.program;
        AfterglowRender::TerminalShaderUniforms u{};
        std::memcpy(u.customParams, program.params().data(), sizeof u.customParams);
        std::memcpy(u.customColors, program.colors().data(), sizeof u.customColors);
        overrides.value(program.instanceId()).apply(u);
        QVariantMap effective;
        for (const auto& parameter : instance.parameters) {
            const auto p = parameter.toMap();
            if (!p.value(QStringLiteral("supported")).toBool()) continue;
            const int slot = p.value(QStringLiteral("slot")).toInt();
            const auto type = p.value(QStringLiteral("type")).toString();
            QVariant value;
            if (type == QLatin1String("color")) {
                const float* c = u.customColors[slot];
                value = QColor::fromRgbF(c[0], c[1], c[2], c[3]).name(QColor::HexArgb);
            } else if (type == QLatin1String("bool")) value = bool(u.customParams[slot / 4][slot % 4]);
            else value = double(u.customParams[slot / 4][slot % 4]);
            effective.insert(p.value(QStringLiteral("id")).toString(), value);
        }
        result.append(QVariantMap{{QStringLiteral("instance"), program.instanceId()},
            {QStringLiteral("shader"), program.id()}, {QStringLiteral("layer"), instance.layer},
            {QStringLiteral("parameters"), instance.parameters}, {QStringLiteral("values"), effective}});
    }
    return result;
}

QString ShaderControls::update(const QString& owner, const QList<quint32>& panes, const QString& id,
                               const QString& scope, const QVariantMap& values)
{
    const auto instance = std::find_if(m_instances.begin(), m_instances.end(), [&](const auto& i) { return i.program->instanceId() == id; });
    if (instance == m_instances.end()) return QStringLiteral("Shader instance expired or belongs to another window; query shaders again");
    if (scope != QLatin1String("pane") && scope != QLatin1String("tab") && scope != QLatin1String("window"))
        return QStringLiteral("Shader scope must be pane, tab or window");
    if (panes.isEmpty() || panes.size() > 128 || panes.contains(0)) return QStringLiteral("Shader update needs 1 to 128 live panes");
    const bool window = instance->layer == QLatin1String("window");
    if (window && scope != QLatin1String("window")) return QStringLiteral("Window shader instances require window scope");
    AfterglowRender::ShaderParameterValues packed;
    if (!values.isEmpty()) {
        const auto error = m_library.validateParameters(instance->program->id(), values, packed);
        if (!error.isEmpty()) return error;
    }
    const auto targets = window ? QList<quint32>{0} : panes;
    auto matches = [&](const auto& item) { return item.owner == owner && item.instance == id && targets.contains(item.pane); };
    const auto removed = std::count_if(m_overrides.begin(), m_overrides.end(), matches);
    if (!values.isEmpty() && m_overrides.size() - removed + targets.size() > 4096)
        return QStringLiteral("Too many shader overrides");
    // Full replacement of this owner's values on each target, atomically.
    m_overrides.removeIf(matches);
    if (!values.isEmpty()) for (auto pane : targets) m_overrides.append({owner, pane, id, packed});
    return {};
}
void ShaderControls::clearOwner(const QString& owner)
{
    m_overrides.removeIf([&](const auto& item) { return item.owner == owner; });
}
void ShaderControls::removePane(quint32 pane)
{
    m_overrides.removeIf([&](const auto& item) { return item.pane == pane; });
}
}
