// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include "TerminalWindow.h"
#include "chrome/ChromeController.h"
#include <algorithm>
#include <cmath>

namespace Afterglow {
void TerminalWindow::releasePaneUi(Pane* pane)
{
    if (m_scriptDialog && m_scriptDialog->pane == pane) finishScriptDialog(false, {});
    if (m_pendingClipboardRead && m_pendingClipboardRead->pane == pane) m_controller->answerClipboard(0);
    if (m_copyPane == pane) stopCopyMode();
    if (m_urlMode.pane == pane || m_hintCancel) stopUrlMode();
    if (m_selectionPane == pane) { m_selectionPane = nullptr; m_autoScroll.stop(); }
    if (m_hover.pane == pane) m_hover = {};
    m_dividerDrag.reset();
    m_controller->cancelTitle();
    m_titlePane = nullptr; m_titleTab = nullptr; m_menuTab = nullptr;
    m_controller->closeContextMenu();
    m_controller->answerConfirm(false);
    m_controller->answerPaste(false);
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

void TerminalWindow::finishTransfer()
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
    updatePaneVisibility();
    updateTitle();
    if (!isVisible() && !m_quick) show();
}

QString TerminalWindow::movePaneTo(Pane* pane, TerminalWindow* destination, Pane* beside, SplitTree::Direction direction)
{
    const int index = tabOf(pane);
    if (index < 0 || !destination || (beside && destination->tabOf(beside) < 0) || pane == beside)
        return QStringLiteral("Invalid pane destination");
    if (m_focused && m_currentTab >= 0) m_tabs[std::size_t(m_currentTab)]->focused = m_focused;
    if (destination != this && destination->m_focused && destination->m_currentTab >= 0)
        destination->m_tabs[std::size_t(destination->m_currentTab)]->focused = destination->m_focused;
    const bool wasFocused = pane == m_focused;
    releasePaneUi(pane);
    auto& tab = *m_tabs[std::size_t(index)];
    tab.splits->setZoomedPane(nullptr);
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
    if (destination != this) finishTransfer();
    destination->finishTransfer();
    destination->requestActivate();
    Q_EMIT destination->paneMoved(pane);
    return {};
}

QString TerminalWindow::detachTabTo(Pane* pane, TerminalWindow* destination)
{
    const int index = tabOf(pane);
    if (index < 0 || !destination || destination == this) return QStringLiteral("Invalid tab destination");
    auto& source = *m_tabs[std::size_t(index)];
    if (index == m_currentTab) source.focused = m_focused;
    QList<Pane*> moved;
    source.splits->forEachPane([&](Pane* p, const QRect&) { moved << p; });
    for (auto* p : moved) releasePaneUi(p);
    auto tab = std::move(m_tabs[std::size_t(index)]);
    removeTabShell(index);
    for (auto* p : moved) reparentPane(p, destination);
    destination->m_tabs.push_back(std::move(tab)); // retains the live tab's identity and ratios
    destination->m_currentTab = int(destination->m_tabs.size()) - 1;
    finishTransfer();
    destination->finishTransfer();
    destination->requestActivate();
    for (auto* p : moved) Q_EMIT destination->paneMoved(p);
    return {};
}

bool TerminalWindow::swapPane(Pane* pane, SplitTree::Neighbour direction)
{
    if (!m_splits || !m_splits->contains(pane)) return false;
    auto* other = m_splits->neighbour(pane, direction);
    if (!other || !m_splits->swap(pane, other)) return false;
    setPaneZoom(nullptr);
    updateChrome();
    m_layoutSize = {};
    scheduleRender();
    return true;
}

bool TerminalWindow::rotatePanes(bool clockwise)
{
    if (!m_splits || !m_splits->rotate(clockwise)) return false;
    setPaneZoom(nullptr);
    updateChrome();
    m_layoutSize = {};
    scheduleRender();
    return true;
}
} // namespace Afterglow
