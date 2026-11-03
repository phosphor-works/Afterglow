// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TerminalWindow.h"

#include "DockArea.h"
#include "chrome/ChromeController.h"
#include "chrome/ChromeLayer.h"
#include "desktop/Accessibility.h"

#include <AfterglowRender/GpuDevice.h>

#include <QGuiApplication>
#include <QStyleHints>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>

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
        // The script still runs: it hears that the pointer left the item,
        // which an area that has gone could not tell it later.
        if (m_dockHover.owner == owner && m_dockHover.dock == id) {
            m_dockHover = {};
            emitDockEvent(**found, QStringLiteral("on_dock_hover"), {}, false);
        }
        // As the hover: the drag that it holds ends before it goes.
        if (m_dockDrag && m_dockDrag->owner == owner && m_dockDrag->dock == id) endDockDrag(false);
        releaseDockAccessibility(found->get());
        m_docks.erase(found);
        // Now, not at the next frame: a window that is not exposed draws none.
        windowAccessibilityChanged(this);
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
    // A dock that does not show is no child of the window: it tells of its
    // items when it shows (placeDocks).
    if (changed && !changed->rect().isEmpty()) dockAccessibilityChanged(changed);
    // An item can come or go under a pointer that does not move. A dock that
    // moves has its place at the next frame, whose relayout sets the shape
    // and the item: until then its rectangle is the old one. One that went
    // is gone now.
    if (change != DockArea::Change::Place || !changed) applyPointerShape();
    scheduleRender();
    return {};
}

void TerminalWindow::finishDockMoves()
{
    if (!std::exchange(m_docksMoved, false)) return;
    // relayout tells the chrome when the tab bar comes or goes and sets the
    // pointer shape and the hovered item, and placeDocks tells assistive
    // technology what shows and each dock's script where it shows.
    relayout();
}

void TerminalWindow::eraseDocks(const QString& owner)
{
    // Released in a loop of their own: the release sends events, which have
    // no place in erase_if's predicate.
    for (const auto& dock : m_docks) if (dock->owner() == owner) releaseDockAccessibility(dock.get());
    // A script that goes hears nothing more.
    if (m_dockDrag && m_dockDrag->owner == owner) m_dockDrag.reset();
    const auto erased = std::erase_if(m_docks, [&](const auto& dock) { return dock->owner() == owner; });
    if (erased == 0) return;
    windowAccessibilityChanged(this);
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
    // went, can show or hide a dock or cut its text anew. A dock that comes
    // or goes changes the window's children, which the next frame tells of
    // (updateTerminalAccessibility). One that shows tells of what changed
    // in it since it last showed, and of where it is now. Each dock whose
    // cells changed tells its script too.
    for (std::size_t i = 0; i < m_docks.size(); ++i) {
        DockArea& dock = *m_docks[i];
        if (dock.setRect(rects[i], m.cellWidth, m.cellHeight) && !dock.rect().isEmpty()) dockAccessibilityChanged(&dock);
    }
    reportDockLayouts();
    return area;
}

void TerminalWindow::reportDockLayouts()
{
    // Every change, not only the last of a burst: a script that drops one
    // would lay its text out for a width that the dock no longer has.
    for (const auto& dock : m_docks) {
        if (const auto layout = dock->takeLayoutChange()) {
            emitDockEvent(*dock, QStringLiteral("on_dock_layout"),
                          {{QStringLiteral("shown"), layout->shown}, {QStringLiteral("cols"), layout->cols},
                           {QStringLiteral("rows"), layout->rows}},
                          false);
        }
    }
}

QSize TerminalWindow::gridSize() const
{
    const auto& m = m_renderer->fonts().fonts->metrics();
    const QSize px = windowPixels();
    return {px.width() / std::max(1, m.cellWidth), px.height() / std::max(1, m.cellHeight)};
}

void TerminalWindow::updateDockHover()
{
    // Each pointer move comes here: a window with no dock and no item to
    // leave has nothing to look at.
    if (m_docks.empty() && m_dockHover.owner.isEmpty()) return;
    // Only a pointer that a click on the item would reach: not one over the
    // chrome, outside the window, or in a drag that the panes, a divider,
    // the chrome or the overview took. A grab whose release went elsewhere
    // ends with its buttons.
    DockHover now;
    const QPoint px = (m_pointer * devicePixelRatio()).toPoint();
    const bool held = QGuiApplication::mouseButtons() != Qt::NoButton;
    const bool dragging = (m_dividerDrag && !m_overview.open) || m_selectionPane || m_reportedButtons != Qt::NoButton
        || (held && (m_chromeGrab || m_overviewButtons != Qt::NoButton));
    if (!dragging) {
        if (const DockArea* dock = dockAt(px)) {
            const QString item = dock->itemAt(px);
            const bool chrome = (!m_overview.open || m_controller->wantsKeyboard()) && chromeVisible()
                && m_chrome->wantsPointer(m_pointer);
            if (!item.isEmpty() && !chrome) now = {dock->owner(), dock->id(), item};
        }
    }
    if (now == m_dockHover) return;
    const DockHover before = std::exchange(m_dockHover, now);
    auto find = [this](const DockHover& hover) -> const DockArea* {
        const auto it = std::find_if(m_docks.begin(), m_docks.end(), [&](const auto& dock) {
            return dock->owner() == hover.owner && dock->id() == hover.dock;
        });
        return it == m_docks.end() ? nullptr : it->get();
    };
    // A dock that the pointer left hears of it, if it is still there. One
    // whose item changed hears only of the new item.
    if (!before.owner.isEmpty() && (before.owner != now.owner || before.dock != now.dock)) {
        if (const DockArea* dock = find(before)) emitDockEvent(*dock, QStringLiteral("on_dock_hover"), {}, false);
    }
    if (const DockArea* dock = now.owner.isEmpty() ? nullptr : find(now))
        emitDockEvent(*dock, QStringLiteral("on_dock_hover"), {{QStringLiteral("item"), now.item}}, false);
}

void TerminalWindow::wheelDock(DockArea& dock, const QPoint& pixel, QWheelEvent* e)
{
    const QString item = dock.itemAt(pixel);
    // Part of a notch over one item does not carry over to another.
    const DockHover at{dock.owner(), dock.id(), item};
    if (at != m_wheelDock) {
        m_wheelDock = at;
        m_dockWheel = {};
    }
    m_dockWheel += e->angleDelta();
    const int up = m_dockWheel.y() / 120;
    const int left = m_dockWheel.x() / 120;
    m_dockWheel -= QPoint(left * 120, up * 120);
    auto send = [&](int steps, const char* forward, const char* back) {
        if (steps == 0) return;
        QVariantMap event{{QStringLiteral("direction"), QString::fromLatin1(steps > 0 ? forward : back)},
                          {QStringLiteral("steps"), std::abs(steps)}};
        if (!item.isEmpty()) event.insert(QStringLiteral("item"), item);
        // A wheel or a touchpad sends events faster than a click does.
        emitDockEvent(dock, QStringLiteral("on_dock_wheel"), event, true);
    };
    send(up, "up", "down");
    send(left, "left", "right");
}

void TerminalWindow::moveDockDrag(const QPointF& pos)
{
    if (!m_dockDrag) return;
    DockDrag& drag = *m_dockDrag;
    const auto it = std::find_if(m_docks.begin(), m_docks.end(), [&](const auto& dock) {
        return dock->owner() == drag.owner && dock->id() == drag.dock;
    });
    // An area that went took its drag with it (setScriptDock, eraseDocks).
    if (it == m_docks.end()) {
        m_dockDrag.reset();
        return;
    }
    const DockArea& dock = **it;
    if (!drag.started && (pos - drag.from).manhattanLength() < QGuiApplication::styleHints()->startDragDistance()) return;
    const QPoint px = (pos * devicePixelRatio()).toPoint();
    const auto& m = m_renderer->fonts().fonts->metrics();
    // Cells of the area, also past its edges: a drag goes on outside it.
    // An area that does not show has no cells to go by.
    QPoint cell = drag.cell;
    if (!dock.rect().isEmpty()) {
        cell = QPoint(int(std::floor(double(px.x() - dock.rect().left()) / std::max(1, m.cellWidth))),
                      int(std::floor(double(px.y() - dock.rect().top()) / std::max(1, m.cellHeight))));
    }
    const QString over = dock.rect().contains(px) ? dock.itemAt(px) : QString();
    const char* phase = drag.started ? "move" : "start";
    if (drag.started && over == drag.over && cell == drag.cell) return;
    drag.started = true;
    drag.over = over;
    drag.cell = cell;
    QVariantMap event{{QStringLiteral("phase"), QString::fromLatin1(phase)}, {QStringLiteral("item"), drag.item},
                      {QStringLiteral("col"), cell.x()}, {QStringLiteral("row"), cell.y()}};
    if (!over.isEmpty()) event.insert(QStringLiteral("over"), over);
    // As hover: at most one call for each cell that the pointer crosses.
    emitDockEvent(dock, QStringLiteral("on_dock_drag"), event, false);
}

void TerminalWindow::endDockDrag(bool released, const QPointF& pos)
{
    if (!m_dockDrag) return;
    if (released) moveDockDrag(pos);
    // moveDockDrag drops a drag whose area went.
    if (!m_dockDrag) return;
    const DockDrag drag = *std::exchange(m_dockDrag, std::nullopt);
    // A press and release with no drag between them was a click.
    if (!drag.started) return;
    const auto it = std::find_if(m_docks.begin(), m_docks.end(), [&](const auto& dock) {
        return dock->owner() == drag.owner && dock->id() == drag.dock;
    });
    if (it == m_docks.end()) return;
    QVariantMap event{{QStringLiteral("phase"), released ? QStringLiteral("end") : QStringLiteral("cancel")},
                      {QStringLiteral("item"), drag.item},
                      {QStringLiteral("col"), drag.cell.x()},
                      {QStringLiteral("row"), drag.cell.y()}};
    if (!drag.over.isEmpty()) event.insert(QStringLiteral("over"), drag.over);
    emitDockEvent(**it, QStringLiteral("on_dock_drag"), event, false);
}

QString TerminalWindow::openScriptTabMenu(Pane* pane)
{
    const int tab = tabOf(pane);
    if (tab < 0) return QStringLiteral("Pane closed");
    // Not over a prompt or the palette, which take the pointer: the tab
    // bar's own menu cannot open there either, and the menu's Close would
    // replace a prompt's pending answer. A menu that is open gives way, as
    // it does to a right click on the tab bar.
    if (m_scriptDialog || m_controller->confirmPromptOpen() || m_controller->titlePromptOpen()
        || m_controller->pasteConfirmOpen() || m_controller->clipboardPromptOpen() || m_controller->paletteOpen())
        return QStringLiteral("A prompt or the palette is open");
    const QPointF at = std::exchange(m_dockClickAt, std::nullopt).value_or(m_pointer.x() >= 0 ? m_pointer : QPointF(0, 0));
    openTabMenu(tab, at);
    return {};
}

QString TerminalWindow::moveTabOf(Pane* pane, int index)
{
    const int tab = tabOf(pane);
    if (tab < 0) return QStringLiteral("Pane closed");
    const int count = int(m_tabs.size());
    if (index < 0 || index >= count) return QStringLiteral("index %1 is not a tab: the window has %2").arg(index).arg(count);
    moveTab(tab, index);
    return {};
}

void TerminalWindow::setDockOptions(const AfterglowVt::TerminalOptions& options)
{
    if (m_docks.empty()) return;
    for (const auto& dock : m_docks) {
        dock->setOptions(options);
        if (!dock->rect().isEmpty()) dockAccessibilityChanged(dock.get());
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

bool TerminalWindow::showsDock(const DockArea* dock) const
{
    return std::any_of(m_docks.begin(), m_docks.end(), [dock](const auto& d) { return d.get() == dock; })
        && !dock->rect().isEmpty();
}

QRect TerminalWindow::globalRect(const QRect& pixels) const
{
    const qreal dpr = devicePixelRatio();
    return QRect(mapToGlobal((QPointF(pixels.topLeft()) / dpr).toPoint()), (QSizeF(pixels.size()) / dpr).toSize());
}

QRect TerminalWindow::accessibleDockRect(const DockArea* dock) const
{
    return showsDock(dock) ? globalRect(dock->rect()) : QRect();
}

QRect TerminalWindow::accessibleDockItemRect(const DockArea* dock, std::size_t index) const
{
    if (!showsDock(dock)) return {};
    const QRect px = dock->itemRect(index);
    return px.isEmpty() ? QRect() : globalRect(px);
}

void TerminalWindow::pressDockItem(const DockArea* dock, std::size_t index)
{
    // Only what a pointer could press: an item of a dock that shows, and
    // not under the chrome (a prompt, a menu, the scrollbar), which would
    // take the click.
    if (!showsDock(dock) || index >= dock->items().size()) return;
    const QPointF centre = QRectF(dock->itemRect(index)).center() / devicePixelRatio();
    if (chromeVisible() && m_chrome->wantsPointer(centre)) return;
    // As a click does: one while the overview closes onto a card lands
    // there, with that card's pane in the event.
    if (!m_overview.open) finishOverviewReveal();
    // A client can press as fast as it can call: the presses count toward
    // the script's callback limit, as a pointer's clicks need not.
    emitDockClick(*dock, dock->items()[index].id, QStringLiteral("left"), true, centre);
}

} // namespace Afterglow
