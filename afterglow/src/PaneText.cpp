// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Pane.h"
#include <QVariant>

#include <algorithm>

namespace Afterglow {

QVariantMap Pane::readSelection(std::size_t limit) const
{
    // The rows that the selection names as the program has them now.
    refreshSession();
    bool available = false;
    bool truncated = false;
    std::string text;
    {
        std::lock_guard lock(m_mutex);
        available = selectionShown();
        // ScriptHost::MaxReadTextBytes checks the effect first: this only
        // bounds a direct caller.
        if (available)
            text = m_selection.text(m_terminal.screen(), m_terminal.options().wordBoundaries, std::min(limit, MaxText), &truncated);
    }
    // Converted after the lock: the reader thread waits on it.
    return {{QStringLiteral("text"), QString::fromStdString(text)}, {QStringLiteral("available"), available},
        {QStringLiteral("truncated"), truncated}};
}

bool Pane::regionArrivingLocked(std::uint64_t id) const
{
    if (!m_session) return false;
    for (const AfterglowVt::SemanticRegion& region : m_terminal.semanticRegions()) {
        if (region.id == id) return regionArrivingLocked(region);
    }
    return false;
}

bool Pane::regionArrivingLocked(const AfterglowVt::SemanticRegion& region) const
{
    // Its rows by their stable ids, as lastOutputArrivingLocked() checks
    // those of the last output: blank places of rows on their way, and
    // older rows that did not come yet.
    return m_session && !m_stateReader.received(region.start.row, std::max(region.start.row, region.end.row));
}

bool Pane::regionArriving(std::uint64_t id) const
{
    std::lock_guard lock(m_mutex);
    return regionArrivingLocked(id);
}

QVariantMap Pane::semanticRegions() const
{
    // Whether a region is available depends on the rows that came.
    refreshSession();
    std::vector<AfterglowVt::SemanticRegion> found;
    std::vector<bool> arriving;
    {
        std::lock_guard lock(m_mutex);
        found = m_terminal.semanticRegions();
        arriving.reserve(found.size());
        for (const auto& region : found) arriving.push_back(regionArrivingLocked(region));
    }
    // The maps are made after the lock: the reader thread waits on it.
    QVariantList regions;
    for (std::size_t i = 0; i < found.size(); ++i) {
        const AfterglowVt::SemanticRegion& region = found[i];
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
            {QStringLiteral("available"), region.available && !arriving[i]}};
        if (region.exitCode) value.insert(QStringLiteral("exit_code"), *region.exitCode);
        regions.append(value);
    }
    return {{QStringLiteral("regions"), regions}};
}

QString Pane::regionText(std::uint64_t id, const AfterglowVt::TextFormat& format) const
{
    refreshSession();
    std::optional<std::string> text;
    {
        std::lock_guard lock(m_mutex);
        text = readRegionLocked(id, MaxText, format).text;
    }
    // Converted after the lock: the reader thread waits on it.
    return text ? QString::fromStdString(*text) : QString();
}

Pane::RegionRead Pane::readRegionLocked(std::uint64_t id, std::size_t limit, const AfterglowVt::TextFormat& format) const
{
    RegionRead read;
    // Rows on their way are blank places in the text: it is not the
    // program's yet.
    if (regionArrivingLocked(id)) return read;
    read.text = m_terminal.semanticRegionText(id, limit, &read.truncated, format);
    return read;
}

QVariantMap Pane::regionResult(RegionRead read)
{
    return {{QStringLiteral("text"), read.text ? QString::fromStdString(*read.text) : QString()},
        {QStringLiteral("available"), read.text.has_value()}, {QStringLiteral("truncated"), read.truncated}};
}

QVariantMap Pane::readRegion(std::uint64_t id, std::size_t limit) const
{
    refreshSession();
    RegionRead read;
    {
        // Scripts and control check the limit first (1 MiB); the pager of an
        // output takes Terminal.pagerOutputLimit.
        std::lock_guard lock(m_mutex);
        read = readRegionLocked(id, limit, {});
    }
    // Converted after the lock: the text can be many megabytes.
    return regionResult(std::move(read));
}

QVariantMap Pane::readOutputRegion(OutputRegion which, std::size_t limit, const AfterglowVt::TextFormat& format) const
{
    // One refresh and one lock for the lookup and the read: the answer to a
    // refresh of a pane that the user cannot see makes it held again, and a
    // second read would ask the daemon again.
    refreshSession();
    RegionRead read;
    {
        std::lock_guard lock(m_mutex);
        std::optional<std::uint64_t> id;
        if (which == OutputRegion::LastVisited) {
            id = m_lastVisitedOutput;
        } else if (!m_terminal.modes().altScreen) {
            const AfterglowVt::Screen& scr = m_terminal.screen();
            const std::uint64_t top = scr.rowsDropped() + scr.viewportTop();
            id = m_terminal.firstCommandOutputIn(top, top + std::uint64_t(std::max(1, scr.rows())) - 1);
        }
        if (id) read = readRegionLocked(*id, limit, format);
    }
    return regionResult(std::move(read));
}

} // namespace Afterglow
