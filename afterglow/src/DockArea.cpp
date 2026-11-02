// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "DockArea.h"

#include <AfterglowVt/Unicode.h>

#include <algorithm>
#include <utility>

namespace Afterglow {

namespace {

AfterglowVt::TerminalOptions dockOptions(AfterglowVt::TerminalOptions options)
{
    options.maxScrollback = 0;
    // Rows read from left to right, whatever the panes do: the runs, the
    // spacers and the items keep the columns they were written in.
    options.bidi = AfterglowVt::BidiMode::Off;
    return options;
}

/// A checked "#rrggbb" colour.
AfterglowVt::Rgb hexColor(const QString& hex)
{
    const int rgb = hex.mid(1).toInt(nullptr, 16);
    return {std::uint8_t((rgb >> 16) & 0xFF), std::uint8_t((rgb >> 8) & 0xFF), std::uint8_t(rgb & 0xFF)};
}

/// SGR parameters for a checked colour: a palette index or "#rrggbb".
std::string colorParams(const QVariant& color, bool background)
{
    if (color.typeId() == QMetaType::QString) {
        const AfterglowVt::Rgb rgb = hexColor(color.toString());
        return std::string(background ? ";48;2;" : ";38;2;") + std::to_string(rgb.r) + ';' + std::to_string(rgb.g)
            + ';' + std::to_string(rgb.b);
    }
    if (color.isValid()) {
        return std::string(background ? ";48;5;" : ";38;5;") + std::to_string(std::clamp(color.toInt(), 0, 255));
    }
    return {};
}

} // namespace

DockArea::DockArea(QString owner, QString id, const AfterglowVt::TerminalOptions& options)
    : m_owner(std::move(owner))
    , m_id(std::move(id))
    , m_options(options)
    , m_terminal(2, 1, m_host, dockOptions(options))
{
}

DockArea::Change DockArea::setContent(const QVariantMap& effect, quint64 order)
{
    const QString edgeName = effect.value(QStringLiteral("edge")).toString();
    const Edge edge = edgeName == QLatin1String("bottom") ? Edge::Bottom
        : edgeName == QLatin1String("left")               ? Edge::Left
        : edgeName == QLatin1String("right")              ? Edge::Right
                                                          : Edge::Top;
    const int size = std::max(1, effect.value(QStringLiteral("size")).toInt());
    const bool tabBar = edge == Edge::Top && effect.value(QStringLiteral("tab_bar")).toBool();
    const QVariant fill = effect.value(QStringLiteral("fill"));
    const QVariantList lines = effect.value(QStringLiteral("lines")).toList();
    const bool placed = m_order == 0 || edge != m_edge || size != m_size || tabBar != m_tabBar;
    // Scripts send the same area again on every event they redraw on.
    if (!placed && fill == m_fill && lines == m_lines) return Change::None;
    m_edge = edge;
    m_size = size;
    m_tabBar = tabBar;
    // Only a move changes the stacking order: new content stays in place.
    if (placed) m_order = order;
    m_lines = lines;
    if (fill != m_fill) {
        m_fill = fill;
        applyOptions();
    }
    draw();
    return placed ? Change::Place : Change::Content;
}

void DockArea::setOptions(const AfterglowVt::TerminalOptions& options)
{
    if (options == m_options) return;
    m_options = options;
    applyOptions();
    draw();
}

void DockArea::applyOptions()
{
    AfterglowVt::TerminalOptions options = dockOptions(m_options);
    // The fill is the default background. The cells without a background
    // and the strip past the last whole cell, which the renderer paints in
    // the default background, then show the same colour.
    if (m_fill.typeId() == QMetaType::QString) {
        options.defaultBackground = hexColor(m_fill.toString());
    } else if (m_fill.isValid()) {
        // Only the first 16 colours come from the theme: the rest are the
        // same in every palette, the terminal's included.
        const int index = std::clamp(m_fill.toInt(), 0, 255);
        options.defaultBackground = index < 16 ? m_options.basePalette[std::size_t(index)] : m_terminal.paletteColor(index);
    }
    m_terminal.setOptions(options);
}

void DockArea::setRect(const QRect& rect, int cellWidth, int cellHeight)
{
    const int width = std::max(1, cellWidth);
    const int height = std::max(1, cellHeight);
    const int columns = std::max(1, rect.width() / width);
    const int rows = std::max(1, rect.height() / height);
    const bool resized = columns != m_columns || std::max(2, columns) != m_terminal.cols() || rows != m_terminal.rows()
        || width != m_cellWidth || height != m_cellHeight;
    m_rect = rect;
    m_cellWidth = width;
    m_cellHeight = height;
    m_columns = columns;
    if (resized) {
        m_terminal.resize(std::max(2, columns), rows, m_cellWidth, m_cellHeight);
        draw();
    }
}

void DockArea::draw()
{
    // No wrap and no cursor. Text past the edge is cut here, before the
    // terminal sees it: with autowrap off, the terminal would write each
    // character past the edge into the last cell.
    m_terminal.feed("\x1b[?7l\x1b[?25l\x1b[0m\x1b[2J");
    const std::vector<Item> before = std::exchange(m_items, {});
    m_text.clear();
    const auto& cursor = m_terminal.screen().cursor;
    // Where the cursor is after the text written so far on the row.
    auto column = [&] { return cursor.pendingWrap ? m_terminal.cols() : cursor.x; };
    // Follows the terminal's clusters to know which code point needs a
    // cell of its own. The terminal starts again at each escape sequence,
    // so this does too.
    AfterglowVt::Unicode::GraphemeSegmenter clusters;
    QString shown;
    // Writes @p text in the colours of @p sgr until the row is full, one
    // code point at a time. Returns false once the row is full: the rest
    // of the row is cut.
    auto write = [&](const std::string& sgr, const QString& text) {
        m_terminal.feed(sgr);
        clusters.reset();
        for (char32_t cp : text.toUcs4()) {
            // A control character would act instead of showing.
            if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp <= 0x9F)) cp = U' ';
            const AfterglowVt::Unicode::Props props = AfterglowVt::Unicode::props(cp);
            if (clusters.breaksBefore(props)) {
                const int width = props.width == 1 && props.ambiguous && m_options.ambiguousWide ? 2 : props.width;
                if (column() + std::max(1, width) > m_columns) {
                    // A wide character with one cell left: the cell keeps
                    // the run's background, not a replacement character.
                    if (column() < m_columns) {
                        m_terminal.feed(" ");
                        shown += QLatin1Char(' ');
                    }
                    return false;
                }
            } else if (cp == AfterglowVt::Unicode::VariationSelector16 && column() >= m_columns
                       && m_columns < m_terminal.cols()) {
                continue; // it would widen the last character into a column that does not show
            }
            const QString one = QString::fromUcs4(&cp, 1);
            m_terminal.feed(one.toStdString());
            shown += one;
        }
        return column() < m_columns;
    };
    for (int row = 0; row < int(m_lines.size()) && row < m_terminal.rows(); ++row) {
        const QVariantList runs = m_lines[row].toList();
        const std::string home = "\x1b[" + std::to_string(row + 1) + ";1H";
        // Spacers share what the other runs leave: the row is written once
        // without them to measure it, as the terminal lays the text out.
        int spacers = 0;
        for (const QVariant& value : runs) spacers += value.toMap().value(QStringLiteral("spacer")).toBool();
        int free = 0;
        if (spacers > 0) {
            m_terminal.feed(home);
            for (const QVariant& value : runs) {
                const QVariantMap run = value.toMap();
                if (run.value(QStringLiteral("spacer")).toBool()) continue;
                if (!write("\x1b[0m", run.value(QStringLiteral("text")).toString())) break;
            }
            free = std::max(0, m_columns - column());
            m_terminal.feed(home + "\x1b[0m\x1b[2K");
        }
        m_terminal.feed(home);
        shown.clear();
        int spacer = 0;
        for (const QVariant& value : runs) {
            const QVariantMap run = value.toMap();
            std::string sgr = "\x1b[0";
            if (run.value(QStringLiteral("bold")).toBool()) sgr += ";1";
            if (run.value(QStringLiteral("italic")).toBool()) sgr += ";3";
            if (run.value(QStringLiteral("underline")).toBool()) sgr += ";4";
            sgr += colorParams(run.value(QStringLiteral("fg")), false);
            // Without a background, the default one: the fill.
            sgr += colorParams(run.value(QStringLiteral("bg")), true);
            QString text;
            if (run.value(QStringLiteral("spacer")).toBool()) {
                // The first spacers take the remainder of an uneven share.
                text = QString(free / spacers + (spacer++ < free % spacers ? 1 : 0), QLatin1Char(' '));
            } else {
                text = run.value(QStringLiteral("text")).toString();
            }
            const int from = column();
            const qsizetype start = shown.size();
            const bool room = write(sgr + 'm', text);
            const int to = std::min(column(), m_columns);
            const QString item = run.value(QStringLiteral("item")).toString();
            if (!item.isEmpty() && to > from) {
                const QString cells = shown.mid(start);
                // A styled label in several runs is one control.
                if (!m_items.empty() && m_items.back().row == row && m_items.back().to == from && m_items.back().id == item) {
                    m_items.back().to = to;
                    m_items.back().text += cells;
                } else {
                    m_items.push_back({row, from, to, item, cells});
                }
            }
            if (!room) break;
        }
        if (row > 0) m_text += QLatin1Char('\n');
        // Spacers make no text of their own past the last run.
        qsizetype end = shown.size();
        while (end > 0 && shown.at(end - 1) == QLatin1Char(' ')) --end;
        m_text += shown.left(end);
    }
    for (Item& item : m_items) {
        qsizetype end = item.text.size();
        while (end > 0 && item.text.at(end - 1) == QLatin1Char(' ')) --end;
        item.text.truncate(end);
    }
    // Accessibility keeps one object for each item while the ids stay in
    // order: a clock that ticks in an item renames it, and does not replace it.
    if (!std::equal(before.begin(), before.end(), m_items.begin(), m_items.end(),
                    [](const Item& a, const Item& b) { return a.id == b.id; }))
        ++m_itemsGeneration;
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
    for (const Item& item : m_items) {
        if (item.row == row && col >= item.from && col < item.to) return item.id;
    }
    return {};
}

QRect DockArea::itemRect(std::size_t index) const
{
    if (m_rect.isEmpty() || index >= m_items.size()) return {};
    const Item& item = m_items[index];
    return QRect(m_rect.x() + item.from * m_cellWidth, m_rect.y() + item.row * m_cellHeight,
                 (item.to - item.from) * m_cellWidth, m_cellHeight);
}

} // namespace Afterglow
