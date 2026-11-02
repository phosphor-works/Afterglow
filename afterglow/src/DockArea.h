// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <AfterglowRender/Snapshot.h>
#include <AfterglowVt/Terminal.h>

#include <QRect>
#include <QString>
#include <QVariantList>

#include <vector>

namespace Afterglow {

/// An area that a script docks at an edge of a window (afterglow.dock): lines
/// of styled text, drawn by the grid renderer like a pane. The text goes into
/// a terminal of its own that runs no program, so widths, emoji, the palette
/// and box drawing are those of the panes. The panes get the space that is
/// left.
class DockArea
{
public:
    enum class Edge { Top, Bottom, Left, Right };

    DockArea(QString owner, QString id, const AfterglowVt::TerminalOptions& options);

    const QString& owner() const noexcept { return m_owner; }
    const QString& id() const noexcept { return m_id; }
    Edge edge() const noexcept { return m_edge; }
    /// Rows for the top and bottom edges, columns for the left and right.
    int size() const noexcept { return m_size; }
    /// Shown in place of the tab bar (top edge only).
    bool replacesTabBar() const noexcept { return m_tabBar; }
    /// The order docks on one edge stack in, from the window's edge inwards.
    quint64 order() const noexcept { return m_order; }

    /// Takes a checked dock effect: edge, size, tab_bar, fill and lines.
    /// Returns whether the area's place changed (edge, size or tab bar).
    bool setContent(const QVariantMap& effect, quint64 order);
    void setOptions(const AfterglowVt::TerminalOptions& options);
    /// Places the area at @p rect, framebuffer pixels, in whole cells.
    void setRect(const QRect& rect, int cellWidth, int cellHeight);
    const QRect& rect() const noexcept { return m_rect; }

    /// The text as drawn, captured again only after a change.
    const AfterglowRender::PaneSnapshot& snapshot();
    /// The item of the run under @p point (framebuffer pixels), or empty.
    QString itemAt(const QPoint& point) const;

private:
    struct NullHost : AfterglowVt::ITerminalHost
    {
        void writeToPty(std::string_view) override { }
    };
    struct Hit
    {
        int row, from, to; ///< columns [from, to)
        QString item;
    };
    void draw();

    QString m_owner;
    QString m_id;
    Edge m_edge = Edge::Top;
    int m_size = 1;
    bool m_tabBar = false;
    quint64 m_order = 0;
    QVariant m_fill;
    QVariantList m_lines;
    QRect m_rect;
    int m_cellWidth = 1;
    int m_cellHeight = 1;
    NullHost m_host;
    AfterglowVt::Terminal m_terminal;
    AfterglowRender::PaneSnapshot m_snapshot;
    bool m_stale = true;
    std::vector<Hit> m_hits;
};

} // namespace Afterglow
