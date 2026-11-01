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
#include <utility>

Q_DECLARE_LOGGING_CATEGORY(lcWindow)

namespace Afterglow {

namespace {

/// Where the deck heads for card @p index of @p count from @p now, the short
/// way round: the position counts on past the last card.
qreal shortWay(qreal now, int index, int count)
{
    qreal delta = std::fmod(qreal(index) - now, qreal(count));
    if (delta > count / 2.0) delta -= count;
    if (delta < -count / 2.0) delta += count;
    return now + delta;
}

/// The deck position @p now, counted in @p before, re-counted in @p after:
/// the card it shows keeps its place when zooms add or take away cards.
qreal rebased(qreal now, const std::vector<Pane*>& before, const std::vector<Pane*>& after)
{
    if (before == after || before.empty() || after.empty()) return now;
    const int count = int(before.size());
    const qreal whole = std::round(now);
    const int shown = (int(std::fmod(whole, qreal(count))) + count) % count;
    // The shown card, or the nearest one before it that is still a card.
    for (int back = 0; back < count; ++back) {
        const auto it = std::find(after.begin(), after.end(), before[std::size_t((shown - back + count) % count)]);
        if (it != after.end()) return qreal(it - after.begin()) + back + (now - whole);
    }
    return now - whole;
}

} // namespace

bool TerminalWindow::overviewShowing() const
{
    return m_overview.open || m_overview.amount.isAnimating() || std::abs(m_overview.amount.value()) > 1e-3;
}

std::vector<Pane*> TerminalWindow::overviewPanes() const
{
    // While the overview shows, every tab's panes; otherwise the current tab's.
    std::vector<Pane*> panes;
    const bool allTabs = overviewShowing();
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
                                                                 const char* setting, const QString& fallback)
{
    if (!cache.curve || curve != cache.spec) {
        cache.spec = curve;
        cache.curve = m_shaders.curves().tryCreate(curve);
        if (!cache.curve) {
            qCWarning(lcWindow).noquote() << setting << curve << "is not a curve; using the default" << fallback;
            if (!fallback.isEmpty()) cache.curve = m_shaders.curves().tryCreate(fallback);
            // Only for a caller that names no default.
            if (!cache.curve) cache.curve = std::make_shared<PhosphorAnimation::Spring>(12.0, 1.0);
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
    return motionSpec(anim.overviewCurve, anim.overviewDuration, m_overviewCurve, "Animation.overviewCurve",
                      AnimationSettings{}.overviewCurve);
}

AfterglowRender::WindowRenderer::OverviewTab TerminalWindow::prepareOtherTab(int tab, const std::vector<Pane*>* only,
    std::uint8_t backgroundAlpha, bool& holding, bool& animating)
{
    AfterglowRender::WindowRenderer::OverviewTab other{tab, {}, {}};
    if (tab < 0 || tab >= int(m_tabs.size()) || !m_tabs[std::size_t(tab)]->splits) return other;
    const Tab& owner = *m_tabs[std::size_t(tab)];
    const bool multiple = owner.splits->paneCount() > 1;
    const QRect area = paneArea();
    const int dividerWidth = std::max(1, int(std::lround(devicePixelRatio())));
    const double overviewAmount = std::clamp(m_overview.amount.value(), 0.0, 1.0);
    owner.splits->forEachPane([&](Pane* p, const QRect& r) {
        if (r.isEmpty() || (only && std::find(only->begin(), only->end(), p) == only->end())) return;
        AfterglowRender::WindowRenderer::Pane wp;
        preparePane(p, r, wp.params, holding, animating);
        wp.snapshot = &p->snapshot();
        wp.params.focused = false;
        wp.params.highlighted = m_overview.open && p == m_overview.highlighted;
        wp.params.cursorBlinkOn = m_blinkOn;
        wp.params.textBlinkOn = m_textBlinkOn;
        const QVariantMap overrides = applyScriptOverrides(p, wp.params, backgroundAlpha);
        const QRect shown = wp.params.rect;
        other.panes.push_back(std::move(wp));
        // The overlays the tab had while it was current: the unfocused dim and
        // the dividers, so a tab switch starts from exactly what was shown.
        if (multiple && p != owner.focused) {
            const double paneDim = overrides.contains(QStringLiteral("unfocusedDim"))
                ? overrides.value(QStringLiteral("unfocusedDim")).toDouble() : m_settings.settings().window.unfocusedDim;
            if (const auto alpha = std::uint8_t(std::lround(paneDim * (1.0 - overviewAmount) * 255)); alpha > 0)
                other.overlays.emplace_back(shown, AfterglowRender::packRgba(0, 0, 0, alpha));
        }
        if (shown.right() < area.right()) {
            other.overlays.emplace_back(QRect(shown.right() + 1, shown.top(), dividerWidth, shown.height()),
                                        m_settings.settings().window.dividerColor);
        }
        if (shown.bottom() < area.bottom()) {
            other.overlays.emplace_back(QRect(shown.left(), shown.bottom() + 1, shown.width(), dividerWidth),
                                        m_settings.settings().window.dividerColor);
        }
    });
    return other;
}

QVariantMap TerminalWindow::applyScriptOverrides(Pane* pane, AfterglowRender::FrameBuilder::PaneParams& params,
                                                 std::uint8_t backgroundAlpha) const
{
    QVariantMap overrides = scriptOverrides(pane);
    const auto blink = overrides.constFind(QStringLiteral("cursorBlink"));
    params.cursorBlinkOverride = blink != overrides.constEnd() ? int(blink->toBool()) : -1;
    const auto opacity = overrides.constFind(QStringLiteral("backgroundOpacity"));
    params.backgroundAlpha = opacity != overrides.constEnd()
        ? std::uint8_t(std::lround(std::clamp(opacity->toDouble(), 0.0, 1.0) * 255)) : backgroundAlpha;
    return overrides;
}

void TerminalWindow::startTabTransition(int from, int to)
{
    if (!m_tabTransitionAvailable || from == to || from < 0 || from >= int(m_tabs.size()) || overviewShowing() || !isExposed()) return;
    const AnimationSettings& anim = m_settings.settings().animation;
    m_tabTransition.from = m_tabs[std::size_t(from)]->id;
    m_tabTransition.direction = to > from ? 1.0f : -1.0f;
    m_tabTransition.progress.start(0.0, 1.0, motionSpec(anim.tabCurve, anim.tabDuration, m_tabCurve, "Animation.tabCurve",
                                                        AnimationSettings{}.tabCurve));
    m_tabTransition.startedAt = std::chrono::steady_clock::now();
}

bool TerminalWindow::paneEffectsAllowed() const
{
    return m_paneEffectsAvailable && m_settings.settings().animation.splits && !overviewShowing()
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
    if (!m_paneEffectsAvailable) {
        // The pack is off or did not compile: nothing it would draw is pending.
        m_paneOpening.clear();
        m_panePulse.clear();
        m_closingPane.reset();
    }
    for (auto it = m_paneOpening.begin(); it != m_paneOpening.end();) {
        if (paneEffectProgress(it->at) >= 1.0f) it = m_paneOpening.erase(it);
        else { effects.active = effects.moving = true; ++it; }
    }
    // The ghost belongs to the tab it closed in: a switch away ends it.
    if (m_closingPane
        && (paneEffectProgress(m_closingPane->at) >= 1.0f || !m_splits || m_currentTab >= int(m_tabs.size())
            || m_tabs[std::size_t(m_currentTab)]->id != m_closingPane->tab)) {
        m_closingPane.reset();
    }
    // Context changes animate, and panes away from here keep their mark.
    for (const auto& context : std::as_const(m_paneContext)) {
        if (paneEffectProgress(context.at) < 1.0f) effects.active = effects.moving = true;
    }
    // The frame that marks a pane away is part of the effects that
    // Animation.splits=false turns off.
    if (m_anyPaneAway && m_paneEffectsAvailable && m_settings.settings().animation.splits) effects.active = true;
    const bool overview = overviewShowing();
    for (auto it = m_panePulse.begin(); it != m_panePulse.end();) {
        const float seconds = std::chrono::duration<float>(std::chrono::steady_clock::now() - it->at).count();
        if (seconds * 1000.0f >= float(it->durationMs)) {
            it = m_panePulse.erase(it);
            continue;
        }
        // A pulse moves only while its pane is drawn: in another tab, or hidden
        // by a zoom, it waits out its time without asking for frames.
        const bool drawn = overview || (tabOf(it.key()) == m_currentTab && m_splits && m_splits->isVisible(it.key()));
        if (drawn) effects.active = effects.moving = true;
        ++it;
    }
    if (m_closingPane) {
        AfterglowRender::WindowRenderer::Pane ghost;
        ghost.snapshot = m_closingPane->snapshot.get();
        ghost.params.rect = m_closingPane->rect;
        ghost.params.contentOrigin = contentRect(m_closingPane->rect).topLeft();
        ghost.params.focused = false;
        ghost.params.backgroundAlpha = m_closingPane->backgroundAlpha;
        effects.closing = ghost;
        effects.closingProgress = paneEffectProgress(m_closingPane->at);
        effects.active = effects.moving = true;
    }
    animating |= effects.moving;
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
            // Only the tab being left is drawn: lay out that one at the window's size.
            if ((*from)->splits) (*from)->splits->layout(paneArea(), std::max(1, int(std::lround(devicePixelRatio()))));
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
    // The overview takes the keyboard: not while a dialog or the chrome has it.
    if (m_scriptDialog || m_controller->wantsKeyboard()) return false;
    const bool fresh = !overviewShowing();
    const auto before = overviewPanes();
    // Every tab shows all of its panes; closing zooms them again.
    m_overview.zoomed.clear();
    for (std::size_t tab = 0; tab < m_tabs.size(); ++tab) {
        SplitTree* tree = m_tabs[tab]->splits.get();
        Pane* zoomed = tree ? tree->zoomedPane() : nullptr;
        if (!zoomed) continue;
        m_overview.zoomed.emplace_back(zoomed);
        if (int(tab) == m_currentTab) setPaneZoom(nullptr);
        else tree->setZoomedPane(nullptr);
    }
    m_overview.open = true;
    layoutOtherTabs();
    const auto panes = overviewPanes();
    const auto focused = std::find(panes.begin(), panes.end(), m_focused.data());
    if (focused == panes.end()) {
        // Not among the cards: leave everything as it was.
        m_overview.open = false;
        restoreOverviewZooms(nullptr);
        return false;
    }
    cancelKeyMode();
    stopCopyMode();
    stopUrlMode();
    if (m_unicodeInput) {
        m_unicodeInput.reset();
        showUnicodeInput();
    }
    if (fresh) {
        // Hidden tabs were laid out again at the window's size: their cards
        // start there, not from where they were when last shown.
        for (std::size_t tab = 0; tab < m_tabs.size(); ++tab) {
            if (int(tab) == m_currentTab || !m_tabs[tab]->splits) continue;
            m_tabs[tab]->splits->forEachPane([this](Pane* p, const QRect&) { motion(p).rectKnown = false; });
        }
    }
    m_overview.original = m_focused;
    m_overview.highlighted = m_focused;
    const auto spec = overviewSpec();
    // Reopening while it closes continues from where it is.
    m_overview.amount.start(m_overview.amount.value(), 1.0, spec);
    const int index = int(focused - panes.begin());
    if (fresh) {
        m_overview.positionTarget = index;
        m_overview.position.start(index, index, spec);
    } else {
        // The short way round: the position may have counted on past many turns.
        const qreal now = rebased(m_overview.position.value(), before, panes);
        m_overview.positionTarget = shortWay(now, index, int(panes.size()));
        m_overview.position.start(now, m_overview.positionTarget, spec);
    }
    m_overview.changedAt = m_overview.movedAt = std::chrono::steady_clock::now();
    updateKeyMode();
    scheduleRender();
    return true;
}

void TerminalWindow::restoreOverviewZooms(Pane* target)
{
    // Each zoom goes back to the tab that holds it now, onto the target when
    // the target is in that tab.
    const int targetTab = target ? tabOf(target) : -1;
    for (const QPointer<Pane>& zoomed : std::exchange(m_overview.zoomed, {})) {
        const int tab = zoomed ? tabOf(zoomed) : -1;
        if (tab < 0) continue;
        Pane* pane = tab == targetTab ? target : zoomed.data();
        if (tab == m_currentTab) setPaneZoom(pane);
        else m_tabs[std::size_t(tab)]->splits->setZoomedPane(pane);
    }
}

void TerminalWindow::closeOverview(Pane* chosen)
{
    if (!m_overview.open) return;
    const auto before = overviewPanes();
    m_overview.open = false;
    Pane* target = chosen && std::find(before.begin(), before.end(), chosen) != before.end() ? chosen : m_overview.original.data();
    restoreOverviewZooms(target);
    if (target && tabOf(target) != m_currentTab) revealPane(target);
    else if (target) setFocusedPane(target);
    const auto spec = overviewSpec();
    m_overview.amount.start(m_overview.amount.value(), 0.0, spec);
    // The zooms can take cards away: count from the card the deck shows.
    const auto after = overviewPanes();
    if (const auto at = std::find(after.begin(), after.end(), target); at != after.end()) {
        // The chosen card, the short way round from where the deck is.
        const qreal now = rebased(m_overview.position.value(), before, after);
        m_overview.positionTarget = shortWay(now, int(at - after.begin()), int(after.size()));
        m_overview.position.start(now, m_overview.positionTarget, spec);
    }
    m_overview.changedAt = std::chrono::steady_clock::now();
    m_overview.highlighted = nullptr;
    m_overview.original = nullptr;
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
    if (overviewShowing()) layoutOtherTabs();
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
