// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Pane.h"
#include <QVariant>

namespace Afterglow {

QVariantMap Pane::readSelection(std::size_t limit) const
{
    std::lock_guard lock(m_mutex);
    const bool available = m_selection.active && m_selectionOnAltScreen == m_terminal.modes().altScreen
        && m_selection.validFor(m_terminal.screen());
    bool truncated = false;
    const auto text = available ? m_selection.text(m_terminal.screen(), m_terminal.options().wordBoundaries,
        std::min(limit, std::size_t(1024 * 1024)), &truncated) : std::string{};
    return {{QStringLiteral("text"), QString::fromStdString(text)}, {QStringLiteral("available"), available},
        {QStringLiteral("truncated"), truncated}};
}

QVariantMap Pane::semanticRegions() const
{
    std::lock_guard lock(m_mutex);
    QVariantList regions;
    for (const auto& region : m_terminal.semanticRegions()) {
        const auto position = [](const auto& point) {
            return QVariantMap{{QStringLiteral("row"), QString::number(point.row)}, {QStringLiteral("col"), point.col}};
        };
        QString kind;
        switch (region.kind) {
        case AfterglowVt::SemanticZoneKind::Prompt: kind = QStringLiteral("prompt"); break;
        case AfterglowVt::SemanticZoneKind::Input: kind = QStringLiteral("input"); break;
        case AfterglowVt::SemanticZoneKind::Output: kind = QStringLiteral("output"); break;
        }
        QVariantMap value{{QStringLiteral("id"), QString::number(region.id)}, {QStringLiteral("kind"), kind},
            {QStringLiteral("start"), position(region.start)}, {QStringLiteral("finish"), position(region.end)},
            {QStringLiteral("open"), region.open}, {QStringLiteral("start_trimmed"), region.startTrimmed},
            {QStringLiteral("available"), region.available}};
        if (region.exitCode) value.insert(QStringLiteral("exit_code"), *region.exitCode);
        regions.append(value);
    }
    return {{QStringLiteral("regions"), regions}};
}

QVariantMap Pane::readRegion(std::uint64_t id, std::size_t limit) const
{
    std::lock_guard lock(m_mutex);
    bool truncated = false;
    const auto text = m_terminal.semanticRegionText(id, std::min(limit, std::size_t(1024 * 1024)), &truncated);
    return {{QStringLiteral("text"), text ? QString::fromStdString(*text) : QString()},
        {QStringLiteral("available"), text.has_value()}, {QStringLiteral("truncated"), truncated}};
}

} // namespace Afterglow
