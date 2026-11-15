// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TerminalWindow.h"
#include "NestedLoop.h"
#include "DockArea.h"
#include <QQuickItem>
#include "desktop/FileDrop.h"
#include "desktop/PointerShape.h"
#include "desktop/FileTransfer.h"
#include "desktop/DragDrop.h"
#include <QMessageBox>
#include "desktop/Accessibility.h"
#include <QAccessible>
#include <QDropEvent>
#include <QDragEnterEvent>
#include "PaneDialog.h"
#include "PaneLaunch.h"
#include "mux/MuxClient.h"
#include "mux/MuxSessions.h"

#include "app/NotificationCenter.h"
#include "control/Control.h"

#include "KeyMapping.h"
#include "chrome/ChromeController.h"

#include <PhosphorWayland/LayerSurface.h>

#include <KWindowEffects>
#include "chrome/ChromeLayer.h"
#include "scripting/ScriptHost.h"
#include "shaders/ShaderLibrary.h"
#include "Pane.h"

#include <AfterglowRender/FontSet.h>
#include <AfterglowRender/GpuDevice.h>
#include <AfterglowVt/Input.h>

#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QUrl>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLoggingCategory>
#include <QDBusConnection>
#include <QDrag>
#include <QMimeData>
#include <QProcess>
#include <QTemporaryFile>
#include <QFutureWatcher>
#include <QtConcurrentRun>
#include <QStandardPaths>
#include <QUuid>
#include <QDBusMessage>
#include <QInputMethod>
#include <QInputMethodEvent>
#include <QInputMethodQueryEvent>
#include <QLocale>
#include <QPlatformSurfaceEvent>
#include <QPointingDevice>
#include <qpa/qwindowsysteminterface.h>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QScreen>
#include <QSurfaceFormat>
#include <QVariantMap>

#include <utility>
#include <QStyleHints>

#include <array>
#include <cmath>
#include <memory>

Q_LOGGING_CATEGORY(lcWindow, "afterglow.window", QtInfoMsg)

namespace {
/// Milliseconds on the steady clock, with their fraction, for the times of
/// finger scrolls and of the frames that move their coast.
double steadyMs()
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
/// Logical pixels of finger travel for each card that the overview moves: a
/// step that does not depend on the font size, as kitty's
/// MAPPED_SCROLL_STEP_PIXELS for touchpads.
constexpr double OverviewFingerStep = 100;
/// A mouse event that a finger on a touchscreen makes (touchMouse).
bool fromTouch(const QPointerEvent* e)
{
    return e->pointingDevice() && e->pointingDevice()->type() == QInputDevice::DeviceType::TouchScreen;
}
/// Command palette ids from here on are script commands.
constexpr int ScriptCommandBase = 10000;
/// Context menu items that are not actions.
constexpr int MenuOpenLink = 20000;
constexpr int MenuCopyLink = 20001;
constexpr int MenuScriptBase = 30000;
/// Tab menu items.
constexpr int TabMenuRename = 40000;
constexpr int TabMenuNew = 40001;
constexpr int TabMenuMoveLeft = 40002;
constexpr int TabMenuMoveRight = 40003;
constexpr int TabMenuClose = 40004;
constexpr int TabMenuCloseOthers = 40005;
constexpr int TabMenuCloseRight = 40006;
constexpr int TabMenuDetach = 40007;
} // namespace

namespace Afterglow {

using AfterglowRender::packRgba;
using AfterglowVt::Terminal;

namespace {
QList<TerminalWindow*> s_windows;

/// Labels for at most this many distinct matches (show_hints).
constexpr std::size_t MaxHints = 1000;
/// A clipboard reply (OSC 52, or each type of an OSC 5522 read) carries at most this much.
constexpr qsizetype MaxClipboardReply = 8 * 1024 * 1024;
/// Half the PTY's input queue (32 MiB): a paste fits behind input that the
/// program has not read yet.
constexpr qsizetype MaxPaste = 16 * 1024 * 1024;

/// The same motion for another value type.
template<typename T>
PhosphorAnimation::MotionSpec<T> specFor(const PhosphorAnimation::MotionSpec<qreal>& spec)
{
    PhosphorAnimation::MotionSpec<T> out;
    out.clock = spec.clock;
    out.profile = spec.profile;
    out.retargetPolicy = spec.retargetPolicy;
    out.maxLifetimeMs = spec.maxLifetimeMs;
    return out;
}

/// Truncates UTF-8 @p bytes to at most @p limit bytes without splitting a character.
QByteArray truncatedUtf8(QByteArray bytes, qsizetype limit)
{
    if (bytes.size() <= limit) return bytes;
    qsizetype end = limit;
    while (end > 0 && (std::uint8_t(bytes[end]) & 0xC0) == 0x80) --end;
    bytes.truncate(end);
    return bytes;
}

/// The key's place on the keyboard, the same for its press, repeats and release.
quint32 physicalKeyOf(const QKeyEvent* e)
{
    return e->nativeScanCode() ? e->nativeScanCode() : quint32(e->key());
}

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

namespace {
quint32 s_nextWindowId = 1;

/// Adds what a project saves of session pane @p pane to @p layout, for the
/// daemon to keep, within MaxLayoutValueBytes: past them, a command saves
/// as the shell, and a title as the program's. @p commands: false for a
/// session of another host, whose commands a project would run here.
void addSessionValues(AfterglowMux::Layout& layout, const Pane& pane, bool commands, std::size_t& used)
{
    AfterglowMux::PaneValues values;
    // At a character's end: the daemon takes no longer title.
    QString title = pane.userTitle();
    while (std::size_t(title.toUtf8().size()) > AfterglowMux::MaxPaneTitle) title.chop(1);
    values.title = title.toStdString();
    if (commands) values.command = sessionCommand(pane.launchCommand());
    if (used + values.bytes() > AfterglowMux::MaxLayoutValueBytes) values.command.clear();
    if (used + values.bytes() > AfterglowMux::MaxLayoutValueBytes) values.title.clear();
    if (values.empty()) return;
    used += values.bytes();
    layout.values.emplace(pane.sessionPaneId(), std::move(values));
}

/// Gives session pane @p pane the @p values that the daemon kept, with its
/// command for a session of this computer (@p commands).
void applySessionValues(Pane& pane, const AfterglowMux::PaneValues& values, bool commands)
{
    QStringList command;
    if (commands) for (const std::string& word : values.command) command << QString::fromStdString(word);
    pane.setLaunchCommand(command);
    pane.setUserTitle(QString::fromStdString(values.title));
}
} // namespace

TerminalWindow::TerminalWindow(Services& services, const WindowOptions& options)
    : m_device(services.device)
    , m_settings(services.settings)
    , m_shaders(services.shaders)
    , m_shaderControls(services.shaders)
    , m_scripts(services.scripts)
    , m_audio(services.audio)
    , m_id(s_nextWindowId++)
{
    installTerminalAccessibility();
    // Only the window with the keyboard has the focus to tell of: an event
    // from another would take the focus state from the one that has it.
    // focusInEvent tells of the pane when the window gets the keyboard, and
    // the chrome's handler below when a prompt lets it go.
    connect(this, &TerminalWindow::focusedPaneChanged, this, [this](Pane* pane) {
        if (pane && m_windowFocused && !m_controller->wantsKeyboard() && QAccessible::isActive()) { QAccessibleEvent event(pane, QAccessible::Focus); QAccessible::updateAccessibility(&event); }
    });
    connect(&m_device, &AfterglowRender::GpuDevice::audioChanged, this, [this] {
        if (m_audioCapturing && isExposed()) scheduleRender();
    });
    connect(this, &QWindow::visibilityChanged, this, &TerminalWindow::updateAudioCapture);
    m_quick = options.quick;
    AfterglowRender::GpuDevice& device = m_device;
    setSurfaceType(QSurface::VulkanSurface);
    setVulkanInstance(device.vulkanInstance());
    // Qt marks a surface without alpha in its format as opaque, and the
    // compositor then stops drawing what is behind it: a translucent
    // background shows stale pixels, which flash on repaints such as focus
    // changes. Asking for alpha always also lets translucency change live.
    QSurfaceFormat surfaceFormat = requestedFormat();
    surfaceFormat.setAlphaBufferSize(8);
    setFormat(surfaceFormat);
    setTitle(QStringLiteral("afterglow"));
    m_renderer = std::make_unique<AfterglowRender::WindowRenderer>(device, this);
    // No pointer over the window until it moves there (the shader contract's "outside").
    m_renderer->setMousePosition(QPointF(-1, -1));

    const auto& m = m_renderer->fonts().fonts->metrics();
    const double dpr = devicePixelRatio();
    const WindowSettings& ws = m_settings.settings().window;
    const int pad = int(std::lround(ws.padding * dpr));
    // The rows asked for, below a tab bar that always shows.
    const QSize initial(ws.initialColumns * m.cellWidth + 2 * pad,
                        ws.initialRows * m.cellHeight + 2 * pad + tabBarHeight(1));
    resize(int(std::lround(initial.width() / dpr)), int(std::lround(initial.height() / dpr)));

    m_clock = std::make_unique<FrameClock>(this);
    m_shaderControls.setClock(m_clock.get());

    m_controller = new ChromeController(this);
    connect(m_controller, &ChromeController::scriptDialogAnswered, this, &TerminalWindow::finishScriptDialog);
    m_keyModeClock.start();
    m_keyModeTimer.setSingleShot(true);
    connect(&m_keyModeTimer, &QTimer::timeout, this, [this] {
        KeyModes::expire(m_keyMode, m_keyModeClock.elapsed());
        updateKeyMode();
    });
    m_chrome = new ChromeLayer(device, this, services.chrome, m_controller);
    connect(m_chrome, &ChromeLayer::updateRequested, this, &TerminalWindow::scheduleRender);
    connect(m_controller, &ChromeController::activateTabRequested, this, &TerminalWindow::switchTab);
    connect(m_controller, &ChromeController::closeTabRequested, this, [this](int index) { requestCloseTabs({index}); });
    connect(m_controller, &ChromeController::confirmPromptChanged, this, &TerminalWindow::scheduleRender);
    connect(m_controller, &ChromeController::confirmAnswered, this, [this](bool accepted) {
        m_pendingConfirmPanes.clear();
        const std::function<void()> close = std::exchange(m_pendingConfirm, {});
        if (accepted && close) {
            close();
        }
    });
    connect(m_controller, &ChromeController::detachTabRequested, this, &TerminalWindow::detachTab);
    connect(m_controller, &ChromeController::moveTabRequested, this, &TerminalWindow::moveTab);
    connect(m_controller, &ChromeController::renameTabRequested, this, &TerminalWindow::renameTab);
    connect(m_controller, &ChromeController::tabMenuRequested, this, &TerminalWindow::openTabMenu);
    // Through the action, which tells the user when the tab cannot open.
    connect(m_controller, &ChromeController::newTabRequested, this, [this] { performAction(Action::NewTab); });
    connect(m_controller, &ChromeController::actionRequested, this, [this](int id) {
        if (id >= ScriptCommandBase) {
            if (id - ScriptCommandBase < m_paletteCommands.size()) {
                runScriptCommand(m_paletteCommands[id - ScriptCommandBase], m_focused, QStringLiteral("palette"));
            }
            return;
        }
        performAction(Action(id));
    });
    connect(&m_scripts, &ScriptHost::commandsChanged, this, &TerminalWindow::updatePaletteActions);
    connect(m_controller, &ChromeController::historyRequested, this, [this](double position) {
        // Hint labels sit on the rows the view had, as in scrollPane.
        Pane* target = inputPane(m_focused);
        if (target && m_urlMode.active && m_urlMode.pane == target) stopUrlMode();
        if (target) target->scrollHistory(position);
        scheduleRender();
    });
    connect(m_controller, &ChromeController::searchRequested, this, &TerminalWindow::runSearch);
    connect(m_controller, &ChromeController::searchOpenChanged, this, [this] {
        if (!m_controller->searchOpen() && m_focused) {
            inputPane(m_focused)->clearSearch();
            m_controller->setSearchStatus({});
        }
        scheduleRender();
    });
    connect(m_controller, &ChromeController::paletteOpenChanged, this, &TerminalWindow::scheduleRender);
    connect(m_controller, &ChromeController::contextMenuChanged, this, &TerminalWindow::scheduleRender);
    connect(m_controller, &ChromeController::titlePromptChanged, this, &TerminalWindow::scheduleRender);
    // The window stays the focus object while the chrome takes the keyboard,
    // so the input method hears of the switch only when asked to query again.
    for (auto changed : {&ChromeController::paletteOpenChanged, &ChromeController::searchOpenChanged,
                         &ChromeController::contextMenuChanged, &ChromeController::titlePromptChanged,
                         &ChromeController::confirmPromptChanged, &ChromeController::pasteConfirmChanged,
                         &ChromeController::clipboardPromptChanged, &ChromeController::scriptDialogChanged}) {
        connect(m_controller, changed, this, [this] {
            refreshInputMethod();
            const bool held = m_controller->wantsKeyboard();
            const bool wasHeld = std::exchange(m_chromeHeldKeyboard, held);
            if (!wasHeld && held) {
                // As for a mode: a composition in progress would stay drawn
                // on the pane, and the event that clears it now goes to the chrome.
                if (m_focused) inputPane(m_focused)->setPreedit({}, 0);
                if (isActive()) QGuiApplication::inputMethod()->reset();
            }
            if (wasHeld && !held && m_focused && m_windowFocused && QAccessible::isActive()) {
                QAccessibleEvent event(m_focused, QAccessible::Focus);
                QAccessible::updateAccessibility(&event);
            }
            // An overlay that opens or closes under a pointer that does not
            // move changes what the pointer is on. Queued: the chrome's
            // bindings, which wantsPointer reads, hear of the change after
            // this slot.
            QMetaObject::invokeMethod(this, &TerminalWindow::applyPointerShape, Qt::QueuedConnection);
        });
    }
    connect(m_controller, &ChromeController::titleAnswered, this, [this](const QString& title) {
        if (m_titlePane) {
            m_titlePane->setUserTitle(title);
        } else {
            // The tab may have closed while the prompt was open.
            // tabsChanged updates the chrome, and only for a new title.
            for (auto& tab : m_tabs) {
                if (tab->id == m_titleTab && tab->title != title) {
                    tab->title = title;
                    Q_EMIT tabsChanged();
                }
            }
        }
        m_titlePane = nullptr;
        m_titleTab = 0;
    });
    connect(m_controller, &ChromeController::menuItemActivated, this, [this](int id) {
        // The items act on the pane the menu was opened on; when it has
        // gone, the pane items go with it.
        const QPointer<Pane> pane = std::exchange(m_menuPane, {});
        const bool paneAlive = pane && tabOf(pane) >= 0;
        if (id == MenuOpenLink) {
            openLink(m_menuLink);
        } else if (id == MenuCopyLink) {
            QGuiApplication::clipboard()->setText(m_menuLink);
        } else if (id >= TabMenuRename) {
            runTabMenuItem(id);
        } else if (id >= MenuScriptBase) {
            if (id - MenuScriptBase < m_menuCommands.size() && paneAlive) {
                runScriptCommand(m_menuCommands[id - MenuScriptBase], pane, QStringLiteral("menu"));
            }
        } else if (paneAlive) {
            actOnPane(pane, Action(id));
        } else {
            constexpr std::array windowItems{Action::CommandPalette, Action::RenameTab, Action::NewTab, Action::CloseTab,
                                             Action::NewWindow, Action::CloseWindow, Action::OpenConfig, Action::ReloadConfig};
            if (std::find(windowItems.begin(), windowItems.end(), Action(id)) != windowItems.end()) performAction(Action(id));
        }
    });
    connect(m_controller, &ChromeController::pasteConfirmChanged, this, &TerminalWindow::scheduleRender);
    connect(m_controller, &ChromeController::clipboardPromptChanged, this, &TerminalWindow::scheduleRender);
    connect(m_controller, &ChromeController::clipboardAnswered, this, [this](int answer) {
        auto pending = std::exchange(m_pendingClipboardRead, std::nullopt);
        if (!pending || !pending->pane) {
            return;
        }
        const AfterglowVt::ClipboardRequest& r = pending->request;
        if (pending->write) {
            if (answer != 0) writeClipboard(pending->pane, r);
            else pending->pane->sendReply(r.refusal("EPERM"));
            return;
        }
        if (answer == 0) {
            pending->pane->sendReply(r.refusal("EPERM"));
            return;
        }
        if (answer == 2 && !r.password.empty() && !r.name.empty()) {
            m_clipboardGrants[pending->pane][QByteArray::fromStdString(r.password)] = ClipboardGrant{};
        }
        answerClipboardRead(pending->pane, r);
    });
    connect(m_controller, &ChromeController::pasteAnswered, this, [this](bool paste) {
        const QByteArray text = std::exchange(m_pendingPaste, {});
        const QPointer<Pane> target = std::exchange(m_pendingPastePane, {});
        // An overlay's program gets it while the overlay still runs.
        if (paste && takesInput(target)) {
            // Encoded outside the terminal's lock: a paste can be 16 MiB,
            // and the reader thread waits on the lock.
            const AfterglowVt::Modes modes = target->withTerminal([](Terminal& t) { return t.modes(); });
            target->sendInput(AfterglowVt::encodePaste(std::string_view(text.constData(), std::size_t(text.size())), modes));
        }
    });
    connect(this, &TerminalWindow::tabsChanged, this, &TerminalWindow::updateChrome);
    updatePaletteActions();

    connect(&m_shaders, &ShaderLibrary::changed, this, &TerminalWindow::applyShaders);
    applyShaders();

    if (m_quick) {
        setUpQuickTerminal();
    }
    applyWindowEffects();

    m_fixedTitle = options.title;
    m_holdNextPane = options.hold;
    if (options.session) {
        m_session = options.session;
        m_sessionLayoutTimer.setSingleShot(true);
        m_sessionLayoutTimer.setInterval(100);
        connect(&m_sessionLayoutTimer, &QTimer::timeout, this, &TerminalWindow::sendSessionLayout);
        // Tabs that open, close, move or take a title, and the focus.
        connect(this, &TerminalWindow::tabsChanged, this, &TerminalWindow::scheduleSessionLayout);
        connect(this, &TerminalWindow::focusedPaneChanged, this, &TerminalWindow::scheduleSessionLayout);
        // A move goes with the next layout, for `afterglow ctl watch` of the session.
        connect(this, &TerminalWindow::paneMoved, this, [this](Pane* pane) {
            if (!pane || !pane->isSessionPane() || m_movedSessionPanes.size() >= AfterglowMux::MaxLayoutPanes) return;
            m_movedSessionPanes.push_back(pane->sessionPaneId());
            scheduleSessionLayout();
        });
        // The session follows the settings of the window that shows it, and
        // one of this computer takes its variables.
        m_session->sendSettings(sessionSettings(m_settings.settings(), !m_session->remote()));
        restoreSession();
    } else if (!options.empty) newTab(options.directoryGiven ? options.workingDirectory : startDirectory(options.workingDirectory), options.command, options.environment);
    updateTitle();
    // QWindow tells nobody of a new title but its own signal.
    connect(this, &QWindow::windowTitleChanged, this, [this] {
        if (QAccessible::isActive()) { QAccessibleEvent event(this, QAccessible::NameChanged); QAccessible::updateAccessibility(&event); }
    });

    connect(&m_device, &AfterglowRender::GpuDevice::fontsChanged, this, &TerminalWindow::scheduleRender);
    connect(&m_settings, &SettingsStore::changed, this, &TerminalWindow::applySettings);
    // What the config file could not use: once for the process, not per window.
    connect(&m_settings, &SettingsStore::problemsChanged, this,
            [](const QStringList& problems) { reportProblems(ProblemSource::Config, problems); });
    reportProblems(ProblemSource::Config, m_settings.problems());
    // Shaders.params entries that apply nowhere, packs that do not draw.
    connect(&m_shaders, &ShaderLibrary::problemsChanged, this,
            [](const QStringList& problems) { reportProblems(ProblemSource::Shaders, problems); });
    reportProblems(ProblemSource::Shaders, m_shaders.problems());

    m_captureTimer.setSingleShot(true);
    connect(&m_captureTimer, &QTimer::timeout, this, &TerminalWindow::expireCaptures);
    m_textBlinkTimer.setInterval(m_settings.settings().textBlinkInterval);
    connect(&m_textBlinkTimer, &QTimer::timeout, this, [this] {
        m_textBlinkOn = !m_textBlinkOn;
        scheduleRender();
    });
    connect(this, &QWindow::windowStateChanged, this, [this] {
        updatePaneVisibility();
        // A minimized window may stay exposed, but it shows nothing.
        if (windowState() == Qt::WindowMinimized) abortCaptures(QStringLiteral("the window is not on screen"));
    });
    m_blinkTimer.setInterval(m_settings.settings().cursorBlinkInterval);
    connect(&m_blinkTimer, &QTimer::timeout, this, [this] {
        m_blinkOn = !m_blinkOn;
        const Pane* shown = inputPane(m_focused);
        if (shown && scriptOverrides(m_focused).value(QStringLiteral("cursorBlink"), shown->snapshot().cursorBlink).toBool() && (shown->snapshot().cursorVisible || !shown->snapshot().extraCursors.empty())) {
            scheduleRender();
        } else {
            // A steady cursor needs no ticks; renderNow starts them again
            // when the focused pane's cursor blinks.
            m_blinkTimer.stop();
            m_blinkOn = true;
        }
    });

    m_autoScroll.setInterval(m_settings.settings().mouse.autoScrollInterval);
    connect(&m_autoScroll, &QTimer::timeout, this, &TerminalWindow::updateSelectionDrag);
    m_momentum.setFriction(m_settings.settings().mouse.momentumScroll);
    m_holdRetry.setSingleShot(true);
    m_holdRetry.setTimerType(Qt::PreciseTimer);
    connect(&m_holdRetry, &QTimer::timeout, this, &TerminalWindow::scheduleRender);

    s_windows.append(this);
}

TerminalWindow::~TerminalWindow()
{
    // The panes of a session that close with the window: the daemon keeps
    // their tabs, and Mux.onClose says what goes, unless Afterglow quits.
    // Before the prompts go: a clipboard read of a session that runs on gets
    // the denial.
    if (m_session) {
        sendSessionLayout();
        const MuxRelease release = windowCloseRelease();
        for (Pane* pane : panes()) pane->setSessionRelease(release);
    }
    // Only the panes still in a tab close with the window: a closed tab's
    // panes were announced when it left, and may still wait for their
    // deferred delete.
    const QList<Pane*> live = findChildren<Pane*>(QString(), Qt::FindDirectChildrenOnly);
    for (Pane* pane : panes()) Q_EMIT paneClosing(pane->id(), tabIdOf(pane), id());
    dropClipboardPrompt();
    if (m_hintCancel) *m_hintCancel = true;
    finishScriptDialog(false, {});
    s_windows.removeAll(this);
    publishLauncherStatus();
    // The panes' docks too, while this window's object is still there to be
    // their parent: the panes that are left are freed only after this body.
    for (const auto& dock : m_docks) releaseDockAccessibility(dock.get());
    for (Pane* pane : live) for (const auto& dock : pane->docks()) releaseDockAccessibility(dock.get());
    // Their renderer goes below: a client hears why.
    abortCaptures(QStringLiteral("the window closed"));
    // The chrome renders on the device through this window; it goes first.
    delete m_chrome;
    m_chrome = nullptr;
    m_renderer.reset();
    // ~QWindow hides a window that still shows, and its visibilityChanged
    // would reach this window's slots after their members are gone.
    QObject::disconnect(this, nullptr, this, nullptr);
}

QList<TerminalWindow*> TerminalWindow::windows()
{
    return s_windows;
}

// --- panes -----------------------------------------------------------------

void TerminalWindow::bindPane(Pane* pane)
{
    // The program's drag takes the keyboard, as a selection's drag does.
    connect(pane, &Pane::programDragStarting, this, &TerminalWindow::releaseSentKeys);
    connect(pane, &Pane::inputRejected, this, [this, pane] {
        NotificationCenter::instance().showNotice(pane, tr("Terminal input rejected"),
            tr("The program closed or its input queue is full."));
    });
    connect(pane, &QObject::destroyed, this, [this, pane] {
        // The tables keyed by the pane that closePane leaves: a new pane at
        // the same address must not inherit an entry (a tab close deletes
        // panes without closePane).
        m_notifyOnFinish.remove(pane);
        m_motion.erase(pane);
        m_growFrom.erase(pane);
        m_historyRows.erase(pane);
        m_clipboardGrants.erase(pane);
        m_paneOpening.remove(pane);
        m_paneContext.remove(pane);
        m_paneOrigin.remove(pane);
        m_panePulse.remove(pane);
        m_overlayFades.erase(pane);
        if (m_statusViews.erase(pane) > 0) {
            publishLauncherStatus();
        }
    });
    connect(pane, &Pane::flashRequested, this, [this, guard = QPointer<Pane>(pane)] {
        if (guard) {
            flashPane(guard);
        }
    });
    // The events below can arrive after the pane's tab or overlay closed:
    // a bell, a notice or a status change then belongs to a program that
    // is ending, and nobody sees its pane.
    connect(pane, &Pane::statusChanged, this, [this, guard = QPointer<Pane>(pane)] {
        if (guard && takesInput(guard)) {
            paneStatusChanged(guard);
        }
    });
    connect(pane, &Pane::searchStatusChanged, this, [this, pane](const QString& status) {
        if (pane == inputPane(m_focused) && m_controller->searchOpen()) m_controller->setSearchStatus(status);
        scheduleRender();
    });
    connect(pane, &Pane::contentChanged, this, [this, pane] {
        // Hints point at text that just changed: they go (show_hints).
        // A frame of an animated image changes no text: they stay. Read only
        // for the panes that need it: it takes the terminal's lock.
        const bool urlPane = m_urlMode.active && m_urlMode.generation && pane == m_urlMode.pane;
        const bool isInputPane = pane == inputPane(m_focused);
        const std::uint64_t text = urlPane || isInputPane ? pane->withTerminal([](Terminal& t) { return t.textGeneration(); }) : 0;
        if (urlPane && text != m_urlMode.generation) stopUrlMode();
        // The input method reads the text again once per frame, not per
        // output batch, and only when the text changed.
        if (isInputPane && (pane != m_inputMethodPane || text != m_inputMethodText)) {
            m_inputMethodPane = pane;
            m_inputMethodText = text;
            m_inputMethodDirty = true;
        }
        // A pane nobody can see is drawn when it shows: its tab becoming
        // current or the overview opening asks for that frame.
        if (paneDrawn(pane)) scheduleRender();
    });
    connect(pane, &Pane::shaderSignalsChanged, this, [this, pane] {
        if (paneDrawn(pane)) scheduleRender();
    });
    connect(pane, &Pane::contextUpdated, this, [this, pane] {
        if (pane == m_focused) {
            updateTitle();
        }
        updateChrome();
        scheduleRender();
    });
    connect(pane, &Pane::commandEnded, this,
            [this, guard = QPointer<Pane>(pane)](const QString& command, int exitCode, qint64 ms) {
        if (!guard || !m_notifyOnFinish.remove(guard.data())) {
            return;
        }
        const QString what = command.isEmpty() ? tr("The command") : command;
        const QString outcome = exitCode == 0 ? tr("finished")
            : exitCode < 0                    ? tr("ended")
                                              : tr("failed (exit %1)").arg(exitCode);
        const QString after = ms < 1000 ? tr("%1 ms").arg(ms) : tr("%1 s").arg(double(ms) / 1000.0, 0, 'f', 1);
        NotificationCenter::instance().showNotice(guard, what, tr("%1 after %2").arg(outcome, after));
    });
    connect(pane, &Pane::titleUpdated, this, [this, pane] {
        // An overlay's title shows as its host's (shownTitle): the host is
        // the pane in the tab, and the one assistive technology knows.
        Pane* const host = hostOf(pane);
        if (host == m_focused) {
            updateTitle();
        }
        if (QAccessible::isActive()) { QAccessibleEvent event(host, QAccessible::NameChanged); QAccessible::updateAccessibility(&event); }
        // Only the pane a tab shows names that tab. Every title shows as a
        // split label, which only renderNow builds: the tab bar's repaint
        // does not bring a frame while the bar is hidden.
        const int index = tabOf(host);
        const Pane* shown = index < 0 ? nullptr : index == m_currentTab ? m_focused.data() : m_tabs[std::size_t(index)]->focused.data();
        if (host == shown && m_tabs[std::size_t(index)]->title.isEmpty()) Q_EMIT tabsChanged();
        if (paneDrawn(pane)) scheduleRender();
    });
    // A project saves the title: the daemon keeps it for the next window.
    connect(pane, &Pane::userTitleChanged, this, &TerminalWindow::scheduleSessionLayout);
    connect(pane, &Pane::closed, this, [this, guard = QPointer<Pane>(pane)] {
        // An overlay's program ending ends the overlay (PaneOverlay), not a pane.
        if (guard && !guard->overlayHost()) {
            closePane(guard);
        }
    });
    connect(pane, &Pane::clipboardWriteRequested, this, [this, guard = QPointer<Pane>(pane)](const QString& text, bool primary) {
        // As for clipboardRequested: a write that the pane sent before its
        // tab or its overlay closed would ask for a program that is ending.
        if (!guard || !takesInput(guard)) return;
        AfterglowVt::ClipboardRequest write;
        write.kind = AfterglowVt::ClipboardRequest::Kind::Write;
        write.osc52 = true;
        write.primary = primary;
        write.data = {{"text/plain;charset=utf-8", text.toStdString()}};
        clipboardWrite(guard, write);
    });
    connect(pane, &Pane::bellRang, this, [this, guard = QPointer<Pane>(pane)] {
        if (guard && takesInput(guard)) {
            ringBell(guard);
        }
    });
    connect(pane, &Pane::notificationRequested, this, [this, guard = QPointer<Pane>(pane)](const QString& title, const QString& body) {
        // Only when the user is not looking at that pane (or the overlay on it) already.
        if (guard && takesInput(guard) && m_settings.settings().notifications && (!isActive() || hostOf(guard) != m_focused)) {
            NotificationCenter::instance().showSimple(guard, title.isEmpty() ? guard->title() : title, body);
        }
    });
    connect(pane, &Pane::clipboardRequested, this,
            [this, guard = QPointer<Pane>(pane)](const AfterglowVt::ClipboardRequest& request) {
                if (!guard) return;
                if (takesInput(guard)) {
                    clipboardRequest(guard, request);
                    return;
                }
                // A request that the pane sent before its tab or its overlay
                // closed: a prompt now would answer a program that is ending.
                // A session that the close leaves running still waits for an
                // answer.
                if (!guard->closeEndsProgram()) guard->sendReply(request.refusal("EPERM"));
            });
    connect(pane, &Pane::desktopNotificationRequested, this,
            [this, guard = QPointer<Pane>(pane)](const AfterglowVt::NotificationRequest& request) {
                if (!guard) {
                    return;
                }
                using Kind = AfterglowVt::NotificationRequest::Kind;
                using Occasion = AfterglowVt::NotificationRequest::Occasion;
                // Only a new notice waits for a pane that shows: a close
                // still takes down what the program showed before its tab
                // or overlay closed, and a query still gets its answer.
                if (request.kind == Kind::Show) {
                    if (!takesInput(guard) || !m_settings.settings().notifications) {
                        return;
                    }
                    // The program's occasion: when the user is not on the pane, or cannot see it.
                    // An overlay's program is on its host, which shows it.
                    Pane* const place = hostOf(guard);
                    const bool focused = isActive() && place == m_focused;
                    const bool visible = paneSeen(place, overviewCards());
                    if ((request.occasion == Occasion::Unfocused && focused)
                        || (request.occasion == Occasion::Invisible && visible)) {
                        return;
                    }
                }
                NotificationCenter::instance().handle(guard, request);
            });
    pane->setDragAnswerWait(m_settings.settings().dragAnswerWait);
    connect(pane, &Pane::terminalProtocolRequested, this, [this, guard = QPointer<Pane>(pane)](int code, const QByteArray& payload) {
        if (!guard) return;
        // A reset reaches only the handlers a program started.
        if (code == 0) {
            if (guard->hasFileTransfers()) guard->fileTransfers()->reset();
            if (guard->hasDragDrop()) guard->dragDrop()->reset();
        }
        else if (code == 72) guard->dragDrop()->handle(payload);
        else if (code == 5113) {
            // The transfer worker starts with the first OSC 5113 message, not
            // with the pane. It gets the password and the delta limit once;
            // applySettings keeps them current.
            const bool created = !guard->hasFileTransfers();
            auto* files = guard->fileTransfers();
            if (created) {
                files->setPassword(m_settings.settings().fileTransferPassword);
                files->setDeltaLimit(m_settings.settings().fileTransferDeltaLimit);
            }
            files->handle(payload);
        }
    });
    connect(pane, &Pane::terminalProtocolLost, this, [guard = QPointer<Pane>(pane)](DragDrop::Lost lost, bool reset) {
        if (!guard) return;
        // As a reset, an overflow reaches only the handlers a program started.
        // A reset in the gap ended the transfers with no reply, and the
        // transfers that the program started after it are unknown here.
        if (guard->hasFileTransfers()) {
            if (reset) guard->fileTransfers()->reset();
            else guard->fileTransfers()->abort();
        }
        // Lost packets can be the start of the pane's first message: the rest
        // must not reach a new handler as a message of its own.
        if (lost == DragDrop::Lost::Packets || guard->hasDragDrop()) guard->dragDrop()->abort(lost, reset);
    });
    connect(pane, &Pane::fileTransferPermissionRequested, this,
            [this, pane = QPointer<Pane>(pane)](const QString& token, bool writing, const QStringList& paths, int total, bool othersRefused) {
        if (!pane) return;
        // Asked by the transfer itself, so the worker exists already.
        const QPointer<FileTransfer> guard = pane->fileTransfers();
        // A request that the pane sent before its tab or its overlay closed:
        // nobody is left to answer it.
        if (!takesInput(pane)) {
            guard->approve(token, false);
            return;
        }
        // The dialog keeps the paths that it shows: a Yes approves those, and
        // the files that come later get a dialog of their own, unless the
        // worker refuses them.
        const FileTransfer::Question question = FileTransfer::question(writing, paths, total, othersRefused, clipboardAsker(pane));
        auto* dialog = new QMessageBox(QMessageBox::Question, tr("File transfer"), question.text, QMessageBox::Yes | QMessageBox::No);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        // A native message box (Plasma's) is a second box that this one
        // opens later, and that adds its buttons again each time it shows:
        // keepDialogOverPane moves a native dialog by a hide and a show.
        dialog->setOption(QMessageBox::Option::DontUseNativeDialog);
        dialog->setTextFormat(Qt::PlainText);
        dialog->setDefaultButton(QMessageBox::No);
        if (!question.detail.isEmpty()) dialog->setDetailedText(question.detail);
        // This window closes. A pane that moved to another window still asks, over that one.
        keepDialogOverPane(dialog, pane);
        connect(dialog, &QDialog::finished, dialog, [guard, token](int result) {
            if (guard) guard->approve(token, result == QMessageBox::Yes);
        });
        if (guard) {
            connect(guard, &QObject::destroyed, dialog, &QDialog::reject);
            connect(guard, &FileTransfer::sessionClosed, dialog, [dialog, token](const QString& closed) { if (closed == token) dialog->reject(); });
        }
        dialog->open();
    });
    // Direct: the window changes the dialog before it shows.
    connect(pane, &Pane::dialogOpened, this, [this, guard = QPointer<Pane>(pane)](QDialog* dialog) {
        if (guard && m_quick) quickDialogOpened(guard, dialog);
    });
    connect(pane, &Pane::pointerShapeRequested, this, [this, guard = QPointer<Pane>(pane)](const QString& shape) {
        if (guard) {
            motion(guard).pointerShape = shape;
            applyPointerShape();
        }
    });
    // An overlay over the pane, when the pane came from another window with it.
    bindOverlay(pane);
}

Pane* TerminalWindow::createPane(const QString& workingDirectory, const QRect& area, const QStringList& command, QString* failure, const QVariantMap& environment, bool fallback)
{
    // Each tab and split of a session's window is the session's: keys, the
    // menu, scripts and afterglow ctl all come here.
    if (m_session) return createSessionLaunchPane(workingDirectory, area, command, failure, environment);
    auto* pane = new Pane(m_settings.settings().terminal, this);
    bindPane(pane);
    applySearchColors(pane);
    pane->setLaunch(workingDirectory, command);
    AfterglowPty::SpawnOptions opts = prepareLaunch(m_settings.settings(), workingDirectory, command, environment);
    pane->setScrollPolicy(m_settings.settings().scrollOnOutput, m_settings.settings().scrollOnInput);
    // Before the program starts: a quick one may exit at once.
    pane->setHold(std::exchange(m_holdNextPane, false));
    pane->setHoldOnFailure(fallback);
    pane->setLaunchedProgram(launchedProgramName(command));
    const auto& m = m_renderer->fonts().fonts->metrics();
    const QRect content = contentRect(area);
    const int cols = std::max(2, content.width() / m.cellWidth);
    const int rows = std::max(1, content.height() / m.cellHeight);
    QString error;
    if (!pane->start(std::move(opts), m_settings.settings().term, cols, rows, m.cellWidth, m.cellHeight, &error)) {
        qCCritical(lcWindow) << "failed to start the shell:" << error;
        if (failure) *failure = error;
        if (fallback) {
            // A project must remain inspectable even if its configured shell
            // fails: a plain sh, in the pane's directory (the project checked
            // that it is one), with the launch's environment and the ssh
            // helper as any pane has them.
            AfterglowPty::SpawnOptions rescue = prepareLaunch(m_settings.settings(), workingDirectory, {QStringLiteral("/bin/sh")}, environment);
            pane->setLaunchedProgram({});
            pane->setHold(true);
            if (!pane->start(std::move(rescue), m_settings.settings().term, cols, rows, m.cellWidth, m.cellHeight, &error))
                pane->holdStartFailure();
        } else {
            delete pane;
            return nullptr;
        }
    }
    Q_EMIT paneCreated(pane);
    return pane;
}

Pane* TerminalWindow::createSessionPane(const std::shared_ptr<MuxClient>& session, std::uint32_t id)
{
    // The program runs under the daemon, which holds nothing open: --hold is
    // for a program of the window, and must not go to the next pane.
    m_holdNextPane = false;
    auto* pane = new Pane(m_settings.settings().terminal, this);
    bindPane(pane);
    applySearchColors(pane);
    pane->setScrollPolicy(m_settings.settings().scrollOnOutput, m_settings.settings().scrollOnInput);
    // The size goes to the daemon with the first layout of the pane.
    pane->showSession(session, id);
    Q_EMIT paneCreated(pane);
    return pane;
}

Pane* TerminalWindow::createSessionLaunchPane(const QString& workingDirectory, const QRect& area, const QStringList& command,
                                              QString* failure, const QVariantMap& environment)
{
    // Its program started already, in the daemon.
    if (const auto placing = std::exchange(m_placingPane, std::nullopt)) return createSessionPane(m_session, *placing);
    if (m_session->remote()) {
        // The host's login shell, or the command, in the session's
        // environment there: this computer's settings name its own paths.
        // It starts where the focused pane's program is, there.
        AfterglowMux::Launch launch;
        for (const QString& word : command) launch.argv.push_back(word.toStdString());
        if (m_focused && m_focused->isSessionPane()) launch.workingDirectory = m_session->directory(m_focused->sessionPaneId()).toStdString();
        launch.launchedProgram = launchedProgramName(command).toStdString();
        const auto& m = m_renderer->fonts().fonts->metrics();
        const QRect content = contentRect(area);
        QString error;
        const auto id = m_session->newPane(launch,
                                           {std::max(2, content.width() / m.cellWidth), std::max(1, content.height() / m.cellHeight),
                                            m.cellWidth, m.cellHeight},
                                           &error);
        if (!id) {
            if (failure) *failure = error;
            return nullptr;
        }
        return createSessionPane(m_session, *id);
    }
    // The program starts as a pane of the window would, with this window's
    // settings, but in the session's environment: the daemon applies what
    // the launch sets and unsets to it.
    AfterglowPty::SpawnOptions opts = prepareLaunch(m_settings.settings(), workingDirectory, command, environment);
    offerTerminalEnvironment(opts, m_settings.settings().term);
    AfterglowMux::Launch launch;
    launch.argv = std::move(opts.argv);
    launch.workingDirectory = std::move(opts.workingDirectory);
    launch.environment = std::move(opts.environment);
    launch.unset = std::move(opts.unsetEnvironment);
    launch.launchedProgram = launchedProgramName(command).toStdString();
    launch.command = sessionCommand(command);
    const auto& m = m_renderer->fonts().fonts->metrics();
    const QRect content = contentRect(area);
    const AfterglowMux::PaneSize size{std::max(2, content.width() / m.cellWidth), std::max(1, content.height() / m.cellHeight), m.cellWidth,
                                      m.cellHeight};
    QString error;
    const auto id = m_session->newPane(launch, size, &error);
    if (!id) {
        qCCritical(lcWindow) << "the session cannot start a pane:" << error;
        if (failure) *failure = error;
        return nullptr;
    }
    Pane* pane = createSessionPane(m_session, *id);
    pane->setLaunch(workingDirectory, command);
    return pane;
}

void TerminalWindow::restoreSession()
{
    const AfterglowMux::Layout& layout = m_session->layout();
    for (const AfterglowMux::LayoutTab& saved : layout.tabs) {
        // The tree in preorder, as SplitTree::restore takes it: nested.
        std::size_t at = 0;
        std::function<QJsonObject()> nested = [&]() -> QJsonObject {
            const AfterglowMux::LayoutNode& node = saved.tree[at++];
            if (node.pane != 0) return {{QStringLiteral("pane"), QJsonObject{{QStringLiteral("id"), qint64(node.pane)}}}};
            QJsonObject first = nested();
            QJsonObject second = nested();
            return {{QStringLiteral("direction"), node.down ? QStringLiteral("down") : QStringLiteral("right")},
                    {QStringLiteral("ratio"), node.ratio},
                    {QStringLiteral("first"), first},
                    {QStringLiteral("second"), second}};
        };
        auto tab = std::make_unique<Tab>();
        tab->title = QString::fromStdString(saved.title);
        Pane* zoomed = nullptr;
        tab->splits = SplitTree::restore(nested(), [&](const QJsonObject& data) {
            const auto id = std::uint32_t(data.value(QStringLiteral("id")).toInteger());
            Pane* pane = createSessionPane(m_session, id);
            if (const auto values = layout.values.find(id); pane && values != layout.values.end())
                applySessionValues(*pane, values->second, !m_session->remote());
            if (id == saved.focused) tab->focused = pane;
            if (id == saved.zoomed) zoomed = pane;
            return pane;
        });
        if (zoomed) tab->splits->setZoomedPane(zoomed);
        m_tabs.push_back(std::move(tab));
    }
    if (m_tabs.empty()) return;
    m_splits = nullptr;
    switchTab(int(std::min(layout.current, m_tabs.size() - 1)));
}

void TerminalWindow::keepSessionOnClose()
{
    m_windowRelease = MuxRelease::Nothing;
}

MuxRelease TerminalWindow::windowCloseRelease() const
{
    if (m_windowRelease) return *m_windowRelease;
    // The session of a project keeps running for its next open.
    if (!m_projectName.isEmpty()) return MuxRelease::Nothing;
    return m_settings.settings().mux.onClose == MuxSettings::Close::Kill ? MuxRelease::Session : MuxRelease::Nothing;
}

AfterglowMux::Layout TerminalWindow::sessionLayout() const
{
    AfterglowMux::Layout layout;
    for (std::size_t i = 0; i < m_tabs.size(); ++i) {
        const Tab& tab = *m_tabs[i];
        if (!tab.splits) continue;
        layout.tabs.push_back(sessionTab(tab, int(i) == m_currentTab ? m_focused.data() : tab.focused.data()));
    }
    layout.current = std::size_t(std::clamp(m_currentTab, 0, std::max(0, int(layout.tabs.size()) - 1)));
    // The panes of the tabs: each is in the layout.
    std::size_t used = 0;
    for (Pane* pane : panes()) {
        if (pane->sessionPaneId() != 0) addSessionValues(layout, *pane, m_session && !m_session->remote(), used);
    }
    return layout;
}

AfterglowMux::LayoutTab TerminalWindow::sessionTab(const Tab& tab, const Pane* focused) const
{
    AfterglowMux::LayoutTab out;
    // The nested snapshot in preorder: a split, its first side, its second.
    std::function<void(const QJsonObject&)> flatten = [&](const QJsonObject& node) {
        if (node.contains(QStringLiteral("pane"))) {
            out.tree.push_back({std::uint32_t(node.value(QStringLiteral("pane")).toObject().value(QStringLiteral("id")).toInteger()), false, 0.5});
            return;
        }
        out.tree.push_back({0, node.value(QStringLiteral("direction")).toString() == QLatin1String("down"),
                            node.value(QStringLiteral("ratio")).toDouble()});
        flatten(node.value(QStringLiteral("first")).toObject());
        flatten(node.value(QStringLiteral("second")).toObject());
    };
    flatten(tab.splits->snapshot([](Pane* p) { return QJsonObject{{QStringLiteral("id"), qint64(p->sessionPaneId())}}; }));
    out.focused = focused && tab.splits->contains(focused) ? focused->sessionPaneId() : 0;
    for (const AfterglowMux::LayoutNode& node : out.tree) {
        if (out.focused == 0 && node.pane != 0) out.focused = node.pane;
    }
    out.zoomed = tab.splits->zoomedPane() ? tab.splits->zoomedPane()->sessionPaneId() : 0;
    // At a character's end: the daemon takes no longer title.
    QString title = tab.title;
    while (std::size_t(title.toUtf8().size()) > AfterglowMux::MaxTabTitle) title.chop(1);
    out.title = title.toStdString();
    return out;
}

Pane* TerminalWindow::sessionPane(std::uint32_t id) const
{
    for (Pane* pane : panes()) {
        if (pane->sessionPaneId() == id) return pane;
    }
    return nullptr;
}

Pane* TerminalWindow::placeSessionPane(std::uint32_t id, Pane* beside, std::optional<SplitTree::Direction> side, const QStringList& command)
{
    if (!m_session || sessionPane(id)) return nullptr;
    // Through the paths of a split and a tab, with their room and their
    // effects: createPane takes the pane that the daemon started.
    m_placingPane = id;
    QString error;
    Pane* placed = side && beside ? splitPane(beside, *side, {}, &error) : nullptr;
    if (!placed && m_placingPane) placed = openTab({}, &error, beside && tabOf(beside) >= 0 ? beside : nullptr);
    if (!placed && m_placingPane) placed = addTab(createSessionPane(m_session, id), -1);
    m_placingPane.reset();
    if (placed && !m_session->remote()) placed->setLaunchCommand(command);
    sendSessionLayout();
    return placed;
}

QString TerminalWindow::breakOutOfSession(Pane* pane, bool wholeTab, QString* session)
{
    const int index = tabOf(pane);
    if (!m_session || index < 0) return QStringLiteral("Invalid pane destination");
    const Tab& tab = *m_tabs[std::size_t(index)];
    AfterglowMux::Layout moved;
    if (wholeTab) {
        moved.tabs.push_back(sessionTab(tab, index == m_currentTab ? m_focused.data() : tab.focused.data()));
    } else {
        moved.addTab(pane->sessionPaneId());
    }
    QList<Pane*> leaving;
    if (wholeTab) tab.splits->forEachPane([&leaving](Pane* p, const QRect&) { leaving.append(p); });
    else leaving.append(pane);
    // Their titles and commands go with them, for the windows of the new session.
    std::size_t used = 0;
    for (const Pane* p : std::as_const(leaving)) addSessionValues(moved, *p, !m_session->remote(), used);
    // The daemon refuses it too: the session would only get another name.
    if (leaving.size() >= panes().size()) return QStringLiteral("These are all the panes of session %1: they stay in its window").arg(m_session->session());
    QString error;
    const auto name = m_session->breakOut(moved, &error);
    if (!name) return error;
    // Their programs run on in the new session: closing them here ends
    // nothing. Before the stream of this session runs again, which brings
    // their PaneClosed.
    for (Pane* p : std::as_const(leaving)) p->setSessionRelease(MuxRelease::Nothing);
    m_breakingOut = true;
    if (wholeTab) closeTab(index);
    else closePane(pane);
    m_breakingOut = false;
    *session = *name;
    return {};
}

QString TerminalWindow::sessionName() const
{
    return m_session ? m_session->session() : QString();
}

QString TerminalWindow::sessionHost() const
{
    return m_session ? m_session->host() : QString();
}

void TerminalWindow::scheduleSessionLayout()
{
    if (m_session && !m_sessionLayoutTimer.isActive()) m_sessionLayoutTimer.start();
}

void TerminalWindow::sendSessionLayout()
{
    m_sessionLayoutTimer.stop();
    if (!m_session || m_tabs.empty()) return;
    AfterglowMux::Layout layout = sessionLayout();
    // Every pane of the window is the session's: one that is not would be a
    // layout that the daemon refuses, and the window would lose the session.
    for (const std::uint32_t id : layout.panes()) {
        if (id == 0) {
            qCWarning(lcWindow) << "a pane of the window of session" << m_session->session() << "is not the session's";
            return;
        }
    }
    // The moves of panes that are still in the layout, in their order.
    for (const std::uint32_t id : std::exchange(m_movedSessionPanes, {})) {
        if (layout.contains(id)) layout.moved.push_back(id);
    }
    m_session->sendLayout(layout);
}

Pane* TerminalWindow::splitFocused(SplitTree::Direction direction, const QStringList& command, const PaneLaunchOptions& options, QString* error)
{
    if (!m_focused) {
        return nullptr;
    }
    const int divider = dividerPixels();
    Pane* const zoomed = m_splits->zoomedPane();
    if (zoomed) {
        // Measure the room unzoomed, in the tree alone: a split refused for
        // lack of room leaves the zoom, and every pane's size, as they were.
        m_splits->setZoomedPane(nullptr);
        m_splits->layout(paneArea(), divider);
    } else {
        // A dock moved, or the scale or the font changed since the last
        // frame: the rectangles below would be from before, and the shell
        // would start at a size it does not keep. Never drawn, they would
        // be empty, and the shell would start at 2x1. A new window size
        // counts from the frame that resizes the swap chain: that frame
        // lays out and resizes the new pane with the others.
        ensureLayout();
    }
    // Start the new shell at the size its requested share of the split will have.
    QRect area = m_splits->rectOf(m_focused);
    const bool sideways = direction == SplitTree::Direction::Right || direction == SplitTree::Direction::Left;
    // Both panes keep the size that resizes and divider drags keep.
    const int available = (sideways ? area.width() : area.height()) - divider;
    const int smallest = sideways ? minimumPaneSize().width() : minimumPaneSize().height();
    if (zoomed) {
        m_splits->setZoomedPane(zoomed);
        m_splits->layout(paneArea(), divider);
    }
    if (available < 2 * smallest) {
        if (error) *error = noRoomForPane();
        return nullptr;
    }
    const double fraction = std::clamp(options.fraction, double(smallest) / available, 1.0 - double(smallest) / available);
    if (sideways) {
        area.setWidth(int(available * fraction));
    } else {
        area.setHeight(int(available * fraction));
    }
    Pane* added = createPane(options.cwd.isEmpty() ? startDirectory(inheritedDirectory(m_focused)) : options.cwd, area, command, error, options.environment);
    // A shell that did not start leaves the zoom too.
    if (!added) return nullptr;
    if (zoomed) {
        setPaneZoom(nullptr);
        relayout();
    }
    // The new pane grows out of the edge being split.
    const QRect old = m_splits->rectOf(m_focused);
    switch (direction) {
    case SplitTree::Direction::Right:
        m_growFrom[added] = QRectF(old.right() + 1, old.top(), 0, old.height());
        break;
    case SplitTree::Direction::Left:
        m_growFrom[added] = QRectF(old.left(), old.top(), 0, old.height());
        break;
    case SplitTree::Direction::Down:
        m_growFrom[added] = QRectF(old.left(), old.bottom() + 1, old.width(), 0);
        break;
    case SplitTree::Direction::Up:
        m_growFrom[added] = QRectF(old.left(), old.top(), old.width(), 0);
        break;
    }
    m_splits->split(m_focused, added, direction, fraction, divider, minimumPaneSize());
    // It opens from the edge it shares with the pane it split from.
    const int edge = direction == SplitTree::Direction::Right ? 1 : direction == SplitTree::Direction::Down ? 2
        : direction == SplitTree::Direction::Left ? 3 : 4;
    m_paneOrigin.insert(added, {m_focused->id(), edge});
    if (paneEffectsAllowed()) m_paneOpening.insert(added, {std::chrono::steady_clock::now(), edge});
    setFocusedPane(added);
    m_layoutSize = QSize();
    // A split from a script or `afterglow ctl` may come in a window that
    // nobody sees: the new pane hears it.
    updatePaneVisibility();
    scheduleRender();
    return added;
}

void TerminalWindow::closePane(Pane* pane)
{
    // The last pane of the window closes the window, and Mux.onClose says
    // what goes. Another one ends its own program. A pane that left its tab
    // already got its release from closeTab.
    if (m_session && !m_breakingOut && tabOf(pane) >= 0) pane->setSessionRelease(panes().size() == 1 ? windowCloseRelease() : MuxRelease::Pane);
    // Its overlay closes first: the program gets SIGHUP, and its result is
    // "cancelled". It goes with no fade, and the pane gets no focus back
    // for a moment. When it shows, its last frame is the one that closes;
    // a popup's grid is only part of the pane, which closes on its own.
    std::shared_ptr<AfterglowRender::PaneSnapshot> shownFrame;
    if (const QPointer<PaneOverlay> overlay = pane->overlay()) {
        if (!overlay->popup() && overlayFade(pane).first >= 0.5)
            shownFrame = std::make_shared<AfterglowRender::PaneSnapshot>(overlay->pane()->takeSnapshot());
        QObject::disconnect(overlay.data(), nullptr, this, nullptr);
        overlay->cancel(QStringLiteral("The pane under the overlay closed"));
        if (overlay) overlayEnded(pane, overlay, true);
    } else if (const auto fade = m_overlayFades.find(pane); fade != m_overlayFades.end()) {
        // An overlay that ended still fades out: from halfway it is what shows.
        if (fade->second.last && !fade->second.lastPopup && overlayFade(pane).first >= 0.5) shownFrame = fade->second.last;
        m_overlayFades.erase(fade);
    }
    const int index = tabOf(pane);
    // A pane that already left its tab (its tab closed, and its program
    // ended before the deferred delete) was announced then. The last pane
    // of a tab is announced by closeTab, with the tab.
    if (index >= 0 && m_tabs[std::size_t(index)]->splits->paneCount() > 1)
        Q_EMIT paneClosing(pane->id(), m_tabs[std::size_t(index)]->id, id());
    if (m_scriptDialog && m_scriptDialog->pane == pane) finishScriptDialog(false, {});
    dropPanePrompts(pane);
    // The closing frame keeps the opacity a script gave the pane.
    AfterglowRender::FrameBuilder::PaneParams last;
    applyScriptOverrides(pane, last, std::uint8_t(std::lround(m_settings.settings().window.backgroundOpacity * 255)));
    m_shaderControls.removePane(pane->id());
    std::erase_if(m_scriptAppearance, [pane](const auto& item) { return item.pane == pane; });
    if (index < 0) {
        return;
    }
    Tab& tab = *m_tabs[std::size_t(index)];
    m_paneOpening.remove(pane);
    m_paneContext.remove(pane);
    m_paneOrigin.remove(pane);
    m_panePulse.remove(pane);
    if (m_scriptKeyMode && m_scriptKeyMode->pane == pane) m_scriptKeyMode->pane = nullptr;
    // Its last frame stays on screen while it closes.
    const QRect drawn = index == m_currentTab && tab.splits->isVisible(pane) ? motion(pane).rect.value().toRect() : QRect();
    if (!tab.splits->remove(pane)) {
        closeTab(index); // the tab's last pane
        return;
    }
    if (paneEffectsAllowed() && !drawn.isEmpty()) {
        // Its docks close with it, in the same frame.
        std::vector<QRect> dockRects;
        paneDockArea(pane, drawn, &dockRects);
        decltype(ClosingPane::docks) docks;
        for (std::size_t i = 0; i < dockRects.size(); ++i) {
            if (!dockRects[i].isEmpty())
                docks.emplace_back(std::make_shared<AfterglowRender::PaneSnapshot>(pane->docks()[i]->snapshot()), dockRects[i]);
        }
        m_closingPane = ClosingPane{shownFrame ? shownFrame : std::make_shared<AfterglowRender::PaneSnapshot>(pane->snapshot()), drawn,
                                    contentRect(pane, drawn).topLeft(), std::move(docks), std::chrono::steady_clock::now(),
                                    tab.id, last.backgroundAlpha};
    }
    // The current tab's focus is m_focused; a background tab's is its saved one.
    if (index == m_currentTab ? m_focused == pane : tab.focused == pane) {
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
    // A background tab's dividers and layout are not the ones on screen: a
    // divider drag and hint labels there go on.
    if (index == m_currentTab) {
        m_dividerDrag.reset(); // the dividers moved
        m_layoutSize = QSize();
    }
    updatePaneVisibility();
    // The tab may show another pane's title now, and lost the pane's script suffix.
    Q_EMIT tabsChanged();
    scheduleRender();
}

void TerminalWindow::dropTabPrompts(quint32 tabId)
{
    if (m_titleTab == tabId) {
        m_titleTab = 0;
        m_controller->cancelTitle();
    }
    if (m_menuTab == tabId) {
        m_menuTab = 0;
        m_controller->closeContextMenu();
    }
}

void TerminalWindow::dropClipboardPrompt()
{
    // The prompt's reply would go to a pane that is leaving: it goes
    // unanswered. A session that the close leaves running gets the denial:
    // its program still waits.
    const auto pending = std::exchange(m_pendingClipboardRead, std::nullopt);
    if (!pending || !pending->pane || pending->pane->closeEndsProgram()) return;
    pending->pane->sendReply(pending->request.refusal("EPERM"));
}

void TerminalWindow::dropPanePrompts(Pane* pane)
{
    if (m_pendingClipboardRead && m_pendingClipboardRead->pane == pane) {
        dropClipboardPrompt();
        m_controller->answerClipboard(0);
    }
    if (m_pendingPastePane == pane) m_controller->answerPaste(false);
    if (m_titlePane == pane) {
        m_controller->cancelTitle();
        m_titlePane = nullptr;
    }
    if (m_menuPane == pane) {
        m_controller->closeContextMenu();
        m_menuPane = nullptr;
    }
    if (m_urlMode.pane == pane) stopUrlMode();
    if (m_selectionPane == pane) {
        m_selectionPane = nullptr;
        m_autoScroll.stop();
    }
    if (m_textDragPane == pane) m_textDragPane = nullptr;
    if (m_reportedPane == pane) {
        m_reportedPane = nullptr;
        m_reportedButtons = Qt::NoButton;
    }
    // Its docks go with it, and the pane is still in its tab to find them by.
    dropDockPointer(pane);
    // A close confirmation about this pane alone has nothing left to ask
    // about. One about a tab or the window still holds for the others.
    if (m_pendingConfirmPanes.removeAll(pane) > 0 && m_pendingConfirmPanes.isEmpty()) m_controller->answerConfirm(false);
}

// --- tabs ------------------------------------------------------------------

int TerminalWindow::tabOf(const Pane* pane) const
{
    for (std::size_t i = 0; i < m_tabs.size(); ++i) {
        if (m_tabs[i]->splits && m_tabs[i]->splits->contains(pane)) {
            return int(i);
        }
    }
    return -1;
}

int TerminalWindow::tabBarHeight(int tabCount) const
{
    // A script's dock draws the tabs instead (afterglow.dock tab_bar).
    if (tabBarReplaced()) return 0;
    const int height = int(std::lround(m_settings.settings().window.tabBarHeight * devicePixelRatio()));
    switch (m_settings.settings().window.tabBar) {
    case WindowSettings::TabBar::Always:
        return height;
    case WindowSettings::TabBar::Never:
        return 0;
    case WindowSettings::TabBar::Auto:
        break;
    }
    return (tabCount < 0 ? int(m_tabs.size()) : tabCount) > 1 ? height : 0;
}

QRect TerminalWindow::paneArea(int tabCount) const
{
    return layoutDocks(windowArea(tabCount), nullptr);
}

QSize TerminalWindow::minimumPaneSize() const
{
    const auto& m = m_renderer->fonts().fonts->metrics();
    const int pad = int(std::lround(m_settings.settings().window.padding * devicePixelRatio()));
    return QSize(4 * m.cellWidth + 2 * pad, 2 * m.cellHeight + 2 * pad);
}

int TerminalWindow::dividerPixels() const
{
    return std::max(1, int(std::lround(devicePixelRatio())));
}

bool TerminalWindow::paneDrawn(const Pane* pane) const
{
    // An overlay is drawn where its host is.
    if (pane && pane->overlayHost()) pane = pane->overlayHost();
    const int index = tabOf(pane);
    if (index < 0) return false;
    if (index == m_currentTab) return m_splits && m_splits->isVisible(pane);
    if (overviewShowing()) return true;
    return m_tabTransition.progress.isAnimating() && m_tabs[std::size_t(index)]->id == m_tabTransition.from;
}

bool TerminalWindow::paneLaidOut(const Pane* pane) const
{
    const Pane* host = pane && pane->overlayHost() ? pane->overlayHost() : pane;
    return m_splits && !m_splits->rectOf(host).isEmpty();
}

QString TerminalWindow::shownTitle(const Pane* pane)
{
    // An overlay's own title while it has one, as the pane's.
    const Pane* overlay = pane->overlay() ? pane->overlay()->pane() : nullptr;
    return overlay && !overlay->title().isEmpty() ? overlay->title() : pane->title();
}

QString TerminalWindow::tabTitle(const Tab& tab, const Pane* focused) const
{
    QString title = !tab.title.isEmpty() ? tab.title : focused ? shownTitle(focused) : QString();
    for (const auto& item : m_scriptAppearance)
        if (item.pane && tab.splits && tab.splits->contains(item.pane) && !item.tab.isEmpty()) title += QStringLiteral(" · ") + item.tab;
    return title;
}

Pane* TerminalWindow::newTab(const QString& workingDirectory, const QStringList& command, const QVariantMap& environment,
                             QString* error, int at)
{
    // Size the shell for the area it will get, with the tab bar if it now
    // shows. addTab() puts it beside the current tab unless @p at says
    // otherwise. The pane exists before its tab, so no tab is ever without a
    // tree.
    Pane* first = createPane(workingDirectory, paneArea(int(m_tabs.size()) + 1), command, error, environment);
    if (!first) {
        return nullptr;
    }
    return addTab(first, at);
}

Pane* TerminalWindow::addTab(Pane* first, int at)
{
    if (at < 0 || at > int(m_tabs.size())) at = m_tabs.empty() ? 0 : m_currentTab + 1;
    auto created = std::make_unique<Tab>();
    created->splits = std::make_unique<SplitTree>(first);
    created->focused = first;
    Tab& tab = *created;
    const bool hadTab = m_splits != nullptr;
    const int from = m_currentTab;
    if (hadTab) m_tabs[std::size_t(from)]->focused = m_focused;
    m_tabs.insert(m_tabs.begin() + at, std::move(created));
    const int previous = hadTab && at <= from ? from + 1 : from; // the current tab's index after the insert
    if (hadTab) m_currentTab = previous;
    m_splits = nullptr; // saved above; switchTab must not save again
    switchTab(at);
    if (hadTab) {
        startTabTransition(previous, at);
        Q_EMIT tabSwitched(m_tabs[std::size_t(previous)]->id, tab.id, at > previous ? 1 : -1);
    }
    return first;
}

QList<Pane*> TerminalWindow::panes() const
{
    QList<Pane*> out;
    for (const auto& tab : m_tabs) {
        if (tab->splits) {
            tab->splits->forEachPane([&out](Pane* p, const QRect&) { out.append(p); });
        }
    }
    return out;
}

QList<Pane*> TerminalWindow::terminalPanes() const
{
    QList<Pane*> out;
    for (Pane* pane : panes()) {
        out.append(pane);
        if (pane->overlay()) out.append(pane->overlay()->pane());
    }
    return out;
}

void TerminalWindow::reportFocus(Pane* pane, bool focused)
{
    pane->sendReply(pane->withTerminal([focused](Terminal& t) { return AfterglowVt::encodeFocus(focused, t.modes()); }));
}

quint32 TerminalWindow::tabIdOf(const Pane* pane) const
{
    const int index = tabOf(pane);
    return index < 0 ? 0 : m_tabs[std::size_t(index)]->id;
}

quint32 TerminalWindow::activeTabId() const
{
    return m_currentTab >= 0 && m_currentTab < int(m_tabs.size()) ? m_tabs[std::size_t(m_currentTab)]->id : 0;
}

QList<TerminalWindow::TabInfo> TerminalWindow::tabs() const
{
    QList<TabInfo> out;
    for (std::size_t i = 0; i < m_tabs.size(); ++i) {
        const Tab& tab = *m_tabs[i];
        const Pane* pane = int(i) == m_currentTab ? m_focused.data() : tab.focused.data();
        out.append({tab.id, int(i), pane ? pane->id() : 0, tabTitle(tab, pane)});
    }
    return out;
}

Pane* TerminalWindow::splitPane(Pane* pane, SplitTree::Direction direction, const PaneLaunchOptions& options, QString* error)
{
    if (tabOf(pane) < 0) return nullptr;
    // As openTab: the pane's tab shows and the pane takes the focus, in
    // this window, which a call from a script or afterglow ctl neither
    // raises nor activates.
    if (const int tab = tabOf(pane); tab != m_currentTab) switchTab(tab);
    setFocusedPane(pane);
    return splitFocused(direction, options.command, options, error);
}

Pane* TerminalWindow::openTab(const PaneLaunchOptions& options, QString* error, Pane* from)
{
    const int beside = from ? tabOf(from) : -1;
    const QString start = options.cwd.isEmpty() ? startDirectory(inheritedDirectory(beside >= 0 ? from : m_focused.data())) : options.cwd;
    return newTab(start, options.command, options.environment, error,
                  beside >= 0 ? beside + 1 : -1);
}

void TerminalWindow::openShellTab(Pane* from)
{
    QString error;
    if (!openTab({}, &error, from)) NotificationCenter::instance().showNotice(m_focused, tr("Could not open a tab"), error);
}

QString TerminalWindow::inheritedDirectory(const Pane* from) const
{
    return from && !fixedStartDirectory(false) ? from->workingDirectory() : QString();
}

QString TerminalWindow::focusedDirectory() const
{
    return m_focused ? m_focused->workingDirectory() : QString();
}

std::optional<QString> TerminalWindow::fixedStartDirectory(bool warn) const
{
    const QString setting = m_settings.settings().window.workingDirectory;
    if (setting.isEmpty() || setting == QLatin1String("inherit")) {
        return std::nullopt;
    }
    if (setting == QLatin1String("home")) {
        return QDir::homePath();
    }
    const QString path = setting == QLatin1String("~") || setting.startsWith(QLatin1String("~/"))
        ? QDir::homePath() + setting.mid(1)
        : setting;
    if (QFileInfo(path).isDir()) {
        return path;
    }
    // A path that is not a directory inherits (Window.workingDirectory).
    if (warn) qCWarning(lcWindow).noquote() << "workingDirectory" << setting << "is not a directory";
    return std::nullopt;
}

QString TerminalWindow::startDirectory(const QString& inherited) const
{
    if (const auto fixed = fixedStartDirectory(true)) return *fixed;
    // A pane whose directory is unknown or gone hands on the home directory.
    return !inherited.isEmpty() && QFileInfo(inherited).isDir() ? inherited : QDir::homePath();
}

QStringList TerminalWindow::closingJobs(const QList<Pane*>& panes) const
{
    QStringList jobs;
    if (!m_settings.settings().confirmClose) return jobs;
    // A session's window that closes whole with Mux.onClose detach ends no
    // program.
    if (m_session && windowCloseRelease() == MuxRelease::Nothing && panes.size() >= this->panes().size()) return jobs;
    for (Pane* p : panes) {
        // An overlay's program runs on the pane too, and closing ends it.
        // Without one, inputPane() is the pane itself: one question each,
        // which for a session pane is a call to the daemon.
        Pane* const overlay = inputPane(p);
        for (Pane* running : {p, overlay == p ? nullptr : overlay}) {
            if (!running) continue;
            // The name is the program's to choose (prctl): shown as plain text.
            if (const QString job = sanitizedText(running->foregroundJob(), 64); !job.isEmpty() && !jobs.contains(job)) {
                jobs << job;
            }
        }
    }
    return jobs;
}

void TerminalWindow::confirmClose(const QList<Pane*>& panes, const QString& what, std::function<void()> close)
{
    confirmClose(closingJobs(panes), panes, what, std::move(close));
}

void TerminalWindow::confirmClose(const QStringList& jobs, const QList<Pane*>& panes, const QString& what, std::function<void()> close)
{
    if (jobs.isEmpty()) {
        close();
        return;
    }
    // One question at a time: a second one (the window's close while a tab's
    // waits) would take over the answer to the first.
    if (m_controller->confirmPromptOpen()) return;
    QString names = jobs.constLast();
    if (jobs.size() > 1) {
        names = jobs.mid(0, jobs.size() - 1).join(QStringLiteral(", ")) + QStringLiteral(" and ") + names;
    }
    m_pendingConfirm = std::move(close);
    m_pendingConfirmPanes.clear();
    for (Pane* p : panes) m_pendingConfirmPanes.append(p);
    m_controller->askConfirm(QStringLiteral("Close %1?").arg(what),
                             QStringLiteral("%1 %2 still running, and closing ends %3.")
                                 .arg(names, jobs.size() == 1 ? QStringLiteral("is") : QStringLiteral("are"),
                                      jobs.size() == 1 ? QStringLiteral("it") : QStringLiteral("them")),
                             QStringLiteral("Close"));
}

void TerminalWindow::requestCloseTabs(const QList<int>& indices)
{
    QList<Pane*> panes;
    QList<quint32> tabs;
    int first = -1;
    for (int i : indices) {
        if (i >= 0 && i < int(m_tabs.size())) {
            if (first < 0) first = i;
            tabs << m_tabs[std::size_t(i)]->id;
            m_tabs[std::size_t(i)]->splits->forEachPane([&panes](Pane* p, const QRect&) { panes << p; });
        }
    }
    if (tabs.isEmpty()) {
        return;
    }
    QString what = QStringLiteral("this tab");
    if (tabs.size() > 1) {
        what = QStringLiteral("%1 tabs").arg(tabs.size());
    } else if (m_tabs.size() == 1) {
        what = QStringLiteral("this window");
    } else if (first != m_currentTab) {
        // Not the tab on screen: name it.
        const Tab& tab = *m_tabs[std::size_t(first)];
        const QString title = tabTitle(tab, tab.focused.data());
        if (!title.isEmpty()) what = QStringLiteral("the tab “%1”").arg(sanitizedText(title, 80));
    }
    // By tab id, not index or address: tabs may move, close or be replaced
    // while the prompt is open.
    confirmClose(panes, what, [this, tabs] {
        // Every tab of a session's window: the window closes, and Mux.onClose
        // says what goes, as when its close button closes it.
        if (m_session && tabs.size() == qsizetype(m_tabs.size())) {
            m_closeConfirmed = true;
            close();
            return;
        }
        for (const quint32 id : tabs) {
            for (std::size_t i = 0; i < m_tabs.size(); ++i) {
                if (m_tabs[i]->id == id) {
                    closeTab(int(i));
                    break;
                }
            }
        }
    });
}

bool TerminalWindow::runAction(Pane* pane, Action action)
{
    // From outside (afterglow ctl, afterglow.action), not while a prompt, a
    // menu, the palette, the search or a script dialog has the keyboard: a
    // key could not act then either. The action would replace the prompt's
    // pending answer (a close, a paste), or open over it.
    if (m_scriptDialog || m_controller->wantsKeyboard()) return false;
    return actOnPane(pane, action);
}

bool TerminalWindow::actOnPane(Pane* pane, Action action)
{
    // What the tab menu does to a tab, a pane's action does to its tab
    // where it is: a tab line closes a tab without showing it first.
    if (const int tab = pane ? tabOf(pane) : -1; tab >= 0 && tab != m_currentTab) {
        switch (action) {
        case Action::CloseTab:
            requestCloseTabs({tab});
            return true;
        case Action::RenameTab:
            renameTab(tab);
            return true;
        case Action::MoveTabLeft:
            moveTab(tab, tab - 1);
            return true;
        case Action::MoveTabRight:
            moveTab(tab, tab + 1);
            return true;
        case Action::DetachTab:
            // The pane named is the one that the new window focuses.
            m_tabs[std::size_t(tab)]->focused = pane;
            detachTab(tab);
            return true;
        case Action::NewTab:
            openShellTab(pane);
            return true;
        default:
            break;
        }
    }
    if (pane && tabOf(pane) >= 0) {
        if (tabOf(pane) != m_currentTab) {
            switchTab(tabOf(pane));
        }
        setFocusedPane(pane);
    }
    return performAction(action);
}

void TerminalWindow::switchTab(int index)
{
    if (index < 0 || index >= int(m_tabs.size())) {
        return;
    }
    if (index == m_currentTab && m_splits) {
        return; // already shown: switching would snap its animations
    }
    if (m_splits && m_currentTab < int(m_tabs.size())) {
        m_tabs[std::size_t(m_currentTab)]->focused = m_focused;
    }
    const quint32 leaving = m_splits && m_currentTab != index && m_currentTab < int(m_tabs.size()) ? m_tabs[std::size_t(m_currentTab)]->id : 0;
    const int direction = index > m_currentTab ? 1 : -1;
    if (m_splits) startTabTransition(m_currentTab, index);
    m_currentTab = index;
    Tab& tab = *m_tabs[std::size_t(index)];
    m_splits = tab.splits.get();
    if (!tab.focused) {
        // A restored project may name no active pane: never show a tab with no focus.
        m_splits->forEachPane([&tab](Pane* p, const QRect&) { if (!tab.focused) tab.focused = p; });
    }
    // Panes appear where they are: no easing from where they were when the
    // tab was last shown (the window may have changed size since).
    m_splits->forEachPane([this](Pane* p, const QRect&) {
        // An overlay's cursor too: it was last drawn in the same place.
        for (Pane* shown : {p, inputPane(p)}) {
            PaneMotion& m = motion(shown);
            m.rectKnown = false;
            m.cursorKnown = false;
        }
    });
    QTimer::singleShot(0, this, &TerminalWindow::updatePaneVisibility);
    m_selectionPane = nullptr;
    m_textDragPane = nullptr;
    // The link under the pointer was in the tab that went: its underline
    // and its pointer shape go with it.
    if (m_hover.pane) {
        m_hover = {};
        applyPointerShape();
    }
    // A divider being dragged belongs to the tab that went: its node may be
    // freed, and a new split could take its address.
    m_dividerDrag.reset();
    setFocusedPane(tab.focused);
    m_layoutSize = QSize();
    Q_EMIT tabsChanged();
    if (leaving) Q_EMIT tabSwitched(leaving, tab.id, direction);
    scheduleRender();
}

void TerminalWindow::closeTab(int index)
{
    if (index < 0 || index >= int(m_tabs.size())) {
        return;
    }
    if (m_scriptDialog && tabOf(m_scriptDialog->pane) == index) finishScriptDialog(false, {});
    if (m_session && !m_breakingOut) {
        // The last tab closes the window, and Mux.onClose says what goes:
        // the daemon keeps the tabs as they were. Another tab ends its own
        // programs.
        const bool window = m_tabs.size() == 1;
        if (window) sendSessionLayout();
        m_tabs[std::size_t(index)]->splits->forEachPane(
            [this, window](Pane* p, const QRect&) { p->setSessionRelease(window ? windowCloseRelease() : MuxRelease::Pane); });
    }
    // Its keys go to the focused pane, as closePane leaves them: the panes
    // stay alive until their deferred delete, out of any tab.
    if (m_scriptKeyMode && m_scriptKeyMode->pane && tabOf(m_scriptKeyMode->pane) == index) m_scriptKeyMode->pane = nullptr;
    for (Pane* p : panes()) if (tabOf(p) == index) m_shaderControls.removePane(p->id());
    std::erase_if(m_scriptAppearance, [this, index](const auto& item) { return tabOf(item.pane) == index; });
    const quint32 closingId = m_tabs[std::size_t(index)]->id;
    dropTabPrompts(closingId);
    QList<Pane*> closing;
    m_tabs[std::size_t(index)]->splits->forEachPane([&closing](Pane* p, const QRect&) { closing.append(p); });
    // Announced while the tab still holds them: the handlers may walk the tabs.
    for (Pane* p : std::as_const(closing)) {
        Q_EMIT paneClosing(p->id(), closingId, id());
        // An overlay goes with its pane, at the deferred delete: what its
        // terminal had here goes now.
        if (Pane* program = inputPane(p); program != p) dropOverlayUi(program);
        dropPanePrompts(p);
    }
    // The tab leaves before its panes are freed: whatever walks the tabs
    // from here on (a destroyed handler, a queued close, an overlay's
    // reply) finds no address of a freed pane.
    m_tabs.erase(m_tabs.begin() + index);
    for (Pane* p : std::as_const(closing)) p->deleteLater();
    if (m_tabs.empty()) {
        qCInfo(lcWindow) << "last tab closed, closing the window";
        // As a transfer of the last tab leaves the window (finishTransfer).
        m_splits = nullptr;
        m_focused = nullptr;
        m_currentTab = 0;
        m_closeConfirmed = true; // asked already, or its program ended
        close();
        return;
    }
    if (index != m_currentTab) {
        // A tab in the background: the current one is untouched.
        if (index < m_currentTab) {
            --m_currentTab;
        }
        m_layoutSize = QSize(); // the tab bar may have gone
        Q_EMIT tabsChanged();
        scheduleRender();
        return;
    }
    // The tab that slides into its place, or the new last one.
    m_splits = nullptr; // gone; switchTab must not save into it
    m_focused = nullptr;
    const int next = std::min(index, int(m_tabs.size()) - 1);
    switchTab(next);
    // A switch for scripts and control clients, from a tab that no longer exists.
    Q_EMIT tabSwitched(closingId, m_tabs[std::size_t(next)]->id, next < index ? -1 : 1);
}

void TerminalWindow::moveTab(int from, int to)
{
    const int count = int(m_tabs.size());
    if (from < 0 || from >= count || to < 0 || to >= count || from == to) {
        return;
    }
    const Tab* current = m_tabs[std::size_t(m_currentTab)].get();
    // The overview's cards go in tab order.
    const auto cards = overviewPanes();
    auto moved = std::move(m_tabs[std::size_t(from)]);
    m_tabs.erase(m_tabs.begin() + from);
    m_tabs.insert(m_tabs.begin() + to, std::move(moved));
    for (int i = 0; i < count; ++i) {
        if (m_tabs[std::size_t(i)].get() == current) {
            m_currentTab = i;
        }
    }
    rebaseOverview(cards);
    // The cards go in tab order up to their limit, so a move can take a pane
    // out of the overview, or put one in.
    if (overviewShowing()) updatePaneVisibility();
    // A drag in the tab bar counts tabs by place: a move from elsewhere
    // (a key, a script) ends it.
    m_controller->notifyTabsMoved();
    Q_EMIT tabsChanged();
    // The moved tab's panes are at a new tab index, and so are those of the
    // tabs between, which shift by one place: scripts and control clients
    // hear of each as of any move.
    for (int i = std::min(from, to); i <= std::max(from, to); ++i) {
        if (m_tabs[std::size_t(i)]->splits)
            m_tabs[std::size_t(i)]->splits->forEachPane([this](Pane* p, const QRect&) { Q_EMIT paneMoved(p); });
    }
    scheduleRender();
}

void TerminalWindow::renameTab(int index)
{
    if (index < 0 || index >= int(m_tabs.size())) {
        return;
    }
    m_titlePane = nullptr;
    m_titleTab = m_tabs[std::size_t(index)]->id;
    m_controller->askTitle(QStringLiteral("Tab title"), m_tabs[std::size_t(index)]->title);
}

void TerminalWindow::detachTab(int index)
{
    if (index < 0 || index >= int(m_tabs.size())) return;
    auto& tab = *m_tabs[std::size_t(index)];
    QPointer<Pane> pane = tab.focused;
    if (!pane) tab.splits->forEachPane([&](Pane* candidate, const QRect&) { if (!pane) pane = candidate; });
    // A transfer can close this window. Let the QML pointer/menu handler return first.
    QTimer::singleShot(0, this, [this, pane] {
        if (pane && tabOf(pane) >= 0) Q_EMIT transferRequested(pane, true);
    });
}

void TerminalWindow::runTabMenuItem(int id)
{
    int index = -1;
    for (std::size_t i = 0; i < m_tabs.size(); ++i) {
        if (m_tabs[i]->id == m_menuTab) {
            index = int(i);
        }
    }
    m_menuTab = 0;
    if (index < 0) {
        return; // the tab closed while its menu was open
    }
    switch (id) {
    case TabMenuRename:
        renameTab(index);
        break;
    case TabMenuNew: {
        // Beside this tab, from its pane, as a new tab of the current one
        // opens: switching to it first would make two tab transitions.
        auto& tab = *m_tabs[std::size_t(index)];
        Pane* from = index == m_currentTab ? m_focused.data() : tab.focused.data();
        // A restored tab that never showed may name no pane yet.
        if (!from) tab.splits->forEachPane([&](Pane* candidate, const QRect&) { if (!from) from = candidate; });
        openShellTab(from);
        break;
    }
    case TabMenuMoveLeft:
        moveTab(index, index - 1);
        break;
    case TabMenuMoveRight:
        moveTab(index, index + 1);
        break;
    case TabMenuDetach:
        detachTab(index);
        break;
    case TabMenuClose:
        requestCloseTabs({index});
        break;
    case TabMenuCloseOthers: {
        QList<int> others;
        for (int i = 0; i < int(m_tabs.size()); ++i) {
            if (i != index) {
                others << i;
            }
        }
        requestCloseTabs(others);
        break;
    }
    case TabMenuCloseRight: {
        QList<int> right;
        for (int i = index + 1; i < int(m_tabs.size()); ++i) {
            right << i;
        }
        requestCloseTabs(right);
        break;
    }
    default:
        break;
    }
}

void TerminalWindow::openTabMenu(int index, const QPointF& pos)
{
    if (index < 0 || index >= int(m_tabs.size())) {
        return;
    }
    m_menuTab = m_tabs[std::size_t(index)]->id;
    m_menuPane = nullptr; // a tab menu has no pane items
    const int count = int(m_tabs.size());
    auto entry = [](int id, const QString& title, const char* icon, bool enabled = true) {
        return QVariant(QVariantMap{{QStringLiteral("id"), id},
                                    {QStringLiteral("title"), title},
                                    {QStringLiteral("icon"), QString::fromLatin1(icon)},
                                    {QStringLiteral("enabled"), enabled}});
    };
    const QVariant separator = QVariantMap{{QStringLiteral("separator"), true}};
    const QVariantList items{
        entry(TabMenuRename, QStringLiteral("Change Tab Title\u2026"), "document-edit"),
        entry(TabMenuNew, QStringLiteral("New Tab"), "tab-new"),
        entry(TabMenuDetach, QStringLiteral("Detach Tab to New Window"), "window-new"),
        separator,
        entry(TabMenuMoveLeft, QStringLiteral("Move Left"), "go-previous", index > 0),
        entry(TabMenuMoveRight, QStringLiteral("Move Right"), "go-next", index < count - 1),
        separator,
        entry(TabMenuClose, QStringLiteral("Close Tab"), "tab-close"),
        entry(TabMenuCloseOthers, QStringLiteral("Close Other Tabs"), "tab-close-other", count > 1),
        entry(TabMenuCloseRight, QStringLiteral("Close Tabs to the Right"), "tab-close-other", index < count - 1),
    };
    m_controller->openContextMenu(pos, items);
}

void TerminalWindow::setFocusedPane(Pane* pane)
{
    if (pane == m_focused) {
        return;
    }
    // Close while the old pane still owns the search and its pending job.
    m_controller->setSearchOpen(false);
    // The palette's actions are for the pane it opened on: a choice made
    // after a script or a control client moved the focus would act on another.
    m_controller->setPaletteOpen(false);
    if (m_focused) inputPane(m_focused)->clearSearch();
    // A pane that left its tree (its program exited) has no reader for the
    // report. An overlay over the pane gets it in the pane's place.
    if (m_focused && m_windowFocused && tabOf(m_focused) >= 0) reportFocus(inputPane(m_focused), false);
    if (m_focused) {
        inputPane(m_focused)->setPreedit({}, 0);
    }
    // The input method is the application's: reset it only for this window's composition.
    if (isActive()) QGuiApplication::inputMethod()->reset();
    stopCopyMode();
    cancelKeyMode();
    // Hints and Unicode input act on the focused pane: they do not carry over.
    if ((m_urlMode.active || m_hintCancel) && m_urlMode.pane != pane) stopUrlMode();
    stopUnicodeInput();
    m_focused = pane;
    // The input method reads the new pane's text at the next frame.
    m_inputMethodDirty = true;
    // The current tab's saved focus follows, so code that reads it (a pane
    // closing, a transfer) never finds an older pane.
    if (pane && m_splits && m_currentTab >= 0 && m_currentTab < int(m_tabs.size())
        && m_tabs[std::size_t(m_currentTab)]->splits.get() == m_splits && m_splits->contains(pane)) {
        m_tabs[std::size_t(m_currentTab)]->focused = pane;
    }
    if (m_splits && m_splits->zoomedPane() && m_splits->contains(pane)) {
        setPaneZoom(pane);
    }
    if (pane && m_windowFocused) reportFocus(inputPane(pane), true);
    updateTitle();
    restartBlink();
    acknowledgeStatus();
    updateChrome();
    Q_EMIT focusedPaneChanged(pane);
}

void TerminalWindow::updateTitle()
{
    if (!m_fixedTitle.isEmpty()) {
        setTitle(m_fixedTitle);
        return;
    }
    QString t = m_focused ? shownTitle(m_focused) : QString();
    if (t.isEmpty()) {
        t = QStringLiteral("afterglow");
    }
    // Where the shell runs, when it is not here as you: [root] or [deploy@prod-1].
    if (const QString context = m_focused ? m_focused->contextLabel() : QString(); !context.isEmpty()) {
        t = QLatin1Char('[') + context + QStringLiteral("] ") + t;
    }
    setTitle(t);
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

void TerminalWindow::setPaneZoom(Pane* pane)
{
    m_splits->setZoomedPane(pane);
    m_dividerDrag.reset();
    m_hover = {};
    m_selectionPane = nullptr;
    m_textDragPane = nullptr;
    stopUrlMode();
    // An overlay's grid moves with its host's.
    m_splits->forEachPane([this](Pane* p, const QRect&) { motion(p).rectKnown = motion(inputPane(p)).rectKnown = false; });
    m_layoutSize = QSize();
    updatePaneVisibility();
    scheduleRender();
}

bool TerminalWindow::resizeFocused(SplitTree::Neighbour direction)
{
    if (!m_focused || !m_splits) return false;
    if (m_splits->zoomedPane()) setPaneZoom(nullptr);
    relayout();
    const auto& metrics = m_renderer->fonts().fonts->metrics();
    const bool vertical = direction == SplitTree::Neighbour::Up || direction == SplitTree::Neighbour::Down;
    const bool changed = m_splits->resizePane(m_focused, direction, 2 * (vertical ? metrics.cellHeight : metrics.cellWidth),
        dividerPixels(), minimumPaneSize());
    m_layoutSize = QSize();
    scheduleRender();
    return changed;
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

void TerminalWindow::scheduleRender()
{
    // requestUpdate() coalesces on its own. Tracking "already requested" here
    // is wrong: Qt drops requests made before the window is exposed, and a
    // stuck flag would then swallow every later request.
    requestUpdate();
}

QRect TerminalWindow::contentRect(const QRect& paneRect) const
{
    const WindowSettings& ws = m_settings.settings().window;
    const int pad = int(std::lround(ws.padding * devicePixelRatio()));
    QRect r = paneRect.adjusted(pad, pad, -pad, -pad);
    if (ws.balancePadding) {
        // The part of a cell the grid cannot use, split between both sides.
        const auto& m = m_renderer->fonts().fonts->metrics();
        const int spareX = std::max(0, r.width()) % m.cellWidth;
        const int spareY = std::max(0, r.height()) % m.cellHeight;
        r.adjust(spareX / 2, spareY / 2, -(spareX - spareX / 2), -(spareY - spareY / 2));
    }
    return r;
}

QRect TerminalWindow::contentRect(const Pane* pane, const QRect& paneRect) const
{
    // An overlay's grid is in its host's, inside the host's docks: all of
    // it, or a popup's cells.
    const PaneOverlay* overlay = pane && pane->overlayHost() ? pane->overlayHost()->overlay() : nullptr;
    if (pane && pane->overlayHost()) pane = pane->overlayHost();
    const QRect content = contentRect(pane ? paneDockArea(pane, paneRect) : paneRect);
    if (!overlay || !overlay->popup()) return content;
    const auto& m = m_renderer->fonts().fonts->metrics();
    const QRect grid = overlay->placement().grid;
    return QRect(content.left() + grid.x() * m.cellWidth, content.top() + grid.y() * m.cellHeight, grid.width() * m.cellWidth,
                 grid.height() * m.cellHeight);
}

void TerminalWindow::relayout()
{
    const QSize px = m_renderer->pixelSize();
    const double dpr = devicePixelRatio();
    const QRect area = placeDocks();
    // Whether a tab_bar dock fits depends on the window, the font and the
    // padding: the QML tab bar comes or goes with it.
    if (m_controller->tabBarHeight() != (tabBarHeight() > 0 ? m_settings.settings().window.tabBarHeight : 0)) updateChrome();
    m_splits->layout(area, dividerPixels());
    // The chrome's overlays for the panes keep off the tab bar and the docks.
    m_controller->setPaneArea(QRectF(QPointF(area.topLeft()) / dpr, QSizeF(area.size()) / dpr));
    // Hint labels sit on cells that may have moved.
    if (m_urlMode.active) stopUrlMode();
    m_splits->forEachPane([this](Pane* p, const QRect& r) { layoutPane(p, r); });
    // An item, a pane edge or an overlay can come or go under a pointer that
    // does not move: the shape follows the new layout, the panes' docks too.
    applyPointerShape();
    m_layoutSize = px;
    m_layoutDpr = dpr;
    m_layoutFontEpoch = m_device.fontEpoch();
    // A split, a zoom, a divider or a tab switch: each lays the panes out.
    scheduleSessionLayout();
}

bool TerminalWindow::layoutStale() const
{
    return m_layoutSize.isEmpty() || m_renderer->pixelSize() != m_layoutSize || devicePixelRatio() != m_layoutDpr
        || m_device.fontEpoch() != m_layoutFontEpoch;
}

void TerminalWindow::ensureLayout()
{
    if (m_docksMoved) finishDockMoves();
    else if (layoutStale()) relayout();
}

void TerminalWindow::renderNow()
{
    if (!m_splits || !isExposed() || !m_renderer->ensureSwapChain()) {
        return;
    }
    qCDebug(lcWindow) << "frame";
    bool sliding = false;
    if (m_quick && m_slide.isAnimating()) {
        m_slide.advance();
        sliding = m_slide.isAnimating();
        if (!sliding && !m_quickShown) {
            hide();
            abortCaptures(QStringLiteral("the window is not on screen"));
            return;
        }
        setQuickOffset(slideOffset());
    }
    ensureLayout();

    std::vector<AfterglowRender::WindowRenderer::Pane> panes;
    std::vector<std::pair<QRect, std::uint32_t>> shading;
    const bool multiple = m_splits->paneCount() > 1;
    bool holding = false;
    m_holdWait = std::numeric_limits<qint64>::max();
    bool animating = m_shaderControls.advance();
    const auto backgroundAlpha = std::uint8_t(std::lround(m_settings.settings().window.backgroundOpacity * 255));
    bool anyBlinkingText = false;
    bool focusedCursorBlinks = false;
    m_anyPaneAway = false;
    // Before the panes: this frame shows the coast where it is now.
    advanceMomentum(animating);
    advanceOverview(animating);
    m_splits->forEachPane([&](Pane* p, const QRect& r) {
        if (r.isEmpty()) return;
        AfterglowRender::WindowRenderer::Pane wp;
        preparePane(p, r, wp.params, holding, animating);
        wp.snapshot = &p->snapshot();
        addPaneDocks(p, wp.params.rect, wp.docks);
        wp.params.focused = p == m_focused && m_windowFocused;
        wp.params.highlighted = overviewShowing() && p == m_overview.highlighted;
        if (const auto opening = m_paneOpening.constFind(p); opening != m_paneOpening.constEnd()) {
            wp.params.openProgress = paneEffectProgress(opening->at);
            wp.params.openEdge = opening->edge;
        }
        applyPaneEffectState(p, wp.params, animating);
        wp.params.cursorBlinkOn = m_blinkOn;
        wp.params.textBlinkOn = m_textBlinkOn;
        const QVariantMap overrides = applyScriptOverrides(p, wp.params, backgroundAlpha);
        refreshLinkHover(p);
        if (m_hover.pane == p) {
            wp.params.hoveredLink = m_hover.link;
            wp.params.hoverRow = m_hover.row;
            wp.params.hoverColStart = m_hover.colStart;
            wp.params.hoverColEnd = m_hover.colEnd;
        }
        // An overlay refreshes its snapshot here, before what reads it below.
        prepareOverlay(p, r, wp, holding, animating);
        // A settled overlay hides the pane's grid: only its blink counts then.
        // A popup shows the pane around it.
        const bool gridShows = !wp.overlay || wp.overlay->popup || wp.overlay->progress < 1.0f;
        anyBlinkingText |= (gridShows && p->snapshot().hasBlinkingText) || (wp.overlay && wp.overlay->snapshot->hasBlinkingText)
            || std::any_of(wp.docks.begin(), wp.docks.end(), [](const auto& dock) { return dock.snapshot->hasBlinkingText; });
        if (p == m_focused) {
            const auto& snap = inputPane(p)->snapshot();
            const bool blinks = wp.params.cursorBlinkOverride >= 0 ? wp.params.cursorBlinkOverride != 0 : snap.cursorBlink;
            focusedCursorBlinks = blinks && (snap.cursorVisible || !snap.extraCursors.empty());
        }
        panes.push_back(wp);
        const QRect shown = wp.params.rect;
        addPaneShading(shading, shown, multiple && p != m_focused, overrides);
        if (PaneMotion& pm = motion(p); pm.bell.isAnimating()) {
            pm.bell.advance();
            // A colour the program set (OSC 21) wins over the theme's: the
            // program that shows, an overlay's over its host.
            const auto& snap = inputPane(p)->snapshot();
            const HighlightColors& colors = m_settings.settings().colors;
            const std::uint32_t rgb = snap.visualBellColorSet ? snap.visualBellColor
                                                              : packRgba(colors.visualBell.r, colors.visualBell.g, colors.visualBell.b);
            const double strength = std::clamp(pm.bell.value(), 0.0, 1.0) * std::clamp(colors.visualBellIntensity, 0.0, 1.0);
            shading.emplace_back(shown, (rgb & 0x00ffffffu) | (std::uint32_t(std::lround(strength * 255)) << 24));
            animating |= pm.bell.isAnimating();
        }
    });
    // The overview shows other tabs' panes too, each prepared like a visible one.
    AfterglowRender::WindowRenderer::Overview overview{m_overview.open, float(m_overview.amount.value()),
        float(m_overview.position.value()), m_overview.changedAt, m_overview.movedAt, m_currentTab, {}};
    if (overviewShowing()) {
        const auto cards = overviewPanes();
        for (int tab = 0; tab < int(m_tabs.size()); ++tab) {
            if (tab == m_currentTab) continue;
            auto other = prepareOtherTab(tab, &cards, backgroundAlpha, holding, animating);
            if (!other.panes.empty()) overview.otherTabs.push_back(std::move(other));
        }
    }
    m_renderer->setOverview(std::move(overview));
    advanceTabTransition(backgroundAlpha, holding, animating);
    advancePaneEffects(animating);
    // A fade that this frame did not advance has a host that no frame draws
    // (another tab, a zoom, an empty pane area, no card for it): it would
    // hold audio capture, so its overlay switches now. A host drawn on a
    // card or in the tab a switch leaves was advanced by prepareOtherTab.
    // Only an advanced fade handed its last frame to the renderer.
    for (auto fade = m_overlayFades.begin(); fade != m_overlayFades.end();) {
        if (std::exchange(fade->second.advanced, false)) ++fade;
        else fade = m_overlayFades.erase(fade);
    }
    // Split labels, in logical pixels for the chrome.
    QVariantList titles;
    if (multiple && m_settings.settings().window.paneTitles && !m_renderer->overviewShowing()) {
        const qreal dpr = devicePixelRatio();
        std::size_t index = 0;
        m_splits->forEachPane([&](Pane* p, const QRect& area) {
            if (area.isEmpty()) return;
            // Below and beside the pane's docks.
            const QRect r = paneDockArea(p, panes[index++].params.rect);
            // A popup has its title in its border: the label keeps the
            // pane's own, which shows around it.
            const bool popup = p->overlay() && p->overlay()->popup();
            titles.append(QVariantMap{{QStringLiteral("x"), r.x() / dpr},
                                      {QStringLiteral("y"), r.y() / dpr},
                                      {QStringLiteral("width"), r.width() / dpr},
                                      {QStringLiteral("height"), r.height() / dpr},
                                      {QStringLiteral("title"), popup ? p->title() : shownTitle(p)},
                                      {QStringLiteral("focused"), p == m_focused},
                                      {QStringLiteral("status"), statusSummary(p)},
                                      {QStringLiteral("context"), p->contextLabel()},
                                      {QStringLiteral("contextKind"), p->contextKind()}});
        });
    }
    m_controller->setPaneTitles(titles);
    QVariantMap history;
    const WindowSettings& windowSettings = m_settings.settings().window;
    const auto focusedEntry = std::find_if(panes.begin(), panes.end(), [this](const auto& wp) {
        return m_focused && wp.snapshot == &m_focused->snapshot();
    });
    // The bar sits on the pane as drawn (it moves with split animations), not over the overview's cards.
    if (focusedEntry != panes.end() && windowSettings.scrollbar != WindowSettings::Scrollbar::Never
        && !m_renderer->overviewShowing()) {
        history = inputPane(m_focused)->historyPosition();
        // A popup's history is its own: the bar goes on its grid, where the
        // pointer reaches it, and not on the pane around it.
        const bool popup = m_focused->overlay() && m_focused->overlay()->popup() && focusedEntry->overlay
            && focusedEntry->overlay->popup;
        const QRectF rect = QRectF(popup ? focusedEntry->overlay->popup->rect : paneDockArea(m_focused, focusedEntry->params.rect));
        // Chrome.qml draws the bar where and as wide as these say (Window.scrollbarWidth).
        history.insert(QStringLiteral("x"), rect.right() / devicePixelRatio() - windowSettings.scrollbarWidth);
        history.insert(QStringLiteral("y"), rect.top() / devicePixelRatio());
        history.insert(QStringLiteral("width"), windowSettings.scrollbarWidth);
        history.insert(QStringLiteral("height"), rect.height() / devicePixelRatio());
        history.insert(QStringLiteral("fade"), windowSettings.scrollbar == WindowSettings::Scrollbar::Auto);
        history.insert(QStringLiteral("hideDelay"), windowSettings.scrollbarHideDelay);
    }
    m_controller->setHistory(history);
    const std::uint32_t clearRgb = m_focused ? m_focused->snapshot().defaultBg : packRgba(0, 0, 0);
    const std::uint32_t clear = (clearRgb & 0x00FFFFFFu) | (std::uint32_t(backgroundAlpha) << 24);
    // Translucency needs a new swapchain. Once content asks for it (a
    // translucent cell background, a script's opacity), it stays: output
    // scrolling such cells in and out must not rebuild the swapchain each
    // time. The new swapchain is made here, so this frame still draws.
    // A script's dock can write a translucent cell background too.
    const auto translucentDock = [](const auto& dock) { return dock.snapshot->hasTransparentBackgrounds; };
    if (!m_contentTranslucent
        && (std::any_of(panes.begin(), panes.end(),
                        [&](const auto& pane) {
                            return pane.params.backgroundAlpha < 255 || pane.snapshot->hasTransparentBackgrounds
                                || (pane.overlay && pane.overlay->snapshot->hasTransparentBackgrounds)
                                || std::any_of(pane.docks.begin(), pane.docks.end(), translucentDock);
                        })
            || std::any_of(m_docks.begin(), m_docks.end(), [](const auto& dock) {
                   return !dock->rect().isEmpty() && dock->snapshot().hasTransparentBackgrounds;
               }))) {
        m_contentTranslucent = true;
        m_renderer->setTranslucent(true);
        if (!m_renderer->ensureSwapChain()) {
            scheduleRender();
            return;
        }
    }
    QRhiTexture* chrome = nullptr;
    if (chromeVisible()) {
        // Qt Quick records its own offscreen frame, so this precedes ours.
        m_chrome->renderIfNeeded(m_renderer->pixelSize());
        chrome = m_chrome->texture();
    }
    std::vector<AfterglowRender::WindowRenderer::Pane> docks;
    for (const auto& dock : m_docks) {
        if (dock->rect().isEmpty()) continue;
        AfterglowRender::WindowRenderer::Pane wp;
        wp.snapshot = &dock->snapshot();
        wp.params.rect = dock->rect();
        wp.params.contentOrigin = dock->rect().topLeft();
        wp.params.backgroundAlpha = backgroundAlpha;
        wp.params.textBlinkOn = m_textBlinkOn;
        anyBlinkingText |= wp.snapshot->hasBlinkingText;
        docks.push_back(wp);
    }
    m_renderer->setDocks(std::move(docks));
    // A swapchain out of date presented nothing: the frame is owed, and output
    // that arrives later may never ask for it. A failed frame (a lost device)
    // is not retried, not even for an animation: every retry would fail the
    // same way and spin the loop.
    using FrameResult = AfterglowRender::WindowRenderer::FrameResult;
    prepareCaptures();
    const FrameResult frame = m_renderer->render(panes, shading, clear, chrome, m_chrome->serial());
    if (frame == FrameResult::Retry)
        scheduleRender();
    // Pointer maps arrive frames after their pack, size or values change:
    // what is under a pointer that stays still moves then.
    if (const std::uint64_t serial = m_renderer->pointerMapSerial(); serial != m_pointerMapSerial) {
        m_pointerMapSerial = serial;
        // A link that changed sets the shape itself.
        if (m_pointer.x() < 0 || m_overview.open || !updateLinkHover()) applyPointerShape();
    }
    updateAudioCapture();
    if (anyBlinkingText && !m_textBlinkTimer.isActive()) {
        m_textBlinkTimer.start();
    } else if (!anyBlinkingText && m_textBlinkTimer.isActive()) {
        m_textBlinkTimer.stop();
        m_textBlinkOn = true;
    }
    // The blink timer runs only while the focused cursor blinks; it stops itself otherwise.
    if (focusedCursorBlinks && m_windowFocused && !m_blinkTimer.isActive()) {
        m_blinkTimer.start();
    }
    if (const QRectF imeRect = inputMethodCursorRect(); imeRect != m_reportedImeRect && isActive()) {
        m_reportedImeRect = imeRect;
        QGuiApplication::inputMethod()->update(Qt::ImCursorRectangle);
    }
    if (std::exchange(m_inputMethodDirty, false) && isActive()) {
        QGuiApplication::inputMethod()->update(Qt::ImQueryInput);
    }
    updateTerminalAccessibility(this);
    if (frame != FrameResult::Failed && (animating || sliding || m_renderer->animating())) {
        scheduleRender(); // something is moving: keep frames coming
    }

    if (holding && (!m_holdRetry.isActive() || m_holdRetry.remainingTime() > m_holdWait)) {
        // A pane is holding its previous frame: look again when the first
        // hold times out. New output re-renders sooner on its own. One look
        // at a time, however many held frames come before it.
        m_holdRetry.start(int(std::clamp<qint64>(m_holdWait, 1, std::numeric_limits<int>::max())));
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
    const auto rectSpec = specFor<QRectF>(motionSpec(anim.splitCurve, anim.splitDuration, m_splitCurve, "Animation.splitCurve",
                                                     AnimationSettings{}.splitCurve));
    if (!m.rectKnown) {
        m.rectKnown = true;
        m.rectTarget = rectTarget;
        const auto grow = m_growFrom.find(pane);
        // Not while the overview shows: its cards do not grow out of a split.
        if (anim.splits && grow != m_growFrom.end() && !overviewShowing()) {
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
    followViewport(pane, m);
    const double followed = m.scrollValue;
    if (m.scroll.isAnimating()) {
        m.scroll.advance();
        m.scrollValue = m.scroll.value() + m.scrollRebase;
        animating |= m.scroll.isAnimating();
    }
    if (!applyScroll(pane, m)) {
        holding = true;
        m_holdWait = std::min(m_holdWait, pane->holdRemaining());
    }
    // A selection being dragged takes in the rows that a wheel, a finger or
    // a coast scrolled under the pointer, as kitty's does: in each frame
    // that draws the view somewhere else, so that it ends on the row drawn
    // under the pointer when the scroll stops. Past the top or bottom rows
    // the auto scroll moves both.
    if (pane == m_selectionPane && m.scrollValue != followed && !m_autoScroll.isActive()) updateSelectionDrag();
    const float shift = m.scrollShift;

    // The cursor glides from cell to cell.
    const AfterglowRender::PaneSnapshot& snap = pane->snapshot();
    const QPointF cellTarget(snap.cursorX * metrics.cellWidth, snap.cursorY * metrics.cellHeight);
    const auto cursorSpec = specFor<QPointF>(motionSpec(anim.cursorCurve, anim.cursorDuration, m_cursorCurve,
                                                        "Animation.cursorCurve", AnimationSettings{}.cursorCurve));
    const bool glide = anim.cursor && snap.cursorVisible;
    // Typing and deleting move the cursor a cell at a time: those moves are
    // immediate, or the cursor would trail behind fast keystrokes. Only
    // jumps (another row, or further along it) glide. No move at all is
    // not a small move: it would restart a glide that is under way.
    const QPointF step = cellTarget - m.cursorTarget;
    const bool smallMove = cellTarget != m.cursorTarget && std::abs(step.y()) < 0.5 && std::abs(step.x()) <= 2.0 * metrics.cellWidth;
    if (!m.cursorKnown || !glide || smallMove) {
        // A session pane's blank replica has its cursor at the top left: the
        // cursor of the first frame drawn is its first sighting, not a move.
        m.cursorKnown = pane->snapshotSynced();
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
    params.contentOrigin = contentRect(pane, shown).topLeft();
    params.cursorPosition = m.cursor.value();
    params.scrollShift = shift;
    params.signals = pane->shaderSignals();
    params.shaderParameters = m_shaderControls.values(pane->id());
    params.id = pane->id();
}

void TerminalWindow::followViewport(Pane* pane, PaneMotion& m)
{
    const auto [offset, written] = pane->withTerminal([](Terminal& t) {
        const auto& screen = t.screen();
        return std::pair{screen.viewportOffset(), std::uint64_t(screen.rowsDropped() + screen.historySize())};
    });
    std::uint64_t& seen = m_historyRows[pane];
    const std::uint64_t added = written >= seen ? written - seen : 0;
    seen = written;
    if (offset == m.appliedOffset) return;
    const qint64 moved = qint64(offset) - qint64(m.appliedOffset);
    if (moved > 0 && std::uint64_t(moved) == added) {
        // Output entered the history while the view is scrolled back, and
        // the terminal moved the view by as much to keep it on the same
        // text: the view stays where it was on that text, with its part of
        // a row (a finger scroll's, a coast's), and an easing goes on,
        // shifted, at its own pace.
        if (m.scroll.isAnimating()) m.scrollRebase += qreal(moved);
        m.scrollValue += qreal(moved);
        m.scrollTarget += qreal(moved);
    } else {
        // Moved by something else (typing, a prompt jump): follow it, and
        // a coast ends.
        stopMomentumOf(pane);
        m.scroll.cancel();
        m.scrollRebase = 0;
        m.scrollValue = qreal(offset);
        m.scrollTarget = qreal(offset);
    }
    m.appliedOffset = offset;
}

bool TerminalWindow::applyScroll(Pane* pane, PaneMotion& m)
{
    const auto rowsBack = std::size_t(std::ceil(std::max<qreal>(0, m.scrollValue - 1e-3)));
    if (rowsBack != m.appliedOffset) {
        m.appliedOffset = pane->withTerminal([rowsBack](Terminal& t) {
            t.setViewportOffset(rowsBack);
            return t.screen().viewportOffset();
        });
    }
    m.scrollShift = float((qreal(m.appliedOffset) - m.scrollValue) * m_renderer->fonts().fonts->metrics().cellHeight);
    if (!pane->refreshSnapshot(m.scrollShift > 0.5f ? 1 : 0)) return false;
    // Labels found on a held frame: this frame is newer than the held one,
    // so it shows other text. Before the chrome draws, so no frame shows
    // them over that text.
    if (m_urlMode.held && pane == m_urlMode.pane) stopUrlMode();
    return true;
}

void TerminalWindow::settleScroll(Pane* pane)
{
    // A mode that reads the rows as drawn (hints, URLs) holds the view: a
    // coast would move it.
    stopMomentumOf(pane);
    PaneMotion& m = motion(pane);
    followViewport(pane, m);
    if (m.scroll.isAnimating()) {
        m.scroll.cancel();
        m.scrollValue = m.scrollTarget;
    }
    applyScroll(pane, m);
    scheduleRender();
}

bool TerminalWindow::scrollPane(Pane* pane, double rows, bool animate)
{
    if (!pane) {
        return false;
    }
    PaneMotion& m = motion(pane);
    // Hint labels sit on the rows the view had.
    if (m_urlMode.active && m_urlMode.pane == pane) stopUrlMode();
    const auto history = double(pane->withTerminal([](Terminal& t) { return t.screen().historySize(); }));
    const double base = m.scroll.isAnimating() ? m.scrollTarget : m.scrollValue;
    const double target = std::clamp(base + rows, 0.0, history);
    if (target == base) return false;
    m.scrollTarget = target;
    const AnimationSettings& anim = m_settings.settings().animation;
    if (!animate || !anim.scroll) {
        m.scroll.cancel();
        m.scrollValue = target;
    } else if (m.scroll.isAnimating()) {
        m.scroll.retarget(target - m.scrollRebase, PhosphorAnimation::RetargetPolicy::PreserveVelocity);
    } else {
        m.scrollRebase = 0;
        m.scroll.start(m.scrollValue, target, motionSpec(anim.scrollCurve, anim.scrollDuration, m_scrollCurve,
                                                         "Animation.scrollCurve", AnimationSettings{}.scrollCurve));
    }
    scheduleRender();
    return true;
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
        if (m_controller->wantsKeyboard()) { m_chrome->queryInputMethod(q); return true; }
        const Qt::InputMethodQueries queries = q->queries();
        if (queries & Qt::ImEnabled) {
            // A mode that holds the keyboard takes plain keys, not composed text.
            q->setValue(Qt::ImEnabled, !keyboardModeActive());
        }
        if (queries & Qt::ImCursorRectangle) {
            q->setValue(Qt::ImCursorRectangle, inputMethodCursorRect());
        }
        if (queries & Qt::ImHints) {
            q->setValue(Qt::ImHints, int(Qt::ImhNoPredictiveText | Qt::ImhNoAutoUppercase));
        }
        // The cursor's row, read under the terminal's lock: only for a query
        // that asks for text. Most ask for the cursor rectangle alone.
        constexpr Qt::InputMethodQueries textQueries = Qt::ImSurroundingText | Qt::ImCursorPosition | Qt::ImAnchorPosition
            | Qt::ImTextBeforeCursor | Qt::ImTextAfterCursor;
        const TerminalText context = m_focused && (queries & textQueries) ? inputPane(m_focused)->accessibleText(true) : TerminalText{};
        if (queries & Qt::ImSurroundingText) q->setValue(Qt::ImSurroundingText, context.text);
        if (queries & Qt::ImCursorPosition) q->setValue(Qt::ImCursorPosition, std::max(0, context.cursor));
        if (queries & Qt::ImAnchorPosition) q->setValue(Qt::ImAnchorPosition, std::max(0, context.cursor));
        if (queries & Qt::ImCurrentSelection) q->setValue(Qt::ImCurrentSelection, QString());
        if (queries & Qt::ImTextBeforeCursor) q->setValue(Qt::ImTextBeforeCursor, context.text.left(std::max(0, context.cursor)));
        if (queries & Qt::ImTextAfterCursor) q->setValue(Qt::ImTextAfterCursor, context.text.mid(std::max(0, context.cursor)));
        q->accept();
        return true;
    }
    case QEvent::InputMethod: {
        auto* ime = static_cast<QInputMethodEvent*>(e);
        if (m_controller->wantsKeyboard()) {
            m_chrome->deliver(e);
            return true;
        }
        // The overview, hints, copy mode, Unicode input and script key modes
        // own the keyboard: composed text must not slip past them to the shell.
        if (keyboardModeActive()) {
            ime->accept();
            return true;
        }
        // Terminal output is not an editable document. Never translate arbitrary
        // IME replacement ranges into destructive shell keystrokes.
        // The text that came with such an edit goes too: without the
        // deletion it would be typed next to what it was to replace. The
        // log says that it happened, without the text.
        if (ime->replacementStart() != 0 || ime->replacementLength() != 0) {
            qCDebug(lcWindow) << "ignored an input method edit of the text around the cursor:" << ime->replacementLength()
                              << "UTF-16 units at" << ime->replacementStart() << "and" << ime->commitString().size()
                              << "UTF-16 units to commit, with its preedit";
            ime->ignore();
            return true;
        }
        if (Pane* target = inputPane(m_focused)) {
            if (!ime->commitString().isEmpty() && target->isHeld()) {
                closePane(target);
                ime->accept();
                return true;
            }
            if (!ime->commitString().isEmpty()) {
                target->clearSelection();
                noteTyping();
                target->sendInput(ime->commitString().toUtf8().toStdString());
            }
            int caret = int(ime->preeditString().size());
            for (const QInputMethodEvent::Attribute& a : ime->attributes()) {
                if (a.type == QInputMethodEvent::Cursor) {
                    caret = a.start;
                }
            }
            target->setPreedit(ime->preeditString(), caret);
            scheduleRender();
        }
        ime->accept();
        return true;
    }
    case QEvent::DragEnter:
    case QEvent::DragMove:
    case QEvent::Drop: {
        auto* drop = static_cast<QDropEvent*>(e);
        if (overviewShowing()) {
            // The cards are not where the layout puts the panes: no drop target.
            for (auto* pane : terminalPanes()) if (pane->hasDragDrop()) pane->dragDrop()->leave();
            drop->ignore();
            return true;
        }
        // A dock is its script's: the pane under it takes no drop.
        Pane* target = pointerTarget(paneUnder(drop->position()), drop->position());
        // The program's commands that wait decide whether the pane takes the
        // drag (t=a, t=A) before the event goes to the protocol or the paste.
        if (target) target->deliverProtocolCommands();
        // Only the handlers a program started (OSC 72): a pane that never
        // asked for drag and drop accepts and offers nothing.
        for (auto* pane : terminalPanes()) if (pane != target && pane->hasDragDrop()) pane->dragDrop()->leave();
        // A selection dragged back onto its own pane came back to where it
        // started: neither its program nor a paste gets it. The source is the
        // window that runs the drag, which need not be this one.
        const auto* from = qobject_cast<const TerminalWindow*>(drop->source());
        if (target && from && from->m_textDragSource && hostOf(target) == from->m_textDragSource) {
            if (target->hasDragDrop()) target->dragDrop()->leave();
            drop->ignore();
            return true;
        }
        if (target && target->hasDragDrop() && !target->isHeld() && !m_controller->wantsKeyboard() && target->dragDrop()->acceptsDrops()) {
            const QPoint pixel = cellPixel(target, drop->position());
            // The drop waits while the program reads it, in an event loop.
            const NestedLoop receiving(this);
            target->dragDrop()->drop(drop, e->type() == QEvent::Drop, cellAt(target, drop->position()), pixel);
            return true;
        }
        // The pane's program does not get this event: it is not over the pane now.
        if (target && target->hasDragDrop()) target->dragDrop()->leave();
        // Files paste as their quoted paths. A drop with a file URL is a file
        // drop whatever else it holds: Qt gives the URLs as its text, and a
        // file list that droppedFiles() refuses pastes nothing. Other text,
        // links from a browser too, pastes with the size limit and the
        // question of a paste, as in Kitty. The types decide first: reading
        // the data costs. A drag of this process is read only at the drop: a
        // program's offer (OSC 72) asks the program for its data each time
        // it is read, and the offer's own pane takes none of it. Text is read
        // only at the drop, as it waits for the source.
        const QMimeData* mime = drop->mimeData();
        const bool dropping = e->type() == QEvent::Drop;
        if (!target || target->isHeld() || m_controller->wantsKeyboard() || !mime->hasText()
            || (target->hasDragDrop() && target->dragDrop()->dragging()) || !(drop->possibleActions() & Qt::CopyAction)) {
            drop->ignore();
            return true;
        }
        const QPointer<Pane> pane(target);
        bool fileDrop = false;
        QString paste;
        {
            // Reading a program's offer runs an event loop until the program
            // sends its data: the pane can go meanwhile, and the window only
            // after the loop.
            const NestedLoop reading(this);
            if (dropping || !drop->source()) {
                const QList<QUrl> urls = mime->hasUrls() ? mime->urls() : QList<QUrl>();
                fileDrop = std::any_of(urls.cbegin(), urls.cend(), [](const QUrl& url) { return url.isLocalFile(); });
                if (fileDrop) paste = droppedFiles(urls);
            }
            if (dropping && !fileDrop) paste = mime->text();
        }
        // The read may have run an event loop: what decided the drop is checked again.
        if (!pane || !terminalPanes().contains(pane.data()) || !m_splits || !m_splits->contains(hostOf(pane.data()))
            || pane->isHeld() || m_controller->wantsKeyboard()
            || overviewShowing() || (paste.isEmpty() && (fileDrop || dropping))) {
            drop->ignore();
            return true;
        }
        drop->setDropAction(Qt::CopyAction);
        drop->accept();
        if (dropping) {
            // The pane in the tab takes the focus; its overlay gets the paste.
            setFocusedPane(hostOf(pane));
            pasteText(pane, paste.toUtf8());
            // The keys that answer the paste question must come to this window.
            if (m_controller->pasteConfirmOpen() && !isActive()) requestActivate();
        }
        return true;
    }
    case QEvent::DragLeave:
        for (auto* pane : terminalPanes()) if (pane->hasDragDrop()) pane->dragDrop()->leave();
        e->accept();
        return true;
    case QEvent::Enter:
        // A pointer that enters and stays still sends no move: what it is
        // on shows now.
        m_pointer = static_cast<QEnterEvent*>(e)->position();
        m_renderer->setMousePosition(m_pointer * devicePixelRatio());
        // No link over the overview's cards, as no move finds one there.
        if (!m_overview.open) updateLinkHover();
        applyPointerShape();
        if (m_readsMouse) scheduleRender();
        break;
    case QEvent::Leave:
        // Packs read a position outside the window as "no pointer".
        m_renderer->setMousePosition(QPointF(-1, -1));
        m_hover = {};
        m_pointer = QPointF(-1, -1);
        updateDockHover();
        leaveChrome();
        scheduleRender();
        break;
    case QEvent::PlatformSurface:
        if (static_cast<QPlatformSurfaceEvent*>(e)->surfaceEventType()
            == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed) {
            m_renderer->releaseSwapChain();
        }
        break;
    case QEvent::Close: {
        // The jobs that the question would name: a name that is all control
        // characters names nothing, and asks nothing. Read once: a second
        // read could find none, and close the window inside its own close.
        const QList<Pane*> all = panes();
        if (const QStringList jobs = m_closeConfirmed ? QStringList() : closingJobs(all); !jobs.isEmpty()) {
            e->ignore();
            confirmClose(jobs, all, QStringLiteral("this window"), [this] {
                m_closeConfirmed = true;
                close();
            });
            return true;
        }
        qCInfo(lcWindow) << "window close requested";
        // The session's tabs as they are now, for the next window; then
        // Mux.onClose says what goes.
        if (m_session) {
            sendSessionLayout();
            for (Pane* p : all) p->setSessionRelease(windowCloseRelease());
        }
        // A drag of this window (a selection, or a program's offer) ends first.
        const auto terminals = terminalPanes();
        if (m_textDragging || std::any_of(terminals.cbegin(), terminals.cend(), [](Pane* p) {
                return p->hasDragDrop() && p->dragDrop()->dragging();
            })) {
            QDrag::cancel();
        }
        const bool handled = QWindow::event(e);
        // The scripts still run: a drag on a dock ends, and the item under
        // the pointer is left.
        dropDockPointer(nullptr);
        // A drag or a drop may run this event in its event loop, from a
        // handler of this window: the window goes once that handler returns.
        NestedLoop::deleteWhenIdle(this);
        return handled;
    }
    default:
        break;
    }
    return QWindow::event(e);
}

void TerminalWindow::exposeEvent(QExposeEvent*)
{
    // Every repaint and resize exposes again: only a change is news.
    if (isExposed() != std::exchange(m_accessibleExposed, isExposed())) {
        QAccessibleEvent accessibleEvent(this, isExposed() ? QAccessible::ObjectShow : QAccessible::ObjectHide);
        QAccessible::updateAccessibility(&accessibleEvent);
    }
    updateAudioCapture();
    if (isExposed() && !m_effectsApplied) {
        m_effectsApplied = true;
        applyWindowEffects();
    }
    if (isExposed()) {
        renderNow();
    } else {
        // No frame comes while hidden.
        abortCaptures(QStringLiteral("the window is not on screen"));
        if (m_textBlinkTimer.isActive()) {
            // Blinking text waits for the next frame.
            m_textBlinkTimer.stop();
            m_textBlinkOn = true;
        }
    }
    updatePaneVisibility();
}

std::vector<Pane*> TerminalWindow::overviewCards() const
{
    return overviewShowing() ? overviewPanes() : std::vector<Pane*>{};
}

bool TerminalWindow::paneSeen(const Pane* pane, const std::vector<Pane*>& cards) const
{
    if (!pane || !isExposed() || windowState() == Qt::WindowMinimized) return false;
    // The overview draws the panes of every tab as live cards, up to its
    // limit: those are what the user sees, the current tab's too.
    if (overviewShowing()) return std::find(cards.begin(), cards.end(), pane) != cards.end();
    return tabOf(pane) == m_currentTab && m_splits && m_splits->isVisible(pane);
}

void TerminalWindow::updatePaneVisibility()
{
    const std::vector<Pane*> cards = overviewCards();
    const bool shows = isExposed() && windowState() != Qt::WindowMinimized;
    for (std::size_t i = 0; i < m_tabs.size(); ++i) {
        const SplitTree* tree = m_tabs[i]->splits.get();
        if (!tree) continue;
        // The tab that a switch leaves is drawn until the switch ends: its
        // animations run on, while the window shows.
        const bool leaving = shows && m_tabTransition.progress.isAnimating() && m_tabs[i]->id == m_tabTransition.from;
        tree->forEachPane([&](Pane* p, const QRect&) {
            const bool visible = paneSeen(p, cards);
            p->setSeen(visible || leaving);
            // A session pane's replica must not answer its program: the
            // daemon's terminal answers the program's own queries.
            if (p->isSessionPane()) p->setSessionVisible(visible);
            else p->withTerminal([visible](Terminal& t) { t.setVisible(visible); });
            // An overlay shows where its host does, on a card too.
            if (PaneOverlay* overlay = p->overlay()) {
                overlay->pane()->setSeen(visible || leaving);
                overlay->pane()->withTerminal([visible](Terminal& t) { t.setVisible(visible); });
            }
        });
    }
}

void TerminalWindow::resizeEvent(QResizeEvent*)
{
    scheduleRender();
}

void TerminalWindow::focusInEvent(QFocusEvent*)
{
    if (m_focused && !m_controller->wantsKeyboard()) { QAccessibleEvent event(m_focused, QAccessible::Focus); QAccessible::updateAccessibility(&event); }
    m_windowFocused = true;
    if (Pane* target = inputPane(m_focused)) reportFocus(target, true);
    restartBlink();
    acknowledgeStatus();
}

void TerminalWindow::focusOutEvent(QFocusEvent*)
{
    // As kitty: a coast is the focused window's (advanceMomentum).
    stopMomentum();
    cancelKeyMode();
    m_controller->closeContextMenu();
    m_windowFocused = false;
    m_blinkTimer.stop();
    m_blinkOn = true;
    // The link modifier's release may go to another window.
    if (!m_hover.url.isEmpty() || m_hover.pane) {
        m_hover = {};
        applyPointerShape();
    }
    if (Pane* target = inputPane(m_focused)) reportFocus(target, false);
    scheduleRender();
}

void TerminalWindow::cancelKeyMode()
{
    m_keyMode = {};
    updateKeyMode();
}

void TerminalWindow::updateKeyMode()
{
    m_controller->setKeyMode(m_scriptKeyMode ? m_scriptKeyMode->label
                             : m_overview.open ? tr("Panes · arrows choose · Enter or 1–9 focus · Esc to cancel")
                             : m_copyPane ? tr("Copy · arrows/hjkl · v select · y copy · Esc to cancel")
                             : m_keyMode.active() ? tr("%1 · Esc to cancel").arg(m_keyMode.label()) : QString());
    m_keyModeTimer.stop();
    if (m_keyMode.active()) m_keyModeTimer.start(int(std::max(qint64(1), m_keyMode.deadline - m_keyModeClock.elapsed())));
    // The overview, copy mode and script key modes start and end here.
    syncInputMethodEnabled();
    scheduleRender();
}

bool TerminalWindow::keyboardModeActive() const
{
    return m_overview.open || m_urlMode.active || m_unicodeInput || m_scriptKeyMode || m_copyPane;
}

void TerminalWindow::refreshInputMethod(Qt::InputMethodQueries queries)
{
    if (isActive()) QGuiApplication::inputMethod()->update(queries);
}

void TerminalWindow::syncInputMethodEnabled()
{
    if (const bool held = keyboardModeActive(); held != m_modeHeldKeyboard) {
        m_modeHeldKeyboard = held;
        if (held) {
            // A composition in progress would stay drawn on the pane (and its
            // card) for the whole mode: the input method's own clearing event,
            // if it sends one, arrives while the mode swallows its events.
            if (m_focused) inputPane(m_focused)->setPreedit({}, 0);
            if (isActive()) QGuiApplication::inputMethod()->reset();
        }
        refreshInputMethod(Qt::ImEnabled);
    }
}

bool TerminalWindow::handleShortcut(QKeyEvent* e)
{
    const bool sequenceWasActive = m_keyMode.active();
    const auto result = m_settings.settings().keyModes.press(e, m_keyMode, m_keyModeClock.elapsed(), bindingBaseKey(e));
    // Only a key in a sequence or a table changes what the window shows for
    // it: any other key would cost a frame.
    if (sequenceWasActive || m_keyMode.active()) updateKeyMode();
    // A held chord runs these once: each repeat would open, split, close or
    // move one more (a moved pane finds itself alone in its new tab, and
    // shows the refusal again), open the configuration again, or turn a
    // toggle back, or reload and show the problems again. The repeat is
    // taken all the same: it is no key for the program. Focus, resize,
    // scroll, font size and the tab and prompt moves repeat.
    static constexpr std::array onceWhileHeld{Action::NewTab,           Action::NewWindow,      Action::SplitRight,
                                              Action::SplitDown,        Action::SplitLeft,      Action::SplitUp,
                                              Action::ClosePane,        Action::CloseTab,       Action::MovePaneToTab,
                                              Action::MovePaneToWindow, Action::DetachTab,      Action::ShowLastOutput,
                                              Action::OpenConfig,       Action::ReloadConfig,   Action::ToggleFullscreen,
                                              Action::TogglePaneZoom,   Action::NotifyOnFinish};
    const auto heldRepeat = [e](Action action) {
        return e->isAutoRepeat() && std::find(onceWhileHeld.begin(), onceWhileHeld.end(), action) != onceWhileHeld.end();
    };
    // The press of a chord whose action had nothing to do (fullscreen in a
    // quick terminal) reached the program: so do its repeats, which the
    // program would otherwise get none of between its press and release.
    const bool pressSent = e->isAutoRepeat() && m_sentKeys.contains(physicalKeyOf(e));
    // Text a binding types, from a key mode or a chord alike.
    const auto typeText = [this, e](const QByteArray& text) {
        Pane* target = inputPane(m_focused);
        // --hold: its program has ended, and a key closes it, as sendKey
        // does. The press is taken as no shortcut: its repeats must not type
        // the text into the pane focused next.
        if (target->isHeld()) {
            m_consumedKeys.insert(physicalKeyOf(e), false);
            closePane(target);
            return;
        }
        target->clearSelection();
        restartBlink();
        target->sendInput(std::string_view(text.constData(), std::size_t(text.size())));
    };
    if (result.consumed) {
        if (result.target.startsWith(Keybinds::TextPrefix) && m_focused) {
            typeText(Keybinds::unescape(result.target.mid(Keybinds::TextPrefix.size())));
        } else if (result.target.startsWith(Keybinds::ScriptPrefix)) {
            // The sequence took its keys already, so a command that no
            // script has is reported here, not typed. A run is a process
            // that a held key must not start again with each repeat.
            if (!e->isAutoRepeat())
                runScriptCommand(result.target.mid(Keybinds::ScriptPrefix.size()), m_focused, QStringLiteral("key"));
        } else {
            for (Action action : Keybinds::allActions()) {
                if (Keybinds::token(action) == result.target) {
                    if (!heldRepeat(action)) performAction(action);
                    else if (pressSent) return false;
                    break;
                }
            }
        }
        return true;
    }
    const Keybinds& kb = m_settings.settings().keybinds;
    const Action action = kb.match(e, bindingBaseKey(e));
    if (action != Action::None) {
        if (heldRepeat(action)) return !pressSent;
        // These keep their key even when there is nothing to do: the
        // encoded chord would reach the shell as a control byte (the
        // overview's Ctrl+Shift+D as EOF, a rotation's as ESC or GS).
        constexpr std::array keepsKey{Action::PaneOverview, Action::RotatePanesClockwise, Action::RotatePanesCounterclockwise,
                                      Action::SwapLeft, Action::SwapRight, Action::SwapUp, Action::SwapDown,
                                      Action::UsePackageShaders, Action::TogglePaneZoom, Action::MovePaneToTab};
        return performAction(action) || std::find(keepsKey.begin(), keepsKey.end(), action) != keepsKey.end();
    }
    if (const QByteArray text = kb.matchText(e, bindingBaseKey(e)); !text.isEmpty()) {
        if (!m_focused) {
            return false;
        }
        typeText(text);
        return true;
    }
    // A key bound to a command that no script has goes to the terminal.
    const QString script = kb.matchScript(e, bindingBaseKey(e));
    if (script.isEmpty() || !scriptCommand(script)) return false;
    // Taken, but not run again for each repeat of a held key.
    if (!e->isAutoRepeat()) runScriptCommand(script, m_focused, QStringLiteral("key"));
    return true;
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
    // The pane's own terminal, or its overlay's while one runs over it.
    Pane* const target = inputPane(m_focused);
    auto scroll = [target, this](long delta) {
        if (!target) {
            return false;
        }
        // Full-screen programs get these keys; there is no history to scroll.
        if (target->withTerminal([](Terminal& t) { return t.modes().altScreen; })) {
            return false;
        }
        // A scroll from a key, the menu, a script or afterglow ctl ends a coast.
        stopMomentumOf(target);
        scrollPane(target, double(delta));
        return true;
    };
    const long page = target ? std::max(1, target->rows() - 1) : 1;

    switch (action) {
    case Action::SaveProject:
    case Action::SwitchProject:
        if (!m_focused) return false;
        Q_EMIT projectRequested(m_focused, action == Action::SaveProject);
        return true;
    case Action::SwapLeft: return swapPane(m_focused, SplitTree::Neighbour::Left);
    case Action::SwapRight: return swapPane(m_focused, SplitTree::Neighbour::Right);
    case Action::SwapUp: return swapPane(m_focused, SplitTree::Neighbour::Up);
    case Action::SwapDown: return swapPane(m_focused, SplitTree::Neighbour::Down);
    case Action::RotatePanesClockwise: return rotatePanes(true);
    case Action::RotatePanesCounterclockwise: return rotatePanes(false);
    case Action::PaneOverview: return openOverview();
    case Action::UsePackageShaders: return usePackageShaders();
    case Action::MovePaneToTab: {
        if (!m_focused) return false;
        // A refused move fails (afterglow ctl and scripts get an error) and
        // keeps its key (keepsKey in handleShortcut). The user hears why.
        const QString error = movePaneTo(m_focused, this, nullptr, SplitTree::Direction::Right);
        if (!error.isEmpty()) NotificationCenter::instance().showNotice(m_focused, tr("Could not move the pane"), error);
        return error.isEmpty();
    }
    case Action::MovePaneToWindow:
    case Action::DetachTab: {
        if (!m_focused) return false;
        // A transfer can close this window: the palette's or the menu's
        // handler that runs this returns first, as for detachTab.
        const bool wholeTab = action == Action::DetachTab;
        QTimer::singleShot(0, this, [this, pane = QPointer<Pane>(m_focused.data()), wholeTab] {
            if (pane && tabOf(pane) >= 0) Q_EMIT transferRequested(pane, wholeTab);
        });
        return true;
    }

    case Action::TogglePaneZoom:
        // The overview restores the zooms that it saw at its open: one made
        // under it would stay, and hide cards of the open deck.
        if (!m_focused || m_splits->paneCount() < 2 || m_overview.open) return false;
        setPaneZoom(m_splits->zoomedPane() ? nullptr : m_focused.data());
        return true;
    case Action::ResizeLeft:
        return resizeFocused(SplitTree::Neighbour::Left);
    case Action::ResizeRight:
        return resizeFocused(SplitTree::Neighbour::Right);
    case Action::ResizeUp:
        return resizeFocused(SplitTree::Neighbour::Up);
    case Action::ResizeDown:
        return resizeFocused(SplitTree::Neighbour::Down);
    case Action::SplitRight:
    case Action::SplitDown:
    case Action::SplitLeft:
    case Action::SplitUp: {
        const SplitTree::Direction direction = action == Action::SplitRight ? SplitTree::Direction::Right
            : action == Action::SplitDown                                   ? SplitTree::Direction::Down
            : action == Action::SplitLeft                                   ? SplitTree::Direction::Left
                                                                            : SplitTree::Direction::Up;
        QString error;
        if (m_focused && !splitFocused(direction, {}, {}, &error)) {
            NotificationCenter::instance().showNotice(m_focused, tr("Could not split the pane"), error);
        }
        return true;
    }
    case Action::ClearScreen:
        if (target) {
            target->clearScreen();
            scheduleRender();
        }
        return true;
    case Action::ResetTerminal:
        if (target) {
            target->resetTerminal();
            scheduleRender();
        }
        return true;
    case Action::RenamePane:
        if (m_focused) {
            m_titlePane = m_focused;
            m_titleTab = 0;
            m_controller->askTitle(QStringLiteral("Split title"), m_focused->userTitle());
        }
        return true;
    case Action::RenameTab:
        renameTab(m_currentTab);
        return true;
    case Action::MoveTabLeft:
        moveTab(m_currentTab, m_currentTab - 1);
        return true;
    case Action::MoveTabRight:
        moveTab(m_currentTab, m_currentTab + 1);
        return true;
    case Action::GotoTab1:
    case Action::GotoTab2:
    case Action::GotoTab3:
    case Action::GotoTab4:
    case Action::GotoTab5:
    case Action::GotoTab6:
    case Action::GotoTab7:
    case Action::GotoTab8:
        if (m_tabs.size() < 2 || int(action) - int(Action::GotoTab1) >= int(m_tabs.size())) {
            return false; // no such tab: the key goes to the program
        }
        switchTab(int(action) - int(Action::GotoTab1));
        return true;
    case Action::GotoTab9:
        if (m_tabs.size() < 2) {
            return false;
        }
        switchTab(int(m_tabs.size()) - 1);
        return true;
    case Action::CloseWindow:
        close();
        return true;
    case Action::NotifyOnFinish:
        if (m_focused) {
            if (!m_notifyOnFinish.remove(m_focused.data())) {
                m_notifyOnFinish.insert(m_focused.data());
            }
        }
        return true;
    case Action::ClosePane:
        if (m_focused) {
            const QPointer<Pane> pane = m_focused.data();
            const bool alone = tabOf(pane) >= 0 && m_tabs[std::size_t(tabOf(pane))]->splits->paneCount() == 1;
            const QString what = !alone ? QStringLiteral("this split")
                : m_tabs.size() == 1    ? QStringLiteral("this window")
                                        : QStringLiteral("this tab");
            confirmClose({pane}, what, [this, pane] {
                if (pane) {
                    closePane(pane);
                }
            });
        }
        return true;
    case Action::NewWindow:
        Q_EMIT newWindowRequested(inheritedDirectory(m_focused));
        return true;
    case Action::NewTab:
        openShellTab(m_focused);
        return true;
    case Action::EndScriptKeyMode:
        // Always ends a script's key mode, so no script can keep the keyboard.
        if (!m_scriptKeyMode) return false;
        m_scriptKeyMode.reset();
        updateKeyMode();
        return true;
    case Action::CancelOverlay:
        // The user's way out of an overlay whose program ignores its keys.
        if (!m_focused || !m_focused->overlay()) return false;
        m_focused->overlay()->cancel(QStringLiteral("The user cancelled the overlay"));
        return true;
    case Action::CloseTab:
        requestCloseTabs({m_currentTab});
        return true;
    case Action::ToggleFullscreen:
        if (m_quick) {
            return false;
        }
        setWindowStates(windowStates() ^ Qt::WindowFullScreen);
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
    case Action::ShowHints:
        // The overview and a script's key mode keep the keys that the labels need.
        if (m_overview.open || m_scriptKeyMode) return false;
        startHints();
        return true;
    case Action::ShowLastOutput: {
        if (!m_focused) return false;
        // Handled from here on: a failure is a notification, not a key for the program.
        const auto fail = [this](const QString& title, const QString& body) {
            NotificationCenter::instance().showNotice(m_focused, title, body);
            return true;
        };
        auto command = QProcess::splitCommand(m_settings.settings().pager);
        if (command.isEmpty() || QStandardPaths::findExecutable(command.front()).isEmpty()) {
            return fail(tr("No pager"), command.isEmpty() ? tr("Terminal.pager is empty.")
                                                          : tr("The pager %1 was not found.").arg(command.front()));
        }
        const std::size_t limit = m_settings.settings().lastOutputLimit;
        const auto output = m_focused->readText(Terminal::TextScope::LastOutput, limit);
        if (m_focused->lastOutputArriving()) {
            return fail(tr("No command output"), tr("The output of the command is still on its way from the session. Try again in a moment."));
        }
        if (!output.value(QStringLiteral("available")).toBool()) {
            return fail(tr("No command output"), tr("Shell integration has not recorded a command yet."));
        }
        auto file = std::make_unique<QTemporaryFile>();
        const QByteArray bytes = output.value(QStringLiteral("text")).toString().toUtf8();
        if (!file->open() || file->write(bytes) != bytes.size() || !file->flush()) {
            return fail(tr("Could not show the output"), tr("The temporary file could not be written."));
        }
        if (output.value(QStringLiteral("truncated")).toBool()) {
            file->write(QStringLiteral("\n[Afterglow: output limited to %1]\n").arg(QLocale::c().formattedDataSize(qint64(limit))).toUtf8());
        }
        const QString path = file->fileName();
        file->close();
        command << path;
        QString error;
        Pane* created = newTab(focusedDirectory(), command, {}, &error);
        if (!created) return fail(tr("Could not show the output"), error);
        // The file goes with the pane: a project saves the tab as a shell in
        // its directory, not as a pager of a file that is gone.
        created->setLaunch(created->launchDirectory(), {});
        file->setParent(created);
        file.release();
        return true;
    }
    case Action::CopyMode:
        if (!m_focused) return false;
        if (m_copyPane) stopCopyMode();
        else {
            // Its keys come before the overview's and a script's key mode:
            // not over them. The other modes end, as for the overview.
            if (m_overview.open || m_scriptKeyMode) return false;
            cancelKeyMode(); stopUrlMode(); stopUnicodeInput(); stopMomentumOf(target);
            m_copyPane = target; m_copyPane->beginCopyMode(); updateKeyMode();
        }
        return true;
    case Action::Copy:
        copySelection(true);
        return true;
    case Action::Paste:
        paste(QClipboard::Clipboard);
        return true;
    case Action::SelectAll:
        if (target) {
            target->selectAll();
            copyOnSelect(target);
            scheduleRender();
        }
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
        // A window that gave its last tab away has no tree until it closes.
        if (!m_splits) return false;
        m_splits->equalize();
        m_layoutSize = QSize();
        scheduleRender();
        return true;
    case Action::FontBigger:
        m_device.adjustFontSize(float(m_settings.settings().fontSizeStep));
        return true;
    case Action::FontSmaller:
        m_device.adjustFontSize(-float(m_settings.settings().fontSizeStep));
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
        // The actions that apply now: Cancel Overlay only over an overlay.
        if (!m_controller->paletteOpen()) updatePaletteActions();
        m_controller->setPaletteOpen(!m_controller->paletteOpen());
        return true;
    case Action::Search:
        m_controller->setSearchOpen(true);
        return true;
    case Action::JumpPromptUp:
    case Action::JumpPromptDown:
        // As the scroll keys: a full-screen program gets the key, as the
        // alternate screen has no prompts to jump to.
        if (!target || target->withTerminal([](Terminal& t) { return t.modes().altScreen; })) return false;
        if (target->jumpToPrompt(action == Action::JumpPromptUp ? -1 : 1)) {
            scheduleRender();
        }
        return true;
    case Action::ReloadConfig: {
        const QStringList before = m_settings.problems();
        m_settings.reload();
        // Asked for: problems that did not change show again (a changed
        // list has shown through problemsChanged).
        if (m_settings.problems() == before) reportProblems(ProblemSource::Config, before, true);
        return true;
    }
    case Action::OpenConfig:
        // No file until the first write (sparse persistence): an editor
        // needs one to open.
        if (!m_settings.ensureFile()) {
            NotificationCenter::instance().showNotice(m_focused, tr("Could not open the configuration"),
                tr("%1 could not be created.").arg(m_settings.path()));
            return true;
        }
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_settings.path()));
        return true;
    case Action::UrlsOpen:
    case Action::UrlsCopy:
        // The overview and a script's key mode keep the keys that the labels need.
        if (m_overview.open || m_scriptKeyMode) return false;
        startUrlMode(action == Action::UrlsCopy);
        return true;
    case Action::UnicodeInput:
        if (m_focused) {
            // The overview and a script's key mode keep the keys they hold.
            if (m_overview.open || m_scriptKeyMode) return false;
            // Labels, copy-mode keys and a sequence would take the hex digits.
            stopUrlMode(); stopCopyMode(); cancelKeyMode();
            m_unicodeInput = QString();
            showUnicodeInput();
        }
        return true;
    default:
        // Bound to a feature this window does not provide: let the key through.
        return false;
    }
}

bool TerminalWindow::chromeVisible() const
{
    return tabBarHeight() > 0 || m_controller->paletteOpen() || m_controller->searchOpen() || m_controller->contextMenuOpen()
        || m_controller->titlePromptOpen() || m_controller->confirmPromptOpen()
        || m_controller->pasteConfirmOpen() || m_controller->clipboardPromptOpen() || !m_controller->paneTitles().isEmpty()
        || m_urlMode.active || m_unicodeInput.has_value() || m_scriptDialog.has_value()
        || !m_controller->scriptStatus().isEmpty() || !m_controller->keyMode().isEmpty()
        || m_controller->history().value(QStringLiteral("size"), 1.0).toDouble() < 1.0;
}

void TerminalWindow::updateChrome()
{
    QStringList titles;
    QStringList scriptStatus;
    for (const auto& item : m_scriptAppearance) if (item.pane == m_focused && !item.status.isEmpty()) scriptStatus << item.status;
    m_controller->setScriptStatus(scriptStatus.join(QStringLiteral(" · ")));
    QVariantList tabStatus;
    QVariantList tabContexts;
    for (const auto& tab : m_tabs) {
        Pane* focused = tab->splits.get() == m_splits ? m_focused : tab->focused;
        titles.append(tabTitle(*tab, focused));
        tabContexts.append(QVariantMap{{QStringLiteral("label"), focused ? focused->contextLabel() : QString()},
                                       {QStringLiteral("kind"), focused ? focused->contextKind() : QString()}});
        // The tab shows the most pressing status among its panes.
        QVariantMap best;
        tab->splits->forEachPane([&](Pane* p, const QRect&) {
            const QVariantMap s = statusSummary(p);
            if (s.value(QStringLiteral("priority")).toInt() > best.value(QStringLiteral("priority")).toInt()) {
                best = s;
            }
        });
        tabStatus.append(best);
    }
    m_controller->setTabs(titles, m_currentTab, tabBarHeight() > 0 ? m_settings.settings().window.tabBarHeight : 0);
    m_controller->setTabStatus(tabStatus);
    m_controller->setTabContexts(tabContexts);

    // Status colours from the palette's bright green, yellow and red.
    const auto& pal = m_settings.settings().terminal.basePalette;
    m_controller->setStatusColors(QColor(pal[10].r, pal[10].g, pal[10].b), QColor(pal[11].r, pal[11].g, pal[11].b),
                                  QColor(pal[9].r, pal[9].g, pal[9].b));
}

void TerminalWindow::reportProblems(ProblemSource source, const QStringList& problems, bool again)
{
    // Every window hears problemsChanged from the one store (or library),
    // and each new window sees the problems of the startup read: show a
    // list once.
    static QStringList shownConfig;
    static QStringList shownShaders;
    QStringList& shown = source == ProblemSource::Config ? shownConfig : shownShaders;
    if (problems.isEmpty() || (problems == shown && !again)) {
        shown = problems;
        return;
    }
    shown = problems;
    constexpr qsizetype Listed = 5;
    QString body = QStringList(problems.mid(0, Listed)).join(QLatin1Char('\n'));
    if (problems.size() > Listed) body += QLatin1Char('\n') + tr("and %n more", nullptr, int(problems.size() - Listed));
    NotificationCenter::instance().showNotice(nullptr, source == ProblemSource::Config ? tr("Configuration problems") : tr("Shader problems"), body);
}

namespace {
/// The chords bound to @p action, then its key sequences and table keys,
/// each in config order.
QStringList shortcutsOf(const Keybinds& keybinds, const KeyModes& modes, Action action)
{
    return keybinds.bindingsOf(action) + modes.bindingsOf(Keybinds::token(action));
}
} // namespace

QStringList TerminalWindow::actionShortcuts(Action action) const
{
    return shortcutsOf(m_settings.settings().keybinds, m_settings.settings().keyModes, action);
}

QStringList TerminalWindow::scriptCommandShortcuts(const QString& command) const
{
    return m_settings.settings().keybinds.bindingsOfScript(command)
        + m_settings.settings().keyModes.bindingsOf(Keybinds::ScriptPrefix + command);
}

QList<ScriptCommand> TerminalWindow::listedScriptCommands() const
{
    QList<ScriptCommand> listed = m_scripts.commands();
    listed.removeIf([](const ScriptCommand& c) { return c.palette.isEmpty(); });
    return listed;
}

std::optional<ScriptCommand> TerminalWindow::scriptCommand(const QString& token) const
{
    const QList<ScriptCommand> commands = m_scripts.commands();
    const auto found = std::find_if(commands.cbegin(), commands.cend(), [&](const ScriptCommand& c) { return c.token() == token; });
    return found == commands.cend() ? std::nullopt : std::optional<ScriptCommand>(*found);
}

void TerminalWindow::updatePaletteActions()
{
    QVariantList actions;
    for (Action a : Keybinds::allActions()) {
        // The palette cannot run itself, and while a script holds the keys
        // the palette cannot open: its chord is the only way.
        if (a == Action::CommandPalette || a == Action::EndScriptKeyMode) {
            continue;
        }
        if (a == Action::CancelOverlay && !(m_focused && m_focused->overlay())) continue;
        actions.append(QVariantMap{{QStringLiteral("id"), int(a)},
                                   {QStringLiteral("title"), Keybinds::title(a)},
                                   {QStringLiteral("shortcut"), actionShortcuts(a).join(QStringLiteral("  "))}});
    }
    m_paletteCommands.clear();
    for (const ScriptCommand& c : listedScriptCommands()) {
        // The script's id first: a script's title cannot pass for one of
        // Afterglow's own actions ("Close Window").
        actions.append(QVariantMap{{QStringLiteral("id"), ScriptCommandBase + int(m_paletteCommands.size())},
                                   {QStringLiteral("title"), tr("%1: %2").arg(c.script, c.palette)},
                                   {QStringLiteral("shortcut"), scriptCommandShortcuts(c.token()).join(QStringLiteral("  "))}});
        m_paletteCommands.append(c.token());
    }
    m_controller->setActions(actions);
}

void TerminalWindow::runScriptCommand(QString token, Pane* pane, const QString& source)
{
    // Palette, key and menu runs have no client: a refusal shows here, with
    // the title that the run's own notifications have.
    ScriptCommand named;
    const int dot = int(token.indexOf(QLatin1Char('.')));
    named.script = token.left(dot);
    named.name = dot < 0 ? QString() : token.mid(dot + 1);
    const QString title = scriptCommand(token).value_or(named).title();
    const QString error = m_scripts.runCommand(token, pane ? pane->id() : 0, source);
    if (!error.isEmpty()) NotificationCenter::instance().showNotice(pane, tr("Could not run %1").arg(title), error);
}

void TerminalWindow::runSearch(const QString& query, int direction)
{
    // An overlay's text while one runs over the pane.
    Pane* target = inputPane(m_focused);
    if (!target) {
        return;
    }
    if (query.isEmpty()) {
        target->clearSearch();
        m_controller->setSearchStatus({});
        scheduleRender();
        return;
    }
    target->search(query, direction, m_controller->searchRegex());
    scheduleRender();
}

QRectF TerminalWindow::inputMethodCursorRect() const
{
    return cursorCellRect(inputPane(m_focused));
}

QRectF TerminalWindow::cursorCellRect(Pane* pane) const
{
    if (!pane || !m_splits) {
        return {};
    }
    const auto& m = const_cast<TerminalWindow*>(this)->m_renderer->fonts().fonts->metrics();
    const auto& snap = pane->snapshot();
    const QRect content = contentRect(pane, m_splits->rectOf(hostOf(pane)));
    const qreal dpr = devicePixelRatio();
    // Scrolled back, the cursor's row is not on screen: its row number
    // would name a history line.
    if (!snap.cursorVisible && pane->withTerminal([](Terminal& t) { return t.screen().viewportOffset() != 0; })) {
        const QPointF middle = QRectF(content).center();
        return QRectF((middle.x() - m.cellWidth / 2.0) / dpr, (middle.y() - m.cellHeight / 2.0) / dpr, m.cellWidth / dpr,
                      m.cellHeight / dpr);
    }
    // Where the grid draws it: the snapshot's column is already the visual
    // one, and a double-size line draws each cell twice as wide.
    const int scale = snap.lineSize(snap.cursorY) != AfterglowVt::LineSize::Single ? 2 : 1;
    return QRectF((content.x() + snap.cursorX * scale * m.cellWidth) / dpr, (content.y() + snap.cursorY * m.cellHeight) / dpr,
                  scale * m.cellWidth / dpr, m.cellHeight / dpr);
}

void TerminalWindow::ringBell(Pane* pane)
{
    const Settings::Bell bell = m_settings.settings().bell;
    if ((bell == Settings::Bell::Attention || bell == Settings::Bell::Both) && !isActive()) {
        alert(0);
    }
    if (bell == Settings::Bell::Visual || bell == Settings::Bell::Both) {
        flashPane(pane);
    }
    if (bell != Settings::Bell::None) {
        playBellSound();
    }
}

void TerminalWindow::flashPane(Pane* pane)
{
    // The flash is drawn over the pane in the tab, also for its overlay's bell.
    pane = hostOf(pane);
    const AnimationSettings& anim = m_settings.settings().animation;
    motion(pane).bell.start(1.0, 0.0, motionSpec(anim.bellCurve, anim.bellDuration, m_bellCurve, "Animation.bellCurve",
                                                 AnimationSettings{}.bellCurve));
    if (paneDrawn(pane)) scheduleRender();
}

void TerminalWindow::playBellSound()
{
    const QString sound = m_settings.settings().bellSound;
    if (sound.isEmpty() || (m_lastBellSound.isValid() && m_lastBellSound.elapsed() < m_settings.settings().bellSoundInterval)) {
        return;
    }
    m_lastBellSound.start();
    if (sound == QLatin1String("default")) {
        // The desktop sound theme's bell (freedesktop sound naming).
        QProcess::startDetached(QStringLiteral("canberra-gtk-play"),
                                {QStringLiteral("--id=bell"), QStringLiteral("--description=afterglow bell")});
        return;
    }
    const QString path = sound.startsWith(QLatin1String("~/")) ? QDir::homePath() + sound.mid(1) : sound;
    if (!QProcess::startDetached(QStringLiteral("pw-play"), {path})) {
        QProcess::startDetached(QStringLiteral("paplay"), {path});
    }
}

void TerminalWindow::openLink(const QString& url)
{
    // A target comes from the terminal and its text can hide it. It must be
    // a URL with a scheme: that also keeps it from passing for an option of
    // the Links.openWith program ("-P", "--script=...").
    const QUrl target(url);
    if (!target.isValid() || target.scheme().isEmpty()) {
        qCWarning(lcWindow).noquote() << "not opening" << url << "(not a URL with a scheme)";
        return;
    }
    const auto open = [this, target, url = url.trimmed()] {
        QStringList command = QProcess::splitCommand(m_settings.settings().links.openWith);
        if (command.isEmpty()) {
            QDesktopServices::openUrl(target);
            return;
        }
        const QString program = command.takeFirst();
        command << url;
        if (!QProcess::startDetached(program, command)) {
            qCWarning(lcWindow).noquote() << "could not run" << program << "to open" << target.toDisplayString();
        }
    };
    if (m_settings.settings().links.schemes.contains(target.scheme().toLower())) {
        open();
        return;
    }
    // Other schemes start other programs: the user sees the whole target
    // first. Not over another question, whose answer this would take.
    if (m_controller->confirmPromptOpen()) return;
    m_pendingConfirm = open;
    m_pendingConfirmPanes.clear();
    m_controller->askConfirm(QStringLiteral("Open this link?"),
                             QStringLiteral("The link opens with the handler for “%1:”. Its target is %2.")
                                 .arg(sanitizedText(target.scheme(), 40), sanitizedText(target.toDisplayString(), 400)),
                             QStringLiteral("Open"));
}

bool TerminalWindow::linkModifierHeld() const
{
    const Qt::KeyboardModifiers modifier = m_settings.settings().links.modifier;
    return modifier == Qt::NoModifier || (QGuiApplication::queryKeyboardModifiers() & modifier);
}

void TerminalWindow::revealPane(Pane* pane)
{
    // An overlay's program shows on its host.
    pane = hostOf(pane);
    const int tab = tabOf(pane);
    if (tab < 0) {
        return;
    }
    if (tab != m_currentTab) {
        switchTab(tab);
    }
    setFocusedPane(pane);
    if (m_quick) {
        // A quick terminal slides in from its edge; a plain show() would map
        // it at the offset it slid out to, off screen but with the keyboard.
        if (!m_quickShown) toggleQuick();
    } else {
        show();
    }
    raise();
    requestActivate();
    scheduleRender();
}

namespace {

int statusPriority(const PaneStatusRecord& r, quint64 acknowledged)
{
    using AfterglowVt::StatusState;
    switch (r.state) {
    case StatusState::Blocked:
        return 4;
    case StatusState::Error:
        return r.serial > acknowledged ? 3 : 0;
    case StatusState::Working:
        return 2;
    case StatusState::Done:
        return r.serial > acknowledged ? 1 : 0;
    case StatusState::Idle:
        return 0; // at rest: nothing to show
    }
    return 0;
}

QString sanitizedStatusText(const QString& s)
{
    return sanitizedText(s, 300);
}

QString statusDescription(const PaneStatusRecord& r)
{
    using AfterglowVt::BlockKind;
    using AfterglowVt::StatusState;
    QString what;
    switch (r.state) {
    case StatusState::Blocked:
        what = r.kind == BlockKind::Permission ? QObject::tr("Needs permission")
            : r.kind == BlockKind::Question    ? QObject::tr("Has a question")
            : r.kind == BlockKind::Auth        ? QObject::tr("Needs authentication")
                                               : QObject::tr("Waiting for you");
        break;
    case StatusState::Working:
        what = r.progress >= 0 ? QObject::tr("Working, %1%").arg(r.progress) : QObject::tr("Working");
        break;
    case StatusState::Done:
        what = QObject::tr("Done");
        break;
    case StatusState::Error:
        what = QObject::tr("Failed");
        break;
    case StatusState::Idle:
        what = QObject::tr("Idle");
        break;
    }
    if (!r.message.isEmpty()) {
        what += QStringLiteral(": ") + sanitizedStatusText(r.message);
    }
    return what;
}

QString statusSource(const PaneStatusRecord& r)
{
    const QString app = sanitizedStatusText(r.app);
    const QString title = sanitizedStatusText(r.title);
    if (!app.isEmpty() && !title.isEmpty()) {
        return app + QStringLiteral(" \u00B7 ") + title;
    }
    return app.isEmpty() ? title : app;
}

} // namespace

void TerminalWindow::paneStatusChanged(Pane* pane)
{
    StatusView& view = m_statusViews[pane];
    view.records = pane->status();

    // A record entering blocked, done or error is worth telling the user
    // about; the most pressing one if several change at once.
    QHash<QString, int> states;
    const PaneStatusRecord* attention = nullptr;
    for (const PaneStatusRecord& r : view.records) {
        states.insert(r.id, int(r.state));
        using AfterglowVt::StatusState;
        // A session pane's first frame brings the records as the daemon kept
        // them: a window showed them, or none did while they changed. Either
        // way they are no change that this window saw happen.
        const bool notable = (r.state == StatusState::Blocked || r.state == StatusState::Done || r.state == StatusState::Error)
            && r.serial > pane->statusBaseline();
        if (notable && view.lastStates.value(r.id, -1) != int(r.state)
            && (!attention || statusPriority(r, 0) > statusPriority(*attention, 0))) {
            attention = &r;
        }
    }
    view.lastStates = std::move(states);

    const bool watching = m_windowFocused && pane == m_focused;
    if (attention && !watching) {
        if (attention->state == AfterglowVt::StatusState::Blocked && !isActive()) {
            alert(0);
        }
        if (m_settings.settings().notifications
            && (!view.lastNotification.isValid() || view.lastNotification.elapsed() >= m_settings.settings().statusNotificationInterval)) {
            view.lastNotification.start();
            const QString source = statusSource(*attention);
            NotificationCenter::instance().showSimple(pane, source.isEmpty() ? pane->title() : source,
                                                      statusDescription(*attention),
                                                      attention->state == AfterglowVt::StatusState::Blocked ? 2 : 1);
        }
    }
    acknowledgeStatus();
    updateChrome();
    scheduleRender();
    publishLauncherStatus();
}

void TerminalWindow::acknowledgeStatus()
{
    if (!m_windowFocused || !m_focused) {
        return;
    }
    const auto it = m_statusViews.find(m_focused);
    if (it == m_statusViews.end()) {
        return;
    }
    quint64 latest = 0;
    for (const PaneStatusRecord& r : it->second.records) {
        latest = std::max(latest, r.serial);
    }
    if (latest > it->second.acknowledged) {
        it->second.acknowledged = latest;
        updateChrome();
        scheduleRender();
    }
}

QVariantMap TerminalWindow::statusSummary(const Pane* pane) const
{
    const auto it = m_statusViews.find(pane);
    if (it == m_statusViews.end()) {
        return {};
    }
    const PaneStatusRecord* pick = nullptr;
    int best = 0;
    for (const PaneStatusRecord& r : it->second.records) {
        const int priority = statusPriority(r, it->second.acknowledged);
        if (priority > best || (priority == best && priority > 0 && r.serial > pick->serial)) {
            best = priority;
            pick = &r;
        }
    }
    if (!pick) {
        return {};
    }
    const QString source = statusSource(*pick);
    const QString description = statusDescription(*pick);
    return {{QStringLiteral("state"), Control::statusStateName(pick->state)},
            {QStringLiteral("progress"), pick->progress},
            {QStringLiteral("priority"), best},
            {QStringLiteral("text"), source.isEmpty() ? description : description + QStringLiteral(" (") + source + QLatin1Char(')')}};
}

void TerminalWindow::publishLauncherStatus()
{
    // The Unity LauncherEntry API, which Plasma's task manager reads:
    // progress from working records, urgency while anything is blocked.
    bool urgent = false;
    int progress = -1;
    for (TerminalWindow* w : windows()) {
        for (const auto& [pane, view] : w->m_statusViews) {
            for (const PaneStatusRecord& r : view.records) {
                urgent |= r.state == AfterglowVt::StatusState::Blocked;
                if (r.state == AfterglowVt::StatusState::Working && r.progress >= 0) {
                    progress = std::max(progress, r.progress);
                }
            }
        }
    }
    static int lastProgress = -2;
    static bool lastUrgent = false;
    if (progress == lastProgress && urgent == lastUrgent) {
        return;
    }
    lastProgress = progress;
    lastUrgent = urgent;
    QDBusMessage signal = QDBusMessage::createSignal(QStringLiteral("/com/phosphorworks/Afterglow"),
                                                     QStringLiteral("com.canonical.Unity.LauncherEntry"),
                                                     QStringLiteral("Update"));
    // --class changes the desktop file name, and the launcher entry with it.
    const QString desktopFile = QGuiApplication::desktopFileName().isEmpty() ? QStringLiteral(AFTERGLOW_APP_ID)
                                                                             : QGuiApplication::desktopFileName();
    signal << QStringLiteral("application://%1.desktop").arg(desktopFile)
           << QVariantMap{{QStringLiteral("progress"), progress >= 0 ? progress / 100.0 : 0.0},
                          {QStringLiteral("progress-visible"), progress >= 0},
                          {QStringLiteral("urgent"), urgent}};
    QDBusConnection::sessionBus().send(signal);
}

void TerminalWindow::applyPointerShape()
{
    // Whatever moved the pointer or what is under it, the shape follows,
    // and so does the dock item under it.
    updateDockHover();
    if (m_pointerHidden) {
        setCursor(Qt::BlankCursor);
        return;
    }
    // A drag keeps the divider's shape over the chrome and the docks too:
    // every frame of it lays the panes out again, and the layout sets the
    // shape. The overview can open during a drag, from the keyboard.
    if (m_dividerDrag && !m_overview.open) {
        setCursor(m_dividerDrag->vertical ? Qt::SplitVCursor : Qt::SplitHCursor);
        return;
    }
    // The overview takes every click but a dock's, and a prompt's over it
    // (mousePressEvent).
    if ((!m_overview.open || m_controller->wantsKeyboard()) && chromeVisible() && m_chrome->wantsPointer(m_pointer)) {
        setCursor(m_chrome->cursorAt(m_pointer));
        return;
    }
    // The same pixels as a press finds the docks and the dividers at.
    if (const DockArea* dock = dockAt(m_pointer); dock && !dividerTakesDock(m_pointer)) {
        setCursor(dock->itemAt(dockPixelOf(*dock, m_pointer)).isEmpty() ? Qt::ArrowCursor : Qt::PointingHandCursor);
        return;
    }
    if (m_overview.open) {
        setCursor(Qt::ArrowCursor);
        return;
    }
    if (const auto divider = m_splits ? m_splits->dividerAt(dividerPixel(m_pointer), dividerTolerance()) : std::nullopt) {
        setCursor(divider->vertical ? Qt::SplitVCursor : Qt::SplitHCursor);
        return;
    }
    if (!m_hover.url.isEmpty()) {
        setCursor(Qt::PointingHandCursor);
        return;
    }
    Pane* pane = m_splits ? pointerTarget(paneShownAt(m_pointer), m_pointer) : nullptr;
    const QString shape = pane ? motion(pane).pointerShape : QString();
    setCursor(protocolPointer(shape, devicePixelRatio()));
}

TerminalWindow::LinkHover TerminalWindow::linkAt(Pane* pane, const QPointF& pos) const
{
    // Only over the cells: not from around them, where the cell clamps to
    // the grid's edge (the padding, or what a pack shows around a pane).
    if (!pane || !overCells(pane, pos)) return {};
    return linkAtCell(pane, cellAt(pane, pos));
}

TerminalWindow::LinkHover TerminalWindow::linkAtCell(Pane* pane, const QPoint& cell) const
{
    LinkHover found;
    const AfterglowRender::PaneSnapshot& snap = pane->snapshot();
    found.version = snap.version;
    if (cell.x() < snap.cols && cell.y() < snap.rows) {
        const AfterglowRender::SnapshotCell& c = snap.logicalAt(cell.x(), cell.y());
        if (c.link != 0 && c.link <= snap.links.size()) {
            found.pane = pane;
            found.link = c.link;
            found.url = QString::fromStdString(snap.links[c.link - 1]);
        } else if (m_settings.settings().links.detectUrls) {
            // A plain-text URL on this row.
            for (const UrlSpan& span : findUrls(snap, cell.y())) {
                if (span.link == 0 && cell.x() >= span.colStart && cell.x() <= span.colEnd) {
                    found.pane = pane;
                    found.row = cell.y();
                    found.colStart = span.colStart;
                    found.colEnd = span.colEnd;
                    found.url = span.url;
                    break;
                }
            }
        }
    }
    return found;
}

void TerminalWindow::openContextMenu(const QPointF& pos, const QString& link)
{
    auto entry = [](int id, const QString& title, const char* icon, const QString& shortcut = {}, bool enabled = true) {
        return QVariant(QVariantMap{{QStringLiteral("id"), id},
                                    {QStringLiteral("title"), title},
                                    {QStringLiteral("icon"), QString::fromLatin1(icon)},
                                    {QStringLiteral("shortcut"), shortcut},
                                    {QStringLiteral("enabled"), enabled}});
    };
    auto action = [&](Action a, const QString& title, const char* icon, bool enabled = true) {
        return entry(int(a), title, icon, actionShortcuts(a).value(0), enabled);
    };
    auto submenu = [](const QString& title, const char* icon, const QVariantList& items) {
        return QVariant(QVariantMap{{QStringLiteral("title"), title},
                                    {QStringLiteral("icon"), QString::fromLatin1(icon)},
                                    {QStringLiteral("items"), items}});
    };
    const QVariant separator = QVariantMap{{QStringLiteral("separator"), true}};
    const bool pane = m_focused != nullptr;
    m_menuPane = m_focused;
    m_menuTab = 0; // a pane menu has no tab items

    QVariantList items;
    m_menuLink = link;
    if (!m_menuLink.isEmpty()) {
        items << entry(MenuOpenLink, QStringLiteral("Open Link"), "internet-web-browser")
              << entry(MenuCopyLink, QStringLiteral("Copy Link Address"), "insert-link") << separator;
    }
    QVariantMap notify = action(Action::NotifyOnFinish, QStringLiteral("Notify on Next Command Finish"),
                                "preferences-desktop-notification-bell", pane)
                             .toMap();
    notify.insert(QStringLiteral("checked"), pane && m_notifyOnFinish.contains(m_focused.data()));
    items << action(Action::Copy, QStringLiteral("Copy"), "edit-copy", pane && inputPane(m_focused)->hasSelection())
          // The offered types, not the text: reading it would wait on the source application.
          << action(Action::Paste, QStringLiteral("Paste"), "edit-paste", [] {
                 const QMimeData* data = QGuiApplication::clipboard()->mimeData();
                 return data && data->hasText();
             }())
          << action(Action::SelectAll, QStringLiteral("Select All"), "edit-select-all", pane)
          << action(Action::Search, QStringLiteral("Find\u2026"), "edit-find") << notify
          << action(Action::CommandPalette, QStringLiteral("Command Palette\u2026"), "system-run") << separator
          << action(Action::ClearScreen, QStringLiteral("Clear"), "edit-clear-all", pane)
          << action(Action::ResetTerminal, QStringLiteral("Reset"), "view-refresh", pane) << separator;
    items << submenu(QStringLiteral("Split"), "view-split-left-right",
                     {action(Action::RenamePane, QStringLiteral("Change Title\u2026"), "document-edit", pane), separator,
                      action(Action::SplitRight, QStringLiteral("Split Right"), "view-split-left-right"),
                      action(Action::SplitDown, QStringLiteral("Split Down"), "view-split-top-bottom"),
                      action(Action::SplitLeft, QStringLiteral("Split Left"), "view-split-left-right"),
                      action(Action::SplitUp, QStringLiteral("Split Up"), "view-split-top-bottom"), separator,
                      action(Action::EqualizeSplits, QStringLiteral("Equalize Splits"), "distribute-horizontal-equal",
                             m_splits && m_splits->paneCount() > 1),
                      action(Action::ClosePane, QStringLiteral("Close Split"), "view-close", pane)});
    items << submenu(QStringLiteral("Tab"), "tab-new",
                     {action(Action::RenameTab, QStringLiteral("Change Tab Title\u2026"), "document-edit"),
                      action(Action::NewTab, QStringLiteral("New Tab"), "tab-new"),
                      action(Action::CloseTab, QStringLiteral("Close Tab"), "tab-close")});
    items << submenu(QStringLiteral("Window"), "window-new",
                     {action(Action::NewWindow, QStringLiteral("New Window"), "window-new"),
                      action(Action::CloseWindow, QStringLiteral("Close Window"), "window-close")});
    m_menuCommands.clear();
    QVariantList scripts;
    for (const ScriptCommand& c : listedScriptCommands()) {
        // Ids from TabMenuRename on are the tab menu's.
        if (MenuScriptBase + m_menuCommands.size() >= TabMenuRename) break;
        // In the Scripts submenu, away from Afterglow's own items.
        scripts << entry(MenuScriptBase + int(m_menuCommands.size()), c.palette, "", scriptCommandShortcuts(c.token()).value(0));
        m_menuCommands.append(c.token());
    }
    if (!scripts.isEmpty()) {
        items << submenu(QStringLiteral("Scripts"), "dialog-scripts", scripts);
    }
    items << separator
          << submenu(QStringLiteral("Config"), "configure",
                     {action(Action::OpenConfig, QStringLiteral("Open Config"), "document-open"),
                      action(Action::ReloadConfig, QStringLiteral("Reload Config"), "view-refresh")});
    m_controller->openContextMenu(pos, items);
}

bool TerminalWindow::chromePromptOpen() const
{
    return m_scriptDialog || m_controller->pasteConfirmOpen() || m_controller->clipboardPromptOpen()
        || m_controller->contextMenuOpen() || m_controller->titlePromptOpen() || m_controller->confirmPromptOpen();
}

bool TerminalWindow::acceptsAccessibleActions() const
{
    // A key cannot act while these hold the keyboard. An action from
    // outside would replace the prompt's pending answer (a close, a paste)
    // or act under a mode's labels.
    return !m_controller->wantsKeyboard() && !keyboardModeActive();
}

bool TerminalWindow::revealForAccessibility(Pane* pane)
{
    if (!pane || tabOf(pane) < 0 || !acceptsAccessibleActions()) return false;
    // A hidden or inactive window would run the action, or open the menu,
    // where nobody sees it and no key reaches it.
    revealPane(pane);
    // switchTab leaves the layout to the next frame: the tab's rectangles
    // are from when it last showed.
    ensureLayout();
    return true;
}

bool TerminalWindow::runAccessibleAction(Pane* pane, Action action)
{
    return revealForAccessibility(pane) && performAction(action);
}

void TerminalWindow::openPaneMenu(Pane* pane)
{
    if (!revealForAccessibility(pane)) return;
    // A pane of a tab that did not show has the snapshot of when it last
    // drew: its cursor may have moved since. The cursor that shows is an
    // overlay's while one runs over the pane.
    Pane* shown = inputPane(pane);
    shown->refreshSnapshot();
    // A link under the cursor is the menu's, as one under a pointer is. The
    // cursor's cell is in the layout, not where a pack draws it.
    const QPointF cursor = cursorCellRect(shown).center();
    openContextMenu(cursor, linkAtCell(shown, cellOf(shown, gridPixelAt(shown, cursor * devicePixelRatio()))).url);
}

bool TerminalWindow::updateLinkHover()
{
    // Not over a pane's dock, where the cell would clamp to the grid's edge.
    Pane* pane = linkModifierHeld() ? pointerTarget(paneUnder(m_pointer), m_pointer) : nullptr;
    const LinkHover found = linkAt(pane, m_pointer);
    const bool changed = found.pane != m_hover.pane || found.link != m_hover.link || found.row != m_hover.row
        || found.colStart != m_hover.colStart || found.colEnd != m_hover.colEnd;
    m_hover = found;
    if (changed) {
        applyPointerShape();
        scheduleRender();
    }
    return changed;
}

void TerminalWindow::refreshLinkHover(Pane* pane)
{
    if (m_hover.pane == pane && pane->snapshot().version != m_hover.version) updateLinkHover();
}

std::vector<TerminalWindow::UrlSpan> TerminalWindow::findUrls(const AfterglowRender::PaneSnapshot& snap, int row)
{
    std::vector<UrlSpan> spans;
    // OSC 8 hyperlinks: runs of cells with the same link.
    for (int x = 0; x < snap.cols;) {
        const std::uint32_t link = snap.logicalAt(x, row).link;
        if (link == 0 || link > snap.links.size()) {
            ++x;
            continue;
        }
        int end = x;
        while (end + 1 < snap.cols && snap.logicalAt(end + 1, row).link == link) {
            ++end;
        }
        spans.push_back({x, end, link, QString::fromStdString(snap.links[link - 1])});
        x = end + 1;
    }
    // URLs in the text.
    QString text;
    std::vector<int> columns;
    for (int x = 0; x < snap.cols; ++x) {
        const AfterglowRender::SnapshotCell& rc = snap.logicalAt(x, row);
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
        if (candidate.isEmpty()) {
            continue;
        }
        const int first = columns[std::size_t(m.capturedStart())];
        const int last = columns[std::size_t(m.capturedStart() + candidate.size() - 1)];
        const bool insideLink = std::any_of(spans.begin(), spans.end(), [&](const UrlSpan& s) {
            return s.link != 0 && first <= s.colEnd && last >= s.colStart;
        });
        if (!insideLink) {
            spans.push_back({first, last, 0,
                             candidate.startsWith(QLatin1String("www.")) ? QStringLiteral("https://") + candidate : candidate});
        }
    }
    return spans;
}

void TerminalWindow::startHints()
{
    stopCopyMode();
    stopUrlMode();
    stopUnicodeInput();
    cancelKeyMode();
    if (!m_focused) return;
    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    m_hintCancel = cancelled;
    // Over an overlay's text while one runs over the pane.
    const QPointer<Pane> pane = inputPane(m_focused);
    // On the rows where the view stops: a scroll that eased on would change
    // the generation, and the hints found would be dropped.
    settleScroll(pane);
    std::uint64_t generation = 0, firstRow = 0;
    auto snapshot = pane->withTerminal([&](Terminal& terminal) {
        AfterglowVt::TextSnapshot snapshot;
        const auto& screen = terminal.screen();
        firstRow = screen.rowsDropped() + screen.viewportTop();
        generation = terminal.textGeneration();
        for (std::size_t row = screen.viewportTop(); row < screen.viewportTop() + std::size_t(screen.rows()); ++row)
            if (!AfterglowVt::appendTextRow(snapshot, screen, row, 65536)) break;
        return snapshot;
    });
    auto* watcher = new QFutureWatcher<std::vector<UrlHint>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, cancelled, pane, generation] {
        auto hints = watcher->result(); watcher->deleteLater();
        // Another mode that took the keyboard meanwhile keeps it.
        if (cancelled != m_hintCancel || *cancelled || !pane || pane != inputPane(m_focused) || keyboardModeActive()
            || pane->withTerminal([](Terminal& t) { return t.textGeneration(); }) != generation) return;
        m_hintCancel.reset();
        activateHints(std::move(hints), false);
        m_urlMode.generation = generation;
    });
    const auto patterns = m_settings.settings().hintPatterns;
    watcher->setFuture(QtConcurrent::run([snapshot = std::move(snapshot), patterns, cancelled, firstRow] {
        std::vector<UrlHint> hints;
        QSet<QString> seen;
        // Matches repeat, and only distinct ones get labels: ask for as many
        // matches as the text could hold, and stop at the labels.
        const std::size_t characters = snapshot.characters + 1;
        for (const auto& value : patterns) {
            if (*cancelled || hints.size() >= MaxHints) break;
            const auto pattern = value.toMap();
            const QString action = pattern.value(QStringLiteral("action"), QStringLiteral("copy")).toString();
            const auto command = pattern.value(QStringLiteral("command")).toStringList();
            const auto result = AfterglowVt::searchText(snapshot, pattern.value(QStringLiteral("pattern")).toString().toStdU32String(), true, *cancelled, characters, false);
            if (!result.error.empty()) qCWarning(lcWindow) << "hint pattern:" << QString::fromStdString(result.error);
            for (const auto& match : result.matches) {
                if (hints.size() >= MaxHints) break;
                QString text;
                for (const auto& line : snapshot.lines) {
                    if (line.cells.empty() || line.cells.back().end() < match.start || line.cells.front().start() > match.end) continue;
                    const auto begin = std::lower_bound(line.cells.begin(), line.cells.end(), match.start,
                        [](const auto& cell, const auto& point) { return cell.end() < point; });
                    const auto end = std::upper_bound(begin, line.cells.end(), match.end,
                        [](const auto& point, const auto& cell) { return point < cell.start(); });
                    text += QString::fromStdU32String(line.text.substr(std::size_t(begin - line.cells.begin()), std::size_t(end - begin)));
                }
                if (text.isEmpty() || seen.contains(text)) continue;
                seen.insert(text);
                hints.push_back({{}, text, int(match.start.row - firstRow), match.start.col, action, command});
            }
        }
        return hints;
    }));
}

void TerminalWindow::startUrlMode(bool copy)
{
    stopCopyMode();
    cancelKeyMode();
    stopUnicodeInput();
    stopUrlMode(); // hints still being found would replace these labels
    if (!m_focused) {
        return;
    }
    // The labels sit on the rows of this snapshot: a scroll that eased on
    // under them would move the text away, and change the generation that
    // the first letter checks.
    settleScroll(inputPane(m_focused));
    const AfterglowRender::PaneSnapshot& snap = inputPane(m_focused)->snapshot();
    const bool detectUrls = m_settings.settings().links.detectUrls;
    std::vector<UrlHint> hints;
    for (int y = 0; y < snap.rows; ++y) {
        for (const UrlSpan& s : findUrls(snap, y)) {
            // Plain-text URLs only when Links.detectUrls asks for them.
            if (s.link == 0 && !detectUrls) continue;
            hints.push_back({QString(), s.url, y, s.colStart});
        }
    }
    activateHints(std::move(hints), copy);
    // The labels sit on the text of this snapshot: output that changes it
    // takes them away, as for show_hints. A held pane draws a snapshot older
    // than its terminal: the labels sit on that drawn text, so they stay
    // until the hold ends, and close then.
    if (m_urlMode.active) {
        const auto current = inputPane(m_focused)->withTerminal([](Terminal& t) { return t.textGeneration(); });
        if (current == snap.generation) {
            m_urlMode.generation = snap.generation;
        } else {
            m_urlMode.held = true;
        }
    }
}

void TerminalWindow::activateHints(std::vector<UrlHint> hints, bool copy)
{
    if (hints.empty()) {
        return;
    }
    // Labels from Hints.alphabet (the home row first), all the same
    // length so none is a prefix of another.
    const QString Alphabet = m_settings.settings().hintAlphabet;
    const int k = int(Alphabet.size());
    if (k < 2) return; // the setting keeps two letters at least
    int length = 1;
    for (long long capacity = k; capacity < (long long)hints.size(); capacity *= k) {
        ++length;
    }
    for (std::size_t i = 0; i < hints.size(); ++i) {
        QString label;
        std::size_t n = i;
        for (int d = 0; d < length; ++d) {
            label.prepend(Alphabet[int(n % std::size_t(k))]);
            n /= std::size_t(k);
        }
        hints[i].label = label;
    }
    m_urlMode = UrlMode{true, copy, inputPane(m_focused), std::move(hints), QString()};
    showUrlHints();
    syncInputMethodEnabled();
}

void TerminalWindow::stopUrlMode()
{
    if (m_hintCancel) { *m_hintCancel = true; m_hintCancel.reset(); }
    m_urlMode = UrlMode{};
    m_controller->setUrlHints({});
    syncInputMethodEnabled();
    scheduleRender();
}

void TerminalWindow::showUrlHints()
{
    if (!m_urlMode.pane || !m_splits || tabOf(hostOf(m_urlMode.pane)) != m_currentTab) {
        stopUrlMode();
        return;
    }
    const auto& m = m_renderer->fonts().fonts->metrics();
    const QPoint origin = contentRect(m_urlMode.pane, m_splits->rectOf(hostOf(m_urlMode.pane))).topLeft();
    const qreal dpr = devicePixelRatio();
    const auto& snap = m_urlMode.pane->snapshot();
    QVariantList list;
    for (const UrlHint& h : m_urlMode.hints) {
        if (!h.label.startsWith(m_urlMode.typed)) {
            continue;
        }
        // On the rows as drawn: up by the scroll shift, with double-width
        // cells on a double-size line (cellAt).
        const int scale = snap.lineSize(h.row) != AfterglowVt::LineSize::Single ? 2 : 1;
        list.append(QVariantMap{{QStringLiteral("x"), (origin.x() + snap.visualColumn(h.col, h.row) * m.cellWidth * scale) / dpr},
                                {QStringLiteral("y"), (origin.y() + h.row * m.cellHeight - drawnScrollShift(m_urlMode.pane)) / dpr},
                                {QStringLiteral("label"), h.label},
                                {QStringLiteral("typed"), int(m_urlMode.typed.size())}});
    }
    m_controller->setUrlHints(list);
    scheduleRender();
}

void TerminalWindow::urlModeKey(QKeyEvent* e)
{
    if (m_urlMode.generation && (!m_urlMode.pane || m_urlMode.pane->withTerminal([](Terminal& t) { return t.textGeneration(); }) != m_urlMode.generation)) {
        stopUrlMode(); return;
    }
    if (e->key() == Qt::Key_Escape) {
        stopUrlMode();
        return;
    }
    if (e->key() == Qt::Key_Backspace) {
        m_urlMode.typed.chop(1);
        showUrlHints();
        return;
    }
    const QString text = e->text().toLower();
    if (text.size() != 1 || !text[0].isLetter()) {
        return;
    }
    const QString typed = m_urlMode.typed + text;
    const UrlHint* exact = nullptr;
    bool any = false;
    for (const UrlHint& h : m_urlMode.hints) {
        if (h.label.startsWith(typed)) {
            any = true;
            if (h.label == typed) {
                exact = &h;
            }
        }
    }
    if (!any) {
        return; // a key no label continues with
    }
    if (exact) {
        const QString url = exact->url;
        const QString action = exact->action;
        const QStringList configuredCommand = exact->command;
        const bool copy = m_urlMode.copy;
        // The pane the hints were found in, not whichever has the focus now.
        const QPointer<Pane> found = m_urlMode.pane;
        stopUrlMode();
        if (action == QLatin1String("command") || action == QLatin1String("open")) {
            // Only these use it: a session pane asks its daemon.
            const QString directory = found ? found->workingDirectory() : QString();
            QString path = url, line = QStringLiteral("1"), column = QStringLiteral("1");
            static const QRegularExpression location(QStringLiteral(R"(^(.*?):(\d+)(?::(\d+))?$)"));
            const auto match = location.match(path);
            if (match.hasMatch()) { path = match.captured(1); line = match.captured(2); if (!match.captured(3).isEmpty()) column = match.captured(3); }
            if (path.startsWith(QLatin1String("~/"))) path.replace(0, 1, QDir::homePath());
            if (QFileInfo(path).isRelative()) path = QDir(directory).absoluteFilePath(path);
            if (action == QLatin1String("open")) QDesktopServices::openUrl(QUrl::fromLocalFile(path));
            else {
                // One pass over each argument: text that a match brings in
                // ("{line}" in a file name) is not a placeholder.
                static const QRegularExpression placeholder(QStringLiteral(R"(\{(match|path|line|column)\})"));
                QStringList args;
                for (const QString& arg : configuredCommand) {
                    QString out;
                    qsizetype last = 0;
                    for (auto it = placeholder.globalMatch(arg); it.hasNext();) {
                        const auto m = it.next();
                        out += arg.mid(last, m.capturedStart() - last);
                        const QString name = m.captured(1);
                        out += name == QLatin1String("match") ? url : name == QLatin1String("path") ? path
                             : name == QLatin1String("line")  ? line : column;
                        last = m.capturedEnd();
                    }
                    args << out + arg.mid(last);
                }
                if (!args.isEmpty()) { const QString program = args.takeFirst(); QProcess::startDetached(program, args, directory); }
            }
        } else if (copy || action == QLatin1String("copy")) {
            QGuiApplication::clipboard()->setText(url);
        } else {
            openLink(url);
        }
        return;
    }
    m_urlMode.typed = typed;
    showUrlHints();
}

void TerminalWindow::stopUnicodeInput()
{
    if (!m_unicodeInput) return;
    m_unicodeInput.reset();
    showUnicodeInput();
}

void TerminalWindow::showUnicodeInput()
{
    syncInputMethodEnabled();
    if (!m_unicodeInput) {
        m_controller->setUnicodeInput({});
        scheduleRender();
        return;
    }
    bool ok = false;
    const uint cp = m_unicodeInput->toUInt(&ok, 16);
    const bool valid = ok && cp > 0 && cp <= 0x10FFFF && !(cp >= 0xD800 && cp <= 0xDFFF);
    const char32_t c = char32_t(cp);
    const QRectF cursor = inputMethodCursorRect();
    m_controller->setUnicodeInput({{QStringLiteral("active"), true},
                                   {QStringLiteral("text"), m_unicodeInput->toUpper()},
                                   {QStringLiteral("preview"), valid ? QString::fromUcs4(&c, 1) : QString()},
                                   {QStringLiteral("x"), cursor.x()},
                                   {QStringLiteral("top"), cursor.y()},
                                   {QStringLiteral("y"), cursor.bottom()}});
    scheduleRender();
}

void TerminalWindow::unicodeInputKey(QKeyEvent* e)
{
    switch (e->key()) {
    case Qt::Key_Escape:
        m_unicodeInput.reset();
        showUnicodeInput();
        return;
    case Qt::Key_Backspace:
        m_unicodeInput->chop(1);
        showUnicodeInput();
        return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Space: {
        bool ok = false;
        const uint cp = m_unicodeInput->toUInt(&ok, 16);
        m_unicodeInput.reset();
        showUnicodeInput();
        if (ok && cp > 0 && cp <= 0x10FFFF && !(cp >= 0xD800 && cp <= 0xDFFF) && m_focused) {
            Pane* const target = inputPane(m_focused);
            // --hold: a key closes a pane whose program has ended.
            if (target->isHeld()) {
                closePane(target);
                return;
            }
            const char32_t c = char32_t(cp);
            target->sendInput(QString::fromUcs4(&c, 1).toStdString());
        }
        return;
    }
    default:
        break;
    }
    const QString text = e->text();
    if (text.size() == 1 && text[0].isLetterOrNumber() && QStringLiteral("0123456789abcdefABCDEF").contains(text[0])
        && m_unicodeInput->size() < 6) {
        *m_unicodeInput += text.toLower();
        showUnicodeInput();
    }
}

bool TerminalWindow::routePointerToChrome(QEvent* e, const QPointF& pos, bool press, bool release)
{
    if (!chromeVisible()) {
        m_chromeGrab = false;
        return false;
    }
    if (m_chromeGrab || m_chrome->wantsPointer(pos, e)) {
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
    auto chains = m_shaders.build(s.shaders, s.shaderParams);
    m_shaderControls.rebuild(chains);
    const auto audio = [](const AfterglowRender::ShaderProgram& p) { return p.requiresAudio(); };
    m_audioUse = {chains.anyAlwaysOn(audio), AfterglowRender::ShaderChains::anyIn(chains.overview, audio),
                  AfterglowRender::ShaderChains::anyIn(chains.tabTransition, audio),
                  AfterglowRender::ShaderChains::anyIn(chains.paneEffects, audio),
                  AfterglowRender::ShaderChains::anyIn(chains.overlayTransition, audio)};
    m_readsMouse = chains.readsMouse();
    m_overviewAvailable = !chains.overview.empty();
    m_tabTransitionAvailable = !chains.tabTransition.empty();
    m_paneEffectsAvailable = !chains.paneEffects.empty();
    m_overlayTransitionAvailable = !chains.overlayTransition.empty();
    // A fade under way has nothing to draw it with: the overlays show as they are.
    if (!m_overlayTransitionAvailable) m_overlayFades.clear();
    if (!m_overviewAvailable) closeOverview(nullptr);
    m_renderer->setShaderChains(std::move(chains));
    updateAudioCapture();
    scheduleRender();
}

void TerminalWindow::applySettings(const Settings& previous)
{
    cancelKeyMode();
    const Settings& now = m_settings.settings();
    if (now.shaders != previous.shaders || now.shaderParams != previous.shaderParams) {
        applyShaders();
    }
    if (now.terminal != previous.terminal) {
        // A session pane takes them too, for what the window reads of them
        // (word boundaries, contrast, bidi). The daemon applies them to the
        // session's terminal, whose frames then bring the colors and modes.
        for (Pane* p : findChildren<Pane*>()) p->setOptions(now.terminal);
        setDockOptions(now.terminal);
    }
    if (m_session) m_session->sendSettings(sessionSettings(now, !m_session->remote()));
    if (now.colors != previous.colors) {
        for (Pane* p : findChildren<Pane*>()) {
            applySearchColors(p);
        }
    }
    if (now.fileTransferPassword != previous.fileTransferPassword) {
        for (Pane* p : findChildren<Pane*>()) {
            if (p->hasFileTransfers()) p->fileTransfers()->setPassword(now.fileTransferPassword);
        }
    }
    if (now.dragAnswerWait != previous.dragAnswerWait) {
        for (Pane* p : findChildren<Pane*>()) p->setDragAnswerWait(now.dragAnswerWait);
    }
    if (now.fileTransferDeltaLimit != previous.fileTransferDeltaLimit) {
        for (Pane* p : findChildren<Pane*>()) {
            if (p->hasFileTransfers()) p->fileTransfers()->setDeltaLimit(now.fileTransferDeltaLimit);
        }
    }
    if (now.scrollOnOutput != previous.scrollOnOutput || now.scrollOnInput != previous.scrollOnInput) {
        for (Pane* p : findChildren<Pane*>()) {
            p->setScrollPolicy(now.scrollOnOutput, now.scrollOnInput);
        }
    }
    m_blinkTimer.setInterval(now.cursorBlinkInterval);
    m_textBlinkTimer.setInterval(now.textBlinkInterval);
    m_autoScroll.setInterval(now.mouse.autoScrollInterval);
    // A coast under way goes on with the new value.
    m_momentum.setFriction(now.mouse.momentumScroll);
    if (now.window.backgroundOpacity != previous.window.backgroundOpacity || now.window.blur != previous.window.blur) {
        applyWindowEffects();
    }
    if (m_quick
        && (now.quickTerminal.height != previous.quickTerminal.height || now.quickTerminal.position != previous.quickTerminal.position
            || now.quickTerminal.screen != previous.quickTerminal.screen)) {
        setUpQuickTerminal();
    }
    m_layoutSize = QSize(); // padding may have changed
    updateChrome();
    updatePaletteActions(); // the bindings shown beside the actions
    scheduleRender();
}

void TerminalWindow::applySearchColors(Pane* pane)
{
    const HighlightColors& c = m_settings.settings().colors;
    pane->setSearchColors(packRgba(c.searchMatch.r, c.searchMatch.g, c.searchMatch.b),
                          packRgba(c.searchCurrentMatch.r, c.searchCurrentMatch.g, c.searchCurrentMatch.b),
                          packRgba(c.searchMatchText.r, c.searchMatchText.g, c.searchMatchText.b));
}

void TerminalWindow::applyWindowEffects()
{
    const WindowSettings& w = m_settings.settings().window;
    const bool translucent = w.backgroundOpacity < 1.0;
    // Content that was translucent once keeps the swapchain translucent (see renderNow).
    m_renderer->setTranslucent(translucent || m_contentTranslucent);
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
    const QuickTerminalSettings& quick = m_settings.settings().quickTerminal;
    // The output it names, or the primary one when it names none or one that is not there.
    QScreen* screen = QGuiApplication::primaryScreen();
    for (QScreen* candidate : QGuiApplication::screens()) {
        if (!quick.screen.isEmpty() && candidate->name() == quick.screen) screen = candidate;
    }
    LayerSurface* surface = LayerSurface::get(this);
    // A shown layer surface keeps its output (LayerSurface::setScreen refuses
    // it), so it keeps that output's size: toggleQuick sets it up again
    // before the next show, on the output named by then.
    if (isVisible() && surface->screen()) screen = surface->screen();
    const QRect avail = screen ? screen->availableGeometry() : QRect(0, 0, 1280, 800);
    m_quickHeight = int(std::lround(avail.height() * quick.height));
    surface->setLayer(LayerSurface::LayerTop);
    const auto edge = quick.position == QuickTerminalSettings::Position::Bottom ? LayerSurface::AnchorBottom : LayerSurface::AnchorTop;
    surface->setAnchors(LayerSurface::Anchors(edge | LayerSurface::AnchorLeft | LayerSurface::AnchorRight));
    surface->setKeyboardInteractivity(LayerSurface::KeyboardInteractivityOnDemand);
    surface->setScope(QStringLiteral("afterglow-quick-terminal"));
    surface->setExclusiveZone(0);
    surface->setDesiredSize(QSize(0, m_quickHeight));
    if (screen && !isVisible()) surface->setScreen(screen);
    // Margins from an earlier edge would push it off the new one.
    setQuickOffset(m_slide.isAnimating() ? slideOffset() : m_quickShown ? 0 : -m_quickHeight);
    resize(avail.width(), m_quickHeight);
    // A resolution change or a panel moving changes the space it fills.
    disconnect(m_quickScreenWatch);
    if (screen) m_quickScreenWatch = connect(screen, &QScreen::availableGeometryChanged, this, &TerminalWindow::setUpQuickTerminal);
}

void TerminalWindow::setQuickOffset(int offset)
{
    const bool bottom = m_settings.settings().quickTerminal.position == QuickTerminalSettings::Position::Bottom;
    PhosphorWayland::LayerSurface::get(this)->setMargins(bottom ? QMargins(0, 0, 0, offset) : QMargins(0, offset, 0, 0));
}

void TerminalWindow::quickDialogOpened(Pane* pane, QDialog* dialog)
{
    dialog->windowHandle()->setTransientParent(nullptr);
    const bool known = std::any_of(m_quickDialogs.begin(), m_quickDialogs.end(), [dialog](const auto& entry) { return entry.first == dialog; });
    if (!known) {
        m_quickDialogs.emplace_back(dialog, pane);
        m_quickDialogAdded = true;
    }
    // Queued: the quick terminal does not slide in the middle of a pane's
    // transfer.
    const auto queued = Qt::ConnectionType(Qt::QueuedConnection | Qt::UniqueConnection);
    connect(dialog, &QDialog::finished, this, &TerminalWindow::settleQuickDialogs, queued);
    connect(dialog, &QObject::destroyed, this, &TerminalWindow::settleQuickDialogs, queued);
    QMetaObject::invokeMethod(this, &TerminalWindow::settleQuickDialogs, Qt::QueuedConnection);
}

void TerminalWindow::settleQuickDialogs()
{
    // A quick terminal that gave its last tab away closes: it shows again
    // for nothing.
    if (m_tabs.empty()) {
        m_quickDialogs.clear();
        m_quickDialogAdded = m_quickOutForDialogs = false;
        return;
    }
    // A pane that went to another window has that window as its parent: one
    // that closed is gone, or is this window's until it is deleted.
    bool paneLeft = false;
    std::erase_if(m_quickDialogs, [this, &paneLeft](const auto& entry) {
        paneLeft |= entry.second && entry.second->parent() != this;
        return !entry.second || tabOf(entry.second) < 0 || !entry.first || !entry.first->isVisible();
    });
    // Only a dialog that came since the last look slides the quick terminal
    // out: one that the user toggled it in over stays behind it.
    if (!m_quickDialogs.empty()) {
        if (std::exchange(m_quickDialogAdded, false) && m_quickShown) {
            toggleQuick();
            m_quickOutForDialogs = true;
        }
        return;
    }
    m_quickDialogAdded = false;
    // A dialog that went with its pane to another window asks there: the
    // quick terminal would take the keyboard from that window.
    if (!std::exchange(m_quickOutForDialogs, false) || paneLeft) return;
    if (!m_quickShown) toggleQuick();
}

int TerminalWindow::slideOffset() const
{
    // The compositor sends no frame callback to a surface that is on no
    // output, and Qt then stops sending update requests: a frame drawn wholly
    // off screen would be the last frame of the slide. One pixel row stays on
    // screen until the slide ends, and the end of a slide out hides the window.
    return std::max(int(std::lround(m_slide.value())), 1 - m_quickHeight);
}

void TerminalWindow::toggleQuick()
{
    if (!m_quick) {
        return;
    }
    // The user's toggle wins over the slide out for dialogs.
    m_quickOutForDialogs = m_quickDialogAdded = false;
    const QuickTerminalSettings& quick = m_settings.settings().quickTerminal;
    const bool animate = quick.animate;
    const auto spec = motionSpec(quick.curve, quick.duration, m_quickCurve, "QuickTerminal.curve", QuickTerminalSettings{}.curve);
    m_quickShown = !m_quickShown;
    if (m_quickShown && !isVisible()) {
        // The output named while it showed, and that output's size.
        setUpQuickTerminal();
        setQuickOffset(animate ? -m_quickHeight : 0);
        show();
    }
    const qreal hidden = -qreal(m_quickHeight);
    if (m_quickShown) {
        requestActivate();
        // A slide with no distance to go does not start, and no frame ends it.
        if (!m_slide.start(animate ? (m_slide.isAnimating() ? m_slide.value() : hidden) : 0.0, 0.0, spec)) setQuickOffset(0);
    } else if (!animate || !m_slide.start(m_slide.isAnimating() ? m_slide.value() : 0.0, hidden, spec)) {
        // Toggled out before its slide in moved, it has no distance to go.
        // A hidden quick terminal has no slide: the next one starts at its edge.
        m_slide.cancel();
        hide();
        abortCaptures(QStringLiteral("the window is not on screen"));
        return;
    }
    scheduleRender();
}

void TerminalWindow::stopCopyMode()
{
    if (m_copyPane) m_copyPane->endCopyMode();
    m_copyPane = nullptr;
    updateKeyMode();
}
bool TerminalWindow::copyModeKey(QKeyEvent* e)
{
    if (!m_copyPane) return false;
    if (m_controller->wantsKeyboard()) { stopCopyMode(); return false; }
    if (m_settings.settings().keybinds.match(e, bindingBaseKey(e)) == Action::CopyMode) { stopCopyMode(); return true; }
    if (m_settings.settings().keybinds.match(e, bindingBaseKey(e)) == Action::Copy) { performAction(Action::Copy); stopCopyMode(); return true; }
    if (!m_copyPane->moveCopyCursor(0, 0)) { stopCopyMode(); return true; }
    int dx = 0, dy = 0, edge = 0;
    switch (e->key()) {
    case Qt::Key_Escape: case Qt::Key_Q: stopCopyMode(); return true;
    case Qt::Key_Y: case Qt::Key_Return: case Qt::Key_Enter:
        performAction(Action::Copy); stopCopyMode(); return true;
    case Qt::Key_V:
        m_copyPane->toggleCopySelection(e->modifiers().testFlag(Qt::ControlModifier) ? AfterglowVt::SelectionMode::Block
            : e->modifiers().testFlag(Qt::ShiftModifier) ? AfterglowVt::SelectionMode::Line : AfterglowVt::SelectionMode::Normal);
        break;
    case Qt::Key_Left: case Qt::Key_H: dx = -1; break;
    case Qt::Key_Right: case Qt::Key_L: dx = 1; break;
    case Qt::Key_Up: case Qt::Key_K: dy = -1; break;
    case Qt::Key_Down: case Qt::Key_J: dy = 1; break;
    case Qt::Key_PageUp: dy = -m_copyPane->rows(); break;
    case Qt::Key_PageDown: dy = m_copyPane->rows(); break;
    case Qt::Key_Home: edge = e->modifiers().testFlag(Qt::ControlModifier) ? 3 : 1; break;
    case Qt::Key_End: edge = e->modifiers().testFlag(Qt::ControlModifier) ? 4 : 2; break;
    case Qt::Key_Slash: stopCopyMode(); performAction(Action::Search); return true;
    default: break; // copy mode owns ordinary input until explicitly left
    }
    m_copyPane->moveCopyCursor(dx, dy, edge);
    scheduleRender();
    return true;
}

void TerminalWindow::keyPressEvent(QKeyEvent* e)
{
    if (!m_splits) return;
    // As in kitty: a key ends a coast (keyReleaseEvent too).
    stopMomentum();
    const quint32 physicalKey = physicalKeyOf(e);
    const bool repeat = e->isAutoRepeat();
    const auto taken = m_consumedKeys.constFind(physicalKey);
    const bool repeatOfTaken = repeat && taken != m_consumedKeys.constEnd();
    const bool repeatOfShortcut = repeatOfTaken && *taken;
    if (!repeat) {
        m_consumedKeys.remove(physicalKey);
        m_sentKeys.remove(physicalKey);
    }
    // A prompt that opened over a mode (a clipboard request, a script's
    // dialog) takes the keys, as copy mode already lets it.
    const bool chrome = m_controller->wantsKeyboard();
    // The held chord that opened a mode or a prompt does not act in it:
    // its repeats would close the overview, copy mode or the palette again.
    if (repeatOfShortcut && (chrome || keyboardModeActive())) return;
    if (copyModeKey(e)) { m_consumedKeys.insert(physicalKey, false); return; }
    if (bool shortcut = false; !chrome && scriptKeyModeKey(e, shortcut)) {
        m_consumedKeys.insert(physicalKey, shortcut);
        return;
    }
    if (!chrome && m_overview.open) {
        m_consumedKeys.insert(physicalKey, false);
        overviewKey(e);
        return;
    }
    if (!chrome && m_urlMode.active) {
        m_consumedKeys.insert(physicalKey, false);
        urlModeKey(e);
        return;
    }
    if (!chrome && m_unicodeInput) {
        m_consumedKeys.insert(physicalKey, false);
        unicodeInputKey(e);
        return;
    }
    if (e->key() == Qt::Key_Control || e->key() == Qt::Key_Shift || e->key() == Qt::Key_Alt) {
        updateLinkHover();
    }
    if (chrome) {
        m_consumedKeys.insert(physicalKey, false);
        cancelKeyMode();
        // The palette keybind still closes the palette, and the search
        // keybind still moves to search, while only they have the keyboard.
        // Over a prompt or a menu they would open on top of it, and an
        // action run from the palette would answer for it.
        const Action a = m_settings.settings().keybinds.match(e, bindingBaseKey(e));
        if (!chromePromptOpen() && (a == Action::CommandPalette || a == Action::Search)) {
            performAction(a);
            return;
        }
        m_chrome->deliver(e);
        return;
    }
    // A mode or a prompt took this key's press and closed on it (Enter on a
    // card, a hint's last letter): its repeats must not reach the shell.
    if (repeatOfTaken && !repeatOfShortcut) return;
    if (handleShortcut(e)) {
        // A shortcut that closed a held pane took the key as no shortcut.
        if (!m_consumedKeys.contains(physicalKey)) m_consumedKeys.insert(physicalKey, true);
        return;
    }
    // A shortcut's key that repeats into no shortcut (a key mode ended) stays here.
    if (repeatOfShortcut) return;
    sendKey(e);
}

void TerminalWindow::keyReleaseEvent(QKeyEvent* e)
{
    // As in kitty: a key, pressed or let go, ends a coast.
    stopMomentum();
    const quint32 physicalKey = physicalKeyOf(e);
    const Qt::KeyboardModifiers linkModifier = m_settings.settings().links.modifier;
    if ((e->key() == Qt::Key_Control && linkModifier == Qt::ControlModifier)
        || (e->key() == Qt::Key_Shift && linkModifier == Qt::ShiftModifier)
        || (e->key() == Qt::Key_Alt && linkModifier == Qt::AltModifier)) {
        // queryKeyboardModifiers() still reports the key during its own release.
        if (!m_hover.url.isEmpty() || m_hover.pane) {
            m_hover = {};
            applyPointerShape();
            scheduleRender();
        }
    }
    // The press reached a program: so does its release, whatever has the
    // keyboard now and whichever pane has the focus. Auto-repeat comes as
    // release/press pairs; only the real release counts.
    if (const auto sent = m_sentKeys.constFind(physicalKey); sent != m_sentKeys.constEnd()) {
        if (!e->isAutoRepeat()) {
            const QPointer<Pane> pane = sent->pane;
            m_sentKeys.erase(sent);
            if (takesInput(pane)) sendKey(e, pane);
        }
        return;
    }
    if (m_consumedKeys.contains(physicalKey)) {
        if (!e->isAutoRepeat()) m_consumedKeys.remove(physicalKey);
        if (m_controller->wantsKeyboard()) m_chrome->deliver(e);
        return;
    }
    // A press this window never saw (the focus came here with the key
    // held): no program gets a release for it.
    if (m_controller->wantsKeyboard()) {
        m_chrome->deliver(e);
    }
}

void TerminalWindow::releaseSentKeys()
{
    // A key that comes up during the drag sends nothing: Qt's drag takes
    // the key events. The keys come up here, the last pressed first, with
    // the modifiers held now. Each one is taken from now on: its later
    // repeats and its release go to no program.
    auto sent = std::exchange(m_sentKeys, {}).asKeyValueRange();
    std::vector<std::pair<quint32, SentKey>> keys(sent.begin(), sent.end());
    std::sort(keys.begin(), keys.end(), [](const auto& a, const auto& b) { return a.second.order > b.second.order; });
    const Qt::KeyboardModifiers modifiers = QGuiApplication::queryKeyboardModifiers();
    for (const auto& [physicalKey, key] : keys) {
        m_consumedKeys.insert(physicalKey, false);
        QKeyEvent release(QEvent::KeyRelease, key.key, modifiers, key.nativeScanCode, 0, 0, key.text);
        if (takesInput(key.pane)) sendKey(&release, key.pane);
    }
}

void TerminalWindow::sendKey(QKeyEvent* e, Pane* pane)
{
    Pane* target = pane ? pane : inputPane(m_focused);
    if (!target) {
        return;
    }
    if (!pane && e->isAutoRepeat()) {
        // A held key repeats only into the pane that got its press, which
        // gets its release too: a pane focused since gets no stray repeats.
        const auto sent = m_sentKeys.constFind(physicalKeyOf(e));
        if (sent != m_sentKeys.constEnd() && sent->pane.data() != target) return;
    }
    QByteArray storage;
    const AfterglowVt::KeyEvent ev = mapKeyEvent(e, storage);
    const std::string bytes = target->withTerminal([&ev](Terminal& t) { return AfterglowVt::encodeKey(ev, t.modes()); });
    if (bytes.empty()) {
        return;
    }
    if (ev.action != AfterglowVt::KeyAction::Release && target->isHeld()) {
        // --hold: its program has ended; any key closes it. The press is
        // taken, not sent: its repeats and its release do not reach the
        // pane focused next.
        m_consumedKeys.insert(physicalKeyOf(e), false);
        closePane(target);
        return;
    }
    if (ev.action != AfterglowVt::KeyAction::Release) {
        target->clearSelection();
        noteTyping();
        m_sentKeys.insert(physicalKeyOf(e), {target, e->key(), e->text(), e->nativeScanCode(), ++m_sentKeyOrder});
    }
    target->sendInput(bytes);
}

void TerminalWindow::noteTyping()
{
    restartBlink();
    if (m_settings.settings().hideMouseWhileTyping && !m_pointerHidden) {
        m_pointerHidden = true;
        setCursor(Qt::BlankCursor);
    }
}

void TerminalWindow::copyOnSelect(Pane* pane)
{
    const Settings::CopyOnSelect mode = m_settings.settings().copyOnSelect;
    if (mode == Settings::CopyOnSelect::Off || !pane || !pane->hasSelection()) {
        return;
    }
    const QString text = pane->selectedText();
    QClipboard* clipboard = QGuiApplication::clipboard();
    if (clipboard->supportsSelection()) {
        clipboard->setText(text, QClipboard::Selection);
    }
    if (mode == Settings::CopyOnSelect::Clipboard) {
        clipboard->setText(text, QClipboard::Clipboard);
    }
}

void TerminalWindow::copySelection(bool toClipboard)
{
    const Pane* target = inputPane(m_focused);
    if (!target || !target->hasSelection()) {
        return;
    }
    QGuiApplication::clipboard()->setText(target->selectedText(),
                                          toClipboard ? QClipboard::Clipboard : QClipboard::Selection);
}

void TerminalWindow::clipboardRequest(Pane* pane, const AfterglowVt::ClipboardRequest& r)
{
    QClipboard* clipboard = QGuiApplication::clipboard();
    if (r.kind == AfterglowVt::ClipboardRequest::Kind::Write) {
        if (r.primary && !clipboard->supportsSelection()) {
            pane->sendReply(r.refusal("ENOSYS"));
            return;
        }
        clipboardWrite(pane, r);
        return;
    }
    const QString buffer = r.primary ? tr("primary selection") : tr("clipboard");
    const Settings::Permission policy = m_settings.settings().clipboardRead;
    if (r.osc52) {
        // An OSC 52 read: no name or password to remember a grant by, so
        // the setting decides, or the user does each time.
        if (policy == Settings::Permission::Deny || (r.primary && !clipboard->supportsSelection())) {
            pane->sendReply(r.refusal("EPERM"));
        } else if (policy == Settings::Permission::Allow) {
            answerClipboardRead(pane, r);
        } else if (m_pendingClipboardRead) {
            pane->sendReply(r.refusal("EBUSY")); // one prompt at a time
        } else {
            m_pendingClipboardRead = PendingClipboardRead{pane, r};
            m_controller->askClipboard(tr("%1 wants to read the %2.").arg(clipboardAsker(pane), buffer), {});
            // The prompt shows here; the window does not jump forward for a program.
            if (!isActive()) alert(0);
        }
        return;
    }
    if (r.primary && !clipboard->supportsSelection()) {
        pane->sendReply(r.refusal("ENOSYS"));
        return;
    }
    // Listing the types needs no permission.
    if (r.mimes.size() == 1 && r.mimes[0] == ".") {
        answerClipboardRead(pane, r);
        return;
    }
    if (policy == Settings::Permission::Deny) {
        pane->sendReply(r.refusal("EPERM"));
        return;
    }
    auto& grants = m_clipboardGrants[pane];
    const QByteArray password = QByteArray::fromStdString(r.password);
    if (!password.isEmpty()) {
        if (const auto it = grants.find(password); it != grants.end()) {
            const ClipboardGrant grant = *it;
            if (grant.oneTime) {
                grants.erase(it);
            }
            // A paste's password reads what was pasted, soon after: not the
            // other buffer, and not whatever the user copies later.
            const bool expired = grant.expires && std::chrono::steady_clock::now() > *grant.expires;
            const bool otherBuffer = grant.primary && *grant.primary != r.primary;
            if (!expired && !otherBuffer) {
                answerClipboardRead(pane, r);
                return;
            }
        }
    }
    if (policy == Settings::Permission::Allow) {
        answerClipboardRead(pane, r);
        return;
    }
    if (m_pendingClipboardRead) {
        pane->sendReply(r.refusal("EBUSY"));
        return;
    }
    // Ask. The name, when the program gave one with a password, can be remembered.
    QStringList types;
    for (const std::string& m : r.mimes) {
        types.append(QString::fromStdString(m));
    }
    const QString name = r.password.empty() ? QString() : sanitizedText(QString::fromStdString(r.name), 80);
    m_pendingClipboardRead = PendingClipboardRead{pane, r};
    m_controller->askClipboard(tr("%1 wants to read the %2 (%3).")
                                   .arg(clipboardAsker(pane, name), buffer, sanitizedText(types.join(QStringLiteral(", ")), 200)),
                               name);
    if (!isActive()) alert(0);
}

QString TerminalWindow::clipboardAsker(Pane* pane, const QString& name) const
{
    QString text = sanitizedText(!name.isEmpty() ? name : !pane->title().isEmpty() ? pane->title() : tr("A program"), 80);
    // An overlay's program is where the pane it runs over is.
    const Pane* place = hostOf(pane);
    if (const int tab = tabOf(place); tab != m_currentTab) text += tr(" in tab %1").arg(tab + 1);
    else if (place != m_focused) text += tr(" in another split");
    return text;
}

void TerminalWindow::clipboardWrite(Pane* pane, const AfterglowVt::ClipboardRequest& r)
{
    switch (m_settings.settings().clipboardWrite) {
    case Settings::Permission::Allow:
        writeClipboard(pane, r);
        return;
    case Settings::Permission::Deny:
        pane->sendReply(r.refusal("EPERM"));
        return;
    case Settings::Permission::Ask:
        break;
    }
    if (m_pendingClipboardRead) {
        pane->sendReply(r.refusal("EBUSY"));
        return;
    }
    m_pendingClipboardRead = PendingClipboardRead{pane, r, true};
    m_controller->askClipboard(tr("%1 wants to replace the %2.").arg(clipboardAsker(pane), r.primary ? tr("primary selection") : tr("clipboard")), {});
    if (!isActive()) alert(0);
}

void TerminalWindow::writeClipboard(Pane* pane, const AfterglowVt::ClipboardRequest& r)
{
    using AfterglowVt::ClipboardAssembler;
    QClipboard* clipboard = QGuiApplication::clipboard();
    const QClipboard::Mode mode = r.primary ? QClipboard::Selection : QClipboard::Clipboard;
    if (r.osc52) {
        clipboard->setText(r.data.empty() ? QString() : QString::fromStdString(r.data.front().second), mode);
        return;
    }
    auto* data = new QMimeData;
    QHash<QString, QByteArray> byType;
    for (const auto& [mime, bytes] : r.data) {
        byType.insert(QString::fromStdString(mime), QByteArray::fromStdString(bytes));
    }
    for (const auto& [alias, target] : r.aliases) {
        if (const auto it = byType.constFind(QString::fromStdString(target)); it != byType.cend()) {
            byType.insert(QString::fromStdString(alias), *it);
        }
    }
    for (auto it = byType.cbegin(); it != byType.cend(); ++it) {
        data->setData(it.key(), it.value());
    }
    clipboard->setMimeData(data, mode);
    pane->sendReply(ClipboardAssembler::writeStatus("DONE", r.id));
}

void TerminalWindow::answerClipboardRead(Pane* pane, const AfterglowVt::ClipboardRequest& r, std::string_view oneTimePassword)
{
    using AfterglowVt::ClipboardAssembler;
    if (r.osc52) {
        const QByteArray text = truncatedUtf8(
            QGuiApplication::clipboard()->text(r.primary ? QClipboard::Selection : QClipboard::Clipboard).toUtf8(), MaxClipboardReply);
        pane->sendReply(r.osc52Reply(std::string_view(text.constData(), std::size_t(text.size()))));
        return;
    }
    const QMimeData* data = QGuiApplication::clipboard()->mimeData(r.primary ? QClipboard::Selection : QClipboard::Clipboard);
    std::string out = ClipboardAssembler::readStatus("OK", r, oneTimePassword);
    for (const std::string& mime : r.mimes) {
        if (mime == ".") {
            std::string list;
            if (data) {
                list = data->formats().join(QLatin1Char(' ')).toStdString();
                if (!list.empty()) {
                    list += '\n';
                }
            }
            out += ClipboardAssembler::readData(".", list, r, oneTimePassword);
            continue;
        }
        if (!data) {
            continue;
        }
        const QString type = QString::fromStdString(mime);
        QByteArray bytes;
        if (data->hasFormat(type)) {
            bytes = data->data(type);
        } else if (type.startsWith(QLatin1String("text/plain")) && data->hasText()) {
            bytes = data->text().toUtf8();
        } else {
            continue;
        }
        if (bytes.size() > MaxClipboardReply) {
            // Too large for one reply: say so, rather than send part of it.
            pane->sendReply(ClipboardAssembler::readStatus("EFBIG", r, oneTimePassword));
            return;
        }
        out += ClipboardAssembler::readData(mime, std::string_view(bytes.constData(), std::size_t(bytes.size())), r,
                                            oneTimePassword);
    }
    out += ClipboardAssembler::readStatus("DONE", r, oneTimePassword);
    if (!pane->sendReply(out)) {
        // The input queue refused it whole: the program still gets an end.
        pane->sendReply(ClipboardAssembler::readStatus("EIO", r, oneTimePassword));
    }
}

bool TerminalWindow::sendPasteEvent(QClipboard::Mode mode)
{
    Pane* target = inputPane(m_focused);
    if (!target || !target->withTerminal([](Terminal& t) { return t.modes().pasteEvents; })) {
        return false;
    }
    // The program gets the list of types and a one-time password to read
    // them: from the buffer that was pasted, for a short time. A new paste
    // replaces the passwords of earlier ones the program did not use.
    auto& grants = m_clipboardGrants[target];
    for (auto it = grants.begin(); it != grants.end();) {
        it = it->oneTime ? grants.erase(it) : std::next(it);
    }
    const QByteArray password = QUuid::createUuid().toByteArray(QUuid::WithoutBraces);
    grants[password] = ClipboardGrant{true, mode == QClipboard::Selection, std::chrono::steady_clock::now() + std::chrono::seconds(m_settings.settings().pasteReadTimeout)};
    AfterglowVt::ClipboardRequest listing;
    listing.primary = mode == QClipboard::Selection;
    listing.mimes = {"."};
    answerClipboardRead(target, listing, std::string_view(password.constData(), std::size_t(password.size())));
    return true;
}

void TerminalWindow::paste(QClipboard::Mode mode)
{
    // An overlay's program gets the paste while one runs over the pane.
    Pane* target = inputPane(m_focused);
    if (!target) {
        return;
    }
    if (sendPasteEvent(mode)) {
        return;
    }
    pasteText(target, QGuiApplication::clipboard()->text(mode).toUtf8());
}

void TerminalWindow::pasteText(Pane* target, const QByteArray& text)
{
    if (text.isEmpty()) {
        return;
    }
    if (text.size() > MaxPaste) {
        NotificationCenter::instance().showNotice(target, tr("Paste too large"),
            tr("The text is %1, and a paste holds at most %2.")
                .arg(QLocale().formattedDataSize(text.size()), QLocale().formattedDataSize(MaxPaste)));
        return;
    }
    // Any raw line terminator can execute input without bracketed paste.
    const bool bracketed = target->withTerminal([](Terminal& t) { return t.modes().bracketedPaste; });
    if (m_settings.settings().confirmUnsafePaste && !bracketed && (text.contains('\n') || text.contains('\r'))) {
        m_pendingPaste = text;
        m_pendingPastePane = target;
        const QByteArray normalized = QByteArray(text).replace("\r\n", "\n").replace('\r', '\n');
        // The preview shows what a shell would get: without the controls
        // that encodePaste drops, a tab as its picture, cut on a character,
        // with how much it leaves out. What the shell gets but the preview
        // could not show as it is (a bidirectional override, a zero-width
        // character, a line separator) shows as its code point.
        const QByteArray shown = truncatedUtf8(normalized, 4000);
        QString preview;
        for (const QChar c : QString::fromUtf8(shown)) {
            const char16_t u = c.unicode();
            if (u == u'\t') preview += QChar(char16_t(0x2409));
            else if (disguisesText(u)) preview += QStringLiteral("<U+%1>").arg(QString::number(uint(u), 16).toUpper().rightJustified(4, QLatin1Char('0')));
            else if (u == u'\n' || (u >= 0x20 && (u < 0x7F || u > 0x9F))) preview += c;
        }
        // "command\n" is one line, not two.
        const int lines = int(normalized.count('\n')) + (normalized.endsWith('\n') ? 0 : 1);
        m_controller->askPaste(preview, std::max(1, lines), int(normalized.size() - shown.size()));
        return;
    }
    // Encoded outside the terminal's lock, as in pasteAnswered.
    const AfterglowVt::Modes modes = target->withTerminal([](Terminal& t) { return t.modes(); });
    target->sendInput(AfterglowVt::encodePaste(std::string_view(text.constData(), std::size_t(text.size())), modes));
}

float TerminalWindow::drawnScrollShift(const Pane* pane) const
{
    const auto found = m_motion.find(pane);
    return found == m_motion.end() ? 0.0f : found->second->scrollShift;
}

QPointF TerminalWindow::windowContentPixel(const QPointF& logical) const
{
    // The window chain draws over the composed panes, so its maps come first.
    return m_renderer->windowContentPoint(logical * devicePixelRatio());
}

QPointF TerminalWindow::paneContentPixel(Pane* pane, const QPointF& logical) const
{
    return m_renderer->paneContentPoint(windowContentPixel(logical), hostOf(pane)->id());
}

QPoint TerminalWindow::dividerPixel(const QPointF& logical) const
{
    const QPointF px = windowContentPixel(logical);
    return QPoint(int(std::floor(px.x())), int(std::floor(px.y())));
}

Pane* TerminalWindow::paneShownAt(const QPointF& logical) const
{
    if (!m_splits || !QRectF(QPointF(0, 0), QSizeF(size())).contains(logical)) return nullptr;
    // A map that sends the point out of the window shows the window's edge
    // there: the pane at that edge.
    const QSize window = windowPixels();
    const QPoint px = dividerPixel(logical);
    return m_splits->paneAt(QPoint(std::clamp(px.x(), 0, std::max(0, window.width() - 1)),
                                   std::clamp(px.y(), 0, std::max(0, window.height() - 1))));
}

QPointF TerminalWindow::gridPixelAt(Pane* pane, const QPointF& pixel) const
{
    const QPoint origin = contentRect(pane, m_splits->rectOf(hostOf(pane))).topLeft();
    // The rows were drawn higher by the scroll shift.
    return pixel - QPointF(origin) + QPointF(0, drawnScrollShift(pane));
}

QPointF TerminalWindow::gridPixel(Pane* pane, const QPointF& pos) const
{
    return gridPixelAt(pane, paneContentPixel(pane, pos));
}

bool TerminalWindow::overCells(Pane* pane, const QPointF& pos) const
{
    const auto& m = m_renderer->fonts().fonts->metrics();
    const QPointF px = gridPixel(pane, pos);
    return px.x() >= 0 && px.y() >= 0 && px.x() < double(pane->cols()) * m.cellWidth
        && px.y() < double(pane->rows()) * m.cellHeight;
}

QPoint TerminalWindow::cellPixel(Pane* pane, const QPointF& pos) const
{
    // The pixel the point is in, as cellAt() finds its cell, on the grid.
    const auto& m = m_renderer->fonts().fonts->metrics();
    const QPointF px = gridPixel(pane, pos);
    return QPoint(std::clamp(int(std::floor(px.x())), 0, std::max(0, pane->cols() * m.cellWidth - 1)),
                  std::clamp(int(std::floor(px.y())), 0, std::max(0, pane->rows() * m.cellHeight - 1)));
}

QPoint TerminalWindow::cellAt(Pane* pane, const QPointF& pos) const
{
    return cellOf(pane, gridPixel(pane, pos));
}

QPoint TerminalWindow::cellOf(Pane* pane, const QPointF& px) const
{
    const auto& m = m_renderer->fonts().fonts->metrics();
    const int col = std::clamp(int(std::floor(px.x() / m.cellWidth)), 0, std::max(0, pane->cols() - 1));
    const int row = std::clamp(int(std::floor(px.y() / m.cellHeight)), 0, std::max(0, pane->rows() - 1));
    // On a double-size line each cell is drawn twice as wide.
    if (pane->snapshot().lineSize(row) != AfterglowVt::LineSize::Single) {
        return QPoint(col / 2, row);
    }
    return QPoint(pane->snapshot().logicalColumn(col, row), row);
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
    // A pane that a tab switch or a zoom took off screen while a button was
    // held: the pointer is over none of its cells. Its release goes where
    // the last report was, and its drags go nowhere.
    const bool shown = paneLaidOut(pane);
    if (!shown && action != int(AfterglowVt::MouseAction::Release)) return true;
    const QPoint cell = shown ? cellAt(pane, pos) : pane == m_lastMousePane ? m_lastMouseCell : QPoint();
    if (action == int(AfterglowVt::MouseAction::Motion) && cell == m_lastMouseCell && pane == m_lastMousePane
        && modes.mouseFormat != AfterglowVt::MouseFormat::SgrPixels) {
        return true;
    }
    m_lastMouseCell = cell;
    m_lastMousePane = pane;

    AfterglowVt::MouseEvent ev;
    ev.button = button < 0 ? AfterglowVt::MouseButton::None : AfterglowVt::MouseButton(button);
    ev.action = AfterglowVt::MouseAction(action);
    ev.mods = mapModifiers(mods);
    ev.col = cell.x();
    ev.row = cell.y();
    const auto& metrics = m_renderer->fonts().fonts->metrics();
    const QPoint px = shown ? cellPixel(pane, pos) : QPoint(cell.x() * metrics.cellWidth, cell.y() * metrics.cellHeight);
    ev.pixelX = px.x();
    ev.pixelY = px.y();
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
    return int(std::floor(gridPixel(pane, pos).y() / m.cellHeight));
}

void TerminalWindow::emitDockEvent(const DockArea& dock, const QString& handler, QVariantMap event, bool limited)
{
    event.insert(QStringLiteral("dock"), dock.id());
    event.insert(QStringLiteral("window"), m_id);
    // A pane's dock speaks of its pane, the window's of the focused pane.
    event.insert(QStringLiteral("scope"), dock.pane() ? QStringLiteral("pane") : QStringLiteral("window"));
    const quint32 pane = dock.pane() ? dock.pane() : m_focused ? m_focused->id() : 0;
    Q_EMIT scriptDockEvent(dock.owner(), handler, pane, event, limited);
}

void TerminalWindow::emitDockClick(const DockArea& dock, const QString& item, const QString& button, bool limited, const QPointF& at)
{
    m_dockClickAt = at;
    emitDockEvent(dock, QStringLiteral("on_dock_click"), {{QStringLiteral("item"), item}, {QStringLiteral("button"), button}}, limited);
}

bool TerminalWindow::pressDock(QMouseEvent* e)
{
    DockArea* dock = dockAt(e->position());
    if (!dock) return false;
    const QPoint px = dockPixelOf(*dock, e->position());
    // A dock is its script's: the panes under it get nothing.
    m_dockButtons |= e->button();
    const QString item = dock->itemAt(px);
    const QString button = e->button() == Qt::LeftButton ? QStringLiteral("left")
        : e->button() == Qt::MiddleButton                ? QStringLiteral("middle")
        : e->button() == Qt::RightButton                 ? QStringLiteral("right")
                                                         : QString();
    if (!item.isEmpty() && !button.isEmpty()) emitDockClick(*dock, item, button, false, e->position());
    // The left button on an item can drag it.
    if (e->button() == Qt::LeftButton && !item.isEmpty()) {
        m_dockDrag = DockDrag{dock->owner(), dock->id(), item, dock->pane(), e->position(), false, {}, dockCell(*dock, px)};
    }
    return true;
}

void TerminalWindow::mousePressEvent(QMouseEvent* e)
{
    if (!m_splits) return;
    stopMomentum();
    // As in kitty, a finger does not show the pointer that typing hid: the
    // pointer is not where the finger is.
    if (m_pointerHidden && !fromTouch(e)) {
        m_pointerHidden = false;
        applyPointerShape();
    }
    // The first button down starts a new gesture: a press whose release
    // went elsewhere (a compositor grab) takes nothing from this one.
    if (e->buttons() == e->button()) {
        m_overviewButtons = Qt::NoButton;
        m_dockButtons = Qt::NoButton;
        endDockDrag(false);
        m_chromeGrab = false;
        m_reportedButtons = Qt::NoButton;
        m_reportedPane = nullptr;
        m_selectionPane = nullptr;
        m_textDragPane = nullptr;
        m_autoScroll.stop();
        // A dock click is where a tab menu opens only until the next click.
        m_dockClickAt.reset();
    }
    // A prompt that opened over the overview takes the pointer first, as
    // outside it.
    if (m_overview.open && m_controller->wantsKeyboard() && routePointerToChrome(e, e->position(), true, false)) return;
    // A dock draws over the overview too, and is its script's there as well.
    if (m_overview.open && pressDock(e)) return;
    if (m_overview.open) {
        // The cards are where the shader draws them: a click takes the one
        // in front. Its release is the overview's too.
        m_overviewButtons |= e->button();
        closeOverview(e->button() == Qt::LeftButton ? m_overview.highlighted.data() : nullptr);
        return;
    }
    if (routePointerToChrome(e, e->position(), true, false)) {
        return;
    }
    // A plain left press in a divider's grab takes the divider, also where
    // the grab reaches into a pane's dock.
    const bool divider = e->button() == Qt::LeftButton && !(e->modifiers() & Qt::ControlModifier)
        && dividerTakesDock(e->position());
    if (!divider && pressDock(e)) return;
    Pane* p = paneShownAt(e->position());
    if (p && p != m_focused) {
        setFocusedPane(p);
    }
    // The cells under the pointer are an overlay's while one runs over the pane.
    Pane* const target = inputPane(m_focused);
    if (!target) {
        return;
    }
    m_pointer = e->position();
    updateLinkHover();
    if (e->button() == Qt::LeftButton && !(e->modifiers() & Qt::ControlModifier)) {
        if (auto divider = m_splits->dividerAt(dividerPixel(e->position()), dividerTolerance())) {
            m_dividerDrag = divider;
            return;
        }
    }
    // A left press where no pane shows (between panes, over the tab bar's
    // band) starts no selection. No press there reaches a program. The menu
    // and a middle paste are the focused pane's, wherever the press is, but
    // not around a popup (below).
    if (!p && e->button() == Qt::LeftButton) return;
    // Around a popup the pane only shows: the press reaches nothing.
    if (!pointerTarget(m_focused, e->position())) return;
    const Qt::KeyboardModifiers linkModifier = m_settings.settings().links.modifier;
    if (e->button() == Qt::LeftButton && linkModifier != Qt::NoModifier && (e->modifiers() & linkModifier)
        && !m_hover.url.isEmpty()) {
        openLink(m_hover.url);
        return;
    }
    // Buttons another pane's program still holds (an overlay opened over it
    // meanwhile): a press here would have its release go there. No press
    // reaches a program until they are up, so each press pairs with its release.
    if (m_reportedButtons != Qt::NoButton && m_reportedPane != target) {
        if (takesInput(m_reportedPane)) return;
        m_reportedButtons = Qt::NoButton; // its pane left: no release is owed
    }
    if (e->button() == Qt::LeftButton && target->hasDragDrop()) target->dragDrop()->press(e->position().toPoint());
    const int code = mouseButtonCode(e->button());
    // A program hears only of presses on its pane: not of one in a gap,
    // which would come to it at a clamped cell nobody pointed at.
    if (p && code >= 0 && sendMouse(target, e->position(), code, int(AfterglowVt::MouseAction::Press), e->modifiers())) {
        // Its motion and release go to this pane even if the focus moves meanwhile.
        if (m_reportedButtons == Qt::NoButton) m_reportedPane = target;
        m_reportedButtons |= e->button();
        return;
    }

    const MouseSettings& mouse = m_settings.settings().mouse;
    if (e->button() == Qt::MiddleButton) {
        if (mouse.middleClick != MouseSettings::MiddleClick::None) {
            paste(mouse.middleClick == MouseSettings::MiddleClick::Clipboard ? QClipboard::Clipboard : QClipboard::Selection);
        }
        return;
    }
    if (e->button() == Qt::RightButton) {
        switch (mouse.rightClick) {
        case MouseSettings::RightClick::Menu: {
            // Only a link under the pointer: not one at the focused pane's
            // edge, where a click on a divider would clamp to.
            Pane* under = paneUnder(e->position());
            openContextMenu(e->position(), under == m_focused ? linkAt(inputPane(m_focused), e->position()).url : QString());
            break;
        }
        case MouseSettings::RightClick::Paste:
            paste(QClipboard::Clipboard);
            break;
        case MouseSettings::RightClick::CopyOrPaste:
            if (target->hasSelection()) {
                copySelection(true);
                target->clearSelection();
                scheduleRender();
            } else {
                paste(QClipboard::Clipboard);
            }
            break;
        case MouseSettings::RightClick::Extend:
            if (target->hasSelection()) {
                const QPoint cell = cellAt(target, e->position());
                target->extendSelection(target->selectionPointAt(cell.x(), cell.y()));
                copyOnSelect(target);
                scheduleRender();
            }
            break;
        }
        return;
    }
    if (e->button() != Qt::LeftButton) {
        return;
    }

    // Count clicks for word (2) and line (3) selection: as in kitty, within
    // half a row of the last press, which a finger's taps keep to even
    // across the edge of a cell.
    const QPoint cell = cellAt(target, e->position());
    const int interval = QGuiApplication::styleHints()->mouseDoubleClickInterval();
    const double radius = m_renderer->fonts().fonts->metrics().cellHeight / devicePixelRatio() / 2;
    const bool series = m_lastClick.isValid() && m_lastClick.elapsed() < interval
        && QLineF(e->position(), m_lastClickAt).length() <= radius && target == m_lastClickPane;
    m_clickCount = series ? m_clickCount % 3 + 1 : 1;
    m_lastClick.start();
    m_lastClickCell = cell;
    m_lastClickAt = e->position();
    m_lastClickPane = target;

    AfterglowVt::SelectionMode mode = AfterglowVt::SelectionMode::Normal;
    if (m_clickCount == 2) {
        mode = AfterglowVt::SelectionMode::Word;
    } else if (m_clickCount == 3) {
        mode = AfterglowVt::SelectionMode::Line;
    } else if (e->modifiers() & Qt::AltModifier) {
        mode = AfterglowVt::SelectionMode::Block;
    }
    // A single plain press inside the selection waits: a move drags the text
    // out, a release clicks (mouseReleaseEvent). Only a mouse drags: the
    // presses of a finger (touchMouse) and of a pen, which Qt makes of the
    // tablet input that the window does not handle, are synthesized, and a
    // finger that strokes over the selection selects. The fourth click of a
    // series counts as one again, inside the line that the third selected:
    // it selects too.
    if (!series && mode == AfterglowVt::SelectionMode::Normal && e->source() == Qt::MouseEventNotSynthesized
        && m_settings.settings().mouse.dragSelection && target->selectionContains(cell.x(), cell.y())) {
        m_textDragPane = target;
        m_textDragFrom = e->position();
        return;
    }
    beginSelection(target, e->position(), mode);
}

void TerminalWindow::beginSelection(Pane* pane, const QPointF& pos, AfterglowVt::SelectionMode mode)
{
    m_selectionPane = pane;
    m_selectionDragged = false;
    m_dragPos = pos;
    const QPoint cell = cellAt(pane, pos);
    pane->startSelection(pane->selectionPointAt(cell.x(), cell.y()), mode,
                         mode == AfterglowVt::SelectionMode::Word || mode == AfterglowVt::SelectionMode::Line);
    scheduleRender();
}

bool TerminalWindow::dragSelectionText(Pane* pane)
{
    const QString text = pane->selectedText();
    if (text.isEmpty()) return false;
    auto* mime = new QMimeData;
    mime->setText(text);
    const QPointer<QDrag> drag = new QDrag(this);
    drag->setMimeData(mime);
    releaseSentKeys();
    // The selection stays: the text is copied, not moved out of the pane.
    // exec() returns when the drop ends, and events go on meanwhile: the
    // window that closes in that time goes after it (QEvent::Close).
    m_textDragSource = hostOf(pane);
    m_textDragging = true;
    const NestedLoop loop(this);
    drag->exec(Qt::CopyAction, Qt::CopyAction);
    m_textDragging = false;
    m_textDragSource = nullptr;
    if (drag) drag->deleteLater();
    // The drag took the button's release: the drags that programs offer
    // (OSC 72) see it here, and the next press starts no double click.
    for (auto* p : terminalPanes()) if (p->hasDragDrop()) p->dragDrop()->release();
    m_lastClick.invalidate();
    return true;
}

void TerminalWindow::updateSelectionDrag()
{
    Pane* pane = m_selectionPane;
    if (!pane) {
        m_autoScroll.stop();
        return;
    }
    // Dragging past the top or bottom scrolls through history, from under
    // any hint labels.
    const int row = rawRowAt(pane, m_dragPos);
    if ((row < 0 || row >= pane->rows()) && m_urlMode.active && m_urlMode.pane == pane) stopUrlMode();
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
    if (row < 0 || row >= pane->rows() || cell != m_lastClickCell) m_selectionDragged = true;
    pane->extendSelection(pane->selectionPointAt(cell.x(), cell.y()));
    scheduleRender();
}

void TerminalWindow::mouseReleaseEvent(QMouseEvent* e)
{
    // A drag that ends over a dock item puts the pointer on it, with no move.
    const auto hover = qScopeGuard([self = QPointer<TerminalWindow>(this)] {
        if (self) self->updateDockHover();
    });
    // Only the press the overview took has its release taken too; any other
    // release ends what its press started (a drag, a reported button, a
    // selection), even when the overview opened or closed in between.
    if (m_overviewButtons & e->button()) {
        m_overviewButtons &= ~e->button();
        return;
    }
    // Only the buttons pressed on a dock: another button's release still
    // ends what its press started on a pane or the chrome.
    if (m_dockButtons & e->button()) {
        m_dockButtons &= ~e->button();
        if (e->button() == Qt::LeftButton) endDockDrag(true, e->position());
        return;
    }
    // An overlay's program may hold the press as well: its drag ends too.
    if (e->button() == Qt::LeftButton) for (auto* pane : terminalPanes()) if (pane->hasDragDrop()) pane->dragDrop()->release();
    if (m_dividerDrag && e->button() == Qt::LeftButton) {
        m_dividerDrag.reset();
        applyPointerShape();
        return;
    }
    if (e->button() == Qt::LeftButton && m_textDragPane) {
        // A press inside the selection that did not drag: the click that it
        // held back, which clears the selection as any click does. Released
        // on another cell, it counts as a drag: it selects nothing, opens no
        // link and moves no cursor.
        Pane* pane = std::exchange(m_textDragPane, nullptr);
        beginSelection(pane, m_textDragFrom, AfterglowVt::SelectionMode::Normal);
        m_selectionDragged = cellAt(pane, e->position()) != cellAt(pane, m_textDragFrom);
    }
    // A press that a pane took has its release there too, over the chrome
    // as well: a selection dragged up to the tab bar ends, and a program
    // hears its button go up.
    // The chrome may hold another button's press meanwhile: that grab is
    // not this button's.
    const bool paneGesture = (m_reportedButtons & e->button()) || (e->button() == Qt::LeftButton && m_selectionPane);
    if (!paneGesture && routePointerToChrome(e, e->position(), false, true)) {
        return;
    }
    if (m_reportedButtons & e->button()) {
        m_reportedButtons &= ~e->button();
        const int code = mouseButtonCode(e->button());
        // To the program that got the press. It was reported, so the
        // release is too: Shift only keeps new presses from the program.
        const QPointer<Pane> pane = m_reportedPane;
        if (m_reportedButtons == Qt::NoButton) m_reportedPane = nullptr;
        if (takesInput(pane)) {
            sendMouse(pane, e->position(), code, int(AfterglowVt::MouseAction::Release), e->modifiers() & ~Qt::ShiftModifier);
        }
        return;
    }
    if (e->button() == Qt::LeftButton && m_selectionPane) {
        m_autoScroll.stop();
        if (m_selectionPane->hasSelection()) {
            copyOnSelect(m_selectionPane);
        } else if (overviewShowing()) {
            m_selectionPane->clearSelection(); // the click was not on the pane as drawn now
        } else if (m_selectionDragged) {
            // A drag whose selection output cleared: it opens no link and
            // moves no cursor, as a click would.
            m_selectionPane->clearSelection();
            scheduleRender();
        } else {
            m_selectionPane->clearSelection();
            // A click ends where it began: let go elsewhere (over the tab
            // bar), it opens no link and moves no cursor.
            const bool inside = pointerTarget(paneUnder(e->position()), e->position()) == m_selectionPane
                && overCells(m_selectionPane, e->position());
            const bool plain = inside && !(e->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier));
            if (m_clickCount == 1 && plain && m_settings.settings().links.modifier == Qt::NoModifier
                && !m_hover.url.isEmpty()) {
                openLink(m_hover.url); // links open with a plain click
            } else if (m_clickCount == 1 && plain) {
                moveCursorToClick(m_selectionPane, e->position());
            }
            scheduleRender();
        }
        m_selectionPane = nullptr;
    }
}

void TerminalWindow::moveCursorToClick(Pane* pane, const QPointF& pos)
{
    // The shell asked (OSC 133 click_events) for clicks on its command line
    // to move the cursor: as many arrow keys as characters between there
    // and here. A line editor moves by character, not by cell.
    const QPoint cell = cellAt(pane, pos);
    const std::string keys = pane->withTerminal([&cell](Terminal& t) -> std::string {
        if (!t.promptTakesClicks() || t.screen().viewportOffset() != 0
            || t.modes().mouseTracking != AfterglowVt::MouseTracking::None) {
            return {};
        }
        const auto& screen = t.screen();
        const auto& cur = screen.cursor;
        // Only on the prompt's own lines: from where it starts to the bottom.
        int promptRow = cur.y;
        while (promptRow > 0 && !screen.row(promptRow).promptStart) {
            --promptRow;
        }
        if (cell.y() < promptRow) {
            return {};
        }
        // Where a row's text ends: a wrapped row runs on into the next; a
        // row that ends in a line break stops after its last character,
        // and the break itself is one more character.
        const auto contentEnd = [&screen, &t](int y) {
            const auto& row = screen.row(y);
            if (row.wrapped) return t.cols();
            int end = t.cols();
            while (end > 0 && row.at(end - 1).codepoint() == 0) --end;
            return end;
        };
        const auto characters = [&screen](int y, int from, int to) {
            long n = 0;
            for (int x = std::max(0, from); x < to; ++x) n += screen.row(y).at(x).isTail() ? 0 : 1;
            return n;
        };
        // Count from the earlier position to the later one, in reading order.
        const bool forward = cell.y() > cur.y || (cell.y() == cur.y && cell.x() > cur.x);
        QPoint from(cur.x, cur.y), to = cell;
        if (!forward) std::swap(from, to);
        if (!screen.row(to.y()).wrapped) to.setX(std::min(to.x(), std::max(contentEnd(to.y()), to.y() == cur.y ? cur.x : 0)));
        long delta = 0;
        for (int y = from.y(); y <= to.y(); ++y) {
            const int start = y == from.y() ? from.x() : 0;
            const int end = y == to.y() ? to.x() : contentEnd(y);
            delta += characters(y, start, end);
            if (y < to.y() && !screen.row(y).wrapped) ++delta; // the line break
        }
        if (!forward) delta = -delta;
        if (delta == 0 || std::labs(delta) > long(t.cols()) * t.rows()) {
            return {};
        }
        AfterglowVt::KeyEvent ev;
        ev.key = delta < 0 ? AfterglowVt::Key::Left : AfterglowVt::Key::Right;
        const std::string one = AfterglowVt::encodeKey(ev, t.modes());
        std::string out;
        for (long i = 0; i < std::labs(delta); ++i) {
            out += one;
        }
        return out;
    });
    if (!keys.empty()) {
        pane->sendInput(keys);
    }
}

void TerminalWindow::mouseMoveEvent(QMouseEvent* e)
{
    if (!m_splits) return;
    // A finger that selects or drags holds its button, which a mouse that
    // moves meanwhile does not: that move is no release that went elsewhere,
    // and the finger's gesture goes on.
    if (!fromTouch(e) && (m_touch.state == TouchGesture::State::Selecting || m_touch.state == TouchGesture::State::Pointer))
        return;
    // Before the overview's return below: typing hid the pointer, and the
    // overview opened from the keyboard must not keep it hidden. A finger
    // does not show it (mousePressEvent).
    if (!fromTouch(e) && std::exchange(m_pointerHidden, false)) applyPointerShape();
    m_renderer->setMousePosition(e->position() * devicePixelRatio());
    m_pointer = e->position();
    // A pack that reads the pointer moves with it, with nothing else changing.
    if (m_readsMouse) scheduleRender();
    // Over the overview too, where a dock press can start one. A release
    // that went elsewhere cuts it off.
    if (m_dockDrag) {
        if (e->buttons() & Qt::LeftButton) moveDockDrag(e->position());
        else endDockDrag(false);
    }
    // The overview draws cards, not the layout: no hover, selection or
    // reports. Only a dock over it shows what a click does.
    if (m_overview.open) {
        if (m_controller->wantsKeyboard()) routePointerToChrome(e, e->position(), false, false);
        applyPointerShape();
        return;
    }
    m_dockButtons &= e->buttons(); // a release that went elsewhere
    m_reportedButtons &= e->buttons();
    if (m_reportedButtons == Qt::NoButton) m_reportedPane = nullptr;
    if (m_selectionPane && !(e->buttons() & Qt::LeftButton)) {
        m_selectionPane = nullptr;
        m_autoScroll.stop();
    }
    // A drag that only a dock's buttons make is the dock's.
    if (m_dockButtons != Qt::NoButton && !(e->buttons() & ~m_dockButtons)) {
        applyPointerShape();
        return;
    }
    if (m_dividerDrag && !(e->buttons() & Qt::LeftButton)) {
        m_dividerDrag.reset(); // its release went elsewhere
    }
    if (m_dividerDrag) {
        m_splits->dragDivider(*m_dividerDrag, dividerPixel(e->position()), dividerPixels(), minimumPaneSize());
        m_layoutSize = QSize();
        scheduleRender();
        return;
    }
    if (m_textDragPane && !(e->buttons() & Qt::LeftButton)) {
        m_textDragPane = nullptr; // its release went elsewhere
    }
    if (Pane* pane = m_textDragPane) {
        // A drag that the pane's program offers (OSC 72) comes first, as for
        // any press.
        if (pane->hasDragDrop()
            && pane->dragDrop()->move(this, e->position().toPoint(), cellAt(pane, e->position()),
                                      cellPixel(pane, e->position()))) {
            m_textDragPane = nullptr;
            return;
        }
        if ((e->position() - m_textDragFrom).manhattanLength() < QGuiApplication::styleHints()->startDragDistance()) {
            return;
        }
        m_textDragPane = nullptr;
        if (dragSelectionText(pane)) return;
        // No text to drag (blanks only, or output cleared the selection
        // meanwhile): the press selects, as any press does.
        beginSelection(pane, m_textDragFrom, AfterglowVt::SelectionMode::Normal);
    }
    updateLinkHover();
    if (!m_selectionPane && routePointerToChrome(e, e->position(), false, false)) {
        updateDockHover();
        setCursor(m_chrome->cursorAt(m_pointer));
        return;
    }
    if (chromeVisible()) {
        // Keep hover state in the chrome current as the pointer leaves it.
        QMouseEvent outside(e->type(), QPointF(-1, -1), QPointF(-1, -1), e->button(), e->buttons(), e->modifiers());
        m_chrome->deliver(&outside);
    }
    // Over a dock, after the chrome and the link under the pointer have
    // seen it leave: no divider, shape or hover report of a pane.
    if (e->buttons() == Qt::NoButton && dockAt(e->position())) {
        applyPointerShape();
        return;
    }
    // After the chrome has seen the pointer leave, which may reset the cursor.
    applyPointerShape();
    if (Pane* target = inputPane(m_focused); target && (e->buttons() & Qt::LeftButton)) {
        const QPoint pixel = cellPixel(target, e->position());
        if (target->hasDragDrop() && target->dragDrop()->move(this, e->position().toPoint(), cellAt(target, e->position()), pixel)) {
            m_autoScroll.stop();
            if (m_selectionPane) m_selectionPane->clearSelection();
            m_selectionPane = nullptr;
            return;
        }
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
    if (code >= 0) {
        // A drag reports to the pane that got the press, clamped to its grid.
        // Not under an overlay that opened since: the cells there are the
        // overlay's. Its release still goes to the pane (mouseReleaseEvent).
        if (takesInput(m_reportedPane) && inputPane(m_reportedPane) == m_reportedPane) {
            sendMouse(m_reportedPane, e->position(), code, int(AfterglowVt::MouseAction::Motion), e->modifiers());
        }
        return;
    }
    // Hover reports only over the focused pane's own cells, not clamped
    // from another or from around them, and over a popup's own cells.
    if (paneUnder(e->position()) == m_focused) {
        if (Pane* target = pointerTarget(m_focused, e->position()); target && overCells(target, e->position()))
            sendMouse(target, e->position(), code, int(AfterglowVt::MouseAction::Motion), e->modifiers());
    }
}

void TerminalWindow::wheelEvent(QWheelEvent* e)
{
    if (!m_splits) return;
    // A finger scroll (touchpad, or touchEvent's) comes in phases: a begin
    // when the fingers touch, updates while they move, an end when they
    // lift. Wayland sends no coast after it, so Afterglow makes one, as kitty
    // does (Mouse.momentumScroll). Fingers that touch end a coast, wherever
    // their scroll goes. A wheel, with no phase, does not: as in kitty, its
    // notches scroll the view on. The times are when the events come, as
    // kitty takes them for a touchpad: an event's timestamp is on the
    // compositor's clock, and the frames that move the coast are not.
    const double now = steadyMs();
    if (e->phase() == Qt::ScrollBegin) m_momentum.begin(now);
    // A dock is its script's: the wheel over it scrolls no pane and moves
    // no card. The chrome draws over docks, so it comes first.
    DockArea* overDock = dockAt(e->position());
    if (m_overview.open && m_controller->wantsKeyboard() && routePointerToChrome(e, e->position(), false, false)) return;
    if (m_overview.open && overDock) {
        wheelDock(*overDock, dockPixelOf(*overDock, e->position()), e);
        return;
    }
    if (m_overview.open) {
        // A wheel moves a card for each notch, and fingers one for each
        // OverviewFingerStep of travel.
        const bool finger = e->source() != Qt::MouseEventNotSynthesized && !e->pixelDelta().isNull();
        const QPoint delta = finger ? e->pixelDelta() : e->angleDelta();
        m_overviewWheel += (delta.y() ? delta.y() : delta.x()) * (finger ? 120 / OverviewFingerStep : 1.0);
        const auto panes = overviewPanes();
        const auto at = std::find(panes.begin(), panes.end(), m_overview.highlighted.data());
        const int current = at == panes.end() ? 0 : int(at - panes.begin());
        const int steps = int(m_overviewWheel / 120);
        m_overviewWheel -= steps * 120;
        if (steps) moveOverview(current - steps, steps > 0 ? -1 : 1);
        return;
    }
    if (routePointerToChrome(e, e->position(), false, false)) {
        return;
    }
    if (overDock) {
        wheelDock(*overDock, dockPixelOf(*overDock, e->position()), e);
        return;
    }
    // Where no pane shows, the wheel scrolls the focused pane's history.
    Pane* shown = paneShownAt(e->position());
    Pane* p = shown ? shown : m_focused.data();
    // An overlay's program, or its history, while one runs over the pane:
    // nothing around a popup.
    p = pointerTarget(p, e->position());
    if (!p) {
        return;
    }
    // A wheel reports notches: steps of Terminal.scrollLines. Qt gives every
    // Wayland wheel event pixels too, from the axis value: they are not the
    // wheel's distance.
    if (e->source() == Qt::MouseEventNotSynthesized) {
        if (e->phase() == Qt::NoScrollPhase)
            scrollNotches(p, e->angleDelta().y() / 120.0, e->position(), e->modifiers(), m_settings.settings().scrollLines);
        return;
    }
    // Fingers report pixels: rows of distance, as kitty reads a
    // high-resolution scroll.
    if (e->phase() == Qt::ScrollEnd) {
        // Qt sends a begin and an end with no distance twice, once for each
        // axis (QWindowSystemInterface::handleWheelEvent): the second end
        // finds the gesture over, and leaves the coast that the first began.
        if (!m_momentum.gesture()) return;
        // A gesture over another pane than its moves were: no coast.
        if (p != m_momentumPane) {
            m_momentum.cancel();
            return;
        }
        if (!m_momentum.release(now)) return;
        m_momentumAt = e->position();
        m_momentumModifiers = e->modifiers();
        m_momentumAltScreen = p->withTerminal([](Terminal& t) { return t.modes().altScreen; });
        scheduleRender();
        return;
    }
    // Less than a pixel: Qt carries it into the next event's pixels.
    if (e->pixelDelta().isNull()) return;
    if (e->phase() == Qt::ScrollUpdate) {
        // Moves over another pane start a gesture of their own, as kitty's
        // over another window do: the coast's speed does not carry over.
        if (p != m_momentumPane) {
            m_momentum.cancel();
            m_momentumPane = p;
        }
        m_momentum.move(e->pixelDelta().y(), now);
    }
    scrollPixels(p, e->pixelDelta().y(), e->position(), e->modifiers());
}

void TerminalWindow::advanceMomentum(bool& animating)
{
    if (!m_momentum.coasting()) return;
    Pane* const pane = m_momentumPane;
    // The coast scrolls what a wheel at its place would: not once a tab, a
    // zoom, the overview, a layout change, an overlay or a popup put
    // something else there, or the pane left. As in kitty, it is the
    // focused window's, and the screen's that it began on: a program that
    // goes to the alternate screen gets no arrow keys from a scroll of the
    // history.
    Pane* const shown = paneShownAt(m_momentumAt);
    if (!pane || !m_windowFocused || m_overview.open || pointerTarget(shown ? shown : m_focused.data(), m_momentumAt) != pane
        || pane->withTerminal([](Terminal& t) { return t.modes().altScreen; }) != m_momentumAltScreen) {
        stopMomentum();
        return;
    }
    bool moved = true;
    if (const double pixels = m_momentum.advance(steadyMs()); pixels != 0)
        moved = scrollPixels(pane, pixels, m_momentumAt, m_momentumModifiers);
    // At the top or the bottom of the history the coast has nothing left to
    // move: no more frames for it.
    if (moved && m_momentum.coasting()) animating = true;
    else stopMomentum();
}

void TerminalWindow::stopMomentum()
{
    m_momentum.cancel();
    m_momentumPane = nullptr;
}

void TerminalWindow::stopMomentumOf(const Pane* pane)
{
    // Not a gesture under way: its own moves scroll the pane too.
    if (pane && pane == m_momentumPane && m_momentum.coasting()) stopMomentum();
}

void TerminalWindow::touchScroll(const QPointF& at, double dy, Qt::ScrollPhase phase, Qt::KeyboardModifiers modifiers)
{
    // As a touchpad's finger scroll, in the form QtWayland gives one: through
    // wheelEvent, which routes it (chrome, docks, the overview, the pane
    // under it) and coasts after the end. A wheel event carries whole
    // pixels, and an angle 12 times as large: the rest waits.
    m_touch.pixelRest += dy;
    const int pixels = int(m_touch.pixelRest);
    m_touch.pixelRest -= pixels;
    if (phase == Qt::ScrollUpdate && pixels == 0) return;
    QWheelEvent wheel(at, mapToGlobal(at), QPoint(0, pixels), QPoint(0, pixels * 12), Qt::NoButton, modifiers, phase, false,
                      Qt::MouseEventSynthesizedByApplication);
    QCoreApplication::sendEvent(this, &wheel);
}

void TerminalWindow::touchMouse(const QTouchEvent* e, QEvent::Type type, const QPointF& at, Qt::MouseButton button,
                                Qt::MouseButtons held)
{
    // As Qt makes mouse events of a touch that a window does not take
    // (QGuiApplicationPrivate::processTouchEvent): through its input
    // processing, which keeps QGuiApplication::mouseButtons() for a
    // program's drag (DragDrop), moves the pointer to the finger before a
    // press, and counts double taps. Its source keeps a touch from dragging
    // the selection (mousePressEvent).
    QWindowSystemInterface::handleMouseEvent<QWindowSystemInterface::SynchronousDelivery>(
        this, e->timestamp(), e->pointingDevice(), at, mapToGlobal(at), held, button, type, e->modifiers(),
        Qt::MouseEventSynthesizedByQt);
}

void TerminalWindow::cancelTouch(const QTouchEvent* e)
{
    using State = TouchGesture::State;
    const TouchGesture gesture = std::exchange(m_touch, TouchGesture{});
    switch (gesture.state) {
    case State::Selecting:
    case State::Pointer:
        // The button goes up, so that nothing is left held down.
        touchMouse(e, QEvent::MouseButtonRelease, gesture.last, Qt::LeftButton, Qt::NoButton);
        break;
    case State::Scrolling:
        // As in kitty, a scroll that the compositor took does not coast. Its
        // end goes where the scroll did, so that a list in the chrome ends
        // its scroll too.
        touchScroll(gesture.start, 0, Qt::ScrollEnd, e->modifiers());
        stopMomentum();
        break;
    default: break;
    }
    if (gesture.state == State::Pointer) leaveChrome();
}

void TerminalWindow::touchEvent(QTouchEvent* e)
{
    // Taken here, so that Qt makes no mouse events of its own from it.
    e->accept();
    using State = TouchGesture::State;
    // kitty's rule (kitty/glfw.c TOUCH_TAP_SLOP): a finger within 8 logical
    // pixels of where it landed, on each axis, is still a tap. Not a
    // setting: it is the size of a finger's wobble, not a preference.
    constexpr double TapSlop = 8.0;
    const double now = steadyMs();
    const Qt::KeyboardModifiers mods = e->modifiers();
    if (e->type() == QEvent::TouchCancel) {
        cancelTouch(e);
        return;
    }
    if (e->type() == QEvent::TouchBegin) {
        // As in kitty: a finger that lands stops a coast, and ends a gesture
        // whose end never came.
        stopMomentum();
        cancelTouch(e);
        const auto& points = e->points();
        if (points.size() != 1) {
            m_touch.state = State::Ignored; // several fingers at once: not a gesture of these
            return;
        }
        m_touch.id = points.front().id();
        // For a window, the scene is the window: scenePosition() is set on
        // every touch point, also on one an application makes.
        const QPointF at = points.front().scenePosition();
        m_touch.start = m_touch.last = at;
        m_touch.downAt = now;
        // Over the chrome, a dock or a divider, the finger is a mouse with its
        // button held, as when Qt made mouse events of it: it drags a tab,
        // the scrollbar or a divider, and taps a button. The overview's cards
        // take a tap and a scroll.
        const bool pointer = !m_overview.open
            && ((chromeVisible() && m_chrome->wantsPointer(at, nullptr)) || dockAt(at)
                || m_splits->dividerAt(dividerPixel(at), dividerTolerance()));
        m_touch.state = pointer ? State::Pointer : State::Pending;
        if (pointer) touchMouse(e, QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton);
        return;
    }
    for (const QEventPoint& point : e->points()) {
        if (m_touch.state == State::Idle || m_touch.state == State::Ignored) break;
        if (point.id() != m_touch.id) {
            // A second finger before the gesture was known ends it. After,
            // it is ignored, as in kitty.
            if (point.state() == QEventPoint::State::Pressed && m_touch.state == State::Pending) m_touch.state = State::Ignored;
            continue;
        }
        const QPointF at = point.scenePosition();
        if (point.state() == QEventPoint::State::Released) {
            switch (m_touch.state) {
            case State::Pending: // a tap: a click where the finger landed
                touchMouse(e, QEvent::MouseButtonPress, m_touch.start, Qt::LeftButton, Qt::LeftButton);
                touchMouse(e, QEvent::MouseButtonRelease, m_touch.start, Qt::LeftButton, Qt::NoButton);
                break;
            case State::Selecting:
            case State::Pointer: touchMouse(e, QEvent::MouseButtonRelease, at, Qt::LeftButton, Qt::NoButton); break;
            // The end goes where the scroll did, to the pane under where the
            // finger landed: that pane coasts.
            case State::Scrolling: touchScroll(m_touch.start, 0, Qt::ScrollEnd, mods); break;
            default: break;
            }
            if (std::exchange(m_touch, TouchGesture{}).state == State::Pointer) leaveChrome();
            break;
        }
        // As in kitty, only a finger that moved does something.
        if (point.state() != QEventPoint::State::Updated) continue;
        switch (m_touch.state) {
        case State::Pending:
            if (std::abs(at.x() - m_touch.start.x()) <= TapSlop && std::abs(at.y() - m_touch.start.y()) <= TapSlop) break;
            if (now - m_touch.downAt >= m_settings.settings().mouse.touchSelectHold) {
                m_touch.state = State::Selecting;
                touchMouse(e, QEvent::MouseButtonPress, m_touch.start, Qt::LeftButton, Qt::LeftButton);
                touchMouse(e, QEvent::MouseMove, at, Qt::NoButton, Qt::LeftButton);
            } else {
                m_touch.state = State::Scrolling;
                // As kitty: the pointer goes to the finger, for packs that
                // read it and for the pane that the scroll goes to.
                touchMouse(e, QEvent::MouseMove, m_touch.start, Qt::NoButton, Qt::NoButton);
                touchScroll(m_touch.start, 0, Qt::ScrollBegin, mods);
                touchScroll(m_touch.start, at.y() - m_touch.start.y(), Qt::ScrollUpdate, mods);
            }
            break;
        case State::Selecting:
        case State::Pointer: touchMouse(e, QEvent::MouseMove, at, Qt::NoButton, Qt::LeftButton); break;
        case State::Scrolling: touchScroll(m_touch.start, at.y() - m_touch.last.y(), Qt::ScrollUpdate, mods); break;
        default: break;
        }
        m_touch.last = at;
    }
    // The sequence is over once every finger has lifted, as in kitty.
    if (e->type() == QEvent::TouchEnd && m_touch.state != State::Idle) cancelTouch(e);
}

void TerminalWindow::leaveChrome()
{
    // The chrome hears of the pointer only through deliver(): as a move out
    // of the window does, this one ends its hover (a tab, the scrollbar
    // that hides when the pointer leaves it). Not during a press that the
    // chrome took: its drag goes on.
    if (!chromeVisible() || m_chromeGrab) return;
    QMouseEvent outside(QEvent::MouseMove, QPointF(-1, -1), QPointF(-1, -1), Qt::NoButton, Qt::NoButton,
                        QGuiApplication::keyboardModifiers());
    m_chrome->deliver(&outside);
}

bool TerminalWindow::scrollPixels(Pane* p, double pixels, const QPointF& at, Qt::KeyboardModifiers modifiers)
{
    const AfterglowVt::Modes modes = p->withTerminal([](Terminal& t) { return t.modes(); });
    // Shift keeps the scroll from a program that reads the mouse, as it
    // keeps a wheel's (scrollNotches).
    const bool reports = modes.mouseTracking != AfterglowVt::MouseTracking::None && !(modifiers & Qt::ShiftModifier);
    // As in kitty, such a program gets only the multiplier's direction.
    const double multiplier = m_settings.settings().mouse.touchScrollMultiplier;
    const double scale = reports ? double((multiplier > 0) - (multiplier < 0)) : multiplier;
    const double rows = pixels * scale * devicePixelRatio() / m_renderer->fonts().fonts->metrics().cellHeight;
    if (!reports && !modes.altScreen) return scrollPane(p, rows, false);
    // A program gets one wheel report, or one arrow key on the alternate
    // screen, for each row of distance.
    scrollNotches(p, rows, at, modifiers, 1);
    return true;
}

void TerminalWindow::scrollNotches(Pane* p, double notches, const QPointF& at, Qt::KeyboardModifiers modifiers, int lines)
{
    // Part of a notch over one pane does not carry over to another.
    if (p != m_wheelPane) {
        m_wheelPane = p;
        m_wheelAccumulator = 0;
    }
    m_wheelAccumulator += notches;
    const int steps = int(m_wheelAccumulator);
    if (steps == 0) {
        return;
    }
    m_wheelAccumulator -= steps;

    const AfterglowVt::Modes modes = p->withTerminal([](Terminal& t) { return t.modes(); });
    if (modes.mouseTracking != AfterglowVt::MouseTracking::None && !(modifiers & Qt::ShiftModifier)) {
        // A program hears only of a wheel over its pane, as of a press: not
        // at a clamped cell nobody pointed at.
        if (!paneShownAt(at)) return;
        const int button = int(steps > 0 ? AfterglowVt::MouseButton::WheelUp : AfterglowVt::MouseButton::WheelDown);
        for (int i = 0; i < std::abs(steps); ++i) {
            sendMouse(p, at, button, int(AfterglowVt::MouseAction::Press), modifiers);
        }
        return;
    }
    if (modes.altScreen && modes.altScroll) {
        AfterglowVt::KeyEvent key;
        key.key = steps > 0 ? AfterglowVt::Key::Up : AfterglowVt::Key::Down;
        std::string bytes;
        for (int i = 0; i < std::abs(steps) * lines; ++i) {
            bytes += AfterglowVt::encodeKey(key, modes);
        }
        p->sendInput(bytes);
        return;
    }
    scrollPane(p, double(steps) * lines);
}

} // namespace Afterglow

namespace Afterglow {
QAccessibleInterface* TerminalWindow::accessibleChrome() const
{
    return m_chrome && m_chrome->rootItem() ? QAccessible::queryAccessibleInterface(m_chrome->rootItem()) : nullptr;
}
QAccessibleInterface* TerminalWindow::accessibleChromeFocus() const
{
    auto* item = m_chrome && m_controller->wantsKeyboard() ? m_chrome->focusedItem() : nullptr;
    return item ? QAccessible::queryAccessibleInterface(item) : nullptr;
}
bool TerminalWindow::chromeHasKeyboard() const
{
    return m_controller->wantsKeyboard();
}
QRect TerminalWindow::accessiblePaneRect(Pane* pane) const
{
    if (!paneLaidOut(pane)) return {};
    return globalRect(contentRect(pane, m_splits->rectOf(hostOf(pane))));
}
std::optional<AfterglowVt::SelectionPoint> TerminalWindow::accessibleCellAt(Pane* pane, const QPoint& global) const
{
    if (!accessiblePaneRect(pane).contains(global)) return std::nullopt;
    // In the layout, as accessibleCellRect() gives the cells: the two are
    // each other's inverse, whatever a pack draws.
    const QPoint cell = cellOf(pane, gridPixelAt(pane, QPointF(mapFromGlobal(global)) * devicePixelRatio()));
    return pane->selectionPointAt(cell.x(), cell.y());
}
QRect TerminalWindow::accessibleCellRect(Pane* pane, AfterglowVt::SelectionPoint point) const
{
    const QRect area = accessiblePaneRect(pane);
    if (area.isEmpty()) return {};
    return pane->withTerminal([&](Terminal& t) {
        const auto& s = t.screen();
        const auto row = s.absoluteIndexOf(point.row) - static_cast<long long>(s.viewportTop());
        if (row < 0 || row >= s.rows()) return QRect();
        const auto& metrics = m_renderer->fonts().fonts->metrics();
        const auto& line = s.absoluteRow(s.viewportTop() + std::size_t(row));
        const double scale = line.lineSize == AfterglowVt::LineSize::Single ? 1 : 2;
        const double cw = metrics.cellWidth * scale / devicePixelRatio();
        const double ch = metrics.cellHeight / devicePixelRatio();
        const int width = std::max(1, line.at(point.col).width());
        // The drawn order of a bidi line is the snapshot's: only while it
        // shows the terminal as it is now, or its rows may be others.
        const auto& snap = pane->snapshot();
        const int column = snap.generation == t.textGeneration() ? snap.visualColumn(point.col, int(row)) : point.col;
        const double shift = drawnScrollShift(pane) / devicePixelRatio();
        return QRectF(area.x() + column * cw, area.y() + row * ch - shift, width * cw, ch).toAlignedRect();
    });
}
}

namespace Afterglow {
QAccessibleInterface* TerminalWindow::accessibleRoot() const
{
    return QAccessible::queryAccessibleInterface(const_cast<TerminalWindow*>(this));
}
}
