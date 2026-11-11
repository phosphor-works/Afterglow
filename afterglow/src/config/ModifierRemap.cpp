// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ModifierRemap.h"

#include <QRegularExpression>
#include <QStringList>

namespace Afterglow {

namespace {

/// A modifier by kitty's names. Qt calls Super Meta. Hyper and Meta are
/// refused: Qt does not report them, and on the common layouts they share
/// Mod4 with Super and Mod1 with Alt.
std::optional<Qt::KeyboardModifier> modifierNamed(const QString& name, QString* error)
{
    const QString n = name.toLower();
    if (n == QLatin1String("shift") || n == QStringLiteral("⇧")) return Qt::ShiftModifier;
    if (n == QLatin1String("ctrl") || n == QLatin1String("control") || n == QStringLiteral("⌃")) return Qt::ControlModifier;
    if (n == QLatin1String("alt") || n == QLatin1String("opt") || n == QLatin1String("option") || n == QStringLiteral("⌥"))
        return Qt::AltModifier;
    if (n == QLatin1String("super") || n == QLatin1String("cmd") || n == QLatin1String("command") || n == QStringLiteral("⌘"))
        return Qt::MetaModifier;
    if (error) {
        *error = n == QLatin1String("hyper") || n == QLatin1String("meta")
            ? QStringLiteral("Afterglow does not tell %1 apart from super and alt, so it cannot be remapped").arg(n)
            : QStringLiteral("\"%1\" is not shift, ctrl, alt or super").arg(name);
    }
    return std::nullopt;
}

} // namespace

std::optional<ModifierRemap> ModifierRemap::parse(const QString& text, QString* error)
{
    ModifierRemap remap;
    for (const QString& token : text.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts)) {
        const qsizetype colon = token.indexOf(QLatin1Char(':'));
        if (colon < 0) {
            if (error) *error = QStringLiteral("\"%1\" is not two modifiers apart by a colon").arg(token);
            return std::nullopt;
        }
        const QString from = token.left(colon);
        if (from.contains(QLatin1Char('+'))) {
            if (error) *error = QStringLiteral("\"%1\": the source must be one modifier").arg(token);
            return std::nullopt;
        }
        const auto source = modifierNamed(from, error);
        if (!source) return std::nullopt;
        Qt::KeyboardModifiers target;
        for (const QString& name : token.mid(colon + 1).split(QLatin1Char('+'))) {
            const auto modifier = modifierNamed(name, error);
            if (!modifier) return std::nullopt;
            target |= *modifier;
        }
        if (target == *source) {
            if (error) *error = QStringLiteral("\"%1\": the source and the destination are the same").arg(token);
            return std::nullopt;
        }
        for (std::size_t i = 0; i < Sources.size(); ++i) {
            if (Sources[i] == *source) remap.m_targets[i] = target;
        }
        remap.m_empty = false;
    }
    return remap;
}

Qt::KeyboardModifiers ModifierRemap::apply(Qt::KeyboardModifiers modifiers) const noexcept
{
    if (m_empty) return modifiers;
    Qt::KeyboardModifiers out = modifiers & ~(Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
    for (std::size_t i = 0; i < Sources.size(); ++i) {
        if (modifiers & Sources[i]) out |= m_targets[i];
    }
    return out;
}

Qt::KeyboardModifier ModifierRemap::singleTarget(Qt::KeyboardModifier source) const noexcept
{
    for (std::size_t i = 0; i < Sources.size(); ++i) {
        if (Sources[i] != source) continue;
        const auto target = m_targets[i].toInt();
        // One bit: a single modifier.
        return target != 0 && (target & (target - 1)) == 0 ? Qt::KeyboardModifier(target) : Qt::NoModifier;
    }
    return Qt::NoModifier;
}

} // namespace Afterglow
