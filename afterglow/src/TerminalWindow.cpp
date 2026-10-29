// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TerminalWindow.h"

#include "KeyMapping.h"
#include "chrome/ChromeController.h"

#include <PhosphorAnimation/Spring.h>
#include <PhosphorWayland/LayerSurface.h>

#include <KWindowEffects>
#include "chrome/ChromeLayer.h"
#include "shaders/ShaderLibrary.h"
#include "Pane.h"

#include <AfterglowRender/FontSet.h>
#include <AfterglowRender/GpuDevice.h>
#include <AfterglowVt/Input.h>

#include <QClipboard>
#include <QDesktopServices>
#include <QUrl>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLoggingCategory>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QInputMethod>
#include <QInputMethodEvent>
#include <QInputMethodQueryEvent>
#include <QPlatformSurfaceEvent>
#include <QRegularExpression>
#include <QScreen>
#include <QVariantMap>

#include <utility>
#include <QStyleHints>

#include <cmath>

Q_LOGGING_CATEGORY(lcWindow, "afterglow.window", QtInfoMsg)

namespace Afterglow {

using AfterglowRender::packRgba;
using AfterglowVt::Terminal;

namespace {
QList<TerminalWindow*> s_windows;

constexpr int BlinkIntervalMs = 530;
constexpr int WheelLines = 3;
constexpr int TabBarLogicalHeight = 30;

/// A critically damped spring: fast, with no overshoot. @p omega sets the speed.
template<typename T>
PhosphorAnimation::MotionSpec<T> springSpec(PhosphorAnimation::IMotionClock* clock, qreal omega)
{
    PhosphorAnimation::MotionSpec<T> spec;
    spec.clock = clock;
    spec.profile.curve = std::make_shared<PhosphorAnimation::Spring>(omega, 1.0);
    spec.retargetPolicy = PhosphorAnimation::RetargetPolicy::PreserveVelocity;
    return spec;
}

constexpr qreal CursorSpeed = 48.0;
constexpr qreal SplitSpeed = 22.0;
constexpr qreal ScrollSpeed = 26.0;

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

TerminalWindow::TerminalWindow(Services& services, const WindowOptions& options)
    : m_device(services.device)
    , m_settings(services.settings)
    , m_engine(services.engine)
    , m_shaders(services.shaders)
{
    m_quick = options.quick;
    AfterglowRender::GpuDevice& device = m_device;
    QQmlEngine& engine = m_engine;
    setSurfaceType(QSurface::VulkanSurface);
    setVulkanInstance(device.vulkanInstance());
    setTitle(QStringLiteral("afterglow"));
    m_renderer = std::make_unique<AfterglowRender::WindowRenderer>(device, this);

    const auto& m = m_renderer->fonts().fonts->metrics();
    const double dpr = devicePixelRatio();
    const WindowSettings& ws = m_settings.settings().window;
    const int pad = int(std::lround(ws.padding * dpr));
    const QSize initial(ws.initialColumns * m.cellWidth + 2 * pad, ws.initialRows * m.cellHeight + 2 * pad);
    resize(int(std::lround(initial.width() / dpr)), int(std::lround(initial.height() / dpr)));

    m_clock = std::make_unique<FrameClock>(this);

    m_controller = new ChromeController(this);
    m_chrome = new ChromeLayer(device, this, engine, m_controller);
    connect(m_chrome, &ChromeLayer::updateRequested, this, &TerminalWindow::scheduleRender);
    connect(m_controller, &ChromeController::activateTabRequested, this, &TerminalWindow::switchTab);
    connect(m_controller, &ChromeController::closeTabRequested, this, &TerminalWindow::closeTab);
    connect(m_controller, &ChromeController::newTabRequested, this,
            [this] { newTab(m_focused ? m_focused->workingDirectory() : QString()); });
    connect(m_controller, &ChromeController::actionRequested, this, [this](int id) { performAction(Action(id)); });
    connect(m_controller, &ChromeController::searchRequested, this, &TerminalWindow::runSearch);
    connect(m_controller, &ChromeController::searchOpenChanged, this, [this] {
        if (!m_controller->searchOpen() && m_focused) {
            m_focused->clearSearch();
            m_controller->setSearchStatus({});
        }
        scheduleRender();
    });
    connect(m_controller, &ChromeController::paletteOpenChanged, this, &TerminalWindow::scheduleRender);
    connect(m_controller, &ChromeController::pasteConfirmChanged, this, &TerminalWindow::scheduleRender);
    connect(m_controller, &ChromeController::pasteAnswered, this, [this](bool paste) {
        const QByteArray text = std::exchange(m_pendingPaste, {});
        if (paste && m_focused) {
            const std::string bytes = m_focused->withTerminal([&text](Terminal& t) {
                return AfterglowVt::encodePaste(std::string_view(text.constData(), std::size_t(text.size())), t.modes());
            });
            m_focused->sendInput(bytes);
        }
    });
    connect(this, &TerminalWindow::tabsChanged, this, &TerminalWindow::updateChrome);

    connect(&m_shaders, &ShaderLibrary::changed, this, &TerminalWindow::applyShaders);
    applyShaders();

    if (m_quick) {
        setUpQuickTerminal();
    }
    applyWindowEffects();

    newTab(options.workingDirectory, options.command);

    connect(&m_device, &AfterglowRender::GpuDevice::fontsChanged, this, &TerminalWindow::scheduleRender);
    connect(&m_settings, &SettingsStore::changed, this, &TerminalWindow::applySettings);

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
    // The chrome renders on the device through this window; it goes first.
    delete m_chrome;
    m_chrome = nullptr;
    m_renderer.reset();
}

QList<TerminalWindow*> TerminalWindow::windows()
{
    return s_windows;
}

// --- panes -----------------------------------------------------------------

Pane* TerminalWindow::createPane(const QString& workingDirectory, const QRect& area, const QStringList& command)
{
    auto* pane = new Pane(m_settings.settings().terminal, this);
    connect(pane, &QObject::destroyed, this, [this, pane] {
        m_motion.erase(pane);
        m_growFrom.erase(pane);
    });
    connect(pane, &Pane::contentChanged, this, &TerminalWindow::scheduleRender);
    connect(pane, &Pane::titleUpdated, this, [this, pane] {
        if (pane == m_focused) {
            updateTitle();
        }
        Q_EMIT tabsChanged();
    });
    connect(pane, &Pane::closed, this, [this, guard = QPointer<Pane>(pane)] {
        if (guard) {
            closePane(guard);
        }
    });
    connect(pane, &Pane::clipboardWriteRequested, this, [](const QString& text, bool primary) {
        QGuiApplication::clipboard()->setText(text, primary ? QClipboard::Selection : QClipboard::Clipboard);
    });
    connect(pane, &Pane::bellRang, this, [this, guard = QPointer<Pane>(pane)] {
        if (guard) {
            ringBell(guard);
        }
    });
    connect(pane, &Pane::notificationRequested, this, [this, guard = QPointer<Pane>(pane)](const QString& title, const QString& body) {
        // Only when the user is not looking at that pane already.
        if (guard && m_settings.settings().notifications && (!isActive() || guard != m_focused)) {
            sendNotification(title, body);
        }
    });
    connect(pane, &Pane::pointerShapeRequested, this, [this, guard = QPointer<Pane>(pane)](const QString& shape) {
        if (guard) {
            motion(guard).pointerShape = shape;
            applyPointerShape();
        }
    });

    AfterglowPty::SpawnOptions opts;
    opts.workingDirectory = workingDirectory.toStdString();
    if (!command.isEmpty()) {
        for (const QString& arg : command) {
            opts.argv.push_back(arg.toStdString());
        }
    } else if (const QString shell = m_settings.settings().shell; !shell.isEmpty()) {
        opts.argv = {shell.toStdString()};
    }
    const auto& m = m_renderer->fonts().fonts->metrics();
    const QRect content = contentRect(area);
    const int cols = std::max(2, content.width() / m.cellWidth);
    const int rows = std::max(1, content.height() / m.cellHeight);
    QString error;
    if (!pane->start(opts, m_settings.settings().term, cols, rows, m.cellWidth, m.cellHeight, &error)) {
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
    // The new pane grows out of the edge being split.
    const QRect old = m_splits->rectOf(m_focused);
    m_growFrom[added] = direction == SplitTree::Direction::Right
        ? QRectF(old.right() + 1, old.top(), 0, old.height())
        : QRectF(old.left(), old.bottom() + 1, old.width(), 0);
    m_splits->split(m_focused, added, direction);
    setFocusedPane(added);
    m_layoutSize = QSize();
    scheduleRender();
}

void TerminalWindow::closePane(Pane* pane)
{
    const int index = tabOf(pane);
    if (index < 0) {
        return;
    }
    Tab& tab = *m_tabs[std::size_t(index)];
    if (!tab.splits->remove(pane)) {
        closeTab(index); // the tab's last pane
        return;
    }
    if (tab.focused == pane || (index == m_currentTab && m_focused == pane)) {
        Pane* next = nullptr;
        tab.splits->forEachPane([&next](Pane* p, const QRect&) {
            if (!next) {
                next = p;
            }
        });
        tab.focused = next;
        if (index == m_currentTab) {
            setFocusedPane(next);
        }
    }
    pane->deleteLater();
    m_layoutSize = QSize();
    scheduleRender();
}

// --- tabs ------------------------------------------------------------------

int TerminalWindow::tabOf(const Pane* pane) const
{
    for (std::size_t i = 0; i < m_tabs.size(); ++i) {
        if (m_tabs[i]->splits->contains(pane)) {
            return int(i);
        }
    }
    return -1;
}

int TerminalWindow::tabBarHeight() const
{
    return m_tabs.size() > 1 ? int(std::lround(TabBarLogicalHeight * devicePixelRatio())) : 0;
}

QRect TerminalWindow::paneArea() const
{
    const QSize px = m_renderer->pixelSize().isEmpty()
        ? (size() * devicePixelRatio())
        : m_renderer->pixelSize();
    const int bar = tabBarHeight();
    return QRect(0, bar, px.width(), std::max(0, px.height() - bar));
}

void TerminalWindow::newTab(const QString& workingDirectory, const QStringList& command)
{
    auto tab = std::make_unique<Tab>();
    m_tabs.push_back(std::move(tab));
    // Size the shell for the area it will get, with the tab bar if it now shows.
    Pane* first = createPane(workingDirectory, paneArea(), command);
    m_tabs.back()->splits = std::make_unique<SplitTree>(first);
    m_tabs.back()->focused = first;
    switchTab(int(m_tabs.size()) - 1);
}

void TerminalWindow::switchTab(int index)
{
    if (index < 0 || index >= int(m_tabs.size())) {
        return;
    }
    if (m_splits && m_currentTab < int(m_tabs.size())) {
        m_tabs[std::size_t(m_currentTab)]->focused = m_focused;
    }
    m_currentTab = index;
    Tab& tab = *m_tabs[std::size_t(index)];
    m_splits = tab.splits.get();
    m_selectionPane = nullptr;
    setFocusedPane(tab.focused);
    m_layoutSize = QSize();
    Q_EMIT tabsChanged();
    scheduleRender();
}

void TerminalWindow::closeTab(int index)
{
    if (index < 0 || index >= int(m_tabs.size())) {
        return;
    }
    if (m_tabs.size() == 1) {
        qCInfo(lcWindow) << "last pane closed, closing the window";
        m_tabs[0]->splits->forEachPane([](Pane* p, const QRect&) { p->deleteLater(); });
        close();
        return;
    }
    m_tabs[std::size_t(index)]->splits->forEachPane([](Pane* p, const QRect&) { p->deleteLater(); });
    m_tabs.erase(m_tabs.begin() + index);
    if (m_currentTab >= index && m_currentTab > 0) {
        --m_currentTab;
    }
    m_splits = nullptr; // the old pointer may be gone; switchTab must not save into it
    m_focused = nullptr;
    switchTab(std::min(m_currentTab, int(m_tabs.size()) - 1));
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
    if (m_focused) {
        m_focused->setPreedit({}, 0);
    }
    QGuiApplication::inputMethod()->reset();
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
    const int pad = int(std::lround(m_settings.settings().window.padding * devicePixelRatio()));
    return paneRect.adjusted(pad, pad, -pad, -pad);
}

void TerminalWindow::relayout()
{
    const QSize px = m_renderer->pixelSize();
    const double dpr = devicePixelRatio();
    m_splits->layout(paneArea(), std::max(1, int(std::lround(dpr))));
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
    bool sliding = false;
    if (m_quick && m_slide.isAnimating()) {
        m_slide.advance();
        PhosphorWayland::LayerSurface::get(this)->setMargins(QMargins(0, int(std::lround(m_slide.value())), 0, 0));
        sliding = m_slide.isAnimating();
        if (!sliding && !m_quickShown) {
            hide();
            return;
        }
    }
    if (m_renderer->pixelSize() != m_layoutSize || devicePixelRatio() != m_layoutDpr
        || m_device.fontEpoch() != m_layoutFontEpoch) {
        relayout();
    }

    std::vector<AfterglowRender::WindowRenderer::Pane> panes;
    std::vector<std::pair<QRect, std::uint32_t>> overlays;
    const bool multiple = m_splits->paneCount() > 1;
    bool holding = false;
    bool animating = false;
    const QRect area = paneArea();
    const int dividerWidth = std::max(1, int(std::lround(devicePixelRatio())));
    const std::uint32_t dim = packRgba(0, 0, 0, std::uint8_t(std::lround(m_settings.settings().window.unfocusedDim * 255)));
    const auto backgroundAlpha = std::uint8_t(std::lround(m_settings.settings().window.backgroundOpacity * 255));
    m_splits->forEachPane([&](Pane* p, const QRect& r) {
        AfterglowRender::WindowRenderer::Pane wp;
        preparePane(p, r, wp.params, holding, animating);
        wp.snapshot = &p->snapshot();
        wp.params.focused = p == m_focused && m_windowFocused;
        wp.params.cursorBlinkOn = m_blinkOn;
        wp.params.backgroundAlpha = backgroundAlpha;
        if (m_hover.pane == p) {
            wp.params.hoveredLink = m_hover.link;
            wp.params.hoverRow = m_hover.row;
            wp.params.hoverColStart = m_hover.colStart;
            wp.params.hoverColEnd = m_hover.colEnd;
        }
        panes.push_back(wp);
        const QRect shown = wp.params.rect;
        if (multiple && p != m_focused) {
            overlays.emplace_back(shown, dim);
        }
        if (PaneMotion& pm = motion(p); pm.bell.isAnimating()) {
            pm.bell.advance();
            overlays.emplace_back(shown, packRgba(255, 255, 255, std::uint8_t(std::clamp(pm.bell.value(), 0.0, 1.0) * 46)));
            animating |= pm.bell.isAnimating();
        }
        // Dividers follow the drawn edges, so they move with split animations.
        if (shown.right() < area.right()) {
            overlays.emplace_back(QRect(shown.right() + 1, shown.top(), dividerWidth, shown.height()),
                                  m_settings.settings().window.dividerColor);
        }
        if (shown.bottom() < area.bottom()) {
            overlays.emplace_back(QRect(shown.left(), shown.bottom() + 1, shown.width(), dividerWidth),
                                  m_settings.settings().window.dividerColor);
        }
    });
    // Split labels, in logical pixels for the chrome.
    QVariantList titles;
    if (multiple && m_settings.settings().window.paneTitles) {
        const qreal dpr = devicePixelRatio();
        std::size_t index = 0;
        m_splits->forEachPane([&](Pane* p, const QRect&) {
            const QRect r = panes[index++].params.rect;
            titles.append(QVariantMap{{QStringLiteral("x"), r.x() / dpr},
                                      {QStringLiteral("y"), r.y() / dpr},
                                      {QStringLiteral("width"), r.width() / dpr},
                                      {QStringLiteral("height"), r.height() / dpr},
                                      {QStringLiteral("title"), p->title()},
                                      {QStringLiteral("focused"), p == m_focused}});
        });
    }
    m_controller->setPaneTitles(titles);
    const std::uint32_t clearRgb = m_focused ? m_focused->snapshot().defaultBg : packRgba(0, 0, 0);
    const std::uint32_t clear = (clearRgb & 0x00FFFFFFu) | (std::uint32_t(backgroundAlpha) << 24);
    QRhiTexture* chrome = nullptr;
    if (chromeVisible()) {
        updateChrome();
        // Qt Quick records its own offscreen frame, so this precedes ours.
        m_chrome->renderIfNeeded(m_renderer->pixelSize());
        chrome = m_chrome->texture();
    }
    m_renderer->render(panes, overlays, clear, chrome, m_chrome->serial());
    if (const QRectF imeRect = inputMethodCursorRect(); imeRect != m_reportedImeRect && isActive()) {
        m_reportedImeRect = imeRect;
        QGuiApplication::inputMethod()->update(Qt::ImCursorRectangle);
    }
    if (animating || sliding || m_renderer->animating()) {
        scheduleRender(); // something is moving: keep frames coming
    }

    if (holding) {
        // A pane is holding its previous frame; look again once the hold may
        // have timed out. New output re-renders sooner on its own.
        QTimer::singleShot(50, this, &TerminalWindow::scheduleRender);
    }
}

// --- animation -------------------------------------------------------------

PaneMotion& TerminalWindow::motion(const Pane* pane)
{
    auto& slot = m_motion[pane];
    if (!slot) {
        slot = std::make_unique<PaneMotion>();
    }
    return *slot;
}

void TerminalWindow::preparePane(Pane* pane, const QRect& layoutRect, AfterglowRender::FrameBuilder::PaneParams& params,
                                 bool& holding, bool& animating)
{
    using PhosphorAnimation::RetargetPolicy;
    PaneMotion& m = motion(pane);
    const AnimationSettings& anim = m_settings.settings().animation;
    const auto& metrics = m_renderer->fonts().fonts->metrics();

    // Where the pane is drawn: its layout rectangle, or on the way there.
    const QRectF rectTarget(layoutRect);
    const auto rectSpec = springSpec<QRectF>(m_clock.get(), SplitSpeed);
    if (!m.rectKnown) {
        m.rectKnown = true;
        m.rectTarget = rectTarget;
        const auto grow = m_growFrom.find(pane);
        if (anim.splits && grow != m_growFrom.end()) {
            m.rect.start(grow->second, rectTarget, rectSpec);
        } else {
            m.rect.start(rectTarget, rectTarget, rectSpec);
        }
        if (grow != m_growFrom.end()) {
            m_growFrom.erase(grow);
        }
    } else if (rectTarget != m.rectTarget) {
        m.rectTarget = rectTarget;
        if (!anim.splits || m_dividerDrag) {
            m.rect.start(rectTarget, rectTarget, rectSpec);
        } else if (m.rect.isAnimating()) {
            m.rect.retarget(rectTarget, RetargetPolicy::PreserveVelocity);
        } else {
            m.rect.start(m.rect.value(), rectTarget, rectSpec);
        }
    }
    m.rect.advance();
    animating |= m.rect.isAnimating();
    const QRect shown = m.rect.value().toRect();

    // Scrolling: the terminal shows whole rows; the rest is a pixel shift.
    const std::size_t offset = pane->withTerminal([](Terminal& t) { return t.screen().viewportOffset(); });
    if (offset != m.appliedOffset) {
        // Moved by something else (typing, output, a prompt jump): follow it.
        m.scroll.cancel();
        m.scrollValue = qreal(offset);
        m.scrollTarget = qreal(offset);
        m.appliedOffset = offset;
    }
    if (m.scroll.isAnimating()) {
        m.scroll.advance();
        m.scrollValue = m.scroll.value();
        animating |= m.scroll.isAnimating();
    }
    const auto rowsBack = std::size_t(std::ceil(std::max<qreal>(0, m.scrollValue - 1e-3)));
    if (rowsBack != offset) {
        m.appliedOffset = pane->withTerminal([rowsBack](Terminal& t) {
            t.setViewportOffset(rowsBack);
            return t.screen().viewportOffset();
        });
    }
    const float shift = float((qreal(m.appliedOffset) - m.scrollValue) * metrics.cellHeight);
    if (!pane->refreshSnapshot(shift > 0.5f ? 1 : 0)) {
        holding = true;
    }

    // The cursor glides from cell to cell.
    const AfterglowRender::PaneSnapshot& snap = pane->snapshot();
    const QPointF cellTarget(snap.cursorX * metrics.cellWidth, snap.cursorY * metrics.cellHeight);
    const auto cursorSpec = springSpec<QPointF>(m_clock.get(), CursorSpeed);
    const bool glide = anim.cursor && snap.cursorVisible;
    // Typing and deleting move the cursor a cell at a time: those moves are
    // immediate, or the cursor would trail behind fast keystrokes. Only
    // jumps (another row, or further along it) glide.
    const QPointF step = cellTarget - m.cursorTarget;
    const bool smallMove = std::abs(step.y()) < 0.5 && std::abs(step.x()) <= 2.0 * metrics.cellWidth;
    if (!m.cursorKnown || !glide || smallMove) {
        m.cursorKnown = true;
        m.cursorTarget = cellTarget;
        m.cursor.start(cellTarget, cellTarget, cursorSpec);
    } else if (cellTarget != m.cursorTarget) {
        m.cursorTarget = cellTarget;
        if (m.cursor.isAnimating()) {
            m.cursor.retarget(cellTarget, RetargetPolicy::PreserveVelocity);
        } else {
            m.cursor.start(m.cursor.value(), cellTarget, cursorSpec);
        }
    }
    m.cursor.advance();
    animating |= m.cursor.isAnimating();

    params.rect = shown;
    params.contentOrigin = contentRect(shown).topLeft();
    params.cursorPosition = m.cursor.value();
    params.scrollShift = shift;
}

void TerminalWindow::scrollPane(Pane* pane, double rows, bool animate)
{
    if (!pane) {
        return;
    }
    PaneMotion& m = motion(pane);
    const auto history = double(pane->withTerminal([](Terminal& t) { return t.screen().historySize(); }));
    const double base = m.scroll.isAnimating() ? m.scrollTarget : m.scrollValue;
    const double target = std::clamp(base + rows, 0.0, history);
    m.scrollTarget = target;
    if (!animate || !m_settings.settings().animation.scroll) {
        m.scroll.cancel();
        m.scrollValue = target;
    } else if (m.scroll.isAnimating()) {
        m.scroll.retarget(target, PhosphorAnimation::RetargetPolicy::PreserveVelocity);
    } else {
        m.scroll.start(m.scrollValue, target, springSpec<qreal>(m_clock.get(), ScrollSpeed));
    }
    scheduleRender();
}

// --- events ----------------------------------------------------------------

bool TerminalWindow::event(QEvent* e)
{
    switch (e->type()) {
    case QEvent::UpdateRequest:
        renderNow();
        return true;
    case QEvent::InputMethodQuery: {
        // The window is its own focus object; tell the input method about the terminal.
        auto* q = static_cast<QInputMethodQueryEvent*>(e);
        const Qt::InputMethodQueries queries = q->queries();
        if (queries & Qt::ImEnabled) {
            q->setValue(Qt::ImEnabled, !m_controller->wantsKeyboard());
        }
        if (queries & Qt::ImCursorRectangle) {
            q->setValue(Qt::ImCursorRectangle, inputMethodCursorRect());
        }
        if (queries & Qt::ImHints) {
            q->setValue(Qt::ImHints, int(Qt::ImhNoPredictiveText | Qt::ImhNoAutoUppercase | Qt::ImhPreferLatin));
        }
        // No surrounding text: a terminal cannot let the input method edit it.
        if (queries & Qt::ImSurroundingText) {
            q->setValue(Qt::ImSurroundingText, QString());
        }
        if (queries & Qt::ImCursorPosition) {
            q->setValue(Qt::ImCursorPosition, 0);
        }
        if (queries & Qt::ImAnchorPosition) {
            q->setValue(Qt::ImAnchorPosition, 0);
        }
        q->accept();
        return true;
    }
    case QEvent::InputMethod: {
        auto* ime = static_cast<QInputMethodEvent*>(e);
        if (m_controller->wantsKeyboard()) {
            m_chrome->deliver(e);
            return true;
        }
        if (m_focused) {
            if (!ime->commitString().isEmpty()) {
                m_focused->clearSelection();
                m_focused->sendInput(ime->commitString().toUtf8().toStdString());
            }
            int caret = int(ime->preeditString().size());
            for (const QInputMethodEvent::Attribute& a : ime->attributes()) {
                if (a.type == QInputMethodEvent::Cursor) {
                    caret = a.start;
                }
            }
            m_focused->setPreedit(ime->preeditString(), caret);
            scheduleRender();
        }
        ime->accept();
        return true;
    }
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
    if (isExposed() && !m_effectsApplied) {
        m_effectsApplied = true;
        applyWindowEffects();
    }
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
    const Action action = m_settings.settings().keybinds.match(e);
    if (action == Action::None) {
        return false;
    }
    return performAction(action);
}

bool TerminalWindow::performAction(Action action)
{
    auto focus = [this](SplitTree::Neighbour dir) {
        if (m_focused) {
            if (Pane* n = m_splits->neighbour(m_focused, dir)) {
                setFocusedPane(n);
            }
        }
        return true;
    };
    auto scroll = [this](long delta) {
        if (!m_focused) {
            return false;
        }
        // Full-screen programs get these keys; there is no history to scroll.
        if (m_focused->withTerminal([](Terminal& t) { return t.modes().altScreen; })) {
            return false;
        }
        scrollPane(m_focused, double(delta));
        return true;
    };
    const long page = m_focused ? std::max(1, m_focused->rows() - 1) : 1;

    switch (action) {
    case Action::SplitRight:
        splitFocused(SplitTree::Direction::Right);
        return true;
    case Action::SplitDown:
        splitFocused(SplitTree::Direction::Down);
        return true;
    case Action::ClosePane:
        if (m_focused) {
            closePane(m_focused);
        }
        return true;
    case Action::NewWindow:
        Q_EMIT newWindowRequested(m_focused ? m_focused->workingDirectory() : QString());
        return true;
    case Action::NewTab:
        newTab(m_focused ? m_focused->workingDirectory() : QString());
        return true;
    case Action::CloseTab:
        closeTab(m_currentTab);
        return true;
    case Action::NextTab:
        if (m_tabs.size() < 2) {
            return false;
        }
        switchTab((m_currentTab + 1) % int(m_tabs.size()));
        return true;
    case Action::PreviousTab:
        if (m_tabs.size() < 2) {
            return false;
        }
        switchTab((m_currentTab + int(m_tabs.size()) - 1) % int(m_tabs.size()));
        return true;
    case Action::Copy:
        copySelection(true);
        return true;
    case Action::Paste:
        paste(QClipboard::Clipboard);
        return true;
    case Action::FocusLeft:
        return focus(SplitTree::Neighbour::Left);
    case Action::FocusRight:
        return focus(SplitTree::Neighbour::Right);
    case Action::FocusUp:
        return focus(SplitTree::Neighbour::Up);
    case Action::FocusDown:
        return focus(SplitTree::Neighbour::Down);
    case Action::EqualizeSplits:
        m_splits->equalize();
        m_layoutSize = QSize();
        scheduleRender();
        return true;
    case Action::FontBigger:
        m_device.adjustFontSize(1.0f);
        return true;
    case Action::FontSmaller:
        m_device.adjustFontSize(-1.0f);
        return true;
    case Action::FontReset:
        m_device.setFontSpec(m_settings.settings().font);
        return true;
    case Action::ScrollPageUp:
        return scroll(page);
    case Action::ScrollPageDown:
        return scroll(-page);
    case Action::ScrollTop:
        return scroll(1L << 30);
    case Action::ScrollBottom:
        return scroll(-(1L << 30));
    case Action::CommandPalette:
        m_controller->setPaletteOpen(!m_controller->paletteOpen());
        return true;
    case Action::Search:
        m_controller->setSearchOpen(true);
        return true;
    case Action::JumpPromptUp:
    case Action::JumpPromptDown:
        if (m_focused && m_focused->jumpToPrompt(action == Action::JumpPromptUp ? -1 : 1)) {
            scheduleRender();
        }
        return true;
    case Action::ReloadConfig:
        m_settings.reload();
        return true;
    case Action::OpenConfig:
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_settings.path()));
        return true;
    default:
        // Bound to a feature this window does not provide: let the key through.
        return false;
    }
}

bool TerminalWindow::chromeVisible() const
{
    return m_tabs.size() > 1 || m_controller->paletteOpen() || m_controller->searchOpen()
        || m_controller->pasteConfirmOpen() || !m_controller->paneTitles().isEmpty();
}

void TerminalWindow::updateChrome()
{
    QStringList titles;
    for (const auto& tab : m_tabs) {
        titles.append(tab->focused ? tab->focused->title() : QString());
    }
    m_controller->setTabs(titles, m_currentTab, m_tabs.size() > 1 ? TabBarLogicalHeight : 0);

    if (m_focused) {
        const auto& snap = m_focused->snapshot();
        auto color = [](std::uint32_t c) { return QColor(int(c & 0xFF), int((c >> 8) & 0xFF), int((c >> 16) & 0xFF)); };
        if (snap.cols > 0) {
            const AfterglowVt::Rgb accent = m_settings.settings().terminal.basePalette[12];
            m_controller->setTheme(color(snap.defaultBg), color(snap.defaultFg), QColor(accent.r, accent.g, accent.b));
        }
    }

    QVariantList actions;
    const Keybinds& kb = m_settings.settings().keybinds;
    for (Action a : Keybinds::allActions()) {
        if (a == Action::CommandPalette) {
            continue;
        }
        actions.append(QVariantMap{{QStringLiteral("id"), int(a)},
                                   {QStringLiteral("title"), Keybinds::title(a)},
                                   {QStringLiteral("shortcut"), kb.bindingsOf(a).join(QStringLiteral("  "))}});
    }
    m_controller->setActions(actions);
}

void TerminalWindow::runSearch(const QString& query, int direction)
{
    if (!m_focused) {
        return;
    }
    if (query.isEmpty()) {
        m_focused->clearSearch();
        m_controller->setSearchStatus({});
        scheduleRender();
        return;
    }
    const Pane::SearchResult r = m_focused->search(query, direction);
    m_controller->setSearchStatus(r.count == 0 ? QStringLiteral("No matches")
                                               : QStringLiteral("%1 of %2").arg(r.current + 1).arg(r.count));
    scheduleRender();
}

QRectF TerminalWindow::inputMethodCursorRect() const
{
    if (!m_focused || !m_splits) {
        return {};
    }
    const auto& m = const_cast<TerminalWindow*>(this)->m_renderer->fonts().fonts->metrics();
    const auto& snap = m_focused->snapshot();
    const QPoint origin = contentRect(m_splits->rectOf(m_focused)).topLeft();
    const qreal dpr = devicePixelRatio();
    return QRectF((origin.x() + snap.cursorX * m.cellWidth) / dpr, (origin.y() + snap.cursorY * m.cellHeight) / dpr,
                  m.cellWidth / dpr, m.cellHeight / dpr);
}

void TerminalWindow::ringBell(Pane* pane)
{
    const Settings::Bell bell = m_settings.settings().bell;
    if ((bell == Settings::Bell::Attention || bell == Settings::Bell::Both) && !isActive()) {
        alert(0);
    }
    if (bell == Settings::Bell::Visual || bell == Settings::Bell::Both) {
        motion(pane).bell.start(1.0, 0.0, springSpec<qreal>(m_clock.get(), 14.0));
        scheduleRender();
    }
}

void TerminalWindow::sendNotification(const QString& title, const QString& body)
{
    QDBusMessage msg = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.Notifications"),
                                                      QStringLiteral("/org/freedesktop/Notifications"),
                                                      QStringLiteral("org.freedesktop.Notifications"),
                                                      QStringLiteral("Notify"));
    const QString summary = title.isEmpty() ? (m_focused ? m_focused->title() : QStringLiteral("afterglow")) : title;
    msg << QStringLiteral("afterglow") << uint(0) << QStringLiteral("io.github.fuddlesworth.Afterglow") << summary
        << body << QStringList() << QVariantMap{{QStringLiteral("desktop-entry"), QStringLiteral("io.github.fuddlesworth.Afterglow")}}
        << int(-1);
    QDBusConnection::sessionBus().asyncCall(msg);
}

void TerminalWindow::applyPointerShape()
{
    if (!m_hover.url.isEmpty()) {
        setCursor(Qt::PointingHandCursor);
        return;
    }
    Pane* pane = m_splits ? m_splits->paneAt((m_pointer * devicePixelRatio()).toPoint()) : nullptr;
    const QString shape = pane ? motion(pane).pointerShape : QString();
    static const QHash<QString, Qt::CursorShape> Shapes = {
        {QStringLiteral("default"), Qt::ArrowCursor},     {QStringLiteral("text"), Qt::IBeamCursor},
        {QStringLiteral("pointer"), Qt::PointingHandCursor}, {QStringLiteral("wait"), Qt::WaitCursor},
        {QStringLiteral("progress"), Qt::BusyCursor},     {QStringLiteral("crosshair"), Qt::CrossCursor},
        {QStringLiteral("move"), Qt::SizeAllCursor},      {QStringLiteral("not-allowed"), Qt::ForbiddenCursor},
        {QStringLiteral("grab"), Qt::OpenHandCursor},     {QStringLiteral("grabbing"), Qt::ClosedHandCursor},
        {QStringLiteral("help"), Qt::WhatsThisCursor},    {QStringLiteral("ew-resize"), Qt::SizeHorCursor},
        {QStringLiteral("e-resize"), Qt::SizeHorCursor},  {QStringLiteral("w-resize"), Qt::SizeHorCursor},
        {QStringLiteral("ns-resize"), Qt::SizeVerCursor}, {QStringLiteral("n-resize"), Qt::SizeVerCursor},
        {QStringLiteral("s-resize"), Qt::SizeVerCursor},  {QStringLiteral("col-resize"), Qt::SplitHCursor},
        {QStringLiteral("row-resize"), Qt::SplitVCursor}, {QStringLiteral("nesw-resize"), Qt::SizeBDiagCursor},
        {QStringLiteral("nwse-resize"), Qt::SizeFDiagCursor},
    };
    if (const auto it = Shapes.constFind(shape); it != Shapes.constEnd()) {
        setCursor(*it);
    } else {
        // The terminal's own pointer: a text beam over the grid.
        setCursor(Qt::IBeamCursor);
    }
}

void TerminalWindow::updateLinkHover()
{
    LinkHover found;
    const bool ctrl = QGuiApplication::queryKeyboardModifiers() & Qt::ControlModifier;
    Pane* pane = ctrl ? m_splits->paneAt((m_pointer * devicePixelRatio()).toPoint()) : nullptr;
    if (pane) {
        const QPoint cell = cellAt(pane, m_pointer);
        const AfterglowRender::PaneSnapshot& snap = pane->snapshot();
        if (cell.x() < snap.cols && cell.y() < snap.rows) {
            const AfterglowRender::SnapshotCell& c = snap.at(cell.x(), cell.y());
            if (c.link != 0 && c.link <= snap.links.size()) {
                found.pane = pane;
                found.link = c.link;
                found.url = QString::fromStdString(snap.links[c.link - 1]);
            } else {
                // A plain-text URL on this row.
                QString text;
                std::vector<int> columns;
                for (int x = 0; x < snap.cols; ++x) {
                    const AfterglowRender::SnapshotCell& rc = snap.at(x, cell.y());
                    if (rc.width == 0) {
                        continue;
                    }
                    const char32_t cp = rc.cp ? rc.cp : U' ';
                    text += QString::fromUcs4(&cp, 1);
                    while (columns.size() < std::size_t(text.size())) {
                        columns.push_back(x);
                    }
                }
                static const QRegularExpression url(
                    QStringLiteral(R"((?:https?|ftp|file)://[^\s<>"'`]+|mailto:[^\s<>"'`]+|www\.[^\s<>"'`]+\.[^\s<>"'`]+)"));
                for (auto it = url.globalMatch(text); it.hasNext();) {
                    const QRegularExpressionMatch m = it.next();
                    QString candidate = m.captured();
                    // Trailing punctuation usually belongs to the sentence, not the URL.
                    while (!candidate.isEmpty() && QStringLiteral(".,;:!?'\")]}").contains(candidate.back())) {
                        if (candidate.back() == QLatin1Char(')') && candidate.count(QLatin1Char('(')) >= candidate.count(QLatin1Char(')'))) {
                            break;
                        }
                        candidate.chop(1);
                    }
                    const int first = columns[std::size_t(m.capturedStart())];
                    const int last = columns[std::size_t(m.capturedStart() + candidate.size() - 1)];
                    if (cell.x() >= first && cell.x() <= last) {
                        found.pane = pane;
                        found.row = cell.y();
                        found.colStart = first;
                        found.colEnd = last;
                        found.url = candidate.startsWith(QLatin1String("www.")) ? QStringLiteral("https://") + candidate
                                                                                : candidate;
                        break;
                    }
                }
            }
        }
    }
    const bool changed = found.pane != m_hover.pane || found.link != m_hover.link || found.row != m_hover.row
        || found.colStart != m_hover.colStart || found.colEnd != m_hover.colEnd;
    m_hover = found;
    if (changed) {
        applyPointerShape();
        scheduleRender();
    }
}

bool TerminalWindow::routePointerToChrome(QEvent* e, const QPointF& pos, bool press, bool release)
{
    if (!chromeVisible()) {
        m_chromeGrab = false;
        return false;
    }
    if (m_chromeGrab || m_chrome->wantsPointer(pos)) {
        m_chrome->deliver(e);
        if (press) {
            m_chromeGrab = true;
        } else if (release) {
            m_chromeGrab = false;
        }
        return true;
    }
    return false;
}

void TerminalWindow::applyShaders()
{
    const Settings& s = m_settings.settings();
    ShaderLibrary::Chains chains = m_shaders.build(s.paneShaders, s.windowShaders, s.shaderParams);
    m_renderer->setShaderChains(std::move(chains.pane), std::move(chains.window));
    scheduleRender();
}

void TerminalWindow::applySettings(const Settings& previous)
{
    const Settings& now = m_settings.settings();
    if (now.paneShaders != previous.paneShaders || now.windowShaders != previous.windowShaders
        || now.shaderParams != previous.shaderParams) {
        applyShaders();
    }
    if (now.terminal != previous.terminal) {
        for (Pane* p : findChildren<Pane*>()) {
            p->setOptions(now.terminal);
        }
    }
    if (now.window.backgroundOpacity != previous.window.backgroundOpacity || now.window.blur != previous.window.blur) {
        applyWindowEffects();
    }
    if (m_quick && now.quickTerminal.height != previous.quickTerminal.height) {
        setUpQuickTerminal();
    }
    m_layoutSize = QSize(); // padding may have changed
    updateChrome();
    scheduleRender();
}

void TerminalWindow::applyWindowEffects()
{
    const WindowSettings& w = m_settings.settings().window;
    const bool translucent = w.backgroundOpacity < 1.0;
    m_renderer->setTranslucent(translucent);
    // Blur needs the native window; before that, exposeEvent applies it.
    if (handle()) {
        KWindowEffects::enableBlurBehind(this, translucent && w.blur);
    }
    scheduleRender();
}

// --- quick terminal --------------------------------------------------------

void TerminalWindow::setUpQuickTerminal()
{
    using PhosphorWayland::LayerSurface;
    const QScreen* screen = QGuiApplication::primaryScreen();
    const QRect avail = screen ? screen->availableGeometry() : QRect(0, 0, 1280, 800);
    m_quickHeight = int(std::lround(avail.height() * m_settings.settings().quickTerminal.height));
    LayerSurface* surface = LayerSurface::get(this);
    surface->setLayer(LayerSurface::LayerTop);
    surface->setAnchors(LayerSurface::Anchors(LayerSurface::AnchorTop | LayerSurface::AnchorLeft
                                              | LayerSurface::AnchorRight));
    surface->setKeyboardInteractivity(LayerSurface::KeyboardInteractivityOnDemand);
    surface->setScope(QStringLiteral("afterglow-quick-terminal"));
    surface->setExclusiveZone(0);
    surface->setDesiredSize(QSize(0, m_quickHeight));
    resize(avail.width(), m_quickHeight);
}

void TerminalWindow::toggleQuick()
{
    if (!m_quick) {
        return;
    }
    const bool animate = m_settings.settings().quickTerminal.animate;
    const auto spec = springSpec<qreal>(m_clock.get(), SplitSpeed);
    const qreal hidden = -qreal(m_quickHeight);
    m_quickShown = !m_quickShown;
    if (m_quickShown) {
        if (!isVisible()) {
            PhosphorWayland::LayerSurface::get(this)->setMargins(QMargins(0, animate ? int(hidden) : 0, 0, 0));
            show();
        }
        requestActivate();
        m_slide.start(animate ? (m_slide.isAnimating() ? m_slide.value() : hidden) : 0.0, 0.0, spec);
    } else {
        if (!animate) {
            hide();
            return;
        }
        m_slide.start(m_slide.isAnimating() ? m_slide.value() : 0.0, hidden, spec);
    }
    scheduleRender();
}

void TerminalWindow::keyPressEvent(QKeyEvent* e)
{
    if (e->key() == Qt::Key_Control) {
        updateLinkHover();
    }
    if (m_controller->wantsKeyboard()) {
        // The palette and search keybinds still toggle while the chrome has the keyboard.
        const Action a = m_settings.settings().keybinds.match(e);
        if (a == Action::CommandPalette || a == Action::Search) {
            performAction(a);
            return;
        }
        m_chrome->deliver(e);
        return;
    }
    if (handleShortcut(e)) {
        return;
    }
    sendKey(e);
}

void TerminalWindow::keyReleaseEvent(QKeyEvent* e)
{
    if (e->key() == Qt::Key_Control) {
        // queryKeyboardModifiers() still reports Ctrl during its own release.
        m_hover = {};
        applyPointerShape();
        scheduleRender();
    }
    if (m_controller->wantsKeyboard()) {
        m_chrome->deliver(e);
        return;
    }
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
    // Several lines into a program without bracketed paste run one by one.
    const bool bracketed = m_focused->withTerminal([](Terminal& t) { return t.modes().bracketedPaste; });
    const QByteArray trimmed = text.trimmed();
    if (m_settings.settings().confirmUnsafePaste && !bracketed && trimmed.contains('\n')) {
        m_pendingPaste = text;
        const QString preview = QString::fromUtf8(trimmed.left(4000));
        m_controller->askPaste(preview, int(trimmed.count('\n')) + 1);
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
    if (routePointerToChrome(e, e->position(), true, false)) {
        return;
    }
    Pane* p = m_splits->paneAt((e->position() * devicePixelRatio()).toPoint());
    if (p && p != m_focused) {
        setFocusedPane(p);
    }
    if (!m_focused) {
        return;
    }
    m_pointer = e->position();
    updateLinkHover();
    if (e->button() == Qt::LeftButton && !(e->modifiers() & Qt::ControlModifier)) {
        const int tolerance = int(std::lround(4 * devicePixelRatio()));
        if (auto divider = m_splits->dividerAt((e->position() * devicePixelRatio()).toPoint(), tolerance)) {
            m_dividerDrag = divider;
            return;
        }
    }
    if (e->button() == Qt::LeftButton && (e->modifiers() & Qt::ControlModifier) && !m_hover.url.isEmpty()) {
        QDesktopServices::openUrl(QUrl(m_hover.url));
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
    if (m_dividerDrag && e->button() == Qt::LeftButton) {
        m_dividerDrag.reset();
        return;
    }
    if (routePointerToChrome(e, e->position(), false, true)) {
        return;
    }
    if (m_reportedButtons & e->button()) {
        m_reportedButtons &= ~e->button();
        const int code = mouseButtonCode(e->button());
        sendMouse(m_focused, e->position(), code, int(AfterglowVt::MouseAction::Release), e->modifiers());
        return;
    }
    if (e->button() == Qt::LeftButton && m_selectionPane) {
        m_autoScroll.stop();
        if (m_selectionPane->hasSelection()) {
            if (m_settings.settings().copyOnSelect) {
                QGuiApplication::clipboard()->setText(m_selectionPane->selectedText(), QClipboard::Selection);
            }
        } else {
            m_selectionPane->clearSelection();
            scheduleRender();
        }
        m_selectionPane = nullptr;
    }
}

void TerminalWindow::mouseMoveEvent(QMouseEvent* e)
{
    m_renderer->setMousePosition(e->position() * devicePixelRatio());
    m_pointer = e->position();
    const QPoint px = (e->position() * devicePixelRatio()).toPoint();
    if (m_dividerDrag) {
        const auto& m = m_renderer->fonts().fonts->metrics();
        const int pad = int(std::lround(m_settings.settings().window.padding * devicePixelRatio()));
        m_splits->dragDivider(*m_dividerDrag, px, std::max(1, int(std::lround(devicePixelRatio()))),
                              QSize(4 * m.cellWidth + 2 * pad, 2 * m.cellHeight + 2 * pad));
        m_layoutSize = QSize();
        scheduleRender();
        return;
    }
    updateLinkHover();
    if (auto divider = m_splits->dividerAt(px, int(std::lround(4 * devicePixelRatio())))) {
        setCursor(divider->vertical ? Qt::SplitVCursor : Qt::SplitHCursor);
    } else {
        applyPointerShape();
    }
    if (!m_selectionPane && routePointerToChrome(e, e->position(), false, false)) {
        return;
    }
    if (chromeVisible()) {
        // Keep hover state in the chrome current as the pointer leaves it.
        QMouseEvent outside(e->type(), QPointF(-1, -1), QPointF(-1, -1), e->button(), e->buttons(), e->modifiers());
        m_chrome->deliver(&outside);
    }
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
    if (routePointerToChrome(e, e->position(), false, false)) {
        return;
    }
    Pane* p = m_splits->paneAt((e->position() * devicePixelRatio()).toPoint());
    if (!p) {
        p = m_focused;
    }
    if (!p) {
        return;
    }
    // Trackpads report pixels: scroll the history by exactly that much.
    if (!e->pixelDelta().isNull()) {
        const AfterglowVt::Modes modes = p->withTerminal([](Terminal& t) { return t.modes(); });
        if (modes.mouseTracking == AfterglowVt::MouseTracking::None && !modes.altScreen) {
            const double cell = m_renderer->fonts().fonts->metrics().cellHeight;
            scrollPane(p, e->pixelDelta().y() * devicePixelRatio() / cell, false);
            return;
        }
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
    scrollPane(p, double(steps) * WheelLines);
}

} // namespace Afterglow
