// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include "TerminalWindow.h"
#include "app/WindowStart.h"
#include "chrome/ChromeController.h"
#include <algorithm>
#include <cmath>

namespace Afterglow {
void TerminalWindow::releasePaneUi(Pane* pane)
{
    if (m_scriptDialog && m_scriptDialog->pane == pane) finishScriptDialog(false, {});
    // The pane goes on in another window: a clipboard read it asked for is
    // denied, as a program expects, rather than dropped.
    if (m_pendingClipboardRead && m_pendingClipboardRead->pane == pane) m_controller->answerClipboard(0);
    // Only what belongs to this pane: a paste or a title the user is
    // giving another pane stays open.
    dropPanePrompts(pane);
    if (m_copyPane == pane) stopCopyMode();
    if (m_hover.pane == pane) m_hover = {};
    m_dividerDrag.reset(); // the layout changes under it
    if (m_menuTab && tabOf(pane) >= 0 && m_tabs[std::size_t(tabOf(pane))].get() == m_menuTab) {
        m_controller->closeContextMenu();
        m_menuTab = nullptr;
    }
    if (std::any_of(m_pendingConfirmPanes.cbegin(), m_pendingConfirmPanes.cend(), [pane](const auto& p) { return p == pane; })) {
        m_controller->answerConfirm(false); // it would close a pane that lives elsewhere now
    }
    if (m_focused == pane) {
        m_controller->setSearchOpen(false);
        pane->clearSearch();
        setFocusedPane(nullptr);
    }
}

void TerminalWindow::reparentPane(Pane* pane, TerminalWindow* destination)
{
    if (destination == this) return;
    m_shaderControls.removePane(pane->id()); // instance IDs belong to the source window
    QObject::disconnect(pane, nullptr, this, nullptr);
    // The destroyed handler that cleans these is disconnected: clean them now.
    m_paneOpening.remove(pane);
    m_paneContext.remove(pane);
    m_paneOrigin.remove(pane);
    m_panePulse.remove(pane);
    m_historyRows.erase(pane);
    if (m_scriptKeyMode && m_scriptKeyMode->pane == pane) m_scriptKeyMode->pane = nullptr;
    if (auto node = m_statusViews.extract(pane); !node.empty()) destination->m_statusViews.insert(std::move(node));
    if (auto node = m_clipboardGrants.extract(pane); !node.empty()) destination->m_clipboardGrants.insert(std::move(node));
    if (m_notifyOnFinish.remove(pane)) destination->m_notifyOnFinish.insert(pane);
    const auto base = destination->m_scriptAppearanceOrder;
    for (auto it = m_scriptAppearance.begin(); it != m_scriptAppearance.end();) {
        if (it->pane != pane) { ++it; continue; }
        it->order += base;
        destination->m_scriptAppearanceOrder = std::max(destination->m_scriptAppearanceOrder, it->order);
        destination->m_scriptAppearance.push_back(std::move(*it));
        it = m_scriptAppearance.erase(it);
    }
    if (const auto it = m_motion.find(pane); it != m_motion.end())
        destination->motion(pane).pointerShape = it->second->pointerShape;
    m_motion.erase(pane); // motion objects reference the source window's clock
    m_growFrom.erase(pane);
    pane->setParent(destination);
    destination->bindPane(pane);
}

void TerminalWindow::removeTabShell(int index)
{
    m_splits = nullptr;
    m_tabs.erase(m_tabs.begin() + index);
    if (index < m_currentTab) --m_currentTab;
    if (m_currentTab >= int(m_tabs.size())) m_currentTab = int(m_tabs.size()) - 1;
}

void TerminalWindow::finishTransfer(quint32 previousTab, int previousIndex)
{
    m_splits = nullptr;
    if (m_tabs.empty()) {
        m_currentTab = 0;
        m_focused = nullptr;
        m_closeConfirmed = true;
        close();
        return;
    }
    switchTab(std::max(0, m_currentTab));
    // Another tab shows now: a switch like any other, with its transition
    // when the tab that was current is still here to leave.
    if (const quint32 now = m_tabs[std::size_t(m_currentTab)]->id; previousTab && previousTab != now) {
        const auto left = std::find_if(m_tabs.begin(), m_tabs.end(), [previousTab](const auto& tab) { return tab->id == previousTab; });
        const int from = left == m_tabs.end() ? previousIndex : int(left - m_tabs.begin());
        if (left != m_tabs.end()) startTabTransition(from, m_currentTab);
        Q_EMIT tabSwitched(previousTab, now, m_currentTab >= from ? 1 : -1);
    }
    updatePaneVisibility();
    updateTitle();
    if (!isVisible() && !m_quick) showForStartMode(*this, m_settings.settings().window);
}

QString TerminalWindow::movePaneTo(Pane* pane, TerminalWindow* destination, Pane* beside, SplitTree::Direction direction)
{
    const int index = tabOf(pane);
    if (index < 0 || !destination || (beside && destination->tabOf(beside) < 0) || pane == beside)
        return QStringLiteral("Invalid pane destination");
    if (m_focused && m_currentTab >= 0) m_tabs[std::size_t(m_currentTab)]->focused = m_focused;
    if (destination != this && destination->m_focused && destination->m_currentTab >= 0)
        destination->m_tabs[std::size_t(destination->m_currentTab)]->focused = destination->m_focused;
    // The tabs each window shows now, to tell a switch from staying put.
    const auto current = [](const TerminalWindow* w) {
        return w->m_currentTab >= 0 && w->m_currentTab < int(w->m_tabs.size()) ? w->m_tabs[std::size_t(w->m_currentTab)]->id : 0;
    };
    const quint32 sourceTab = current(this), destinationTab = current(destination);
    const int sourceIndex = m_currentTab, destinationIndex = destination->m_currentTab;
    const bool wasFocused = pane == m_focused;
    releasePaneUi(pane);
    auto& tab = *m_tabs[std::size_t(index)];
    // Another pane keeps its zoom; the tree drops the zoom of the pane it removes.
    if (tab.splits->paneCount() == 1) removeTabShell(index);
    else {
        tab.splits->remove(pane);
        if (wasFocused || tab.focused == pane) {
            tab.focused = nullptr;
            tab.splits->forEachPane([&](Pane* next, const QRect&) { if (!tab.focused) tab.focused = next; });
        }
    }
    reparentPane(pane, destination);
    if (beside) {
        auto& target = *destination->m_tabs[std::size_t(destination->tabOf(beside))];
        target.splits->setZoomedPane(nullptr);
        target.splits->split(beside, pane, direction);
        target.focused = pane;
        destination->m_currentTab = destination->tabOf(beside);
    } else {
        auto target = std::make_unique<Tab>();
        target->splits = std::make_unique<SplitTree>(pane);
        target->focused = pane;
        destination->m_tabs.push_back(std::move(target));
        destination->m_currentTab = int(destination->m_tabs.size()) - 1;
    }
    if (destination != this) finishTransfer(sourceTab, sourceIndex);
    destination->finishTransfer(destination == this ? sourceTab : destinationTab,
                                destination == this ? sourceIndex : destinationIndex);
    destination->requestActivate();
    Q_EMIT destination->paneMoved(pane);
    return {};
}

QString TerminalWindow::detachTabTo(Pane* pane, TerminalWindow* destination)
{
    const int index = tabOf(pane);
    if (index < 0 || !destination || destination == this) return QStringLiteral("Invalid tab destination");
    // Both windows' current tabs keep their focus through the transfer: each
    // is shown again from its saved focus.
    if (m_focused && m_currentTab >= 0) m_tabs[std::size_t(m_currentTab)]->focused = m_focused;
    if (destination->m_focused && destination->m_currentTab >= 0)
        destination->m_tabs[std::size_t(destination->m_currentTab)]->focused = destination->m_focused;
    const quint32 sourceTab = m_currentTab >= 0 ? m_tabs[std::size_t(m_currentTab)]->id : 0;
    const int sourceIndex = m_currentTab;
    const quint32 destinationTab = destination->m_currentTab >= 0 && destination->m_currentTab < int(destination->m_tabs.size())
        ? destination->m_tabs[std::size_t(destination->m_currentTab)]->id : 0;
    const int destinationIndex = destination->m_currentTab;
    auto& source = *m_tabs[std::size_t(index)];
    QList<Pane*> moved;
    source.splits->forEachPane([&](Pane* p, const QRect&) { moved << p; });
    for (auto* p : moved) releasePaneUi(p);
    auto tab = std::move(m_tabs[std::size_t(index)]);
    removeTabShell(index);
    for (auto* p : moved) reparentPane(p, destination);
    destination->m_tabs.push_back(std::move(tab)); // retains the live tab's identity and ratios
    destination->m_currentTab = int(destination->m_tabs.size()) - 1;
    finishTransfer(sourceTab, sourceIndex);
    destination->finishTransfer(destinationTab, destinationIndex);
    destination->requestActivate();
    for (auto* p : moved) Q_EMIT destination->paneMoved(p);
    return {};
}

void TerminalWindow::settleRearrangement(bool wasZoomed)
{
    // The tree dropped a zoom: every pane shows again, from where it is.
    if (wasZoomed) {
        setPaneZoom(nullptr);
        return;
    }
    // Otherwise the panes ease to their new places (Animation.splitCurve).
    // Labels and the link under the pointer sit on cells that move.
    stopUrlMode();
    m_hover = {};
    m_dividerDrag.reset();
}

bool TerminalWindow::swapPane(Pane* pane, SplitTree::Neighbour direction)
{
    if (!m_splits || !m_splits->contains(pane)) return false;
    auto* other = m_splits->neighbour(pane, direction);
    const bool zoomed = m_splits->zoomedPane() != nullptr;
    if (!other || !m_splits->swap(pane, other)) return false;
    settleRearrangement(zoomed);
    updateChrome();
    m_layoutSize = {};
    scheduleRender();
    return true;
}

bool TerminalWindow::rotatePanes(bool clockwise)
{
    if (!m_splits) return false;
    const bool zoomed = m_splits->zoomedPane() != nullptr;
    if (!m_splits->rotate(clockwise)) return false;
    settleRearrangement(zoomed);
    updateChrome();
    m_layoutSize = {};
    scheduleRender();
    return true;
}
} // namespace Afterglow
