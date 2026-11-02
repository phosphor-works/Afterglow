// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TerminalWindow.h"

#include "DockArea.h"
#include "desktop/Accessibility.h"

#include <AfterglowRender/GpuDevice.h>

#include <QAccessible>

#include <algorithm>
#include <array>
#include <cstdint>

namespace Afterglow {

namespace {
// Each script's docks in one window, and every script's together.
constexpr std::size_t MaxOwnerDocks = 8;
constexpr std::size_t MaxDocks = 32;

bool sideEdge(const DockArea& dock)
{
    return dock.edge() == DockArea::Edge::Left || dock.edge() == DockArea::Edge::Right;
}
} // namespace

QString TerminalWindow::setScriptDock(const QString& owner, const QVariantMap& effect)
{
    const QString id = effect.value(QStringLiteral("id")).toString();
    auto found = std::find_if(m_docks.begin(), m_docks.end(), [&](const auto& dock) {
        return dock->owner() == owner && dock->id() == id;
    });
    DockArea::Change change = DockArea::Change::Place;
    DockArea* changed = nullptr;
    if (effect.value(QStringLiteral("kind")) == QLatin1String("undock")) {
        if (found == m_docks.end()) return {};
        releaseDockAccessibility(found->get());
        m_docks.erase(found);
    } else {
        if (found == m_docks.end()) {
            const auto owned = std::count_if(m_docks.begin(), m_docks.end(), [&](const auto& d) { return d->owner() == owner; });
            if (std::size_t(owned) >= MaxOwnerDocks || m_docks.size() >= MaxDocks)
                return QStringLiteral("too many docks: a script has at most %1 in a window, and a window at most %2")
                    .arg(MaxOwnerDocks)
                    .arg(MaxDocks);
            m_docks.push_back(std::make_unique<DockArea>(owner, id, m_settings.settings().terminal));
            found = std::prev(m_docks.end());
        }
        changed = found->get();
        change = changed->setContent(effect, m_dockOrder + 1);
        if (change == DockArea::Change::Place) ++m_dockOrder;
    }
    // The same effect again: nothing to draw.
    if (change == DockArea::Change::None) return {};
    // The panes take what the docks leave, and the tab bar may come or go:
    // once in the next frame, however many docks move before it. New text
    // in a dock that stays where it is changes neither.
    if (change == DockArea::Change::Place) m_docksMoved = true;
    if (changed) dockAccessibilityChanged(changed);
    // An item can come or go under a pointer that does not move.
    applyPointerShape();
    scheduleRender();
    return {};
}

void TerminalWindow::finishDockMoves()
{
    if (!std::exchange(m_docksMoved, false)) return;
    // relayout tells the chrome when the tab bar comes or goes and sets the
    // pointer shape, and placeDocks tells assistive technology what shows.
    relayout();
}

void TerminalWindow::eraseDocks(const QString& owner)
{
    const auto erased = std::erase_if(m_docks, [&](const auto& dock) {
        if (dock->owner() != owner) return false;
        releaseDockAccessibility(dock.get());
        return true;
    });
    if (erased == 0) return;
    m_docksMoved = true;
    applyPointerShape();
    scheduleRender();
}

QSize TerminalWindow::windowPixels() const
{
    return m_renderer->pixelSize().isEmpty() ? size() * devicePixelRatio() : m_renderer->pixelSize();
}

QRect TerminalWindow::windowArea(int tabCount) const
{
    const QSize px = windowPixels();
    const int bar = tabBarHeight(tabCount);
    return QRect(0, bar, px.width(), std::max(0, px.height() - bar));
}

bool TerminalWindow::tabBarReplaced() const
{
    if (std::none_of(m_docks.begin(), m_docks.end(), [](const auto& dock) { return dock->replacesTabBar(); })) return false;
    // Only a tab_bar dock that shows replaces the bar. The layout without
    // the bar is the one that holds while a dock replaces it.
    bool placed = false;
    layoutDocks(QRect(QPoint(0, 0), windowPixels()), nullptr, &placed);
    return placed;
}

QRect TerminalWindow::layoutDocks(QRect area, std::vector<QRect>* rects, bool* tabBarPlaced) const
{
    if (rects) rects->assign(m_docks.size(), QRect());
    if (tabBarPlaced) *tabBarPlaced = false;
    if (m_docks.empty()) return area;
    const auto& m = m_renderer->fonts().fonts->metrics();
    const QSize minimum = minimumPaneSize();
    // From the window's edge inward: top and bottom docks span the window,
    // and left and right ones fit between them. A tab_bar dock is outermost
    // at the top, where the tab bar is. Then the dock whose place was set
    // first is outermost. No allocation: this runs for each pane drawn.
    std::array<std::uint8_t, MaxDocks> order;
    const std::size_t count = std::min(m_docks.size(), MaxDocks);
    for (std::size_t i = 0; i < count; ++i) order[i] = std::uint8_t(i);
    // The orders differ, so the sort needs no stability.
    std::sort(order.begin(), order.begin() + std::ptrdiff_t(count), [this](std::uint8_t a, std::uint8_t b) {
        const DockArea& x = *m_docks[a];
        const DockArea& y = *m_docks[b];
        if (sideEdge(x) != sideEdge(y)) return !sideEdge(x);
        if (x.replacesTabBar() != y.replacesTabBar()) return x.replacesTabBar();
        return x.order() < y.order();
    });
    for (std::size_t n = 0; n < count; ++n) {
        const std::size_t i = order[n];
        const DockArea& dock = *m_docks[i];
        const bool side = sideEdge(dock);
        const int extent = dock.size() * (side ? m.cellWidth : m.cellHeight);
        // A dock that would leave the panes too small is not shown.
        if (side ? area.width() - extent < minimum.width() : area.height() - extent < minimum.height()) continue;
        QRect rect;
        switch (dock.edge()) {
        case DockArea::Edge::Top:
            rect = QRect(area.left(), area.top(), area.width(), extent);
            area.setTop(area.top() + extent);
            break;
        case DockArea::Edge::Bottom:
            rect = QRect(area.left(), area.bottom() + 1 - extent, area.width(), extent);
            area.setBottom(area.bottom() - extent);
            break;
        case DockArea::Edge::Left:
            rect = QRect(area.left(), area.top(), extent, area.height());
            area.setLeft(area.left() + extent);
            break;
        case DockArea::Edge::Right:
            rect = QRect(area.right() + 1 - extent, area.top(), extent, area.height());
            area.setRight(area.right() - extent);
            break;
        }
        if (rects) (*rects)[i] = rect;
        if (tabBarPlaced && dock.replacesTabBar()) *tabBarPlaced = true;
    }
    return area;
}

QRect TerminalWindow::placeDocks()
{
    std::vector<QRect> rects;
    const QRect area = layoutDocks(windowArea(), &rects);
    const auto& m = m_renderer->fonts().fonts->metrics();
    // A new window size, font or padding, as well as a dock that came or
    // went, can show or hide a dock or cut its text anew. A new dock has no
    // rect yet, so fewer shown docks than last time means some went.
    std::size_t kept = 0;
    std::size_t shown = 0;
    bool reordered = false;
    for (std::size_t i = 0; i < m_docks.size(); ++i) {
        DockArea& dock = *m_docks[i];
        const QRect before = dock.rect();
        const QString text = dock.text();
        dock.setRect(rects[i], m.cellWidth, m.cellHeight);
        const bool was = !before.isEmpty();
        const bool now = !dock.rect().isEmpty();
        kept += was;
        shown += now;
        reordered |= was != now;
        if (now && was && dock.text() != text) dockAccessibilityChanged(&dock);
    }
    reordered |= kept != std::exchange(m_shownDocks, shown);
    if (reordered && QAccessible::isActive()) {
        QAccessibleEvent event(this, QAccessible::ObjectReorder);
        QAccessible::updateAccessibility(&event);
    }
    return area;
}

void TerminalWindow::setDockOptions(const AfterglowVt::TerminalOptions& options)
{
    if (m_docks.empty()) return;
    for (const auto& dock : m_docks) {
        const QString text = dock->text();
        dock->setOptions(options);
        if (!dock->rect().isEmpty() && dock->text() != text) dockAccessibilityChanged(dock.get());
    }
    // The items move with the widths of the text.
    applyPointerShape();
}

DockArea* TerminalWindow::dockAt(const QPoint& pixel) const
{
    for (const auto& dock : m_docks) {
        if (dock->rect().contains(pixel)) return dock.get();
    }
    return nullptr;
}

QList<const DockArea*> TerminalWindow::accessibleDocks() const
{
    QList<const DockArea*> shown;
    for (const auto& dock : m_docks) {
        if (!dock->rect().isEmpty()) shown.append(dock.get());
    }
    return shown;
}

QRect TerminalWindow::accessibleDockRect(const DockArea* dock) const
{
    const auto shown = std::find_if(m_docks.begin(), m_docks.end(), [dock](const auto& d) { return d.get() == dock; });
    if (shown == m_docks.end() || dock->rect().isEmpty()) return {};
    const qreal dpr = devicePixelRatio();
    const QRect px = dock->rect();
    return QRect(mapToGlobal((QPointF(px.topLeft()) / dpr).toPoint()), (QSizeF(px.size()) / dpr).toSize());
}

} // namespace Afterglow
