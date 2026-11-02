// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TerminalWindow.h"

#include <AfterglowRender/GpuDevice.h>

#include <algorithm>

namespace Afterglow {

namespace {
// Each script's docks in one window, and every script's together.
constexpr std::size_t MaxOwnerDocks = 8;
constexpr std::size_t MaxDocks = 32;
} // namespace

QString TerminalWindow::setScriptDock(const QString& owner, const QVariantMap& effect)
{
    const QString id = effect.value(QStringLiteral("id")).toString();
    auto found = std::find_if(m_docks.begin(), m_docks.end(), [&](const auto& dock) {
        return dock->owner() == owner && dock->id() == id;
    });
    bool moved = true;
    if (effect.value(QStringLiteral("kind")) == QLatin1String("undock")) {
        if (found == m_docks.end()) return {};
        m_docks.erase(found);
    } else {
        if (found == m_docks.end()) {
            const auto owned = std::count_if(m_docks.begin(), m_docks.end(), [&](const auto& d) { return d->owner() == owner; });
            if (std::size_t(owned) >= MaxOwnerDocks || m_docks.size() >= MaxDocks) return QStringLiteral("Too many docks");
            m_docks.push_back(std::make_unique<DockArea>(owner, id, m_settings.settings().terminal));
            found = std::prev(m_docks.end());
        }
        moved = (*found)->setContent(effect, ++m_dockOrder);
    }
    // The panes take what the docks leave, and the tab bar may come or go.
    // New text in a dock that stays where it is changes neither.
    if (moved) {
        if (m_splits) relayout();
        updateChrome();
    }
    scheduleRender();
    return {};
}

bool TerminalWindow::tabBarReplaced() const
{
    return std::any_of(m_docks.begin(), m_docks.end(), [](const auto& dock) { return dock->replacesTabBar(); });
}

QRect TerminalWindow::layoutDocks(QRect area, std::vector<QRect>* rects) const
{
    if (rects) rects->assign(m_docks.size(), QRect());
    if (m_docks.empty()) return area;
    const auto& m = m_renderer->fonts().fonts->metrics();
    const QSize minimum = minimumPaneSize();
    // From the window's edge inwards: the oldest dock on an edge is outermost.
    // Top and bottom docks span the window; left and right ones fit between.
    std::vector<std::size_t> order(m_docks.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [this](std::size_t a, std::size_t b) {
        const bool aSide = m_docks[a]->edge() == DockArea::Edge::Left || m_docks[a]->edge() == DockArea::Edge::Right;
        const bool bSide = m_docks[b]->edge() == DockArea::Edge::Left || m_docks[b]->edge() == DockArea::Edge::Right;
        return aSide != bSide ? !aSide : m_docks[a]->order() < m_docks[b]->order();
    });
    for (const std::size_t i : order) {
        const DockArea& dock = *m_docks[i];
        const bool side = dock.edge() == DockArea::Edge::Left || dock.edge() == DockArea::Edge::Right;
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
    }
    return area;
}

void TerminalWindow::placeDocks()
{
    std::vector<QRect> rects;
    const QSize px = m_renderer->pixelSize().isEmpty() ? size() * devicePixelRatio() : m_renderer->pixelSize();
    const int bar = tabBarHeight();
    layoutDocks(QRect(0, bar, px.width(), std::max(0, px.height() - bar)), &rects);
    const auto& m = m_renderer->fonts().fonts->metrics();
    for (std::size_t i = 0; i < m_docks.size(); ++i) m_docks[i]->setRect(rects[i], m.cellWidth, m.cellHeight);
}

DockArea* TerminalWindow::dockAt(const QPoint& pixel) const
{
    for (const auto& dock : m_docks) {
        if (dock->rect().contains(pixel)) return dock.get();
    }
    return nullptr;
}

} // namespace Afterglow
