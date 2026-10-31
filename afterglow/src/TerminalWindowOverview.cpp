// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include "TerminalWindow.h"
#include "chrome/ChromeController.h"
#include <PhosphorAnimation/CurveRegistry.h>
#include <PhosphorAnimation/Spring.h>
#include <QKeyEvent>
#include <QLoggingCategory>
#include <algorithm>
#include <cmath>

Q_DECLARE_LOGGING_CATEGORY(lcWindow)

namespace Afterglow {

std::vector<Pane*> TerminalWindow::overviewPanes() const
{
    // While the overview shows, every tab's panes; otherwise the current tab's.
    std::vector<Pane*> panes;
    const bool allTabs = m_overview.open || m_overview.amount.value() != 0.0 || m_overview.amount.isAnimating();
    for (std::size_t tab = 0; tab < m_tabs.size(); ++tab) {
        const SplitTree* tree = m_tabs[tab]->splits.get();
        if (!tree || (!allTabs && int(tab) != m_currentTab)) continue;
        tree->forEachPane([&](Pane* pane, const QRect& rect) {
            if (!rect.isEmpty() && panes.size() < std::size_t(AfterglowRender::MaxShaderPanes)) panes.push_back(pane);
        });
    }
    return panes;
}

void TerminalWindow::layoutOtherTabs()
{
    // Hidden tabs keep the layout of when they were last shown; the overview
    // shows them at the window's current size.
    const int dividerWidth = std::max(1, int(std::lround(devicePixelRatio())));
    for (std::size_t tab = 0; tab < m_tabs.size(); ++tab) {
        if (int(tab) != m_currentTab && m_tabs[tab]->splits) m_tabs[tab]->splits->layout(paneArea(), dividerWidth);
    }
}

PhosphorAnimation::MotionSpec<qreal> TerminalWindow::overviewSpec()
{
    const AnimationSettings& anim = m_settings.settings().animation;
    if (!m_overview.curve || anim.overviewCurve != m_overview.curveSpec) {
        m_overview.curveSpec = anim.overviewCurve;
        m_overview.curve = m_shaders.curves().tryCreate(anim.overviewCurve);
        if (!m_overview.curve) {
            qCWarning(lcWindow).noquote() << "Animation.overviewCurve" << anim.overviewCurve << "is not a curve; using the default spring";
            m_overview.curve = std::make_shared<PhosphorAnimation::Spring>(12.0, 0.82);
        }
    }
    PhosphorAnimation::MotionSpec<qreal> spec;
    spec.clock = m_clock.get();
    spec.profile.curve = m_overview.curve;
    spec.profile.duration = anim.overviewDuration;
    spec.retargetPolicy = PhosphorAnimation::RetargetPolicy::PreserveVelocity;
    return spec;
}

bool TerminalWindow::openOverview()
{
    if (m_overview.open || !m_overviewAvailable || !m_splits || !m_focused) return false;
    cancelKeyMode();
    m_overview.zoomed = m_splits->zoomedPane();
    if (m_overview.zoomed) setPaneZoom(nullptr);
    m_overview.open = true;
    layoutOtherTabs();
    const auto panes = overviewPanes();
    const auto focused = std::find(panes.begin(), panes.end(), m_focused.data());
    if (focused == panes.end()) {
        m_overview.open = false;
        return false;
    }
    m_overview.original = m_focused;
    m_overview.highlighted = m_focused;
    const auto spec = overviewSpec();
    // Reopening while it closes continues from where it is.
    m_overview.amount.start(m_overview.amount.value(), 1.0, spec);
    const qreal index = qreal(focused - panes.begin());
    if (!m_overview.position.isAnimating() && m_overview.amount.value() <= 0.0) m_overview.position.start(index, index, spec);
    else m_overview.position.start(m_overview.position.value(), index, spec);
    m_overview.changedAt = m_overview.movedAt = std::chrono::steady_clock::now();
    updateKeyMode();
    scheduleRender();
    return true;
}

void TerminalWindow::closeOverview(Pane* chosen)
{
    if (!m_overview.open) return;
    const auto panes = overviewPanes();
    m_overview.open = false;
    Pane* target = chosen && std::find(panes.begin(), panes.end(), chosen) != panes.end() ? chosen : m_overview.original.data();
    const bool otherTab = target && tabOf(target) != m_currentTab;
    // A zoomed tab returns zoomed: on the chosen pane, or as it was when
    // the choice is in another tab.
    if (m_overview.zoomed) setPaneZoom(target && !otherTab ? target : m_overview.zoomed.data());
    if (otherTab) revealPane(target);
    else if (target) setFocusedPane(target);
    const auto spec = overviewSpec();
    m_overview.amount.start(m_overview.amount.value(), 0.0, spec);
    if (const auto at = std::find(panes.begin(), panes.end(), target); at != panes.end())
        m_overview.position.start(m_overview.position.value(), qreal(at - panes.begin()), spec);
    m_overview.changedAt = std::chrono::steady_clock::now();
    m_overview.highlighted = nullptr;
    m_overview.original = nullptr;
    m_overview.zoomed = nullptr;
    updateKeyMode();
    scheduleRender();
}

void TerminalWindow::moveOverview(int index)
{
    const auto panes = overviewPanes();
    if (!m_overview.open || panes.empty()) return;
    const int count = int(panes.size());
    index = ((index % count) + count) % count;
    if (panes[std::size_t(index)] == m_overview.highlighted) return;
    m_overview.highlighted = panes[std::size_t(index)];
    m_overview.movedAt = std::chrono::steady_clock::now();
    scheduleRender();
}

void TerminalWindow::overviewKey(QKeyEvent* e)
{
    const auto panes = overviewPanes();
    const auto at = std::find(panes.begin(), panes.end(), m_overview.highlighted.data());
    const int current = at == panes.end() ? 0 : int(at - panes.begin());
    const Action action = m_settings.settings().keybinds.match(e);
    if (e->key() == Qt::Key_Escape || action == Action::PaneOverview) {
        closeOverview(nullptr);
    } else if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter || e->key() == Qt::Key_Space) {
        closeOverview(m_overview.highlighted);
    } else if (e->key() == Qt::Key_Left || e->key() == Qt::Key_Backtab || e->key() == Qt::Key_H) {
        moveOverview(current - 1);
    } else if (e->key() == Qt::Key_Right || e->key() == Qt::Key_Tab || e->key() == Qt::Key_L) {
        moveOverview(current + 1);
    } else if ((e->key() == Qt::Key_Up || e->key() == Qt::Key_K || e->key() == Qt::Key_Down || e->key() == Qt::Key_J)
               && !panes.empty()) {
        // The first pane of the previous or next tab, wrapping round.
        const bool down = e->key() == Qt::Key_Down || e->key() == Qt::Key_J;
        const int count = int(panes.size());
        const int here = tabOf(panes[std::size_t(current)]);
        int index = current;
        for (int step = 1; step <= count; ++step) {
            const int candidate = ((current + (down ? step : -step)) % count + count) % count;
            if (tabOf(panes[std::size_t(candidate)]) != here) { index = candidate; break; }
        }
        // Up lands on the first pane of that tab, as Down does.
        const int tab = tabOf(panes[std::size_t(index)]);
        while (index > 0 && tabOf(panes[std::size_t(index - 1)]) == tab) --index;
        moveOverview(index);
    } else if (e->key() == Qt::Key_Home) {
        moveOverview(0);
    } else if (e->key() == Qt::Key_End) {
        moveOverview(int(panes.size()) - 1);
    } else if (e->key() >= Qt::Key_1 && e->key() <= Qt::Key_9 && e->key() - Qt::Key_1 < int(panes.size())) {
        closeOverview(panes[std::size_t(e->key() - Qt::Key_1)]);
    }
}

void TerminalWindow::advanceOverview(bool& animating)
{
    if (m_overview.open || m_overview.amount.isAnimating()) layoutOtherTabs();
    const auto panes = overviewPanes();
    if (m_overview.open) {
        // A tab switch or a closed pane can take the overview's panes away.
        if (panes.empty() || (std::find(panes.begin(), panes.end(), m_overview.highlighted.data()) == panes.end()
                              && std::find(panes.begin(), panes.end(), m_overview.original.data()) == panes.end())) {
            closeOverview(nullptr);
        } else if (std::find(panes.begin(), panes.end(), m_overview.highlighted.data()) == panes.end()) {
            m_overview.highlighted = m_overview.original;
        }
    }
    if (m_overview.open) {
        const auto at = std::find(panes.begin(), panes.end(), m_overview.highlighted.data());
        const qreal target = qreal(at - panes.begin());
        if (target != m_overview.position.to() || !m_overview.position.isAnimating()) {
            if (m_overview.position.isAnimating()) m_overview.position.retarget(target);
            else if (m_overview.position.value() != target) m_overview.position.start(m_overview.position.value(), target, overviewSpec());
        }
    }
    m_overview.amount.advance();
    m_overview.position.advance();
    animating |= m_overview.amount.isAnimating() || m_overview.position.isAnimating();
}

} // namespace Afterglow
