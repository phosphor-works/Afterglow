// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Pane.h"
#include <QVariant>

namespace Afterglow {

QVariantMap Pane::readSelection(std::size_t limit) const
{
    // The rows that the selection names as the program has them now.
    refreshSession();
    std::lock_guard lock(m_mutex);
    const bool available = selectionShown();
    bool truncated = false;
    const auto text = available ? m_selection.text(m_terminal.screen(), m_terminal.options().wordBoundaries,
        // ScriptHost::MaxReadTextBytes checks the effect first: this only bounds a direct caller.
        std::min(limit, std::size_t(1024 * 1024)), &truncated) : std::string{};
    return {{QStringLiteral("text"), QString::fromStdString(text)}, {QStringLiteral("available"), available},
        {QStringLiteral("truncated"), truncated}};
}

QVariantMap Pane::semanticRegions() const
{
    // Whether a region is available depends on the rows that came.
    refreshSession();
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
    refreshSession();
    std::lock_guard lock(m_mutex);
    bool truncated = false;
    // Scripts and control check the limit first (1 MiB); the pager of an
    // output takes Terminal.lastOutputLimit.
    const auto text = m_terminal.semanticRegionText(id, limit, &truncated);
    return {{QStringLiteral("text"), text ? QString::fromStdString(*text) : QString()},
        {QStringLiteral("available"), text.has_value()}, {QStringLiteral("truncated"), truncated}};
}

} // namespace Afterglow
