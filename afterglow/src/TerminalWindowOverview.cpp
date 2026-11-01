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

PhosphorAnimation::MotionSpec<qreal> TerminalWindow::motionSpec(const QString& curve, int durationMs, CurveCache& cache,
                                                                 const char* setting)
{
    if (!cache.curve || curve != cache.spec) {
        cache.spec = curve;
        cache.curve = m_shaders.curves().tryCreate(curve);
        if (!cache.curve) {
            qCWarning(lcWindow).noquote() << setting << curve << "is not a curve; using a spring";
            cache.curve = std::make_shared<PhosphorAnimation::Spring>(12.0, 1.0);
        }
    }
    PhosphorAnimation::MotionSpec<qreal> spec;
    spec.clock = m_clock.get();
    spec.profile.curve = cache.curve;
    spec.profile.duration = durationMs;
    spec.retargetPolicy = PhosphorAnimation::RetargetPolicy::PreserveVelocity;
    return spec;
}

PhosphorAnimation::MotionSpec<qreal> TerminalWindow::overviewSpec()
{
    const AnimationSettings& anim = m_settings.settings().animation;
    return motionSpec(anim.overviewCurve, anim.overviewDuration, m_overviewCurve, "Animation.overviewCurve");
}

AfterglowRender::WindowRenderer::OverviewTab TerminalWindow::prepareOtherTab(int tab, const std::vector<Pane*>* only,
    std::uint8_t backgroundAlpha, bool& holding, bool& animating)
{
    AfterglowRender::WindowRenderer::OverviewTab other{tab, {}};
    if (tab < 0 || tab >= int(m_tabs.size()) || !m_tabs[std::size_t(tab)]->splits) return other;
    m_tabs[std::size_t(tab)]->splits->forEachPane([&](Pane* p, const QRect& r) {
        if (r.isEmpty() || (only && std::find(only->begin(), only->end(), p) == only->end())) return;
        AfterglowRender::WindowRenderer::Pane wp;
        preparePane(p, r, wp.params, holding, animating);
        wp.snapshot = &p->snapshot();
        wp.params.focused = false;
        wp.params.highlighted = m_overview.open && p == m_overview.highlighted;
        wp.params.cursorBlinkOn = m_blinkOn;
        wp.params.textBlinkOn = m_textBlinkOn;
        wp.params.backgroundAlpha = backgroundAlpha;
        other.panes.push_back(std::move(wp));
    });
    return other;
}

void TerminalWindow::startTabTransition(int from, int to)
{
    const bool overview = m_overview.open || m_overview.amount.isAnimating() || std::abs(m_overview.amount.value()) > 1e-3;
    if (!m_tabTransitionAvailable || from == to || from < 0 || from >= int(m_tabs.size()) || overview || !isExposed()) return;
    const AnimationSettings& anim = m_settings.settings().animation;
    m_tabTransition.from = m_tabs[std::size_t(from)]->id;
    m_tabTransition.direction = to > from ? 1.0f : -1.0f;
    m_tabTransition.progress.start(0.0, 1.0, motionSpec(anim.tabCurve, anim.tabDuration, m_tabCurve, "Animation.tabCurve"));
    m_tabTransition.startedAt = std::chrono::steady_clock::now();
}

bool TerminalWindow::paneEffectsAllowed() const
{
    const bool overview = m_overview.open || m_overview.amount.isAnimating() || std::abs(m_overview.amount.value()) > 1e-3;
    return m_paneEffectsAvailable && m_settings.settings().animation.splits && !overview
        && !m_tabTransition.progress.isAnimating() && isExposed();
}

float TerminalWindow::paneEffectProgress(std::chrono::steady_clock::time_point at) const
{
    const float seconds = std::chrono::duration<float>(std::chrono::steady_clock::now() - at).count();
    return std::clamp(seconds * 1000.0f / float(std::max(1, m_settings.settings().animation.paneEffectDuration)), 0.0f, 1.0f);
}

void TerminalWindow::advancePaneEffects(bool& animating)
{
    AfterglowRender::WindowRenderer::PaneEffects effects;
    for (auto it = m_paneOpening.begin(); it != m_paneOpening.end();) {
        if (paneEffectProgress(it->at) >= 1.0f) it = m_paneOpening.erase(it);
        else { effects.active = true; ++it; }
    }
    if (m_closingPane && paneEffectProgress(m_closingPane->at) >= 1.0f) m_closingPane.reset();
    if (m_closingPane) {
        AfterglowRender::WindowRenderer::Pane ghost;
        ghost.snapshot = m_closingPane->snapshot.get();
        ghost.params.rect = m_closingPane->rect;
        ghost.params.contentOrigin = contentRect(m_closingPane->rect).topLeft();
        ghost.params.focused = false;
        ghost.params.backgroundAlpha = std::uint8_t(std::lround(m_settings.settings().window.backgroundOpacity * 255));
        effects.closing = ghost;
        effects.closingProgress = paneEffectProgress(m_closingPane->at);
        effects.active = true;
    }
    animating |= effects.active;
    m_renderer->setPaneEffects(std::move(effects));
}

void TerminalWindow::advanceTabTransition(std::uint8_t backgroundAlpha, bool& holding, bool& animating)
{
    AfterglowRender::WindowRenderer::TabTransition transition;
    if (m_tabTransition.progress.isAnimating()) {
        m_tabTransition.progress.advance();
        const auto from = std::find_if(m_tabs.begin(), m_tabs.end(), [&](const auto& tab) { return tab->id == m_tabTransition.from; });
        // A tab closed during its own switch has nothing left to show.
        if (from == m_tabs.end() || int(from - m_tabs.begin()) == m_currentTab) {
            m_tabTransition.progress.cancel();
        } else if (m_tabTransition.progress.isAnimating()) {
            layoutOtherTabs();
            transition = {true, float(m_tabTransition.progress.value()), m_tabTransition.direction, m_tabTransition.startedAt,
                          prepareOtherTab(int(from - m_tabs.begin()), nullptr, backgroundAlpha, holding, animating)};
            animating = true;
        }
    }
    m_renderer->setTabTransition(std::move(transition));
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
    m_overview.positionTarget = index;
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
    if (const auto at = std::find(panes.begin(), panes.end(), target); at != panes.end()) {
        // The chosen card, the short way round from where the deck is.
        const int count = int(panes.size());
        const qreal now = m_overview.position.value();
        qreal delta = std::fmod(qreal(at - panes.begin()) - now, qreal(count));
        if (delta > count / 2.0) delta -= count;
        if (delta < -count / 2.0) delta += count;
        m_overview.positionTarget = now + delta;
        m_overview.position.start(now, m_overview.positionTarget, spec);
    }
    m_overview.changedAt = std::chrono::steady_clock::now();
    m_overview.highlighted = nullptr;
    m_overview.original = nullptr;
    m_overview.zoomed = nullptr;
    updateKeyMode();
    scheduleRender();
}

void TerminalWindow::moveOverview(int index, int direction)
{
    const auto panes = overviewPanes();
    if (!m_overview.open || panes.empty()) return;
    const int count = int(panes.size());
    index = ((index % count) + count) % count;
    if (panes[std::size_t(index)] == m_overview.highlighted) return;
    const auto at = std::find(panes.begin(), panes.end(), m_overview.highlighted.data());
    const int current = at == panes.end() ? 0 : int(at - panes.begin());
    // The position keeps counting, so wrapping round carries the deck on.
    int delta = ((index - current) % count + count) % count;
    if (direction < 0 || (direction == 0 && delta * 2 > count)) delta -= count;
    m_overview.positionTarget += delta;
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
        moveOverview(current - 1, -1);
    } else if (e->key() == Qt::Key_Right || e->key() == Qt::Key_Tab || e->key() == Qt::Key_L) {
        moveOverview(current + 1, 1);
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
        moveOverview(index, down ? 1 : -1);
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
    if (m_overview.open && !panes.empty()) {
        // Panes opened or closed meanwhile shift the indices: keep the target
        // on the highlighted card, the short way round.
        const int count = int(panes.size());
        const auto at = std::find(panes.begin(), panes.end(), m_overview.highlighted.data());
        const int index = int(at - panes.begin());
        const int heading = int(std::lround(std::fmod(std::fmod(m_overview.positionTarget, count) + count, count))) % count;
        if (heading != index) {
            int delta = ((index - heading) % count + count) % count;
            if (delta * 2 > count) delta -= count;
            m_overview.positionTarget += delta;
        }
        const qreal target = m_overview.positionTarget;
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
