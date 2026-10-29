// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TerminalWindow.h"

#include "KeyMapping.h"
#include "Pane.h"

#include <AfterglowRender/FontSet.h>
#include <AfterglowRender/GpuDevice.h>
#include <AfterglowVt/Input.h>

#include <QClipboard>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLoggingCategory>
#include <QPlatformSurfaceEvent>
#include <QStyleHints>

#include <cmath>

Q_LOGGING_CATEGORY(lcWindow, "afterglow.window", QtInfoMsg)

namespace Afterglow {

using AfterglowRender::packRgba;
using AfterglowVt::Terminal;

namespace {
QList<TerminalWindow*> s_windows;

constexpr int PaddingLogical = 6;
constexpr int InitialCols = 100;
constexpr int InitialRows = 30;
constexpr int BlinkIntervalMs = 530;
constexpr int WheelLines = 3;
constexpr std::uint32_t DividerColor = packRgba(0x3a, 0x3a, 0x42);
constexpr std::uint32_t UnfocusedDim = packRgba(0, 0, 0, 48);

int mouseButtonCode(Qt::MouseButton b)
{
    switch (b) {
    case Qt::LeftButton:
        return 0;
    case Qt::MiddleButton:
        return 1;
    case Qt::RightButton:
        return 2;
    default:
        return -1;
    }
}
} // namespace

TerminalWindow::TerminalWindow(AfterglowRender::GpuDevice& device, const QString& workingDirectory)
    : m_device(device)
{
    setSurfaceType(QSurface::VulkanSurface);
    setVulkanInstance(device.vulkanInstance());
    setTitle(QStringLiteral("afterglow"));
    m_renderer = std::make_unique<AfterglowRender::WindowRenderer>(device, this);

    const auto& m = m_renderer->fonts().fonts->metrics();
    const double dpr = devicePixelRatio();
    const int pad = int(std::lround(PaddingLogical * dpr));
    const QSize initial(InitialCols * m.cellWidth + 2 * pad, InitialRows * m.cellHeight + 2 * pad);
    resize(int(std::lround(initial.width() / dpr)), int(std::lround(initial.height() / dpr)));

    Pane* first = createPane(workingDirectory, QRect(QPoint(0, 0), initial));
    m_splits = std::make_unique<SplitTree>(first);
    setFocusedPane(first);

    connect(&m_device, &AfterglowRender::GpuDevice::fontsChanged, this, &TerminalWindow::scheduleRender);

    m_blinkTimer.setInterval(BlinkIntervalMs);
    connect(&m_blinkTimer, &QTimer::timeout, this, [this] {
        m_blinkOn = !m_blinkOn;
        if (m_focused && m_focused->snapshot().cursorBlink && m_focused->snapshot().cursorVisible) {
            scheduleRender();
        }
    });

    m_autoScroll.setInterval(50);
    connect(&m_autoScroll, &QTimer::timeout, this, &TerminalWindow::updateSelectionDrag);

    s_windows.append(this);
}

TerminalWindow::~TerminalWindow()
{
    s_windows.removeAll(this);
    m_renderer.reset();
}

QList<TerminalWindow*> TerminalWindow::windows()
{
    return s_windows;
}

// --- panes -----------------------------------------------------------------

Pane* TerminalWindow::createPane(const QString& workingDirectory, const QRect& area)
{
    auto* pane = new Pane(this);
    connect(pane, &Pane::contentChanged, this, &TerminalWindow::scheduleRender);
    connect(pane, &Pane::titleUpdated, this, [this, pane] {
        if (pane == m_focused) {
            updateTitle();
        }
    });
    connect(pane, &Pane::closed, this, [this, guard = QPointer<Pane>(pane)] {
        if (guard) {
            closePane(guard);
        }
    });
    connect(pane, &Pane::clipboardWriteRequested, this, [](const QString& text, bool primary) {
        QGuiApplication::clipboard()->setText(text, primary ? QClipboard::Selection : QClipboard::Clipboard);
    });
    connect(pane, &Pane::bellRang, this, [this] {
        if (!isActive()) {
            alert(0);
        }
    });

    AfterglowPty::SpawnOptions opts;
    opts.workingDirectory = workingDirectory.toStdString();
    const auto& m = m_renderer->fonts().fonts->metrics();
    const QRect content = contentRect(area);
    const int cols = std::max(2, content.width() / m.cellWidth);
    const int rows = std::max(1, content.height() / m.cellHeight);
    QString error;
    if (!pane->start(opts, cols, rows, m.cellWidth, m.cellHeight, &error)) {
        qCCritical(lcWindow) << "failed to start the shell:" << error;
    }
    return pane;
}

void TerminalWindow::splitFocused(SplitTree::Direction direction)
{
    if (!m_focused) {
        return;
    }
    // Start the new shell at the size its half of the split will have.
    QRect area = m_splits->rectOf(m_focused);
    const int divider = std::max(1, int(std::lround(devicePixelRatio())));
    if (direction == SplitTree::Direction::Right) {
        area.setWidth((area.width() - divider) / 2);
    } else {
        area.setHeight((area.height() - divider) / 2);
    }
    Pane* added = createPane(m_focused->workingDirectory(), area);
    m_splits->split(m_focused, added, direction);
    setFocusedPane(added);
    m_layoutSize = QSize();
    scheduleRender();
}

void TerminalWindow::closePane(Pane* pane)
{
    if (!m_splits->contains(pane)) {
        return;
    }
    if (!m_splits->remove(pane)) {
        // The last pane: the window goes with it.
        qCInfo(lcWindow) << "last pane closed, closing the window";
        pane->deleteLater();
        close();
        return;
    }
    if (m_focused == pane) {
        Pane* next = nullptr;
        m_splits->forEachPane([&next](Pane* p, const QRect&) {
            if (!next) {
                next = p;
            }
        });
        setFocusedPane(next);
    }
    pane->deleteLater();
    m_layoutSize = QSize();
    scheduleRender();
}

void TerminalWindow::setFocusedPane(Pane* pane)
{
    if (pane == m_focused) {
        return;
    }
    if (m_focused && m_windowFocused) {
        Pane* old = m_focused;
        old->sendInput(old->withTerminal([](Terminal& t) { return AfterglowVt::encodeFocus(false, t.modes()); }));
    }
    m_focused = pane;
    if (pane && m_windowFocused) {
        pane->sendInput(pane->withTerminal([](Terminal& t) { return AfterglowVt::encodeFocus(true, t.modes()); }));
    }
    updateTitle();
    restartBlink();
}

void TerminalWindow::updateTitle()
{
    const QString t = m_focused ? m_focused->title() : QString();
    setTitle(t.isEmpty() ? QStringLiteral("afterglow") : t);
}

void TerminalWindow::restartBlink()
{
    m_blinkOn = true;
    if (m_windowFocused) {
        m_blinkTimer.start();
    }
    scheduleRender();
}

// --- rendering -------------------------------------------------------------

void TerminalWindow::scheduleRender()
{
    // requestUpdate() coalesces on its own. Tracking "already requested" here
    // is wrong: Qt drops requests made before the window is exposed, and a
    // stuck flag would then swallow every later request.
    requestUpdate();
}

QRect TerminalWindow::contentRect(const QRect& paneRect) const
{
    const int pad = int(std::lround(PaddingLogical * devicePixelRatio()));
    return paneRect.adjusted(pad, pad, -pad, -pad);
}

void TerminalWindow::relayout()
{
    const QSize px = m_renderer->pixelSize();
    const double dpr = devicePixelRatio();
    m_splits->layout(QRect(QPoint(0, 0), px), std::max(1, int(std::lround(dpr))));
    const auto& m = m_renderer->fonts().fonts->metrics();
    m_splits->forEachPane([&](Pane* p, const QRect& r) {
        const QRect content = contentRect(r);
        const int cols = std::max(2, content.width() / m.cellWidth);
        const int rows = std::max(1, content.height() / m.cellHeight);
        p->resize(cols, rows, m.cellWidth, m.cellHeight);
    });
    m_layoutSize = px;
    m_layoutDpr = dpr;
    m_layoutFontEpoch = m_device.fontEpoch();
}

void TerminalWindow::renderNow()
{
    if (!isExposed() || !m_renderer->ensureSwapChain()) {
        return;
    }
    qCDebug(lcWindow) << "frame";
    if (m_renderer->pixelSize() != m_layoutSize || devicePixelRatio() != m_layoutDpr
        || m_device.fontEpoch() != m_layoutFontEpoch) {
        relayout();
    }

    std::vector<AfterglowRender::WindowRenderer::Pane> panes;
    std::vector<std::pair<QRect, std::uint32_t>> overlays;
    const bool multiple = m_splits->paneCount() > 1;
    bool holding = false;
    m_splits->forEachPane([&](Pane* p, const QRect& r) {
        if (!p->refreshSnapshot()) {
            holding = true;
        }
        AfterglowRender::WindowRenderer::Pane wp;
        wp.snapshot = &p->snapshot();
        wp.params.rect = r;
        wp.params.contentOrigin = contentRect(r).topLeft();
        wp.params.focused = p == m_focused && m_windowFocused;
        wp.params.cursorBlinkOn = m_blinkOn;
        panes.push_back(wp);
        if (multiple && p != m_focused) {
            overlays.emplace_back(r, UnfocusedDim);
        }
    });
    for (const auto& d : m_splits->dividers()) {
        overlays.emplace_back(d.rect, DividerColor);
    }
    const std::uint32_t clear = m_focused ? m_focused->snapshot().defaultBg : packRgba(0, 0, 0);
    m_renderer->render(panes, overlays, clear);

    if (holding) {
        // A pane is holding its previous frame; look again once the hold may
        // have timed out. New output re-renders sooner on its own.
        QTimer::singleShot(50, this, &TerminalWindow::scheduleRender);
    }
}

// --- events ----------------------------------------------------------------

bool TerminalWindow::event(QEvent* e)
{
    switch (e->type()) {
    case QEvent::UpdateRequest:
        renderNow();
        return true;
    case QEvent::PlatformSurface:
        if (static_cast<QPlatformSurfaceEvent*>(e)->surfaceEventType()
            == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed) {
            m_renderer->releaseSwapChain();
        }
        break;
    case QEvent::Close: {
        qCInfo(lcWindow) << "window close requested";
        const bool handled = QWindow::event(e);
        deleteLater();
        return handled;
    }
    default:
        break;
    }
    return QWindow::event(e);
}

void TerminalWindow::exposeEvent(QExposeEvent*)
{
    if (isExposed()) {
        renderNow();
    }
}

void TerminalWindow::resizeEvent(QResizeEvent*)
{
    scheduleRender();
}

void TerminalWindow::focusInEvent(QFocusEvent*)
{
    m_windowFocused = true;
    if (m_focused) {
        m_focused->sendInput(m_focused->withTerminal([](Terminal& t) { return AfterglowVt::encodeFocus(true, t.modes()); }));
    }
    restartBlink();
}

void TerminalWindow::focusOutEvent(QFocusEvent*)
{
    m_windowFocused = false;
    m_blinkTimer.stop();
    m_blinkOn = true;
    if (m_focused) {
        m_focused->sendInput(m_focused->withTerminal([](Terminal& t) { return AfterglowVt::encodeFocus(false, t.modes()); }));
    }
    scheduleRender();
}

bool TerminalWindow::handleShortcut(QKeyEvent* e)
{
    const Qt::KeyboardModifiers mods = e->modifiers() & ~Qt::KeypadModifier;
    const int key = e->key();

    if (mods == (Qt::ControlModifier | Qt::ShiftModifier)) {
        switch (key) {
        case Qt::Key_E:
            splitFocused(SplitTree::Direction::Right);
            return true;
        case Qt::Key_O:
            splitFocused(SplitTree::Direction::Down);
            return true;
        case Qt::Key_W:
            if (m_focused) {
                closePane(m_focused);
            }
            return true;
        case Qt::Key_N: {
            auto* w = new TerminalWindow(m_device, m_focused ? m_focused->workingDirectory() : QString());
            w->show();
            return true;
        }
        case Qt::Key_V:
            paste(QClipboard::Clipboard);
            return true;
        case Qt::Key_C:
            copySelection(true);
            return true;
        case Qt::Key_Left:
        case Qt::Key_Right:
        case Qt::Key_Up:
        case Qt::Key_Down: {
            if (!m_focused) {
                return true;
            }
            const auto dir = key == Qt::Key_Left    ? SplitTree::Neighbour::Left
                             : key == Qt::Key_Right ? SplitTree::Neighbour::Right
                             : key == Qt::Key_Up    ? SplitTree::Neighbour::Up
                                                    : SplitTree::Neighbour::Down;
            if (Pane* n = m_splits->neighbour(m_focused, dir)) {
                setFocusedPane(n);
            }
            return true;
        }
        case Qt::Key_Plus:
        case Qt::Key_Equal:
            m_splits->equalize();
            m_layoutSize = QSize();
            scheduleRender();
            return true;
        default:
            break;
        }
    }

    if (mods == Qt::ControlModifier) {
        switch (key) {
        case Qt::Key_Equal:
        case Qt::Key_Plus:
            m_device.adjustFontSize(1.0f);
            return true;
        case Qt::Key_Minus:
            m_device.adjustFontSize(-1.0f);
            return true;
        case Qt::Key_0: {
            AfterglowRender::FontSpec spec = m_device.fontSpec();
            spec.sizePoints = AfterglowRender::FontSpec{}.sizePoints;
            m_device.setFontSpec(spec);
            return true;
        }
        default:
            break;
        }
    }

    if (mods == Qt::ShiftModifier && m_focused) {
        long delta = 0;
        const int page = std::max(1, m_focused->rows() - 1);
        switch (key) {
        case Qt::Key_PageUp:
            delta = page;
            break;
        case Qt::Key_PageDown:
            delta = -page;
            break;
        case Qt::Key_Home:
            delta = 1L << 30;
            break;
        case Qt::Key_End:
            delta = -(1L << 30);
            break;
        default:
            return false;
        }
        const bool alt = m_focused->withTerminal([](Terminal& t) { return t.modes().altScreen; });
        if (alt) {
            return false; // full-screen programs get these keys
        }
        m_focused->withTerminal([delta](Terminal& t) { t.scrollViewport(delta); });
        scheduleRender();
        return true;
    }
    return false;
}

void TerminalWindow::keyPressEvent(QKeyEvent* e)
{
    if (handleShortcut(e)) {
        return;
    }
    sendKey(e);
}

void TerminalWindow::keyReleaseEvent(QKeyEvent* e)
{
    // Auto-repeat comes as release/press pairs; only the press carries it.
    if (!e->isAutoRepeat()) {
        sendKey(e);
    }
}

void TerminalWindow::sendKey(QKeyEvent* e)
{
    if (!m_focused) {
        return;
    }
    QByteArray storage;
    const AfterglowVt::KeyEvent ev = mapKeyEvent(e, storage);
    const std::string bytes = m_focused->withTerminal([&ev](Terminal& t) { return AfterglowVt::encodeKey(ev, t.modes()); });
    if (bytes.empty()) {
        return;
    }
    if (ev.action != AfterglowVt::KeyAction::Release) {
        m_focused->clearSelection();
        restartBlink();
    }
    m_focused->sendInput(bytes);
}

void TerminalWindow::copySelection(bool toClipboard)
{
    if (!m_focused || !m_focused->hasSelection()) {
        return;
    }
    QGuiApplication::clipboard()->setText(m_focused->selectedText(),
                                          toClipboard ? QClipboard::Clipboard : QClipboard::Selection);
}

void TerminalWindow::paste(QClipboard::Mode mode)
{
    if (!m_focused) {
        return;
    }
    const QByteArray text = QGuiApplication::clipboard()->text(mode).toUtf8();
    if (text.isEmpty()) {
        return;
    }
    const std::string bytes = m_focused->withTerminal([&text](Terminal& t) {
        return AfterglowVt::encodePaste(std::string_view(text.constData(), std::size_t(text.size())), t.modes());
    });
    m_focused->sendInput(bytes);
}

QPoint TerminalWindow::cellAt(Pane* pane, const QPointF& pos) const
{
    const auto& m = m_renderer->fonts().fonts->metrics();
    const QPoint origin = contentRect(m_splits->rectOf(pane)).topLeft();
    const QPointF px = pos * devicePixelRatio() - QPointF(origin);
    const int col = std::clamp(int(std::floor(px.x() / m.cellWidth)), 0, std::max(0, pane->cols() - 1));
    const int row = std::clamp(int(std::floor(px.y() / m.cellHeight)), 0, std::max(0, pane->rows() - 1));
    return QPoint(col, row);
}

bool TerminalWindow::sendMouse(Pane* pane, const QPointF& pos, int button, int action,
                               Qt::KeyboardModifiers mods)
{
    if (!pane || (mods & Qt::ShiftModifier)) {
        return false;
    }
    const AfterglowVt::Modes modes = pane->withTerminal([](Terminal& t) { return t.modes(); });
    if (modes.mouseTracking == AfterglowVt::MouseTracking::None) {
        return false;
    }
    const QPoint cell = cellAt(pane, pos);
    if (action == int(AfterglowVt::MouseAction::Motion) && cell == m_lastMouseCell
        && modes.mouseFormat != AfterglowVt::MouseFormat::SgrPixels) {
        return true;
    }
    m_lastMouseCell = cell;

    AfterglowVt::MouseEvent ev;
    ev.button = button < 0 ? AfterglowVt::MouseButton::None : AfterglowVt::MouseButton(button);
    ev.action = AfterglowVt::MouseAction(action);
    ev.mods = mapModifiers(mods);
    ev.col = cell.x();
    ev.row = cell.y();
    const QPoint origin = contentRect(m_splits->rectOf(pane)).topLeft();
    const QPointF px = pos * devicePixelRatio() - QPointF(origin);
    ev.pixelX = std::max(0, int(px.x()));
    ev.pixelY = std::max(0, int(px.y()));
    ev.buttonHeld = m_reportedButtons != Qt::NoButton;
    const std::string bytes = AfterglowVt::encodeMouse(ev, modes);
    if (!bytes.empty()) {
        pane->sendInput(bytes);
    }
    return true;
}

int TerminalWindow::rawRowAt(Pane* pane, const QPointF& pos) const
{
    const auto& m = m_renderer->fonts().fonts->metrics();
    const QPoint origin = contentRect(m_splits->rectOf(pane)).topLeft();
    return int(std::floor((pos.y() * devicePixelRatio() - origin.y()) / m.cellHeight));
}

void TerminalWindow::mousePressEvent(QMouseEvent* e)
{
    Pane* p = m_splits->paneAt((e->position() * devicePixelRatio()).toPoint());
    if (p && p != m_focused) {
        setFocusedPane(p);
    }
    if (!m_focused) {
        return;
    }
    const int code = mouseButtonCode(e->button());
    if (code >= 0 && sendMouse(m_focused, e->position(), code, int(AfterglowVt::MouseAction::Press), e->modifiers())) {
        m_reportedButtons |= e->button();
        return;
    }

    if (e->button() == Qt::MiddleButton) {
        paste(QClipboard::Selection);
        return;
    }
    if (e->button() != Qt::LeftButton) {
        return;
    }

    // Count clicks on the same cell for word (2) and line (3) selection.
    const QPoint cell = cellAt(m_focused, e->position());
    const int interval = QGuiApplication::styleHints()->mouseDoubleClickInterval();
    if (m_lastClick.isValid() && m_lastClick.elapsed() < interval && cell == m_lastClickCell) {
        m_clickCount = m_clickCount % 3 + 1;
    } else {
        m_clickCount = 1;
    }
    m_lastClick.start();
    m_lastClickCell = cell;

    AfterglowVt::SelectionMode mode = AfterglowVt::SelectionMode::Normal;
    if (m_clickCount == 2) {
        mode = AfterglowVt::SelectionMode::Word;
    } else if (m_clickCount == 3) {
        mode = AfterglowVt::SelectionMode::Line;
    } else if (e->modifiers() & Qt::AltModifier) {
        mode = AfterglowVt::SelectionMode::Block;
    }
    m_selectionPane = m_focused;
    m_dragPos = e->position();
    m_focused->startSelection(m_focused->selectionPointAt(cell.x(), cell.y()), mode,
                              mode == AfterglowVt::SelectionMode::Word || mode == AfterglowVt::SelectionMode::Line);
    scheduleRender();
}

void TerminalWindow::updateSelectionDrag()
{
    Pane* pane = m_selectionPane;
    if (!pane) {
        m_autoScroll.stop();
        return;
    }
    // Dragging past the top or bottom scrolls through history.
    const int row = rawRowAt(pane, m_dragPos);
    if (row < 0) {
        pane->withTerminal([](Terminal& t) { t.scrollViewport(1); });
        if (!m_autoScroll.isActive()) {
            m_autoScroll.start();
        }
    } else if (row >= pane->rows()) {
        pane->withTerminal([](Terminal& t) { t.scrollViewport(-1); });
        if (!m_autoScroll.isActive()) {
            m_autoScroll.start();
        }
    } else {
        m_autoScroll.stop();
    }
    const QPoint cell = cellAt(pane, m_dragPos);
    pane->extendSelection(pane->selectionPointAt(cell.x(), cell.y()));
    scheduleRender();
}

void TerminalWindow::mouseReleaseEvent(QMouseEvent* e)
{
    if (m_reportedButtons & e->button()) {
        m_reportedButtons &= ~e->button();
        const int code = mouseButtonCode(e->button());
        sendMouse(m_focused, e->position(), code, int(AfterglowVt::MouseAction::Release), e->modifiers());
        return;
    }
    if (e->button() == Qt::LeftButton && m_selectionPane) {
        m_autoScroll.stop();
        if (m_selectionPane->hasSelection()) {
            QGuiApplication::clipboard()->setText(m_selectionPane->selectedText(), QClipboard::Selection);
        } else {
            m_selectionPane->clearSelection();
            scheduleRender();
        }
        m_selectionPane = nullptr;
    }
}

void TerminalWindow::mouseMoveEvent(QMouseEvent* e)
{
    if (m_selectionPane && (e->buttons() & Qt::LeftButton)) {
        m_dragPos = e->position();
        updateSelectionDrag();
        return;
    }
    if (!m_focused) {
        return;
    }
    int code = -1;
    if (m_reportedButtons & Qt::LeftButton) {
        code = 0;
    } else if (m_reportedButtons & Qt::MiddleButton) {
        code = 1;
    } else if (m_reportedButtons & Qt::RightButton) {
        code = 2;
    }
    sendMouse(m_focused, e->position(), code, int(AfterglowVt::MouseAction::Motion), e->modifiers());
}

void TerminalWindow::wheelEvent(QWheelEvent* e)
{
    Pane* p = m_splits->paneAt((e->position() * devicePixelRatio()).toPoint());
    if (!p) {
        p = m_focused;
    }
    if (!p) {
        return;
    }
    m_wheelAccumulator += e->angleDelta().y() / 120.0;
    const int steps = int(m_wheelAccumulator);
    if (steps == 0) {
        return;
    }
    m_wheelAccumulator -= steps;

    const AfterglowVt::Modes modes = p->withTerminal([](Terminal& t) { return t.modes(); });
    if (modes.mouseTracking != AfterglowVt::MouseTracking::None && !(e->modifiers() & Qt::ShiftModifier)) {
        const int button = int(steps > 0 ? AfterglowVt::MouseButton::WheelUp : AfterglowVt::MouseButton::WheelDown);
        for (int i = 0; i < std::abs(steps); ++i) {
            sendMouse(p, e->position(), button, int(AfterglowVt::MouseAction::Press), e->modifiers());
        }
        return;
    }
    if (modes.altScreen && modes.altScroll) {
        AfterglowVt::KeyEvent key;
        key.key = steps > 0 ? AfterglowVt::Key::Up : AfterglowVt::Key::Down;
        std::string bytes;
        for (int i = 0; i < std::abs(steps) * WheelLines; ++i) {
            bytes += AfterglowVt::encodeKey(key, modes);
        }
        p->sendInput(bytes);
        return;
    }
    p->withTerminal([steps](Terminal& t) { t.scrollViewport(long(steps) * WheelLines); });
    scheduleRender();
}

} // namespace Afterglow
