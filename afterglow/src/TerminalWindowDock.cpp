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
// Each script's docks at one pane, and every script's together.
constexpr std::size_t MaxOwnerPaneDocks = 4;
constexpr std::size_t MaxPaneDocks = 8;

bool sideEdge(const DockArea& dock)
{
    return dock.edge() == DockArea::Edge::Left || dock.edge() == DockArea::Edge::Right;
}

// The last order given to a change of place. One count for every window: a
// pane takes its docks to another window, where a new dock still goes last.
quint64 s_dockOrder = 0;
} // namespace

template<typename Match>
DockArea* TerminalWindow::firstShownPaneDock(Match&& match) const
{
    // Only the panes drawn as laid out: the overview draws cards, which take
    // the pointer, and a pane hidden by a zoom shows none of its docks.
    if (!m_splits || m_overview.open) return nullptr;
    DockArea* found = nullptr;
    m_splits->forEachPane([&](Pane* pane, const QRect& rect) {
        if (found || rect.isEmpty()) return;
        for (const auto& dock : pane->docks()) {
            if (!dock->rect().isEmpty() && match(dock.get())) {
                found = dock.get();
                return;
            }
        }
    });
    return found;
}

QPoint TerminalWindow::dockPixel(const QPointF& logical) const
{
    // The pixel the point is in, not the nearest corner: cells and items
    // start at a pixel's left and top edges.
    const QPointF px = logical * devicePixelRatio();
    return QPoint(int(std::floor(px.x())), int(std::floor(px.y())));
}

QString TerminalWindow::setScriptDock(const QString& owner, const QVariantMap& effect, Pane* pane)
{
    const QString id = effect.value(QStringLiteral("id")).toString();
    const bool atPane = effect.value(QStringLiteral("scope")) == QLatin1String("pane");
    if (atPane && (!pane || tabOf(pane) < 0)) return QStringLiteral("Pane closed");
    auto& docks = atPane ? pane->docks() : m_docks;
    const quint32 paneId = atPane ? pane->id() : 0;
    auto found = std::find_if(docks.begin(), docks.end(), [&](const auto& dock) {
        return dock->owner() == owner && dock->id() == id;
    });
    DockArea::Change change = DockArea::Change::Place;
    DockArea* changed = nullptr;
    if (effect.value(QStringLiteral("kind")) == QLatin1String("undock")) {
        if (found == docks.end()) return {};
        // The script still runs: it hears that the pointer left the item,
        // which an area that has gone could not tell it later.
        if (m_dockHover.owner == owner && m_dockHover.dock == id && m_dockHover.pane == paneId) leaveDockHover();
        // As the hover: the drag that it holds ends before it goes.
        if (m_dockDrag && m_dockDrag->owner == owner && m_dockDrag->dock == id && m_dockDrag->pane == paneId)
            endDockDrag(false);
        releaseDockAccessibility(found->get());
        docks.erase(found);
        // Now, not at the next frame: a window that is not exposed draws none.
        windowAccessibilityChanged(this);
    } else {
        if (found == docks.end()) {
            const auto owned = std::count_if(docks.begin(), docks.end(), [&](const auto& d) { return d->owner() == owner; });
            const std::size_t ownerLimit = atPane ? MaxOwnerPaneDocks : MaxOwnerDocks;
            const std::size_t limit = atPane ? MaxPaneDocks : MaxDocks;
            if (std::size_t(owned) >= ownerLimit || docks.size() >= limit)
                return QStringLiteral("too many docks: a script has at most %1 in a %2, and a %2 at most %3")
                    .arg(ownerLimit)
                    .arg(atPane ? QStringLiteral("pane") : QStringLiteral("window"))
                    .arg(limit);
            docks.push_back(std::make_unique<DockArea>(owner, id, m_settings.settings().terminal, paneId));
            found = std::prev(docks.end());
        }
        changed = found->get();
        change = changed->setContent(effect, s_dockOrder + 1);
        if (change == DockArea::Change::Place) ++s_dockOrder;
    }
    // The same effect again: nothing to draw.
    if (change == DockArea::Change::None) return {};
    // The panes take what the docks leave, and the tab bar may come or go:
    // once in the next frame, however many docks move before it. New text
    // in a dock that stays where it is changes neither. A pane in another
    // tab is laid out in its tab's last layout, now: its docks show on its
    // overview card, and its shell gets the size that they leave.
    if (change == DockArea::Change::Place) {
        if (!atPane || tabOf(pane) == m_currentTab) m_docksMoved = true;
        else layoutBackgroundPane(pane);
    }
    // A dock that does not show is no child of the window: it tells of its
    // items when it shows (placeDocks).
    if (changed && showsDock(changed)) dockAccessibilityChanged(changed);
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
    const auto mine = [&](const auto& dock) { return dock->owner() == owner; };
    for (const auto& dock : m_docks) if (mine(dock)) releaseDockAccessibility(dock.get());
    // A script that goes hears nothing more.
    if (m_dockDrag && m_dockDrag->owner == owner) m_dockDrag.reset();
    auto erased = std::erase_if(m_docks, mine);
    for (Pane* pane : panes()) {
        for (const auto& dock : pane->docks()) if (mine(dock)) releaseDockAccessibility(dock.get());
        if (std::erase_if(pane->docks(), mine) == 0) continue;
        erased += 1;
        if (tabOf(pane) != m_currentTab) layoutBackgroundPane(pane);
    }
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

namespace {
/// From the edge of @p area inward: top and bottom docks span it, and left
/// and right ones fit between them. A tab_bar dock is outermost at the top,
/// where the tab bar is. Then the dock whose place was set first is
/// outermost. With @p minimum, a dock that would leave less than it does
/// not show. Without it, only the docks that show now do, at the size that
/// they have or what is left of @p area. Returns what the docks leave. No
/// allocation: this runs for each pane drawn.
QRect stackDocks(const std::vector<std::unique_ptr<DockArea>>& docks, QRect area, const QSize* minimum, int cellWidth,
                 int cellHeight, std::vector<QRect>* rects, bool* tabBarPlaced)
{
    if (rects) rects->assign(docks.size(), QRect());
    if (tabBarPlaced) *tabBarPlaced = false;
    if (docks.empty()) return area;
    std::array<std::uint8_t, MaxDocks> order;
    const std::size_t count = std::min(docks.size(), MaxDocks);
    for (std::size_t i = 0; i < count; ++i) order[i] = std::uint8_t(i);
    // The orders differ, so the sort needs no stability.
    std::sort(order.begin(), order.begin() + std::ptrdiff_t(count), [&docks](std::uint8_t a, std::uint8_t b) {
        const DockArea& x = *docks[a];
        const DockArea& y = *docks[b];
        if (sideEdge(x) != sideEdge(y)) return !sideEdge(x);
        if (x.replacesTabBar() != y.replacesTabBar()) return x.replacesTabBar();
        return x.order() < y.order();
    });
    for (std::size_t n = 0; n < count; ++n) {
        const std::size_t i = order[n];
        const DockArea& dock = *docks[i];
        const bool side = sideEdge(dock);
        int extent = dock.size() * (side ? cellWidth : cellHeight);
        if (minimum) {
            // A dock that would leave the panes too small is not shown.
            if (side ? area.width() - extent < minimum->width() : area.height() - extent < minimum->height()) continue;
        } else {
            if (dock.rect().isEmpty()) continue;
            // A split animation can draw the pane smaller than the layout
            // that placed the dock: it takes no more than is left.
            extent = std::min(side ? dock.rect().width() : dock.rect().height(), side ? area.width() : area.height());
            if (extent <= 0) continue;
        }
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
} // namespace

QRect TerminalWindow::layoutDocks(QRect area, std::vector<QRect>* rects, bool* tabBarPlaced) const
{
    const auto& m = m_renderer->fonts().fonts->metrics();
    const QSize minimum = minimumPaneSize();
    return stackDocks(m_docks, area, &minimum, m.cellWidth, m.cellHeight, rects, tabBarPlaced);
}

QRect TerminalWindow::paneDockArea(const Pane* pane, const QRect& paneRect, std::vector<QRect>* rects) const
{
    if (pane->docks().empty()) {
        if (rects) rects->clear();
        return paneRect;
    }
    const auto& m = m_renderer->fonts().fonts->metrics();
    return stackDocks(pane->docks(), paneRect, nullptr, m.cellWidth, m.cellHeight, rects, nullptr);
}

QRect TerminalWindow::placePaneDocks(Pane* pane, const QRect& paneRect)
{
    if (pane->docks().empty()) return paneRect;
    const auto& m = m_renderer->fonts().fonts->metrics();
    const QSize minimum = minimumPaneSize();
    // The overview places other tabs' docks each frame: no allocation.
    std::vector<QRect>& rects = m_paneDockRects;
    const QRect area = stackDocks(pane->docks(), paneRect, &minimum, m.cellWidth, m.cellHeight, &rects, nullptr);
    for (std::size_t i = 0; i < pane->docks().size(); ++i) {
        DockArea& dock = *pane->docks()[i];
        if (dock.setRect(rects[i], m.cellWidth, m.cellHeight) && !dock.rect().isEmpty()) dockAccessibilityChanged(&dock);
        reportDockLayout(dock);
    }
    return area;
}

void TerminalWindow::layoutPane(Pane* pane, const QRect& paneRect)
{
    // A pane hidden by a zoom shows no dock, and its shell keeps its size.
    if (paneRect.isEmpty()) {
        placePaneDocks(pane, QRect());
        return;
    }
    const auto& m = m_renderer->fonts().fonts->metrics();
    const QRect content = contentRect(placePaneDocks(pane, paneRect));
    const int cols = std::max(2, content.width() / m.cellWidth);
    const int rows = std::max(1, content.height() / m.cellHeight);
    pane->resize(cols, rows, m.cellWidth, m.cellHeight);
    // An overlay has the pane's grid, and its program hears of each change.
    if (PaneOverlay* overlay = pane->overlay()) overlay->pane()->resize(cols, rows, m.cellWidth, m.cellHeight);
}

void TerminalWindow::placeTabDocks(int tab)
{
    if (tab < 0 || tab >= int(m_tabs.size()) || !m_tabs[std::size_t(tab)]->splits) return;
    m_tabs[std::size_t(tab)]->splits->forEachPane([this](Pane* p, const QRect& r) { placePaneDocks(p, r); });
}

void TerminalWindow::layoutBackgroundPane(Pane* pane)
{
    const int tab = tabOf(pane);
    if (tab < 0) return;
    const auto& splits = m_tabs[std::size_t(tab)]->splits;
    if (splits) layoutPane(pane, splits->isVisible(pane) ? splits->rectOf(pane) : QRect());
}

void TerminalWindow::addPaneDocks(Pane* pane, const QRect& shown,
                                  std::vector<AfterglowRender::WindowRenderer::Pane::Dock>& out)
{
    if (pane->docks().empty()) return;
    // Where the pane is drawn, which a split animation moves: the docks keep
    // the cells that the layout gave them, and move with it. The rectangles
    // reuse one vector: this runs for each pane drawn.
    paneDockArea(pane, shown, &m_paneDockRects);
    for (std::size_t i = 0; i < m_paneDockRects.size(); ++i) {
        if (!m_paneDockRects[i].isEmpty()) out.push_back({&pane->docks()[i]->snapshot(), m_paneDockRects[i]});
    }
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
    for (const auto& dock : m_docks) reportDockLayout(*dock);
}

void TerminalWindow::reportDockLayout(DockArea& dock)
{
    if (const auto layout = dock.takeLayoutChange()) {
        emitDockEvent(dock, QStringLiteral("on_dock_layout"),
                      {{QStringLiteral("shown"), layout->shown}, {QStringLiteral("cols"), layout->cols},
                       {QStringLiteral("rows"), layout->rows}},
                      false);
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
    if (m_docks.empty() && m_dockHover.owner.isEmpty() && !firstShownPaneDock([](DockArea*) { return true; })) return;
    // Only a pointer that a click on the item would reach: not one over the
    // chrome, outside the window, or in a drag that the panes, a divider,
    // the chrome or the overview took. A grab whose release went elsewhere
    // ends with its buttons.
    DockHover now;
    const QPoint px = dockPixel(m_pointer);
    const bool held = QGuiApplication::mouseButtons() != Qt::NoButton;
    const bool dragging = (m_dividerDrag && !m_overview.open) || m_selectionPane || m_reportedButtons != Qt::NoButton
        || (held && (m_chromeGrab || m_overviewButtons != Qt::NoButton));
    if (!dragging) {
        if (const DockArea* dock = dockAt(px)) {
            const QString item = dock->itemAt(px);
            const bool chrome = (!m_overview.open || m_controller->wantsKeyboard()) && chromeVisible()
                && m_chrome->wantsPointer(m_pointer);
            if (!item.isEmpty() && !chrome) now = {dock->owner(), dock->id(), item, dock->pane()};
        }
    }
    if (now == m_dockHover) return;
    const DockHover before = std::exchange(m_dockHover, now);
    auto find = [this](const DockHover& hover) -> const DockArea* { return findDock(hover.owner, hover.dock, hover.pane); };
    // A dock that the pointer left hears of it, if it is still there. One
    // whose item changed hears only of the new item.
    if (!before.owner.isEmpty() && (before.owner != now.owner || before.dock != now.dock || before.pane != now.pane)) {
        if (const DockArea* dock = find(before)) emitDockEvent(*dock, QStringLiteral("on_dock_hover"), {}, false);
    }
    if (const DockArea* dock = now.owner.isEmpty() ? nullptr : find(now))
        emitDockEvent(*dock, QStringLiteral("on_dock_hover"), {{QStringLiteral("item"), now.item}}, false);
}

void TerminalWindow::leaveDockHover()
{
    const DockHover before = std::exchange(m_dockHover, {});
    if (before.owner.isEmpty()) return;
    if (const DockArea* dock = findDock(before.owner, before.dock, before.pane))
        emitDockEvent(*dock, QStringLiteral("on_dock_hover"), {}, false);
}

void TerminalWindow::dropDockPointer(const Pane* pane)
{
    // While the docks are still here to name: their scripts hear that the
    // drag ended and the pointer left the item.
    const quint32 id = pane ? pane->id() : 0;
    if (m_dockDrag && (!pane || m_dockDrag->pane == id)) endDockDrag(false);
    if (!m_dockHover.owner.isEmpty() && (!pane || m_dockHover.pane == id)) leaveDockHover();
}

void TerminalWindow::wheelDock(DockArea& dock, const QPoint& pixel, QWheelEvent* e)
{
    const QString item = dock.itemAt(pixel);
    // Part of a notch over one item does not carry over to another.
    const DockHover at{dock.owner(), dock.id(), item, dock.pane()};
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
    // An undock and a pane that leaves end the drag while its area is here
    // (setScriptDock, dropDockPointer). Only a script that went, which
    // hears nothing more, leaves one with no area (eraseDocks).
    const DockArea* found = findDock(drag.owner, drag.dock, drag.pane);
    if (!found) {
        m_dockDrag.reset();
        return;
    }
    const DockArea& dock = *found;
    // A pane's area that stopped showing (its tab went to the background, a
    // zoom hid its pane, the overview opened) has nothing under the pointer.
    if (dock.pane() && !showsDock(&dock)) {
        endDockDrag(false);
        return;
    }
    if (!drag.started && (pos - drag.from).manhattanLength() < QGuiApplication::styleHints()->startDragDistance()) return;
    const QPoint px = dockPixel(pos);
    // Cells of the area, also past its edges: a drag goes on outside it. A
    // window area that does not show has no cells to go by: the last cell,
    // first the press's, holds.
    const QPoint cell = dock.rect().isEmpty() ? drag.cell : dockCell(dock, px);
    const QString over = dock.rect().contains(px) ? dock.itemAt(px) : QString();
    const bool moving = drag.started;
    const char* phase = moving ? "move" : "start";
    if (moving && over == drag.over && cell == drag.cell) return;
    drag.started = true;
    drag.over = over;
    drag.cell = cell;
    QVariantMap event{{QStringLiteral("phase"), QString::fromLatin1(phase)}, {QStringLiteral("item"), drag.item},
                      {QStringLiteral("col"), cell.x()}, {QStringLiteral("row"), cell.y()}};
    if (!over.isEmpty()) event.insert(QStringLiteral("over"), over);
    // At most one call for each new item or cell under the pointer. A fast
    // drag crosses cells faster than a script needs them: moves count
    // toward the callback limit, and the next move or the end carries what
    // a dropped one would have told. Start, end and cancel always go.
    emitDockEvent(dock, QStringLiteral("on_dock_drag"), event, moving);
}

QPoint TerminalWindow::dockCell(const DockArea& dock, const QPoint& pixel) const
{
    const auto& m = m_renderer->fonts().fonts->metrics();
    return QPoint(int(std::floor(double(pixel.x() - dock.rect().left()) / std::max(1, m.cellWidth))),
                  int(std::floor(double(pixel.y() - dock.rect().top()) / std::max(1, m.cellHeight))));
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
    const DockArea* dock = findDock(drag.owner, drag.dock, drag.pane);
    if (!dock) return;
    QVariantMap event{{QStringLiteral("phase"), released ? QStringLiteral("end") : QStringLiteral("cancel")},
                      {QStringLiteral("item"), drag.item},
                      {QStringLiteral("col"), drag.cell.x()},
                      {QStringLiteral("row"), drag.cell.y()}};
    if (!drag.over.isEmpty()) event.insert(QStringLiteral("over"), drag.over);
    emitDockEvent(*dock, QStringLiteral("on_dock_drag"), event, false);
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
    bool any = false;
    auto apply = [&](DockArea& dock) {
        any = true;
        dock.setOptions(options);
        if (!dock.rect().isEmpty()) dockAccessibilityChanged(&dock);
    };
    for (const auto& dock : m_docks) apply(*dock);
    for (Pane* pane : panes()) for (const auto& dock : pane->docks()) apply(*dock);
    // The items move with the widths of the text.
    if (any) applyPointerShape();
}

DockArea* TerminalWindow::findDock(const QString& owner, const QString& id, quint32 pane) const
{
    auto match = [&](const auto& dock) { return dock->owner() == owner && dock->id() == id; };
    if (pane == 0) {
        const auto it = std::find_if(m_docks.begin(), m_docks.end(), match);
        return it == m_docks.end() ? nullptr : it->get();
    }
    // Through the trees, not panes(): a drag asks at each pointer move.
    DockArea* found = nullptr;
    for (const auto& tab : m_tabs) {
        if (!tab->splits || found) continue;
        tab->splits->forEachPane([&](Pane* p, const QRect&) {
            if (found || p->id() != pane) return;
            const auto it = std::find_if(p->docks().begin(), p->docks().end(), match);
            if (it != p->docks().end()) found = it->get();
        });
    }
    return found;
}

int TerminalWindow::dividerTolerance() const
{
    return int(std::lround(4 * devicePixelRatio()));
}

DockArea* TerminalWindow::dockAt(const QPoint& pixel) const
{
    for (const auto& dock : m_docks) {
        if (dock->rect().contains(pixel)) return dock.get();
    }
    // The window's docks are over the panes and the overview, the panes'
    // ones in the panes.
    return firstShownPaneDock([&pixel](DockArea* d) { return d->rect().contains(pixel); });
}

bool TerminalWindow::dividerTakesDock(const QPoint& pixel) const
{
    // A divider's grab reaches into a pane's dock at the pane's edge, as
    // into the pane's text. A window's dock is over the dividers.
    if (!m_splits || m_overview.open || !m_splits->dividerAt(pixel, dividerTolerance())) return false;
    const DockArea* dock = dockAt(pixel);
    return dock && dock->pane() != 0;
}

Pane* TerminalWindow::paneUnder(const QPointF& logical) const
{
    const QPoint pixel = dockPixel(logical);
    if (!m_splits || dockAt(pixel)) return nullptr;
    return m_splits->paneAt(pixel);
}

QList<const DockArea*> TerminalWindow::accessibleDocks() const
{
    QList<const DockArea*> shown;
    for (const auto& dock : m_docks) {
        if (!dock->rect().isEmpty()) shown.append(dock.get());
    }
    firstShownPaneDock([&shown](DockArea* d) {
        shown.append(d);
        return false;
    });
    return shown;
}

bool TerminalWindow::showsDock(const DockArea* dock) const
{
    // By address first: an accessibility object can ask about a dock that went.
    if (std::any_of(m_docks.begin(), m_docks.end(), [dock](const auto& d) { return d.get() == dock; }))
        return !dock->rect().isEmpty();
    return firstShownPaneDock([dock](DockArea* d) { return d == dock; }) != nullptr;
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
    // A client can press as fast as it can call: the presses count toward
    // the script's callback limit, as a pointer's clicks need not.
    emitDockClick(*dock, dock->items()[index].id, QStringLiteral("left"), true, centre);
}

} // namespace Afterglow
