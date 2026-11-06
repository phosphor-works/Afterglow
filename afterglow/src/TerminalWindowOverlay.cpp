// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include "TerminalWindow.h"
#include "chrome/ChromeController.h"

#include <AfterglowRender/FontSet.h>
#include <AfterglowRender/GpuDevice.h>
#include <AfterglowVt/Input.h>

#include <QAccessible>
#include <QGuiApplication>
#include <QInputMethod>

#include <algorithm>

namespace Afterglow {

using AfterglowVt::Terminal;

Pane* TerminalWindow::inputPane(Pane* pane)
{
    return pane && pane->overlay() ? pane->overlay()->pane() : pane;
}

Pane* TerminalWindow::pointerTarget(Pane* pane, const QPointF& pos) const
{
    Pane* target = inputPane(pane);
    if (!target || target == pane || !m_splits) return target;
    // The program's own cells: a popup's grid, which its border is not.
    const QRect grid = contentRect(target, m_splits->rectOf(hostOf(target)));
    return pane->overlay()->popup() && !grid.contains((pos * devicePixelRatio()).toPoint()) ? nullptr : target;
}

QString TerminalWindow::openOverlay(const QString& owner, const QString& request, Pane* pane, const OverlaySpec& spec,
                                    PaneOverlay::Reply reply, QString& code)
{
    code = QStringLiteral("no_pane");
    if (!pane || tabOf(pane) < 0) return QStringLiteral("Pane closed");
    code = QStringLiteral("busy");
    if (pane->overlay()) return QStringLiteral("The pane has an overlay already");
    if (m_scriptDialog) return QStringLiteral("A script dialog is open");
    // Another script's key mode would take the overlay's keys from the user.
    if (m_scriptKeyMode && m_scriptKeyMode->owner != owner) return QStringLiteral("Another script holds the keyboard");

    const Settings& settings = m_settings.settings();
    const auto& metrics = m_renderer->fonts().fonts->metrics();
    PaneOverlay::Launch launch;
    launch.terminal = settings.terminal;
    launch.spawn.environment = settings.environment;
    launch.term = settings.term;
    // The host's grid: the overlay shows where it does.
    launch.cols = pane->cols();
    launch.rows = pane->rows();
    launch.cellWidth = metrics.cellWidth;
    launch.cellHeight = metrics.cellHeight;
    QString error;
    PaneOverlay* overlay = PaneOverlay::start(*pane, spec, launch, &error);
    if (!overlay) {
        code = QStringLiteral("spawn_failed");
        return error;
    }
    code.clear();
    overlay->setReply(owner, request, std::move(reply));
    Pane* program = overlay->pane();
    applySearchColors(program);
    program->setScrollPolicy(settings.scrollOnOutput, settings.scrollOnInput);
    bindOverlay(pane);

    // The overlay takes the keyboard: no mode of the pane is left behind it.
    const bool focused = pane == m_focused;
    if (focused) {
        stopCopyMode();
        cancelKeyMode();
        stopUrlMode();
        stopUnicodeInput();
        m_controller->setSearchOpen(false);
        pane->clearSearch();
        pane->setPreedit({}, 0);
        if (isActive()) QGuiApplication::inputMethod()->reset();
    }
    if (m_selectionPane == pane) {
        m_selectionPane = nullptr;
        m_autoScroll.stop();
    }
    if (m_reportedPane == pane) {
        // The release of a held button goes to the program that got the press.
        m_reportedButtons = Qt::NoButton;
        m_reportedPane = nullptr;
    }
    if (m_hover.pane == pane) m_hover = {};
    // Onto the overlay's pane, as for a dialog: the overview's original card
    // could be in another tab.
    closeOverview(pane);
    if (tabOf(pane) != m_currentTab) switchTab(tabOf(pane));
    if (focused) {
        // The focus stays on the pane: its program hears that it lost it.
        if (m_windowFocused) pane->sendReply(pane->withTerminal([](Terminal& t) { return AfterglowVt::encodeFocus(false, t.modes()); }));
        m_inputMethodDirty = true;
        updateTitle();
        updateChrome();
        if (m_windowFocused && QAccessible::isActive()) {
            QAccessibleEvent event(pane, QAccessible::Focus);
            QAccessible::updateAccessibility(&event);
        }
    } else {
        setFocusedPane(pane);
    }
    startOverlayFade(pane, 1.0);
    // The overlay shows here; the window does not jump forward for it.
    if (!isActive()) alert(0);
    refreshInputMethod();
    scheduleRender();
    return {};
}

void TerminalWindow::startOverlayFade(Pane* host, qreal target)
{
    // The overlay transition draws the fade. Without it the overlay switches
    // at once, and no frame waits for a fade.
    if (!m_overlayTransitionAvailable) {
        m_overlayFades.erase(host);
        return;
    }
    const AnimationSettings& anim = m_settings.settings().animation;
    OverlayFade& fade = m_overlayFades[host];
    fade.last.reset();
    const qreal from = fade.progress.isAnimating() ? std::clamp(fade.progress.value(), 0.0, 1.0) : 1.0 - target;
    fade.progress.start(from, target, motionSpec(anim.overlayCurve, anim.overlayDuration, m_overlayCurve, "Animation.overlayCurve",
                                                 AnimationSettings{}.overlayCurve));
    updateAudioCapture();
}

void TerminalWindow::bindOverlay(Pane* host)
{
    PaneOverlay* overlay = host->overlay();
    if (!overlay) return;
    // Its terminal is a pane's: output, the bell, the clipboard, a title.
    bindPane(overlay->pane());
    connect(overlay, &PaneOverlay::ended, this, [this, host = QPointer<Pane>(host), guard = QPointer<PaneOverlay>(overlay)] {
        if (host && guard) overlayEnded(host, guard);
    });
}

void TerminalWindow::overlayEnded(Pane* host, PaneOverlay* overlay)
{
    Pane* program = overlay->pane();
    QObject::disconnect(overlay, nullptr, this, nullptr);
    // What belongs to the overlay's terminal goes with it.
    dropPanePrompts(program);
    if (m_copyPane == program) stopCopyMode();
    if (m_hover.pane == program) m_hover = {};
    if (m_lastClickPane == program) m_lastClickPane = nullptr;
    const bool focused = host == m_focused;
    if (focused && m_controller->searchOpen()) m_controller->setSearchOpen(false);
    program->clearSearch();
    program->setPreedit({}, 0);
    if (focused && isActive()) QGuiApplication::inputMethod()->reset();
    // It fades out from where its fade is, on its last frame.
    if (tabOf(host) >= 0 && m_overlayTransitionAvailable) {
        startOverlayFade(host, 0.0);
        OverlayFade& fade = m_overlayFades[host];
        fade.last = std::make_shared<AfterglowRender::PaneSnapshot>(program->snapshot());
        fade.lastPopup = overlay->popup() ? std::optional(overlay->placement()) : std::nullopt;
        fade.lastTitle = overlay->title();
    }
    // The host has the input again, its view and cursor where they were.
    if (focused) {
        if (m_windowFocused) host->sendReply(host->withTerminal([](Terminal& t) { return AfterglowVt::encodeFocus(true, t.modes()); }));
        m_inputMethodDirty = true;
        if (m_windowFocused && QAccessible::isActive()) {
            QAccessibleEvent event(host, QAccessible::Focus);
            QAccessible::updateAccessibility(&event);
        }
    }
    // Its program gets SIGHUP when it goes, if it still runs.
    overlay->deleteLater();
    updateTitle();
    updateChrome();
    refreshInputMethod();
    applyPointerShape();
    scheduleRender();
}

void TerminalWindow::closeOverlays(const QString& owner, const QString& prefix, const QString& message)
{
    // Collected first: a cancel takes the overlay off its host at once.
    QList<QPointer<PaneOverlay>> ending;
    for (Pane* pane : panes()) {
        PaneOverlay* overlay = pane->overlay();
        if (overlay && overlay->owner() == owner && overlay->request().startsWith(prefix)) ending.append(overlay);
    }
    for (const auto& overlay : std::as_const(ending)) {
        if (overlay) overlay->cancel(message);
    }
}

std::pair<qreal, bool> TerminalWindow::overlayFade(const Pane* pane) const
{
    const auto fade = m_overlayFades.find(pane);
    if (fade == m_overlayFades.end()) return {pane && pane->overlay() ? 1.0 : 0.0, false};
    return {std::clamp(fade->second.progress.value(), 0.0, 1.0), fade->second.progress.isAnimating()};
}

void TerminalWindow::prepareOverlay(Pane* pane, const QRect& layoutRect, AfterglowRender::WindowRenderer::Pane& wp, bool& holding,
                                    bool& animating)
{
    PaneOverlay* overlay = pane->overlay();
    const auto fade = m_overlayFades.find(pane);
    if (!overlay && fade == m_overlayFades.end()) return;
    float progress = 1.0f;
    std::shared_ptr<AfterglowRender::PaneSnapshot> last;
    std::optional<OverlaySpec::Placement> lastPopup;
    QString lastTitle;
    if (fade != m_overlayFades.end()) {
        fade->second.progress.advance();
        progress = float(std::clamp(fade->second.progress.value(), 0.0, 1.0));
        last = fade->second.last;
        lastPopup = fade->second.lastPopup;
        lastTitle = fade->second.lastTitle;
        if (fade->second.progress.isAnimating()) {
            animating = true;
        } else {
            // Settled: an overlay that runs shows whole, one that ended is gone.
            m_overlayFades.erase(fade);
            if (!overlay) return;
            last.reset();
        }
    }
    AfterglowRender::WindowRenderer::Pane::Overlay shown;
    shown.params = wp.params;
    shown.progress = progress;
    // A popup's grid is its own part of the host's, wherever the host is drawn.
    if (overlay && overlay->popup()) preparePopup(overlay->placement(), overlay->title(), wp.params.contentOrigin, shown);
    else if (!overlay && lastPopup) preparePopup(*lastPopup, lastTitle, wp.params.contentOrigin, shown);
    if (overlay) {
        Pane* program = overlay->pane();
        AfterglowRender::FrameBuilder::PaneParams own;
        preparePane(program, layoutRect, own, holding, animating);
        shown.snapshot = &program->snapshot();
        // Its own cursor and scroll position, in the host's place.
        shown.params.cursorPosition = own.cursorPosition;
        shown.params.scrollShift = own.scrollShift;
        shown.params.hoveredLink = 0;
        shown.params.hoverRow = -1;
        if (m_hover.pane == program) {
            shown.params.hoveredLink = m_hover.link;
            shown.params.hoverRow = m_hover.row;
            shown.params.hoverColStart = m_hover.colStart;
            shown.params.hoverColEnd = m_hover.colEnd;
        }
    } else {
        // An overlay that ended: its last frame, still, while it fades out.
        shown.snapshot = last.get();
        shown.params.cursorPosition.reset();
        shown.params.scrollShift = 0;
        shown.params.hoveredLink = 0;
        shown.params.hoverRow = -1;
    }
    wp.overlay = shown;
}

void TerminalWindow::preparePopup(const OverlaySpec::Placement& placement, const QString& title, QPoint origin,
                                  AfterglowRender::WindowRenderer::Pane::Overlay& shown) const
{
    const auto& m = m_renderer->fonts().fonts->metrics();
    const QRect grid = placement.grid;
    shown.params.contentOrigin = origin + QPoint(grid.x() * m.cellWidth, grid.y() * m.cellHeight);
    shown.params.rect = QRect(shown.params.contentOrigin, QSize(grid.width() * m.cellWidth, grid.height() * m.cellHeight));
    shown.popup = popupLook(m_settings.settings(), placement.border, title);
}

AfterglowRender::WindowRenderer::Pane::Overlay::Popup popupLook(const Settings& settings, bool border, const QString& title)
{
    AfterglowRender::WindowRenderer::Pane::Overlay::Popup popup;
    popup.border = border;
    popup.title = title.toStdString();
    // The theme's: its popup border for the line, its text for the title.
    popup.borderColor = settings.window.popupBorderColor;
    popup.titleColor = AfterglowRender::packRgba(settings.terminal.defaultForeground);
    popup.dim = float(settings.window.popupDim);
    return popup;
}

} // namespace Afterglow
