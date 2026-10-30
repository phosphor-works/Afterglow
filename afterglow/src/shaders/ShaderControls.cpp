// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ShaderControls.h"
#include <QColor>
#include <PhosphorAnimation/Spring.h>
#include <QUuid>
#include <algorithm>
#include <cstring>

namespace Afterglow {
bool ShaderControls::advance(bool finish)
{
    bool active = false;
    for (auto& item : m_overrides) {
        if (!item.transition) continue;
        auto& t = *item.transition;
        if (finish) t.progress.finish();
        else t.progress.advance();
        const double progress = std::clamp(double(t.progress.value()), 0.0, 1.0);
        item.packed = t.target;
        for (int i = 0; i < 32; ++i) {
            if (!(t.target.paramMask & (std::uint32_t(1) << i))) continue;
            item.packed.params[i] = (t.discrete & (std::uint32_t(1) << i))
                ? (t.progress.isAnimating() ? t.from.params[i] : t.target.params[i])
                : float(std::lerp(double(t.from.params[i]), double(t.target.params[i]), progress));
        }
        for (int i = 0; i < 16; ++i) {
            if (!(t.target.colorMask & (1u << i))) continue;
            const float* a = t.from.colors.data() + i * 4;
            const float* b = t.target.colors.data() + i * 4;
            const auto color = PhosphorAnimation::Interpolate<QColor>::lerp(
                QColor::fromRgbF(a[0], a[1], a[2], a[3]), QColor::fromRgbF(b[0], b[1], b[2], b[3]), progress);
            float* out = item.packed.colors.data() + i * 4;
            out[0] = color.redF(); out[1] = color.greenF(); out[2] = color.blueF(); out[3] = color.alphaF();
        }
        if (t.progress.isAnimating()) active = true;
        else { item.packed = t.target; item.transition.reset(); }
    }
    return active;
}

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
                               const QString& scope, const QVariantMap& values, const QVariantMap& transition)
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
    const auto durationValue = transition.value(QStringLiteral("duration_ms"), 0.0);
    const double duration = durationValue.toDouble();
    const auto curve = transition.value(QStringLiteral("curve"), QStringLiteral("ease-in-out")).toString();
    if ((durationValue.metaType().id() != QMetaType::Double && durationValue.metaType().id() != QMetaType::Int && durationValue.metaType().id() != QMetaType::LongLong)
        || !std::isfinite(duration) || duration < 0 || duration > 60000
        || (curve != QLatin1String("linear") && curve != QLatin1String("ease-in-out") && curve != QLatin1String("spring")))
        return QStringLiteral("Transition needs duration_ms between 0 and 60000 and linear, ease-in-out or spring curve");
    for (auto it = transition.begin(); it != transition.end(); ++it)
        if (it.key() != QLatin1String("duration_ms") && it.key() != QLatin1String("curve")) return QStringLiteral("Unknown transition option");
    if (duration > 0 && !m_clock) return QStringLiteral("No animation clock");
    const auto targets = window ? QList<quint32>{0} : panes;
    auto matches = [&](const auto& item) { return item.owner == owner && item.instance == id && targets.contains(item.pane); };
    const auto removed = std::count_if(m_overrides.begin(), m_overrides.end(), matches);
    if (!values.isEmpty() && m_overrides.size() - removed + targets.size() > 4096)
        return QStringLiteral("Too many shader overrides");
    // Advance once before taking starts; retargeting preserves the visible position.
    advance();
    QList<Override> replacements;
    if (!values.isEmpty()) for (auto pane : targets) {
        Override item{owner, pane, id, packed, {}};
        if (duration > 0) {
            auto t = std::make_shared<Transition>();
            t->from.params = instance->program->params(); t->from.colors = instance->program->colors();
            t->from.merge(this->values(pane).value(id));
            t->from.paramMask = packed.paramMask; t->from.colorMask = packed.colorMask;
            t->target = packed;
            for (const auto& metadata : instance->parameters) {
                const auto p = metadata.toMap(); const int slot = p.value(QStringLiteral("slot")).toInt();
                if (slot >= 0 && slot < 32 && (p.value(QStringLiteral("type")) == QLatin1String("bool")
                    || p.value(QStringLiteral("type")) == QLatin1String("int"))) t->discrete |= std::uint32_t(1) << slot;
            }
            PhosphorAnimation::MotionSpec<qreal> spec;
            spec.clock = m_clock; spec.profile.duration = duration; spec.maxLifetimeMs = int(std::ceil(duration));
            if (curve == QLatin1String("spring")) spec.profile.curve = std::make_shared<PhosphorAnimation::Spring>(12.0, 1.0);
            else {
                auto easing = std::make_shared<PhosphorAnimation::Easing>();
                easing->x1 = curve == QLatin1String("linear") ? 0.0 : 0.42; easing->y1 = 0.0;
                easing->x2 = curve == QLatin1String("linear") ? 1.0 : 0.58; easing->y2 = 1.0;
                spec.profile.curve = easing;
            }
            t->progress.start(0.0, 1.0, spec); t->progress.advance();
            item.packed = t->from; item.transition = std::move(t);
        }
        replacements.append(std::move(item));
    }
    m_overrides.removeIf(matches);
    m_overrides.append(replacements);
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
