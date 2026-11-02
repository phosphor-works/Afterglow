// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "DockArea.h"

#include <algorithm>

namespace Afterglow {

namespace {

AfterglowVt::TerminalOptions dockOptions(AfterglowVt::TerminalOptions options)
{
    options.maxScrollback = 0;
    return options;
}

/// SGR parameters for a checked colour: a palette index or "#rrggbb".
std::string colorParams(const QVariant& color, bool background)
{
    if (color.typeId() == QMetaType::QString) {
        const QString hex = color.toString();
        const int rgb = hex.mid(1).toInt(nullptr, 16);
        return std::string(background ? ";48;2;" : ";38;2;") + std::to_string((rgb >> 16) & 0xFF) + ';'
            + std::to_string((rgb >> 8) & 0xFF) + ';' + std::to_string(rgb & 0xFF);
    }
    if (color.isValid()) {
        return std::string(background ? ";48;5;" : ";38;5;") + std::to_string(std::clamp(color.toInt(), 0, 255));
    }
    return {};
}

/// The text of a run as the terminal must print it: a control character
/// would act instead of showing, so each becomes a space.
std::string printable(const QString& text)
{
    QString shown = text;
    for (QChar& c : shown) {
        const char16_t u = c.unicode();
        if (u < 0x20 || u == 0x7F || (u >= 0x80 && u <= 0x9F)) c = QLatin1Char(' ');
    }
    return shown.toStdString();
}

} // namespace

DockArea::DockArea(QString owner, QString id, const AfterglowVt::TerminalOptions& options)
    : m_owner(std::move(owner)), m_id(std::move(id)), m_terminal(2, 1, m_host, dockOptions(options))
{
}

bool DockArea::setContent(const QVariantMap& effect, quint64 order)
{
    const QString edge = effect.value(QStringLiteral("edge")).toString();
    const Edge before = m_edge;
    const int size = m_size;
    const bool tabBar = m_tabBar;
    m_edge = edge == QLatin1String("bottom") ? Edge::Bottom
        : edge == QLatin1String("left")      ? Edge::Left
        : edge == QLatin1String("right")     ? Edge::Right
                                             : Edge::Top;
    m_size = std::max(1, effect.value(QStringLiteral("size")).toInt());
    m_tabBar = m_edge == Edge::Top && effect.value(QStringLiteral("tab_bar")).toBool();
    m_fill = effect.value(QStringLiteral("fill"));
    m_lines = effect.value(QStringLiteral("lines")).toList();
    const bool moved = m_order == 0 || m_edge != before || m_size != size || m_tabBar != tabBar;
    // Only a move changes the stacking order: new content stays in place.
    if (moved) m_order = order;
    draw();
    return moved;
}

void DockArea::setOptions(const AfterglowVt::TerminalOptions& options)
{
    m_terminal.setOptions(dockOptions(options));
    draw();
}

void DockArea::setRect(const QRect& rect, int cellWidth, int cellHeight)
{
    const int cols = std::max(2, rect.width() / std::max(1, cellWidth));
    const int rows = std::max(1, rect.height() / std::max(1, cellHeight));
    const bool resized = cols != m_terminal.cols() || rows != m_terminal.rows() || cellWidth != m_cellWidth
        || cellHeight != m_cellHeight;
    m_rect = rect;
    m_cellWidth = std::max(1, cellWidth);
    m_cellHeight = std::max(1, cellHeight);
    if (resized) {
        m_terminal.resize(cols, rows, m_cellWidth, m_cellHeight);
        draw();
    }
}

void DockArea::draw()
{
    // No wrap and no cursor: a run that is too long is cut at the edge.
    const std::string fill = colorParams(m_fill, true);
    m_terminal.feed("\x1b[?7l\x1b[?25l\x1b[0" + fill + "m\x1b[2J");
    m_hits.clear();
    const auto& cursor = m_terminal.screen().cursor;
    const int cols = m_terminal.cols();
    // Where the cursor is after the text written so far on the row.
    auto column = [&] { return cursor.pendingWrap ? cols : cursor.x; };
    for (int row = 0; row < int(m_lines.size()) && row < m_terminal.rows(); ++row) {
        const QVariantList runs = m_lines[row].toList();
        const std::string home = "\x1b[" + std::to_string(row + 1) + ";1H";
        // Spacers share what the other runs leave: the row is written once
        // without them to measure it, as the terminal lays the text out.
        int spacers = 0;
        for (const QVariant& value : runs) spacers += value.toMap().value(QStringLiteral("spacer")).toBool();
        int free = 0;
        if (spacers > 0) {
            m_terminal.feed(home + "\x1b[0m");
            for (const QVariant& value : runs) {
                if (!value.toMap().value(QStringLiteral("spacer")).toBool())
                    m_terminal.feed(printable(value.toMap().value(QStringLiteral("text")).toString()));
            }
            free = std::max(0, cols - column());
            m_terminal.feed(home + "\x1b[0" + fill + "m\x1b[2K");
        }
        m_terminal.feed(home);
        int spacer = 0;
        for (const QVariant& value : runs) {
            const QVariantMap run = value.toMap();
            std::string sgr = "\x1b[0";
            if (run.value(QStringLiteral("bold")).toBool()) sgr += ";1";
            if (run.value(QStringLiteral("italic")).toBool()) sgr += ";3";
            if (run.value(QStringLiteral("underline")).toBool()) sgr += ";4";
            sgr += colorParams(run.value(QStringLiteral("fg")), false);
            const QVariant bg = run.value(QStringLiteral("bg"));
            sgr += bg.isValid() ? colorParams(bg, true) : fill;
            m_terminal.feed(sgr + 'm');
            const int from = column();
            if (run.value(QStringLiteral("spacer")).toBool()) {
                // The first spacers take the remainder of an uneven share.
                const int width = free / spacers + (spacer++ < free % spacers ? 1 : 0);
                m_terminal.feed(std::string(std::size_t(width), ' '));
            } else {
                m_terminal.feed(printable(run.value(QStringLiteral("text")).toString()));
            }
            const int to = column();
            const QString item = run.value(QStringLiteral("item")).toString();
            if (!item.isEmpty() && to > from) m_hits.push_back({row, from, to, item});
        }
    }
    m_stale = true;
}

const AfterglowRender::PaneSnapshot& DockArea::snapshot()
{
    if (m_stale) {
        m_snapshot.capture(m_terminal);
        m_stale = false;
    }
    return m_snapshot;
}

QString DockArea::itemAt(const QPoint& point) const
{
    if (!m_rect.contains(point)) return {};
    const int col = (point.x() - m_rect.x()) / m_cellWidth;
    const int row = (point.y() - m_rect.y()) / m_cellHeight;
    for (const Hit& hit : m_hits) {
        if (hit.row == row && col >= hit.from && col < hit.to) return hit.item;
    }
    return {};
}

} // namespace Afterglow
