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
    /// The order docks on one edge stack in, from the window's edge inward:
    /// the order of the last change of place.
    quint64 order() const noexcept { return m_order; }

    /// What a setContent() call changed.
    enum class Change {
        None, ///< the same effect again: nothing to draw
        Content, ///< new text or fill in the same place
        Place, ///< the first content, or a new edge, size or tab bar
    };
    /// Takes a checked dock effect: edge, size, tab_bar, fill and lines.
    /// A new place takes @p order; new content keeps the old one.
    Change setContent(const QVariantMap& effect, quint64 order);
    /// The panes' terminal options. The area keeps its own scrollback, bidi
    /// and background on top of them.
    void setOptions(const AfterglowVt::TerminalOptions& options);
    /// Places the area at @p rect, framebuffer pixels, in whole cells. An
    /// empty @p rect hides it. Returns whether it moved or drew again.
    bool setRect(const QRect& rect, int cellWidth, int cellHeight);
    const QRect& rect() const noexcept { return m_rect; }

    /// The text as drawn, captured again only after a change.
    const AfterglowRender::PaneSnapshot& snapshot();
    /// The item of the run under @p point (framebuffer pixels), or empty.
    QString itemAt(const QPoint& point) const;
    /// The text as drawn, one line for each row, for accessibility.
    const QString& text() const noexcept { return m_text; }

    /// A run that a click reports, as drawn. Adjacent runs of one item on a
    /// row are one.
    struct Item
    {
        int row, from, to; ///< columns [from, to)
        QString id;
        QString text; ///< the cells that show, without trailing blanks
    };
    const std::vector<Item>& items() const noexcept { return m_items; }
    /// Counts the changes to the order of the item ids. New text, cells or
    /// widths of the same items leave it as it is.
    quint64 itemsGeneration() const noexcept { return m_itemsGeneration; }
    /// Where item @p index shows, framebuffer pixels; empty when it does not.
    QRect itemRect(std::size_t index) const;

private:
    struct NullHost : AfterglowVt::ITerminalHost
    {
        void writeToPty(std::string_view) override { }
    };
    /// Gives the terminal the panes' options with the area's own on top.
    void applyOptions();
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
    /// The columns that show. The terminal has at least 2, so a dock 1
    /// column wide lays its text out in fewer columns than the terminal has.
    int m_columns = 1;
    AfterglowVt::TerminalOptions m_options; ///< the panes' options
    NullHost m_host;
    AfterglowVt::Terminal m_terminal;
    AfterglowRender::PaneSnapshot m_snapshot;
    bool m_stale = true;
    std::vector<Item> m_items;
    quint64 m_itemsGeneration = 0;
    QString m_text;
};

} // namespace Afterglow
