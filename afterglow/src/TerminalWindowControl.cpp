// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
// What afterglow ctl and scripts ask of a window: the neighbors and edges
// of its panes, scrolls, its state and size, the font and the opacity, and
// the frames that it captures.
#include "TerminalWindow.h"
#include "mux/MuxClient.h"

#include <AfterglowRender/FontSet.h>
#include <AfterglowRender/GpuDevice.h>

#include <algorithm>
#include <cmath>

namespace Afterglow {

SplitTree* TerminalWindow::laidOutTree(const Pane* pane)
{
    const int tab = pane ? tabOf(pane) : -1;
    if (tab < 0 || !m_tabs[std::size_t(tab)]->splits) return nullptr;
    SplitTree& tree = *m_tabs[std::size_t(tab)]->splits;
    // Until a layout, a new pane has an empty rectangle, which lies left of
    // and above every other. The next frame lays the shown tab out anyway;
    // another tab's tree gets the area that it would show in.
    if (tab == m_currentTab) ensureLayout();
    else tree.layout(paneArea(), dividerPixels());
    return &tree;
}

Pane* TerminalWindow::neighborOf(const Pane* pane, SplitTree::Neighbor direction)
{
    const SplitTree* tree = laidOutTree(pane);
    return tree ? tree->neighbor(pane, direction) : nullptr;
}

std::array<Pane*, 4> TerminalWindow::neighborsOf(const Pane* pane)
{
    std::array<Pane*, 4> out{};
    // One layout for the four.
    if (const SplitTree* tree = laidOutTree(pane)) {
        for (const SplitTree::Neighbor side :
             {SplitTree::Neighbor::Left, SplitTree::Neighbor::Right, SplitTree::Neighbor::Up, SplitTree::Neighbor::Down})
            out[std::size_t(side)] = tree->neighbor(pane, side);
    }
    return out;
}

bool TerminalWindow::resizePane(Pane* pane, SplitTree::Neighbor direction, int cells)
{
    SplitTree* tree = cells > 0 ? laidOutTree(pane) : nullptr;
    if (!tree) return false;
    // The edges where the user sees them, or would in the pane's tab. A zoom
    // hides them: the resize goes to the tree without it, and the zoom ends,
    // and the tab shows, only when an edge moved. A pane with no edge on
    // that side changes nothing. A change of the zoom clears the dividers
    // until the next layout: none without a zoom, or a refusal leaves the
    // tab with no divider to drag.
    Pane* const zoomed = tree->zoomedPane();
    if (zoomed) tree->setZoomedPane(nullptr);
    const auto& metrics = m_renderer->fonts().fonts->metrics();
    const bool vertical = direction == SplitTree::Neighbor::Up || direction == SplitTree::Neighbor::Down;
    const bool changed = tree->resizePane(pane, direction, cells * (vertical ? metrics.cellHeight : metrics.cellWidth),
        dividerPixels(), minimumPaneSize());
    if (zoomed) tree->setZoomedPane(zoomed);
    if (!changed) return false;
    if (const int tab = tabOf(pane); tab != m_currentTab) switchTab(tab);
    if (zoomed) setPaneZoom(nullptr);
    m_layoutSize = QSize();
    scheduleSessionLayout();
    scheduleRender();
    return true;
}

bool TerminalWindow::scrollsHistory(Pane* pane)
{
    // The alternate screen of a full-screen program has no history: the
    // keys go to the program.
    return pane && !pane->readTerminal([](const AfterglowVt::Terminal& t) { return t.modes().altScreen; });
}

qint64 TerminalWindow::jumpPrompts(Pane* pane, qint64 count)
{
    // A jump from a key, the menu, a script or afterglow ctl ends a coast.
    stopMomentumOf(pane);
    // One jump for each prompt, to the newest that there is.
    qint64 moved = 0;
    while (moved < std::abs(count) && pane->jumpToPrompt(count < 0 ? -1 : 1)) ++moved;
    if (moved == 0) return 0;
    // Hint labels sit on the rows the view had, as in scrollPane.
    if (m_urlMode.active && m_urlMode.pane == pane) stopUrlMode();
    scheduleRender();
    return moved;
}

QString TerminalWindow::scrollFromControl(Pane* pane, const QString& unit, qint64 amount)
{
    Pane* const target = inputPane(pane);
    if (!target) return QStringLiteral("no pane");
    // No ID in the words: a client of a session knows the pane by the
    // daemon's.
    if (!scrollsHistory(target)) return QStringLiteral("the pane shows a full-screen program, which has no scrollback");
    if (unit == QLatin1String("prompt")) {
        if (amount != 0 && jumpPrompts(target, amount) == 0)
            return QStringLiteral("the pane has no prompt %1").arg(amount < 0 ? QStringLiteral("above") : QStringLiteral("below"));
        return {};
    }
    // Rows up are positive in scrollPane.
    double rows = 0;
    if (unit == QLatin1String("lines")) rows = -double(amount);
    else if (unit == QLatin1String("pages")) rows = -double(amount) * pageRows(target);
    else if (unit == QLatin1String("top")) rows = ScrollToEnd;
    else if (unit == QLatin1String("bottom")) rows = -ScrollToEnd;
    else return QStringLiteral("the unit is lines, pages, prompt, top or bottom");
    // At once, not eased: a read that follows sees the new view, also in a
    // tab that does not show, which no frame scrolls.
    settleScroll(target);
    if (scrollPane(target, rows, false)) applyScroll(target, motion(target));
    return {};
}

void TerminalWindow::setNextPaneLaunch(bool hold, std::optional<std::string> input)
{
    m_holdNextPane = hold;
    m_inputNextPane = std::move(input);
}

double TerminalWindow::backgroundOpacity() const
{
    return m_opacityOverride.value_or(m_settings.settings().window.backgroundOpacity);
}

void TerminalWindow::setFontSize(double points, bool relative)
{
    if (relative) {
        m_device.adjustFontSize(float(points));
        return;
    }
    AfterglowRender::FontSpec spec = m_device.fontSpec();
    spec.sizePoints = std::clamp(float(points), AfterglowRender::FontSpec::MinPoints, AfterglowRender::FontSpec::MaxPoints);
    m_device.setFontSpec(spec);
}

void TerminalWindow::setOpacityOverride(double opacity)
{
    m_opacityOverride = std::clamp(opacity, 0.0, 1.0);
    applyWindowEffects();
}

QString TerminalWindow::setWindowState(const QString& state, int cols, int rows)
{
    if (m_quick) return QStringLiteral("the quick terminal has its own size and place");
    constexpr Qt::WindowStates filling = Qt::WindowFullScreen | Qt::WindowMaximized;
    const bool fullScreen = state == QLatin1String("fullscreen"), maximized = state == QLatin1String("maximized"),
               normal = state == QLatin1String("normal");
    if (!state.isEmpty() && !fullScreen && !maximized && !normal) return QStringLiteral("the state is fullscreen, maximized or normal");
    // Before anything changes: a refused size leaves the state as it was.
    const bool sized = cols > 0 && rows > 0;
    if (sized && (fullScreen || maximized || (state.isEmpty() && (windowState() & filling))))
        return QStringLiteral("a full-screen or maximized window takes no size: make it normal with the size");
    m_sizeWhenNormal.reset();
    const QSize size = sized ? windowSizeFor(cols, rows, -1, m_focused) : QSize();
    if (normal && (windowState() & filling)) {
        // The window is normal only when the compositor says so: until then
        // a size would go to the maximized window, and then Qt gives the
        // window back the size that it had before (QWaylandXdgSurface).
        // The size waits for the compositor's state (event()).
        if (sized) m_sizeWhenNormal = size;
        showNormal();
        return {};
    }
    if (fullScreen) showFullScreen();
    else if (maximized) showMaximized();
    else if (normal) showNormal();
    if (sized) resizeToPixels(size);
    return {};
}

QSize TerminalWindow::windowSizeFor(int cols, int rows, int tabCount, const Pane* pane) const
{
    // The grid of one pane with its padding, inside the pane's docks and
    // the window's, below the tab bar. A grid below the panes' minimum is
    // that minimum: the window docks' look below sees the room that the
    // pane would take, not a grid that no layout gives a pane.
    const auto& m = m_renderer->fonts().fonts->metrics();
    const int pad = int(std::lround(m_settings.settings().window.padding * devicePixelRatio()));
    const QSize grid = QSize(cols * m.cellWidth + 2 * pad, rows * m.cellHeight + 2 * pad).expandedTo(minimumPaneSize());
    const QSize size = grid + dockRoom(pane);
    // A tab_bar dock replaces the bar once the window has room for it, as
    // it has at this size: then the bar takes no room. Whether it fits at
    // the size that the window has now does not count. With the grid at
    // least the minimum, every window dock fits at this size.
    bool dockBar = false;
    layoutDocks(QRect(QPoint(0, 0), size), nullptr, &dockBar);
    return dockBar ? size : size + QSize(0, policyTabBarHeight(tabCount));
}

void TerminalWindow::resizeToPixels(const QSize& pixels)
{
    const double dpr = devicePixelRatio();
    resize(int(std::lround(pixels.width() / dpr)), int(std::lround(pixels.height() / dpr)));
}

void TerminalWindow::keepFocus(Pane* pane)
{
    const int tab = pane ? tabOf(pane) : -1;
    if (tab < 0) return;
    if (tab != m_currentTab) switchTab(tab);
    setFocusedPane(pane);
}

void TerminalWindow::setTabTitle(Pane* pane, const QString& title)
{
    const int tab = pane ? tabOf(pane) : -1;
    if (tab < 0 || m_tabs[std::size_t(tab)]->title == title) return;
    m_tabs[std::size_t(tab)]->title = title;
    // As the Rename Tab prompt: the chrome and the session's layout follow.
    Q_EMIT tabsChanged();
}

void TerminalWindow::captureFrame(Pane* pane, CaptureScope scope, CaptureReply reply)
{
    const QString windowHidden = QStringLiteral("the window is not on screen");
    if (!isExposed() || windowState() == Qt::WindowMinimized || (m_quick && !m_quickShown)) {
        reply(windowHidden, QStringLiteral("not_shown"), {});
        return;
    }
    Pane* host = hostOf(pane);
    if (scope != CaptureScope::Window && (!m_splits || tabOf(host) != m_currentTab)) {
        reply(QStringLiteral("the pane is not on screen"), QStringLiteral("not_shown"), {});
        return;
    }
    // The session panes that the capture draws, with the screens that the
    // daemon held while they did not show: the capture waits for them, as a
    // read of the text does (Pane::refreshSession). One wait for all.
    if (m_session) {
        const std::vector<Pane*> cards = overviewCards();
        std::map<std::uint32_t, std::uint64_t> tickets;
        for (Pane* p : panes()) {
            if (!paneSeen(p, cards)) continue;
            if (const std::uint64_t ticket = p->askSessionRefresh()) tickets[p->sessionPaneId()] = ticket;
        }
        if (!tickets.empty()) m_session->waitRefreshed(tickets);
    }
    auto capture = std::make_shared<PendingCapture>();
    capture->scope = scope;
    capture->target = host;
    capture->reply = std::move(reply);
    capture->deadline = QDeadlineTimer(CaptureTimeoutMs);
    m_pendingCaptures.push_back(capture);
    armCaptureTimer();
    scheduleRender();
}

bool TerminalWindow::captureWaits(const PendingCapture& capture) const
{
    // The frame answers these: not_shown for the overview.
    if (capture.scope == CaptureScope::Window || !capture.target || overviewShowing()) return false;
    // A tab switch blends two tabs.
    if (tabSwitching()) return true;
    // A pane that is not drawn keeps the motion it had: it would wait for good.
    if (capture.scope != CaptureScope::Pane || !paneLaidOut(capture.target)) return false;
    // A pane on the way to its place (a split, a resize, a swap) is cut
    // where it settles.
    const auto motion = m_motion.find(capture.target.data());
    return motion != m_motion.end() && motion->second->rect.isAnimating();
}

void TerminalWindow::prepareCaptures()
{
    // After the frame's animations advanced: what waits is decided on the
    // state that this frame draws.
    bool waiting = false;
    for (const auto& capture : m_pendingCaptures) {
        // Asked already: the redraw after a Retry frame serves it.
        if (capture->ticket) continue;
        if (captureWaits(*capture)) {
            // The frames go on while it waits: its bound is on a frame, not
            // on the wait. No readback until then.
            capture->deadline.setRemainingTime(CaptureTimeoutMs);
            waiting = true;
            continue;
        }
        // The renderer runs it inside this window's render(), and the window
        // answers its captures before it frees the renderer.
        capture->ticket = m_renderer->captureNextFrame([this, weak = std::weak_ptr<PendingCapture>(capture)](QImage image) {
            if (auto drawn = weak.lock()) captureDrawn(drawn, image);
        });
    }
    if (waiting) armCaptureTimer();
}

void TerminalWindow::captureDrawn(const std::shared_ptr<PendingCapture>& capture, const QImage& image)
{
    capture->ticket = 0;
    if (image.isNull()) {
        finishCapture(capture, QStringLiteral("the frame could not be read"), QStringLiteral("failed"));
        return;
    }
    const QString paneHidden = QStringLiteral("the pane is not on screen");
    Pane* target = capture->target;
    QRect crop = image.rect();
    if (capture->scope != CaptureScope::Window) {
        // The pane can close, or its tab go to the background, before the frame.
        if (!target || !m_splits || tabOf(target) != m_currentTab) {
            finishCapture(capture, paneHidden, QStringLiteral("not_shown"));
            return;
        }
        // The overview draws cards, not the layout.
        if (overviewShowing()) {
            finishCapture(capture, QStringLiteral("the overview shows"), QStringLiteral("not_shown"));
            return;
        }
        crop = paneArea();
        if (capture->scope == CaptureScope::Pane) {
            if (!paneLaidOut(target)) {
                finishCapture(capture, paneHidden, QStringLiteral("not_shown"));
                return;
            }
            crop = m_splits->rectOf(target);
        }
    }
    crop = crop.intersected(image.rect());
    if (crop.isEmpty()) {
        finishCapture(capture, paneHidden, QStringLiteral("not_shown"));
        return;
    }
    finishCapture(capture, {}, {}, crop == image.rect() ? image : image.copy(crop));
}

void TerminalWindow::finishCapture(std::shared_ptr<PendingCapture> capture, const QString& error, const QString& code,
                                   const QImage& image)
{
    std::erase(m_pendingCaptures, capture);
    if (capture->ticket) m_renderer->cancelCapture(std::exchange(capture->ticket, 0));
    armCaptureTimer();
    if (const CaptureReply reply = std::exchange(capture->reply, {})) reply(error, code, image);
}

void TerminalWindow::abortCaptures(const QString& error)
{
    m_captureTimer.stop();
    for (const auto& capture : std::exchange(m_pendingCaptures, {})) {
        if (capture->ticket) m_renderer->cancelCapture(std::exchange(capture->ticket, 0));
        if (const CaptureReply reply = std::exchange(capture->reply, {})) reply(error, QStringLiteral("not_shown"), {});
    }
}

void TerminalWindow::expireCaptures()
{
    const auto pending = m_pendingCaptures;
    for (const auto& capture : pending) {
        if (capture->deadline.hasExpired()) {
            finishCapture(capture, QStringLiteral("no frame was drawn in time"), QStringLiteral("failed"));
        }
    }
    armCaptureTimer();
}

void TerminalWindow::armCaptureTimer()
{
    std::optional<qint64> first;
    for (const auto& capture : m_pendingCaptures) {
        const qint64 left = capture->deadline.remainingTime();
        if (!first || left < *first) first = left;
    }
    if (first) {
        m_captureTimer.start(int(*first));
    } else {
        m_captureTimer.stop();
    }
}

} // namespace Afterglow
