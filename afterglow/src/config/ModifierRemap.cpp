// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ModifierRemap.h"

#include <QRegularExpression>
#include <QStringList>

namespace Afterglow {

namespace {

/// A modifier by kitty's names. Qt and key bindings call Super Meta. Kitty's
/// hyper and meta are refused: Qt reports only Shift, Control, Alt and Meta
/// (Super), and a Hyper or Meta key gives whichever of them the layout maps
/// it to.
std::optional<Qt::KeyboardModifier> modifierNamed(const QString& name, const QString& token, QString* error)
{
    const QString n = name.toLower();
    if (n == QLatin1String("shift") || n == QStringLiteral("⇧")) return Qt::ShiftModifier;
    if (n == QLatin1String("ctrl") || n == QLatin1String("control") || n == QStringLiteral("⌃")) return Qt::ControlModifier;
    if (n == QLatin1String("alt") || n == QLatin1String("opt") || n == QLatin1String("option") || n == QStringLiteral("⌥"))
        return Qt::AltModifier;
    if (n == QLatin1String("super") || n == QLatin1String("cmd") || n == QLatin1String("command") || n == QStringLiteral("⌘"))
        return Qt::MetaModifier;
    if (error) {
        if (n.isEmpty())
            *error = QStringLiteral("\"%1\" has a modifier with no name").arg(token);
        else if (n == QLatin1String("meta"))
            *error = QStringLiteral("Kitty's meta is not a modifier of Afterglow. Write super for the key that key "
                                    "bindings call Meta");
        else if (n == QLatin1String("hyper"))
            *error = QStringLiteral("Kitty's hyper is not a modifier of Afterglow");
        else
            *error = QStringLiteral("\"%1\" is not shift, ctrl, alt or super").arg(name);
    }
    return std::nullopt;
}

/// The modifiers that @p names joins with "+".
std::optional<Qt::KeyboardModifiers> modifiersNamed(const QString& names, const QString& token, QString* error)
{
    Qt::KeyboardModifiers modifiers;
    for (const QString& name : names.split(QLatin1Char('+'))) {
        const auto modifier = modifierNamed(name, token, error);
        if (!modifier) return std::nullopt;
        modifiers |= *modifier;
    }
    return modifiers;
}

} // namespace

std::optional<ModifierRemap> ModifierRemap::parse(const QString& text, QString* error)
{
    // Unicode white space, nearly as Python's str.split() in kitty.
    static const QRegularExpression Spaces(QStringLiteral("\\s+"), QRegularExpression::UseUnicodePropertiesOption);
    ModifierRemap remap;
    for (const QString& token : text.split(Spaces, Qt::SkipEmptyParts)) {
        const qsizetype colon = token.indexOf(QLatin1Char(':'));
        if (colon < 0) {
            if (error) *error = QStringLiteral("\"%1\" has no colon between a modifier and what it becomes").arg(token);
            return std::nullopt;
        }
        const auto source = modifiersNamed(token.left(colon), token, error);
        if (!source) return std::nullopt;
        // "ctrl+ctrl" is one modifier, as in kitty.
        const auto sourceBits = source->toInt();
        if (sourceBits & (sourceBits - 1)) {
            if (error) *error = QStringLiteral("\"%1\" has more than one modifier before the colon").arg(token);
            return std::nullopt;
        }
        const auto target = modifiersNamed(token.mid(colon + 1), token, error);
        if (!target) return std::nullopt;
        if (*target == *source) {
            if (error) *error = QStringLiteral("\"%1\" makes a modifier into itself").arg(token);
            return std::nullopt;
        }
        for (std::size_t i = 0; i < Sources.size(); ++i) {
            if (Sources[i] == *source) remap.m_targets[i] = *target;
        }
    }
    return remap;
}

Qt::KeyboardModifiers ModifierRemap::apply(Qt::KeyboardModifiers modifiers) const noexcept
{
    if (isEmpty()) return modifiers;
    Qt::KeyboardModifiers out = modifiers & ~(Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
    for (std::size_t i = 0; i < Sources.size(); ++i) {
        if (modifiers & Sources[i]) out |= m_targets[i];
    }
    return out;
}

Qt::KeyboardModifier ModifierRemap::singleTarget(Qt::KeyboardModifier source) const noexcept
{
    for (std::size_t i = 0; i < Sources.size(); ++i) {
        if (Sources[i] != source || m_targets[i] == Identity[i]) continue;
        const auto target = m_targets[i].toInt();
        // One bit: a single modifier.
        return (target & (target - 1)) == 0 ? Qt::KeyboardModifier(target) : Qt::NoModifier;
    }
    return Qt::NoModifier;
}

} // namespace Afterglow
