// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TerminalWindow.h"
#include <QQuickWindow>
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
#include "audio/AudioController.h"
#include "shell/ShellIntegration.h"

#include "app/NotificationCenter.h"

#include "KeyMapping.h"
#include "chrome/ChromeController.h"

#include <PhosphorAnimation/CurveRegistry.h>
#include <PhosphorAnimation/Spring.h>
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
#include <QDateTime>
#include <QMimeData>
#include <QProcess>
#include <QTemporaryFile>
#include <QFutureWatcher>
#include <QtConcurrentRun>
#include <QStandardPaths>
#include <QUuid>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QInputMethod>
#include <QInputMethodEvent>
#include <QInputMethodQueryEvent>
#include <QPlatformSurfaceEvent>
#include <QRegularExpression>
#include <QScreen>
#include <QSurfaceFormat>
#include <QVariantMap>

#include <utility>
#include <QStyleHints>

#include <cmath>

Q_LOGGING_CATEGORY(lcWindow, "afterglow.window", QtInfoMsg)

namespace {
/// Command palette ids from here on are script actions.
constexpr int ScriptActionBase = 10000;
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

namespace {
quint32 s_nextWindowId = 1;
}

TerminalWindow::TerminalWindow(Services& services, const WindowOptions& options)
    : m_device(services.device)
    , m_settings(services.settings)
    , m_engine(services.engine)
    , m_shaders(services.shaders)
    , m_shaderControls(services.shaders)
    , m_scripts(services.scripts)
    , m_audio(services.audio)
    , m_id(s_nextWindowId++)
{
    installTerminalAccessibility();
    connect(this, &TerminalWindow::focusedPaneChanged, this, [](Pane* pane) {
        if (pane && QAccessible::isActive()) { QAccessibleEvent event(pane, QAccessible::Focus); QAccessible::updateAccessibility(&event); }
    });
    connect(this, &TerminalWindow::tabsChanged, this, [this] {
        if (QAccessible::isActive()) { QAccessibleEvent event(this, QAccessible::ObjectReorder); QAccessible::updateAccessibility(&event); }
    });
    connect(&m_device, &AfterglowRender::GpuDevice::audioChanged, this, [this] {
        if (m_audioReactive && isExposed()) scheduleRender();
    });
    connect(this, &QWindow::visibilityChanged, this, [this] {
        m_audio.setActive(this, m_audioReactive && isExposed() && visibility() != QWindow::Minimized);
    });
    m_quick = options.quick;
    AfterglowRender::GpuDevice& device = m_device;
    QQmlEngine& engine = m_engine;
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

    const auto& m = m_renderer->fonts().fonts->metrics();
    const double dpr = devicePixelRatio();
    const WindowSettings& ws = m_settings.settings().window;
    const int pad = int(std::lround(ws.padding * dpr));
    const QSize initial(ws.initialColumns * m.cellWidth + 2 * pad, ws.initialRows * m.cellHeight + 2 * pad);
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
    m_chrome = new ChromeLayer(device, this, engine, m_controller);
    connect(m_chrome, &ChromeLayer::updateRequested, this, &TerminalWindow::scheduleRender);
    connect(m_controller, &ChromeController::activateTabRequested, this, &TerminalWindow::switchTab);
    connect(m_controller, &ChromeController::closeTabRequested, this, [this](int index) { requestCloseTabs({index}); });
    connect(m_controller, &ChromeController::confirmPromptChanged, this, &TerminalWindow::scheduleRender);
    connect(m_controller, &ChromeController::confirmAnswered, this, [this](bool accepted) {
        const std::function<void()> close = std::exchange(m_pendingConfirm, {});
        if (accepted && close) {
            close();
        }
    });
    connect(m_controller, &ChromeController::detachTabRequested, this, &TerminalWindow::detachTab);
    connect(m_controller, &ChromeController::moveTabRequested, this, &TerminalWindow::moveTab);
    connect(m_controller, &ChromeController::renameTabRequested, this, &TerminalWindow::renameTab);
    connect(m_controller, &ChromeController::tabMenuRequested, this, &TerminalWindow::openTabMenu);
    connect(m_controller, &ChromeController::newTabRequested, this, [this] { newTab(startDirectory(focusedDirectory())); });
    connect(m_controller, &ChromeController::actionRequested, this, [this](int id) {
        if (id >= ScriptActionBase) {
            if (id - ScriptActionBase < m_paletteScriptActions.size()) {
                m_scripts.runAction(m_paletteScriptActions[id - ScriptActionBase], m_focused ? m_focused->id() : 0);
            }
            return;
        }
        performAction(Action(id));
    });
    connect(&m_scripts, &ScriptHost::actionsChanged, this, &TerminalWindow::updateChrome);
    connect(m_controller, &ChromeController::historyRequested, this, [this](double position) {
        if (m_focused) m_focused->scrollHistory(position);
        scheduleRender();
    });
    connect(m_controller, &ChromeController::searchRequested, this, &TerminalWindow::runSearch);
    connect(m_controller, &ChromeController::searchOpenChanged, this, [this] {
        if (!m_controller->searchOpen() && m_focused) {
            m_focused->clearSearch();
            m_controller->setSearchStatus({});
        }
        scheduleRender();
    });
    connect(m_controller, &ChromeController::paletteOpenChanged, this, &TerminalWindow::scheduleRender);
    connect(m_controller, &ChromeController::contextMenuChanged, this, &TerminalWindow::scheduleRender);
    connect(m_controller, &ChromeController::titlePromptChanged, this, &TerminalWindow::scheduleRender);
    connect(m_controller, &ChromeController::titleAnswered, this, [this](const QString& title) {
        if (m_titlePane) {
            m_titlePane->setUserTitle(title);
        } else {
            // The tab may have closed while the prompt was open.
            for (auto& tab : m_tabs) {
                if (tab.get() == m_titleTab) {
                    tab->title = title;
                    updateChrome();
                    Q_EMIT tabsChanged();
                }
            }
        }
        m_titlePane = nullptr;
        m_titleTab = nullptr;
    });
    connect(m_controller, &ChromeController::menuItemActivated, this, [this](int id) {
        if (id == MenuOpenLink) {
            openLink(m_menuLink);
        } else if (id == MenuCopyLink) {
            QGuiApplication::clipboard()->setText(m_menuLink);
        } else if (id >= TabMenuRename) {
            runTabMenuItem(id);
        } else if (id >= MenuScriptBase) {
            if (id - MenuScriptBase < m_menuScriptActions.size()) {
                m_scripts.runAction(m_menuScriptActions[id - MenuScriptBase], m_focused ? m_focused->id() : 0);
            }
        } else {
            performAction(Action(id));
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
        if (answer == 0) {
            pending->pane->sendInput(r.osc52 ? r.osc52Reply({}) : AfterglowVt::ClipboardAssembler::readStatus("EPERM", r));
            return;
        }
        if (answer == 2 && !r.password.empty() && !r.name.empty()) {
            m_clipboardGrants[pending->pane][QByteArray::fromStdString(r.password)] = ClipboardGrant{false};
        }
        answerClipboardRead(pending->pane, r);
    });
    connect(m_controller, &ChromeController::pasteAnswered, this, [this](bool paste) {
        const QByteArray text = std::exchange(m_pendingPaste, {});
        const QPointer<Pane> target = std::exchange(m_pendingPastePane, {});
        if (paste && target && tabOf(target) >= 0) {
            const std::string bytes = target->withTerminal([&text](Terminal& t) {
                return AfterglowVt::encodePaste(std::string_view(text.constData(), std::size_t(text.size())), t.modes());
            });
            target->sendInput(bytes);
        }
    });
    connect(this, &TerminalWindow::tabsChanged, this, &TerminalWindow::updateChrome);

    connect(&m_shaders, &ShaderLibrary::changed, this, &TerminalWindow::applyShaders);
    applyShaders();

    if (m_quick) {
        setUpQuickTerminal();
    }
    applyWindowEffects();

    m_fixedTitle = options.title;
    m_holdNextPane = options.hold;
    if (!options.empty) newTab(options.directoryGiven ? options.workingDirectory : startDirectory(options.workingDirectory), options.command, options.environment);
    updateTitle();

    connect(&m_device, &AfterglowRender::GpuDevice::fontsChanged, this, &TerminalWindow::scheduleRender);
    connect(&m_settings, &SettingsStore::changed, this, &TerminalWindow::applySettings);

    m_textBlinkTimer.setInterval(500);
    connect(&m_textBlinkTimer, &QTimer::timeout, this, [this] {
        m_textBlinkOn = !m_textBlinkOn;
        scheduleRender();
    });
    connect(this, &QWindow::windowStateChanged, this, [this] { updatePaneVisibility(); });
    m_blinkTimer.setInterval(m_settings.settings().cursorBlinkInterval);
    connect(&m_blinkTimer, &QTimer::timeout, this, [this] {
        m_blinkOn = !m_blinkOn;
        if (m_focused && scriptOverrides(m_focused).value(QStringLiteral("cursorBlink"), m_focused->snapshot().cursorBlink).toBool() && (m_focused->snapshot().cursorVisible || !m_focused->snapshot().extraCursors.empty())) {
            scheduleRender();
        }
    });

    m_autoScroll.setInterval(50);
    connect(&m_autoScroll, &QTimer::timeout, this, &TerminalWindow::updateSelectionDrag);

    s_windows.append(this);
}

TerminalWindow::~TerminalWindow()
{
    for (auto* pane : findChildren<Pane*>(QString(), Qt::FindDirectChildrenOnly))
        Q_EMIT paneClosing(pane->id(), tabIdOf(pane), id());
    if (m_hintCancel) *m_hintCancel = true;
    finishScriptDialog(false, {});
    s_windows.removeAll(this);
    publishLauncherStatus();
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

void TerminalWindow::bindPane(Pane* pane)
{
    connect(pane, &Pane::inputRejected, this, [this, pane] {
        NotificationCenter::instance().showSimple(pane, this, QStringLiteral("Terminal input rejected"),
            QStringLiteral("The program closed or its input queue is full."));
    });
    connect(pane, &QObject::destroyed, this, [this, pane] {
        m_motion.erase(pane);
        m_growFrom.erase(pane);
        m_clipboardGrants.erase(pane);
        if (m_statusViews.erase(pane) > 0) {
            publishLauncherStatus();
        }
    });
    connect(pane, &Pane::flashRequested, this, [this, guard = QPointer<Pane>(pane)] {
        if (guard) {
            motion(guard).bell.start(1.0, 0.0, springSpec<qreal>(m_clock.get(), 14.0));
            scheduleRender();
        }
    });
    connect(pane, &Pane::statusChanged, this, [this, guard = QPointer<Pane>(pane)] {
        if (guard) {
            paneStatusChanged(guard);
        }
    });
    connect(pane, &Pane::searchStatusChanged, this, [this, pane](const QString& status) {
        if (pane == m_focused && m_controller->searchOpen()) m_controller->setSearchStatus(status);
        scheduleRender();
    });
    connect(pane, &Pane::contentChanged, this, &TerminalWindow::scheduleRender);
    connect(pane, &Pane::contentChanged, this, [this, pane] {
        if (pane == m_focused && isActive()) QGuiApplication::inputMethod()->update(Qt::ImQueryInput);
    });
    connect(pane, &Pane::shaderSignalsChanged, this, &TerminalWindow::scheduleRender);
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
        const QString what = command.isEmpty() ? QStringLiteral("The command") : command;
        const QString outcome = exitCode == 0 ? QStringLiteral("finished")
            : exitCode < 0                    ? QStringLiteral("ended")
                                              : QStringLiteral("failed (exit %1)").arg(exitCode);
        NotificationCenter::instance().showSimple(guard, this, what, QStringLiteral("%1 after %2 s").arg(outcome).arg(ms / 1000));
    });
    connect(pane, &QObject::destroyed, this, [this, pane] { m_notifyOnFinish.remove(pane); });
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
            NotificationCenter::instance().showSimple(guard, this, title.isEmpty() ? guard->title() : title, body);
        }
    });
    connect(pane, &Pane::clipboardRequested, this,
            [this, guard = QPointer<Pane>(pane)](const AfterglowVt::ClipboardRequest& request) {
                if (guard) {
                    clipboardRequest(guard, request);
                }
            });
    connect(pane, &Pane::desktopNotificationRequested, this,
            [this, guard = QPointer<Pane>(pane)](const AfterglowVt::NotificationRequest& request) {
                if (!guard) {
                    return;
                }
                using Kind = AfterglowVt::NotificationRequest::Kind;
                using Occasion = AfterglowVt::NotificationRequest::Occasion;
                if (request.kind == Kind::Show) {
                    if (!m_settings.settings().notifications) {
                        return;
                    }
                    // The program's occasion: when the user is not on the pane, or cannot see it.
                    const bool focused = isActive() && guard == m_focused;
                    const bool visible = isExposed() && windowState() != Qt::WindowMinimized && tabOf(guard) == m_currentTab && m_splits->isVisible(guard);
                    if ((request.occasion == Occasion::Unfocused && focused)
                        || (request.occasion == Occasion::Invisible && visible)) {
                        return;
                    }
                }
                NotificationCenter::instance().handle(guard, this, request);
            });
    connect(pane, &Pane::terminalProtocolRequested, this, [this, guard = QPointer<Pane>(pane)](int code, const QByteArray& payload) {
        if (!guard) return;
        auto* files = guard->fileTransfers();
        files->setPassword(m_settings.settings().fileTransferPassword);
        if (code == 0) { files->reset(); guard->dragDrop()->reset(); }
        else if (code == -1) { files->abort(); guard->dragDrop()->abort(); }
        else if (code == 72) guard->dragDrop()->handle(payload);
        else if (code == 5113) files->handle(payload);
    });
    connect(pane, &Pane::fileTransferPermissionRequested, this,
            [this, guard = QPointer<FileTransfer>(pane->fileTransfers())](const QString& token, bool writing, const QStringList& paths) {
        auto* dialog = new QMessageBox(QMessageBox::Question, tr("File transfer"),
            writing ? tr("Allow this terminal program to write files on this computer? Existing files can be replaced.")
                    : tr("Allow this terminal program to read the requested files on this computer?"),
            QMessageBox::Yes | QMessageBox::No);
        connect(this, &QObject::destroyed, dialog, &QDialog::reject);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setTextFormat(Qt::PlainText);
        dialog->setDefaultButton(QMessageBox::No);
        if (!paths.isEmpty()) dialog->setDetailedText(paths.join(QLatin1Char('\n')));
        dialog->winId(); dialog->windowHandle()->setTransientParent(this);
        connect(dialog, &QDialog::finished, dialog, [guard, token](int result) { if (guard) guard->approve(token, result == QMessageBox::Yes); });
        if (guard) {
            connect(guard, &QObject::destroyed, dialog, &QDialog::reject);
            connect(guard, &FileTransfer::sessionClosed, dialog, [dialog, token](const QString& closed) { if (closed == token) dialog->reject(); });
        }
        dialog->open();
    });
    connect(pane, &Pane::pointerShapeRequested, this, [this, guard = QPointer<Pane>(pane)](const QString& shape) {
        if (guard) {
            motion(guard).pointerShape = shape;
            applyPointerShape();
        }
    });

}

Pane* TerminalWindow::createPane(const QString& workingDirectory, const QRect& area, const QStringList& command, QString* failure, const QVariantMap& environment, bool fallback)
{
    auto* pane = new Pane(m_settings.settings().terminal, this);
    bindPane(pane);
    pane->setLaunch(workingDirectory, command);
    AfterglowPty::SpawnOptions opts;
    opts.workingDirectory = workingDirectory.toStdString();
    opts.environment = m_settings.settings().environment;
    for (auto it = environment.cbegin(); it != environment.cend(); ++it) {
        const auto name = it.key().toStdString();
        std::erase_if(opts.environment, [&](const auto& entry) { return entry.first == name; });
        if (it.value().metaType().id() == QMetaType::Bool) opts.unsetEnvironment.push_back(name);
        else opts.environment.emplace_back(name, it.value().toString().toStdString());
    }
    pane->setScrollPolicy(m_settings.settings().scrollOnOutput, m_settings.settings().scrollOnInput);
    // Before the program starts: a quick one may exit at once.
    pane->setHold(std::exchange(m_holdNextPane, false));
    pane->setHoldOnFailure(fallback);
    if (!command.isEmpty()) {
        pane->setLaunchedProgram(QFileInfo(command.first()).fileName());
        for (const QString& arg : command) {
            opts.argv.push_back(arg.toStdString());
        }
    } else if (const QString shell = m_settings.settings().shell; !shell.isEmpty()) {
        opts.argv = {shell.toStdString()};
    }
    ShellIntegration::prepare(opts, m_settings.settings().shellIntegration);
    // Integrated shells wrap ssh when the helper is named; a launch's own values win.
    const auto offer = [&](const char* name, const QString& value) {
        if (std::none_of(opts.environment.begin(), opts.environment.end(), [&](const auto& e) { return e.first == name; }))
            opts.environment.emplace_back(name, value.toStdString());
    };
    if (m_settings.settings().sshIntegration) offer("AFTERGLOW_SSH_HELPER", QCoreApplication::applicationFilePath());
    offer("AFTERGLOW_SSH_TERMINFO", m_settings.settings().sshTerminfo);
    const auto& m = m_renderer->fonts().fonts->metrics();
    const QRect content = contentRect(area);
    const int cols = std::max(2, content.width() / m.cellWidth);
    const int rows = std::max(1, content.height() / m.cellHeight);
    QString error;
    if (!pane->start(opts, m_settings.settings().term, cols, rows, m.cellWidth, m.cellHeight, &error)) {
        qCCritical(lcWindow) << "failed to start the shell:" << error;
        if (failure) *failure = error;
        if (fallback) {
            // A project must remain inspectable even if its configured shell fails.
            AfterglowPty::SpawnOptions fallback;
            fallback.argv = {"/bin/sh"};
            fallback.workingDirectory = QDir::homePath().toStdString();
            fallback.environment = m_settings.settings().environment;
            pane->setHold(true);
            if (!pane->start(fallback, m_settings.settings().term, cols, rows, m.cellWidth, m.cellHeight, &error))
                pane->holdStartFailure();
        } else {
            delete pane;
            return nullptr;
        }
    }
    Q_EMIT paneCreated(pane);
    return pane;
}

Pane* TerminalWindow::splitFocused(SplitTree::Direction direction, const QStringList& command, const PaneLaunchOptions& options, QString* error)
{
    if (!m_focused) {
        return nullptr;
    }
    if (m_splits->zoomedPane()) {
        setPaneZoom(nullptr);
        relayout();
    }
    // Start the new shell at the size its requested share of the split will have.
    QRect area = m_splits->rectOf(m_focused);
    const int divider = std::max(1, int(std::lround(devicePixelRatio())));
    const bool sideways = direction == SplitTree::Direction::Right || direction == SplitTree::Direction::Left;
    if (sideways) {
        area.setWidth(int((area.width() - divider) * options.fraction));
    } else {
        area.setHeight(int((area.height() - divider) * options.fraction));
    }
    Pane* added = createPane(options.cwd.isEmpty() ? startDirectory(m_focused->workingDirectory()) : options.cwd, area, command, error, options.environment);
    if (!added) return nullptr;
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
    m_splits->split(m_focused, added, direction, options.fraction);
    setFocusedPane(added);
    m_layoutSize = QSize();
    scheduleRender();
    return added;
}

void TerminalWindow::closePane(Pane* pane)
{
    Q_EMIT paneClosing(pane->id(), tabIdOf(pane), id());
    const int index = tabOf(pane);
    if (m_scriptDialog && m_scriptDialog->pane == pane) finishScriptDialog(false, {});
    m_shaderControls.removePane(pane->id());
    std::erase_if(m_scriptAppearance, [pane](const auto& item) { return item.pane == pane; });
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
    updatePaneVisibility();
    m_layoutSize = QSize();
    scheduleRender();
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

int TerminalWindow::tabBarHeight() const
{
    const int height = int(std::lround(TabBarLogicalHeight * devicePixelRatio()));
    switch (m_settings.settings().window.tabBar) {
    case WindowSettings::TabBar::Always:
        return height;
    case WindowSettings::TabBar::Never:
        return 0;
    case WindowSettings::TabBar::Auto:
        break;
    }
    return m_tabs.size() > 1 ? height : 0;
}

QRect TerminalWindow::paneArea() const
{
    const QSize px = m_renderer->pixelSize().isEmpty()
        ? (size() * devicePixelRatio())
        : m_renderer->pixelSize();
    const int bar = tabBarHeight();
    return QRect(0, bar, px.width(), std::max(0, px.height() - bar));
}

Pane* TerminalWindow::newTab(const QString& workingDirectory, const QStringList& command, const QVariantMap& environment, QString* error)
{
    // A new tab opens beside the current one.
    const int at = m_tabs.empty() ? 0 : m_currentTab + 1;
    if (m_splits && m_currentTab < int(m_tabs.size())) {
        m_tabs[std::size_t(m_currentTab)]->focused = m_focused;
    }
    m_tabs.insert(m_tabs.begin() + at, std::make_unique<Tab>());
    Tab& tab = *m_tabs[std::size_t(at)];
    // Size the shell for the area it will get, with the tab bar if it now shows.
    Pane* first = createPane(workingDirectory, paneArea(), command, error, environment);
    if (!first) {
        m_tabs.erase(m_tabs.begin() + at);
        return nullptr;
    }
    tab.splits = std::make_unique<SplitTree>(first);
    tab.focused = first;
    m_splits = nullptr; // the current tab's index moved; nothing to save
    switchTab(at);
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

quint32 TerminalWindow::tabIdOf(const Pane* pane) const
{
    const int index = tabOf(pane);
    return index < 0 ? 0 : m_tabs[std::size_t(index)]->id;
}

quint32 TerminalWindow::activeTabId() const
{
    return m_currentTab < int(m_tabs.size()) ? m_tabs[std::size_t(m_currentTab)]->id : 0;
}

QList<TerminalWindow::TabInfo> TerminalWindow::tabs() const
{
    QList<TabInfo> out;
    for (std::size_t i = 0; i < m_tabs.size(); ++i) {
        const Tab& tab = *m_tabs[i];
        const Pane* pane = int(i) == m_currentTab ? m_focused.data() : tab.focused.data();
        out.append({tab.id, int(i), pane ? pane->id() : 0,
                    tab.title.isEmpty() && pane ? pane->title() : tab.title});
    }
    return out;
}

Pane* TerminalWindow::splitPane(Pane* pane, SplitTree::Direction direction, const PaneLaunchOptions& options, QString* error)
{
    if (tabOf(pane) < 0) return nullptr;
    revealPane(pane);
    return splitFocused(direction, options.command, options, error);
}

Pane* TerminalWindow::openTab(const PaneLaunchOptions& options, QString* error)
{
    return newTab(options.cwd.isEmpty() ? startDirectory(focusedDirectory()) : options.cwd,
                  options.command, options.environment, error);
}

QString TerminalWindow::focusedDirectory() const
{
    return m_focused ? m_focused->workingDirectory() : QString();
}

QString TerminalWindow::startDirectory(const QString& inherited) const
{
    const QString setting = m_settings.settings().window.workingDirectory;
    if (setting.isEmpty() || setting == QLatin1String("inherit")) {
        return inherited;
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
    qCWarning(lcWindow).noquote() << "workingDirectory" << setting << "is not a directory";
    return inherited;
}

void TerminalWindow::confirmClose(const QList<Pane*>& panes, const QString& what, std::function<void()> close)
{
    QStringList jobs;
    if (m_settings.settings().confirmClose) {
        for (Pane* p : panes) {
            if (const QString job = p->foregroundJob(); !job.isEmpty() && !jobs.contains(job)) {
                jobs << job;
            }
        }
    }
    if (jobs.isEmpty()) {
        close();
        return;
    }
    QString names = jobs.constLast();
    if (jobs.size() > 1) {
        names = jobs.mid(0, jobs.size() - 1).join(QStringLiteral(", ")) + QStringLiteral(" and ") + names;
    }
    m_pendingConfirm = std::move(close);
    m_controller->askConfirm(QStringLiteral("Close %1?").arg(what),
                             QStringLiteral("%1 %2 still running, and closing ends %3.")
                                 .arg(names, jobs.size() == 1 ? QStringLiteral("is") : QStringLiteral("are"),
                                      jobs.size() == 1 ? QStringLiteral("it") : QStringLiteral("them")),
                             QStringLiteral("Close"));
}

void TerminalWindow::requestCloseTabs(const QList<int>& indices)
{
    QList<Pane*> panes;
    QList<const Tab*> tabs;
    for (int i : indices) {
        if (i >= 0 && i < int(m_tabs.size())) {
            tabs << m_tabs[std::size_t(i)].get();
            m_tabs[std::size_t(i)]->splits->forEachPane([&panes](Pane* p, const QRect&) { panes << p; });
        }
    }
    if (tabs.isEmpty()) {
        return;
    }
    const QString what = tabs.size() > 1           ? QStringLiteral("%1 tabs").arg(tabs.size())
                         : int(m_tabs.size()) == 1 ? QStringLiteral("this window")
                                                   : QStringLiteral("this tab");
    // By tab, not index: tabs may move or close while the prompt is open.
    confirmClose(panes, what, [this, tabs] {
        for (const Tab* tab : tabs) {
            for (std::size_t i = 0; i < m_tabs.size(); ++i) {
                if (m_tabs[i].get() == tab) {
                    closeTab(int(i));
                    break;
                }
            }
        }
    });
}

bool TerminalWindow::runAction(Pane* pane, Action action)
{
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
    if (m_splits && m_currentTab < int(m_tabs.size())) {
        m_tabs[std::size_t(m_currentTab)]->focused = m_focused;
    }
    m_currentTab = index;
    Tab& tab = *m_tabs[std::size_t(index)];
    m_splits = tab.splits.get();
    // Panes appear where they are: no easing from where they were when the
    // tab was last shown (the window may have changed size since).
    m_splits->forEachPane([this](Pane* p, const QRect&) {
        PaneMotion& m = motion(p);
        m.rectKnown = false;
        m.cursorKnown = false;
    });
    QTimer::singleShot(0, this, &TerminalWindow::updatePaneVisibility);
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
    if (m_scriptDialog && tabOf(m_scriptDialog->pane) == index) finishScriptDialog(false, {});
    for (Pane* p : panes()) if (tabOf(p) == index) m_shaderControls.removePane(p->id());
    std::erase_if(m_scriptAppearance, [this, index](const auto& item) { return tabOf(item.pane) == index; });
    if (m_tabs.size() == 1) {
        qCInfo(lcWindow) << "last pane closed, closing the window";
        m_tabs[0]->splits->forEachPane([this](Pane* p, const QRect&) { Q_EMIT paneClosing(p->id(), tabIdOf(p), id()); p->deleteLater(); });
        m_closeConfirmed = true; // asked already, or its program ended
        close();
        return;
    }
    const Tab* closing = m_tabs[std::size_t(index)].get();
    if (m_titleTab == closing) {
        m_controller->cancelTitle();
        m_titleTab = nullptr;
    }
    if (m_menuTab == closing) {
        m_menuTab = nullptr;
    }
    m_tabs[std::size_t(index)]->splits->forEachPane([this](Pane* p, const QRect&) { Q_EMIT paneClosing(p->id(), tabIdOf(p), id()); p->deleteLater(); });
    m_tabs.erase(m_tabs.begin() + index);
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
    switchTab(std::min(index, int(m_tabs.size()) - 1));
}

void TerminalWindow::moveTab(int from, int to)
{
    const int count = int(m_tabs.size());
    if (from < 0 || from >= count || to < 0 || to >= count || from == to) {
        return;
    }
    const Tab* current = m_tabs[std::size_t(m_currentTab)].get();
    auto moved = std::move(m_tabs[std::size_t(from)]);
    m_tabs.erase(m_tabs.begin() + from);
    m_tabs.insert(m_tabs.begin() + to, std::move(moved));
    for (int i = 0; i < count; ++i) {
        if (m_tabs[std::size_t(i)].get() == current) {
            m_currentTab = i;
        }
    }
    Q_EMIT tabsChanged();
    scheduleRender();
}

void TerminalWindow::renameTab(int index)
{
    if (index < 0 || index >= int(m_tabs.size())) {
        return;
    }
    m_titlePane = nullptr;
    m_titleTab = m_tabs[std::size_t(index)].get();
    m_controller->askTitle(QStringLiteral("Tab title"), m_titleTab->title);
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
        if (m_tabs[i].get() == m_menuTab) {
            index = int(i);
        }
    }
    m_menuTab = nullptr;
    if (index < 0) {
        return; // the tab closed while its menu was open
    }
    switch (id) {
    case TabMenuRename:
        renameTab(index);
        break;
    case TabMenuNew:
        switchTab(index);
        newTab(startDirectory(focusedDirectory()));
        break;
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
    m_menuTab = m_tabs[std::size_t(index)].get();
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
    if (m_focused) m_focused->clearSearch();
    if (m_focused && m_windowFocused) {
        Pane* old = m_focused;
        old->sendInput(old->withTerminal([](Terminal& t) { return AfterglowVt::encodeFocus(false, t.modes()); }));
    }
    if (m_focused) {
        m_focused->setPreedit({}, 0);
    }
    QGuiApplication::inputMethod()->reset();
    stopCopyMode();
    cancelKeyMode();
    m_focused = pane;
    if (m_splits && m_splits->zoomedPane() && m_splits->contains(pane)) {
        setPaneZoom(pane);
    }
    if (pane && m_windowFocused) {
        pane->sendInput(pane->withTerminal([](Terminal& t) { return AfterglowVt::encodeFocus(true, t.modes()); }));
    }
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
    QString t = m_focused ? m_focused->title() : QString();
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
    stopUrlMode();
    m_splits->forEachPane([this](Pane* p, const QRect&) { motion(p).rectKnown = false; });
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
    const int pad = int(std::lround(m_settings.settings().window.padding * devicePixelRatio()));
    const bool vertical = direction == SplitTree::Neighbour::Up || direction == SplitTree::Neighbour::Down;
    const bool changed = m_splits->resizePane(m_focused, direction, 2 * (vertical ? metrics.cellHeight : metrics.cellWidth),
        std::max(1, int(std::lround(devicePixelRatio()))), QSize(4 * metrics.cellWidth + 2 * pad, 2 * metrics.cellHeight + 2 * pad));
    m_layoutSize = QSize();
    scheduleRender();
    return changed;
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

void TerminalWindow::relayout()
{
    const QSize px = m_renderer->pixelSize();
    const double dpr = devicePixelRatio();
    m_splits->layout(paneArea(), std::max(1, int(std::lround(dpr))));
    const auto& m = m_renderer->fonts().fonts->metrics();
    m_splits->forEachPane([&](Pane* p, const QRect& r) {
        if (r.isEmpty()) return; // hidden by zoom: retain the PTY's existing size
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
    if (!m_splits || !isExposed() || !m_renderer->ensureSwapChain()) {
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
    bool animating = m_shaderControls.advance();
    const QRect area = paneArea();
    const int dividerWidth = std::max(1, int(std::lround(devicePixelRatio())));
    const auto backgroundAlpha = std::uint8_t(std::lround(m_settings.settings().window.backgroundOpacity * 255));
    bool anyBlinkingText = false;
    advanceOverview(animating);
    // The overview shader draws the cards: fade the unfocused dim out under it.
    const double overviewAmount = std::clamp(m_overview.amount.value(), 0.0, 1.0);
    m_splits->forEachPane([&](Pane* p, const QRect& r) {
        if (r.isEmpty()) return;
        AfterglowRender::WindowRenderer::Pane wp;
        preparePane(p, r, wp.params, holding, animating);
        wp.snapshot = &p->snapshot();
        wp.params.focused = p == m_focused && m_windowFocused;
        wp.params.highlighted = m_overview.open && p == m_overview.highlighted;
        const auto overrides = scriptOverrides(p);
        wp.params.cursorBlinkOn = m_blinkOn;
        wp.params.cursorBlinkOverride = overrides.contains(QStringLiteral("cursorBlink"))
            ? int(overrides.value(QStringLiteral("cursorBlink")).toBool()) : -1;
        wp.params.textBlinkOn = m_textBlinkOn;
        anyBlinkingText |= p->snapshot().hasBlinkingText;
        wp.params.backgroundAlpha = overrides.contains(QStringLiteral("backgroundOpacity"))
            ? std::uint8_t(std::lround(overrides.value(QStringLiteral("backgroundOpacity")).toDouble() * 255)) : backgroundAlpha;
        if (m_hover.pane == p) {
            wp.params.hoveredLink = m_hover.link;
            wp.params.hoverRow = m_hover.row;
            wp.params.hoverColStart = m_hover.colStart;
            wp.params.hoverColEnd = m_hover.colEnd;
        }
        panes.push_back(wp);
        const QRect shown = wp.params.rect;
        if (multiple && p != m_focused) {
            const double paneDim = overrides.contains(QStringLiteral("unfocusedDim"))
                ? overrides.value(QStringLiteral("unfocusedDim")).toDouble() : m_settings.settings().window.unfocusedDim;
            if (const auto alpha = std::uint8_t(std::lround(paneDim * (1.0 - overviewAmount) * 255)); alpha > 0)
                overlays.emplace_back(shown, packRgba(0, 0, 0, alpha));
        }
        if (PaneMotion& pm = motion(p); pm.bell.isAnimating()) {
            pm.bell.advance();
            overlays.emplace_back(shown, (p->snapshot().visualBellColor & 0x00ffffffu) | (std::uint32_t(std::clamp(pm.bell.value(), 0.0, 1.0) * 46) << 24));
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
    if (multiple && m_settings.settings().window.paneTitles && !m_renderer->overviewShowing()) {
        const qreal dpr = devicePixelRatio();
        std::size_t index = 0;
        m_splits->forEachPane([&](Pane* p, const QRect& area) {
            if (area.isEmpty()) return;
            const QRect r = panes[index++].params.rect;
            titles.append(QVariantMap{{QStringLiteral("x"), r.x() / dpr},
                                      {QStringLiteral("y"), r.y() / dpr},
                                      {QStringLiteral("width"), r.width() / dpr},
                                      {QStringLiteral("height"), r.height() / dpr},
                                      {QStringLiteral("title"), p->title()},
                                      {QStringLiteral("focused"), p == m_focused},
                                      {QStringLiteral("status"), statusSummary(p)},
                                      {QStringLiteral("context"), p->contextLabel()},
                                      {QStringLiteral("contextKind"), p->contextKind()}});
        });
    }
    m_controller->setPaneTitles(titles);
    QVariantMap history;
    const WindowSettings& windowSettings = m_settings.settings().window;
    if (m_focused && windowSettings.scrollbar != WindowSettings::Scrollbar::Never) {
        history = m_focused->historyPosition();
        const QRectF rect = QRectF(m_splits->rectOf(m_focused));
        history.insert(QStringLiteral("x"), rect.right() / devicePixelRatio() - 12);
        history.insert(QStringLiteral("y"), rect.top() / devicePixelRatio());
        history.insert(QStringLiteral("height"), rect.height() / devicePixelRatio());
        history.insert(QStringLiteral("fade"), windowSettings.scrollbar == WindowSettings::Scrollbar::Auto);
        history.insert(QStringLiteral("hideDelay"), windowSettings.scrollbarHideDelay);
    }
    m_controller->setHistory(history);
    const std::uint32_t clearRgb = m_focused ? m_focused->snapshot().defaultBg : packRgba(0, 0, 0);
    const std::uint32_t clear = (clearRgb & 0x00FFFFFFu) | (std::uint32_t(backgroundAlpha) << 24);
    QRhiTexture* chrome = nullptr;
    if (chromeVisible()) {
        updateChrome();
        // Qt Quick records its own offscreen frame, so this precedes ours.
        m_chrome->renderIfNeeded(m_renderer->pixelSize());
        chrome = m_chrome->texture();
    }
    m_renderer->setTranslucent(backgroundAlpha < 255 || std::any_of(panes.begin(), panes.end(), [](const auto& pane) {
        return pane.snapshot->hasTransparentBackgrounds;
    }));
    m_renderer->render(panes, overlays, clear, chrome, m_chrome->serial());
    if (anyBlinkingText && !m_textBlinkTimer.isActive()) {
        m_textBlinkTimer.start();
    } else if (!anyBlinkingText && m_textBlinkTimer.isActive()) {
        m_textBlinkTimer.stop();
        m_textBlinkOn = true;
    }
    if (const QRectF imeRect = inputMethodCursorRect(); imeRect != m_reportedImeRect && isActive()) {
        m_reportedImeRect = imeRect;
        QGuiApplication::inputMethod()->update(Qt::ImCursorRectangle);
    }
    updateTerminalAccessibility(this);
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

std::shared_ptr<const PhosphorAnimation::Curve> TerminalWindow::splitCurve()
{
    const QString& spec = m_settings.settings().animation.splitCurve;
    if (!m_splitCurve || spec != m_splitCurveSpec) {
        m_splitCurveSpec = spec;
        m_splitCurve = m_shaders.curves().tryCreate(spec);
        if (!m_splitCurve) {
            qCWarning(lcWindow).noquote() << "Animation.splitCurve" << spec << "is not a curve; using the default spring";
            m_splitCurve = std::make_shared<PhosphorAnimation::Spring>(SplitSpeed, 1.0);
        }
    }
    return m_splitCurve;
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
    auto rectSpec = springSpec<QRectF>(m_clock.get(), SplitSpeed);
    rectSpec.profile.curve = splitCurve();
    rectSpec.profile.duration = anim.splitDuration;
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
    params.signals = pane->shaderSignals();
    params.shaderParameters = m_shaderControls.values(pane->id());
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
        if (m_controller->wantsKeyboard()) { m_chrome->queryInputMethod(q); return true; }
        const Qt::InputMethodQueries queries = q->queries();
        if (queries & Qt::ImEnabled) {
            q->setValue(Qt::ImEnabled, !m_controller->wantsKeyboard());
        }
        if (queries & Qt::ImCursorRectangle) {
            q->setValue(Qt::ImCursorRectangle, inputMethodCursorRect());
        }
        if (queries & Qt::ImHints) {
            q->setValue(Qt::ImHints, int(Qt::ImhNoPredictiveText | Qt::ImhNoAutoUppercase));
        }
        const TerminalText context = m_focused ? m_focused->accessibleText(true) : TerminalText{};
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
        // Terminal output is not an editable document. Never translate arbitrary
        // IME replacement ranges into destructive shell keystrokes.
        if (ime->replacementStart() != 0 || ime->replacementLength() != 0) {
            ime->ignore();
            return true;
        }
        if (m_focused) {
            if (!ime->commitString().isEmpty() && m_focused->isHeld()) {
                closePane(m_focused);
                ime->accept();
                return true;
            }
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
    case QEvent::DragEnter:
    case QEvent::DragMove:
    case QEvent::Drop: {
        auto* drop = static_cast<QDropEvent*>(e);
        Pane* target = m_splits ? m_splits->paneAt((drop->position() * devicePixelRatio()).toPoint()) : nullptr;
        for (auto* pane : panes()) if (pane != target) pane->dragDrop()->leave();
        if (target && !target->isHeld() && !m_controller->wantsKeyboard() && target->dragDrop()->acceptsDrops()) {
            const QPoint pixel = (drop->position() * devicePixelRatio()).toPoint() - contentRect(m_splits->rectOf(target)).topLeft();
            target->dragDrop()->drop(drop, e->type() == QEvent::Drop, cellAt(target, drop->position()), pixel, this);
            return true;
        }
        const QString paths = droppedFiles(drop->mimeData());
        if (!target || target->isHeld() || m_controller->wantsKeyboard() || paths.isEmpty()
            || !(drop->possibleActions() & Qt::CopyAction)) {
            drop->ignore();
            return true;
        }
        drop->setDropAction(Qt::CopyAction);
        drop->accept();
        if (e->type() == QEvent::Drop) {
            setFocusedPane(target);
            const auto bytes = paths.toUtf8();
            target->sendInput(target->withTerminal([&](Terminal& t) {
                return AfterglowVt::encodePaste(std::string_view(bytes.constData(), std::size_t(bytes.size())), t.modes());
            }));
        }
        return true;
    }
    case QEvent::DragLeave:
        for (auto* pane : panes()) pane->dragDrop()->leave();
        e->accept();
        return true;
    case QEvent::PlatformSurface:
        if (static_cast<QPlatformSurfaceEvent*>(e)->surfaceEventType()
            == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed) {
            m_renderer->releaseSwapChain();
        }
        break;
    case QEvent::Close: {
        if (!m_closeConfirmed && m_settings.settings().confirmClose) {
            const QList<Pane*> all = panes();
            if (std::any_of(all.cbegin(), all.cend(), [](Pane* p) { return !p->foregroundJob().isEmpty(); })) {
                e->ignore();
                confirmClose(all, QStringLiteral("this window"), [this] {
                    m_closeConfirmed = true;
                    close();
                });
                return true;
            }
        }
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
    QAccessibleEvent accessibleEvent(this, isExposed() ? QAccessible::ObjectShow : QAccessible::ObjectHide);
    QAccessible::updateAccessibility(&accessibleEvent);
    m_audio.setActive(this, m_audioReactive && isExposed() && visibility() != QWindow::Minimized);
    if (isExposed() && !m_effectsApplied) {
        m_effectsApplied = true;
        applyWindowEffects();
    }
    if (isExposed()) {
        renderNow();
    }
    updatePaneVisibility();
}

void TerminalWindow::updatePaneVisibility()
{
    const bool shown = isExposed() && windowState() != Qt::WindowMinimized;
    for (std::size_t i = 0; i < m_tabs.size(); ++i) {
        const bool visible = shown && int(i) == m_currentTab;
        const SplitTree* tree = m_tabs[i]->splits.get();
        tree->forEachPane([visible, tree](Pane* p, const QRect&) {
            p->withTerminal([visible, tree, p](Terminal& t) { t.setVisible(visible && tree->isVisible(p)); });
        });
    }
}

void TerminalWindow::resizeEvent(QResizeEvent*)
{
    scheduleRender();
}

void TerminalWindow::focusInEvent(QFocusEvent*)
{
    if (m_focused) { QAccessibleEvent event(m_focused, QAccessible::Focus); QAccessible::updateAccessibility(&event); }
    m_windowFocused = true;
    if (m_focused) {
        m_focused->sendInput(m_focused->withTerminal([](Terminal& t) { return AfterglowVt::encodeFocus(true, t.modes()); }));
    }
    restartBlink();
    acknowledgeStatus();
}

void TerminalWindow::focusOutEvent(QFocusEvent*)
{
    cancelKeyMode();
    m_controller->closeContextMenu();
    m_windowFocused = false;
    m_blinkTimer.stop();
    m_blinkOn = true;
    if (m_focused) {
        m_focused->sendInput(m_focused->withTerminal([](Terminal& t) { return AfterglowVt::encodeFocus(false, t.modes()); }));
    }
    scheduleRender();
}

void TerminalWindow::cancelKeyMode()
{
    m_keyMode = {};
    updateKeyMode();
}

void TerminalWindow::updateKeyMode()
{
    m_controller->setKeyMode(m_overview.open ? tr("Panes · arrows choose · Enter or 1–9 focus · Esc cancel")
                             : m_copyPane ? tr("Copy · arrows/hjkl · v select · y copy") : m_keyMode.label());
    m_keyModeTimer.stop();
    if (m_keyMode.active()) m_keyModeTimer.start(int(std::max(qint64(1), m_keyMode.deadline - m_keyModeClock.elapsed())));
    scheduleRender();
}

bool TerminalWindow::handleShortcut(QKeyEvent* e)
{
    const auto result = m_settings.settings().keyModes.press(e, m_keyMode, m_keyModeClock.elapsed());
    updateKeyMode();
    if (result.consumed) {
        if (result.target.startsWith(QStringLiteral("text:")) && m_focused) {
            const QByteArray bytes = Keybinds::unescape(result.target.mid(5));
            m_focused->clearSelection();
            m_focused->sendInput(std::string_view(bytes.constData(), std::size_t(bytes.size())));
        } else if (result.target.startsWith(QStringLiteral("script:"))) {
            m_scripts.runAction(result.target.mid(7), m_focused ? m_focused->id() : 0);
        } else {
            for (Action action : Keybinds::allActions()) {
                if (Keybinds::token(action) == result.target) { performAction(action); break; }
            }
        }
        return true;
    }
    const Keybinds& kb = m_settings.settings().keybinds;
    const Action action = kb.match(e);
    if (action != Action::None) {
        return performAction(action);
    }
    if (const QByteArray text = kb.matchText(e); !text.isEmpty()) {
        if (!m_focused) {
            return false;
        }
        m_focused->clearSelection();
        restartBlink();
        m_focused->sendInput(std::string_view(text.constData(), std::size_t(text.size())));
        return true;
    }
    const QString script = kb.matchScript(e);
    return !script.isEmpty() && m_scripts.runAction(script, m_focused ? m_focused->id() : 0);
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
    case Action::MovePaneToTab:
        return m_focused && movePaneTo(m_focused, this, nullptr, SplitTree::Direction::Right).isEmpty();
    case Action::MovePaneToWindow:
    case Action::DetachTab:
        if (!m_focused) return false;
        Q_EMIT transferRequested(m_focused, action == Action::DetachTab);
        return true;

    case Action::TogglePaneZoom:
        if (!m_focused || m_splits->paneCount() < 2) return false;
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
        splitFocused(SplitTree::Direction::Right);
        return true;
    case Action::SplitDown:
        splitFocused(SplitTree::Direction::Down);
        return true;
    case Action::SplitLeft:
        splitFocused(SplitTree::Direction::Left);
        return true;
    case Action::SplitUp:
        splitFocused(SplitTree::Direction::Up);
        return true;
    case Action::ClearScreen:
        if (m_focused) {
            m_focused->clearSelection();
            // At a prompt the shell redraws it on the cleared screen.
            const bool atPrompt = m_focused->withTerminal([](Terminal& t) {
                t.clearScreenAndHistory();
                return t.atShellPrompt() && !t.modes().altScreen;
            });
            if (atPrompt) {
                m_focused->sendInput("\x0c");
            }
            scheduleRender();
        }
        return true;
    case Action::ResetTerminal:
        if (m_focused) {
            m_focused->clearSelection();
            m_focused->withTerminal([](Terminal& t) { t.reset(); });
            scheduleRender();
        }
        return true;
    case Action::RenamePane:
        if (m_focused) {
            m_titlePane = m_focused;
            m_titleTab = nullptr;
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
        if (m_tabs.size() < 2) {
            return false; // the key goes to the program
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
        Q_EMIT newWindowRequested(focusedDirectory());
        return true;
    case Action::NewTab:
        newTab(startDirectory(focusedDirectory()));
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
    case Action::ShowHints: startHints(); return true;
    case Action::ShowLastOutput: {
        if (!m_focused) return false;
        auto command = QProcess::splitCommand(m_settings.settings().pager);
        if (command.isEmpty() || QStandardPaths::findExecutable(command.front()).isEmpty()) return false;
        const auto output = m_focused->readText(Terminal::TextScope::LastOutput, 1024 * 1024);
        if (!output.value(QStringLiteral("available")).toBool()) {
            NotificationCenter::instance().showSimple(m_focused, this, tr("No command output"), tr("Shell integration has not recorded a command yet."));
            return false;
        }
        auto file = std::make_unique<QTemporaryFile>();
        const QByteArray bytes = output.value(QStringLiteral("text")).toString().toUtf8();
        if (!file->open() || file->write(bytes) != bytes.size() || !file->flush()) return false;
        if (output.value(QStringLiteral("truncated")).toBool()) file->write("\n[Afterglow: output limited to 1 MiB]\n");
        const QString path = file->fileName();
        file->close();
        command << path;
        Pane* created = newTab(focusedDirectory(), command);
        if (!created) return false;
        file->setParent(created);
        file.release();
        return true;
    }
    case Action::CopyMode:
        if (!m_focused) return false;
        if (m_copyPane) stopCopyMode();
        else { cancelKeyMode(); m_copyPane = m_focused; m_copyPane->beginCopyMode(); updateKeyMode(); }
        return true;
    case Action::Copy:
        copySelection(true);
        return true;
    case Action::Paste:
        paste(QClipboard::Clipboard);
        return true;
    case Action::SelectAll:
        if (m_focused) {
            m_focused->selectAll();
            copyOnSelect(m_focused);
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
    case Action::UrlsOpen:
    case Action::UrlsCopy:
        startUrlMode(action == Action::UrlsCopy);
        return true;
    case Action::UnicodeInput:
        if (m_focused) {
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
        QString title = !tab->title.isEmpty() ? tab->title : focused ? focused->title() : QString();
        for (const auto& item : m_scriptAppearance)
            if (item.pane && tab->splits->contains(item.pane) && !item.tab.isEmpty()) title += QStringLiteral(" · ") + item.tab;
        titles.append(title);
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
    m_controller->setTabs(titles, m_currentTab, tabBarHeight() > 0 ? TabBarLogicalHeight : 0);
    m_controller->setTabStatus(tabStatus);
    m_controller->setTabContexts(tabContexts);

    if (m_focused) {
        const auto& snap = m_focused->snapshot();
        if (snap.cols > 0) {
            // Status colours from the palette's bright green, yellow and red.
            const auto& pal = m_settings.settings().terminal.basePalette;
            m_controller->setStatusColors(QColor(pal[10].r, pal[10].g, pal[10].b), QColor(pal[11].r, pal[11].g, pal[11].b),
                                          QColor(pal[9].r, pal[9].g, pal[9].b));
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
    m_paletteScriptActions.clear();
    for (const ScriptHost::ScriptAction& a : m_scripts.actions()) {
        actions.append(QVariantMap{{QStringLiteral("id"), ScriptActionBase + int(m_paletteScriptActions.size())},
                                   {QStringLiteral("title"), a.title},
                                   {QStringLiteral("shortcut"), kb.bindingsOfScript(a.token).join(QStringLiteral("  "))}});
        m_paletteScriptActions.append(a.token);
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
    m_focused->search(query, direction, m_controller->searchRegex());
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
    if (bell != Settings::Bell::None) {
        playBellSound();
    }
}

void TerminalWindow::playBellSound()
{
    const QString sound = m_settings.settings().bellSound;
    if (sound.isEmpty() || (m_lastBellSound.isValid() && m_lastBellSound.elapsed() < 250)) {
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
    QStringList command = QProcess::splitCommand(m_settings.settings().links.openWith);
    if (command.isEmpty()) {
        QDesktopServices::openUrl(QUrl(url));
        return;
    }
    const QString program = command.takeFirst();
    command << url;
    if (!QProcess::startDetached(program, command)) {
        qCWarning(lcWindow).noquote() << "could not run" << program << "to open" << url;
    }
}

bool TerminalWindow::linkModifierHeld() const
{
    const Qt::KeyboardModifiers modifier = m_settings.settings().links.modifier;
    return modifier == Qt::NoModifier || (QGuiApplication::queryKeyboardModifiers() & modifier);
}

void TerminalWindow::revealPane(Pane* pane)
{
    const int tab = tabOf(pane);
    if (tab < 0) {
        return;
    }
    if (tab != m_currentTab) {
        switchTab(tab);
    }
    setFocusedPane(pane);
    show();
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

QString statusStateName(AfterglowVt::StatusState s)
{
    using AfterglowVt::StatusState;
    switch (s) {
    case StatusState::Working:
        return QStringLiteral("working");
    case StatusState::Blocked:
        return QStringLiteral("blocked");
    case StatusState::Done:
        return QStringLiteral("done");
    case StatusState::Error:
        return QStringLiteral("error");
    case StatusState::Idle:
        break;
    }
    return QStringLiteral("idle");
}

/// Program-supplied text shown outside the grid: drop bidirectional
/// overrides and invisible formatting, which could disguise it.
QString sanitizedStatusText(const QString& s)
{
    QString out;
    out.reserve(s.size());
    for (const QChar c : s) {
        const char16_t u = c.unicode();
        const bool invisible = (u >= 0x200B && u <= 0x200F) || (u >= 0x202A && u <= 0x202E)
            || (u >= 0x2060 && u <= 0x2069) || u == 0xFEFF || u == 0x061C || u == 0x00AD;
        if (!invisible) {
            out.append(c);
        }
    }
    return out.left(300);
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
        const bool notable = r.state == StatusState::Blocked || r.state == StatusState::Done || r.state == StatusState::Error;
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
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (m_settings.settings().notifications && now - view.lastNotification >= 3000) {
            view.lastNotification = now;
            const QString source = statusSource(*attention);
            NotificationCenter::instance().showSimple(pane, this, source.isEmpty() ? pane->title() : source,
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
    return {{QStringLiteral("state"), statusStateName(pick->state)},
            {QStringLiteral("progress"), pick->progress},
            {QStringLiteral("priority"), best},
            {QStringLiteral("text"), source.isEmpty() ? description : source + QStringLiteral(" \u2014 ") + description}};
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
    signal << QStringLiteral("application://com.phosphorworks.Afterglow.desktop")
           << QVariantMap{{QStringLiteral("progress"), progress >= 0 ? progress / 100.0 : 0.0},
                          {QStringLiteral("progress-visible"), progress >= 0},
                          {QStringLiteral("urgent"), urgent}};
    QDBusConnection::sessionBus().send(signal);
}

void TerminalWindow::applyPointerShape()
{
    if (m_pointerHidden) {
        setCursor(Qt::BlankCursor);
        return;
    }
    if (chromeVisible() && m_chrome->wantsPointer(m_pointer)) {
        setCursor(m_chrome->cursorAt(m_pointer));
        return;
    }
    if (!m_hover.url.isEmpty()) {
        setCursor(Qt::PointingHandCursor);
        return;
    }
    Pane* pane = m_splits ? m_splits->paneAt((m_pointer * devicePixelRatio()).toPoint()) : nullptr;
    const QString shape = pane ? motion(pane).pointerShape : QString();
    setCursor(protocolPointer(shape, devicePixelRatio()));
}

TerminalWindow::LinkHover TerminalWindow::linkAt(Pane* pane, const QPointF& pos) const
{
    LinkHover found;
    if (!pane) {
        return found;
    }
    const QPoint cell = cellAt(pane, pos);
    const AfterglowRender::PaneSnapshot& snap = pane->snapshot();
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

void TerminalWindow::openContextMenu(const QPointF& pos)
{
    const Keybinds& kb = m_settings.settings().keybinds;
    auto entry = [](int id, const QString& title, const char* icon, const QString& shortcut = {}, bool enabled = true) {
        return QVariant(QVariantMap{{QStringLiteral("id"), id},
                                    {QStringLiteral("title"), title},
                                    {QStringLiteral("icon"), QString::fromLatin1(icon)},
                                    {QStringLiteral("shortcut"), shortcut},
                                    {QStringLiteral("enabled"), enabled}});
    };
    auto action = [&](Action a, const QString& title, const char* icon, bool enabled = true) {
        return entry(int(a), title, icon, kb.bindingsOf(a).value(0), enabled);
    };
    auto submenu = [](const QString& title, const char* icon, const QVariantList& items) {
        return QVariant(QVariantMap{{QStringLiteral("title"), title},
                                    {QStringLiteral("icon"), QString::fromLatin1(icon)},
                                    {QStringLiteral("items"), items}});
    };
    const QVariant separator = QVariantMap{{QStringLiteral("separator"), true}};
    const bool pane = m_focused != nullptr;

    QVariantList items;
    m_menuLink = linkAt(m_focused, pos).url;
    if (!m_menuLink.isEmpty()) {
        items << entry(MenuOpenLink, QStringLiteral("Open Link"), "internet-web-browser")
              << entry(MenuCopyLink, QStringLiteral("Copy Link Address"), "insert-link") << separator;
    }
    QVariantMap notify = action(Action::NotifyOnFinish, QStringLiteral("Notify on Next Command Finish"),
                                "preferences-desktop-notification-bell", pane)
                             .toMap();
    notify.insert(QStringLiteral("checked"), pane && m_notifyOnFinish.contains(m_focused.data()));
    items << action(Action::Copy, QStringLiteral("Copy"), "edit-copy", pane && m_focused->hasSelection())
          << action(Action::Paste, QStringLiteral("Paste"), "edit-paste", !QGuiApplication::clipboard()->text().isEmpty())
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
    m_menuScriptActions.clear();
    QVariantList scripts;
    for (const ScriptHost::ScriptAction& a : m_scripts.actions()) {
        scripts << entry(MenuScriptBase + int(m_menuScriptActions.size()), a.title, "",
                         kb.bindingsOfScript(a.token).value(0));
        m_menuScriptActions.append(a.token);
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

void TerminalWindow::updateLinkHover()
{
    Pane* pane = m_splits && linkModifierHeld() ? m_splits->paneAt((m_pointer * devicePixelRatio()).toPoint()) : nullptr;
    const LinkHover found = linkAt(pane, m_pointer);
    const bool changed = found.pane != m_hover.pane || found.link != m_hover.link || found.row != m_hover.row
        || found.colStart != m_hover.colStart || found.colEnd != m_hover.colEnd;
    m_hover = found;
    if (changed) {
        applyPointerShape();
        scheduleRender();
    }
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
    cancelKeyMode();
    if (!m_focused) return;
    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    m_hintCancel = cancelled;
    const QPointer<Pane> pane = m_focused;
    std::uint64_t generation = 0, firstRow = 0;
    auto snapshot = pane->withTerminal([&](Terminal& terminal) {
        AfterglowVt::TextSnapshot snapshot;
        const auto& screen = terminal.screen();
        firstRow = screen.rowsDropped() + screen.viewportTop();
        generation = terminal.generation();
        for (std::size_t row = screen.viewportTop(); row < screen.viewportTop() + std::size_t(screen.rows()); ++row)
            if (!AfterglowVt::appendTextRow(snapshot, screen, row, 65536)) break;
        return snapshot;
    });
    auto* watcher = new QFutureWatcher<std::vector<UrlHint>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, cancelled, pane, generation] {
        auto hints = watcher->result(); watcher->deleteLater();
        if (cancelled != m_hintCancel || *cancelled || !pane || pane != m_focused
            || pane->withTerminal([](Terminal& t) { return t.generation(); }) != generation) return;
        m_hintCancel.reset();
        activateHints(std::move(hints), false);
        m_urlMode.generation = generation;
    });
    const auto patterns = m_settings.settings().hintPatterns;
    watcher->setFuture(QtConcurrent::run([snapshot = std::move(snapshot), patterns, cancelled, firstRow] {
        std::vector<UrlHint> hints;
        QSet<QString> seen;
        for (const auto& value : patterns) {
            if (*cancelled || hints.size() >= 1000) break;
            const auto pattern = value.toMap();
            const QString action = pattern.value(QStringLiteral("action"), QStringLiteral("copy")).toString();
            const auto command = pattern.value(QStringLiteral("command")).toStringList();
            if (action != QLatin1String("copy") && action != QLatin1String("open") && action != QLatin1String("command")) continue;
            if (action == QLatin1String("command") && (command.isEmpty() || command.size() > 32)) continue;
            const auto result = AfterglowVt::searchText(snapshot, pattern.value(QStringLiteral("pattern")).toString().toStdU32String(), true, *cancelled, 1000 - hints.size());
            if (!result.error.empty()) qCWarning(lcWindow) << "hint pattern:" << QString::fromStdString(result.error);
            for (const auto& match : result.matches) {
                QString text;
                for (const auto& line : snapshot.lines) {
                    if (line.cells.empty() || line.cells.back().end < match.start || line.cells.front().start > match.end) continue;
                    const auto begin = std::lower_bound(line.cells.begin(), line.cells.end(), match.start,
                        [](const auto& cell, const auto& point) { return cell.end < point; });
                    const auto end = std::upper_bound(begin, line.cells.end(), match.end,
                        [](const auto& point, const auto& cell) { return point < cell.start; });
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
    if (!m_focused) {
        return;
    }
    const AfterglowRender::PaneSnapshot& snap = m_focused->snapshot();
    std::vector<UrlHint> hints;
    for (int y = 0; y < snap.rows; ++y) {
        for (const UrlSpan& s : findUrls(snap, y)) {
            hints.push_back({QString(), s.url, y, s.colStart});
        }
    }
    activateHints(std::move(hints), copy);
}

void TerminalWindow::activateHints(std::vector<UrlHint> hints, bool copy)
{
    if (hints.empty()) {
        return;
    }
    // Labels from the home row first, all the same length so none is a
    // prefix of another.
    static const QString Alphabet = QStringLiteral("asdfjklghqweruiopzxcvbnmty");
    const int k = int(Alphabet.size());
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
    m_urlMode = UrlMode{true, copy, m_focused, std::move(hints), QString()};
    showUrlHints();
}

void TerminalWindow::stopUrlMode()
{
    if (m_hintCancel) { *m_hintCancel = true; m_hintCancel.reset(); }
    m_urlMode = UrlMode{};
    m_controller->setUrlHints({});
    scheduleRender();
}

void TerminalWindow::showUrlHints()
{
    if (!m_urlMode.pane || !m_splits || tabOf(m_urlMode.pane) != m_currentTab) {
        stopUrlMode();
        return;
    }
    const auto& m = m_renderer->fonts().fonts->metrics();
    const QPoint origin = contentRect(m_splits->rectOf(m_urlMode.pane)).topLeft();
    const qreal dpr = devicePixelRatio();
    QVariantList list;
    for (const UrlHint& h : m_urlMode.hints) {
        if (!h.label.startsWith(m_urlMode.typed)) {
            continue;
        }
        list.append(QVariantMap{{QStringLiteral("x"), (origin.x() + m_urlMode.pane->snapshot().visualColumn(h.col, h.row) * m.cellWidth) / dpr},
                                {QStringLiteral("y"), (origin.y() + h.row * m.cellHeight) / dpr},
                                {QStringLiteral("label"), h.label},
                                {QStringLiteral("typed"), int(m_urlMode.typed.size())}});
    }
    m_controller->setUrlHints(list);
    scheduleRender();
}

void TerminalWindow::urlModeKey(QKeyEvent* e)
{
    if (m_urlMode.generation && (!m_urlMode.pane || m_urlMode.pane->withTerminal([](Terminal& t) { return t.generation(); }) != m_urlMode.generation)) {
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
        stopUrlMode();
        if (action == QLatin1String("command") || action == QLatin1String("open")) {
            QString path = url, line = QStringLiteral("1"), column = QStringLiteral("1");
            static const QRegularExpression location(QStringLiteral(R"(^(.*?):(\d+)(?::(\d+))?$)"));
            const auto match = location.match(path);
            if (match.hasMatch()) { path = match.captured(1); line = match.captured(2); if (!match.captured(3).isEmpty()) column = match.captured(3); }
            if (path.startsWith(QStringLiteral("~/"))) path.replace(0, 1, QDir::homePath());
            if (QFileInfo(path).isRelative()) path = QDir(focusedDirectory()).absoluteFilePath(path);
            if (action == QLatin1String("open")) QDesktopServices::openUrl(QUrl::fromLocalFile(path));
            else {
                QStringList args = configuredCommand;
                for (QString& arg : args) arg.replace(QStringLiteral("{match}"), url).replace(QStringLiteral("{path}"), path)
                    .replace(QStringLiteral("{line}"), line).replace(QStringLiteral("{column}"), column);
                if (!args.isEmpty()) { const QString program = args.takeFirst(); QProcess::startDetached(program, args, focusedDirectory()); }
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

void TerminalWindow::showUnicodeInput()
{
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
            const char32_t c = char32_t(cp);
            m_focused->sendInput(QString::fromUcs4(&c, 1).toStdString());
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
    auto chains = m_shaders.build(s.shaders, s.shaderParams);
    m_shaderControls.rebuild(chains);
    m_audioReactive = chains.audioReactive();
    m_overviewAvailable = !chains.overview.empty();
    if (!m_overviewAvailable) closeOverview(nullptr);
    m_renderer->setShaderChains(std::move(chains));
    m_audio.setActive(this, m_audioReactive && isExposed() && visibility() != QWindow::Minimized);
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
        for (Pane* p : findChildren<Pane*>()) {
            p->setOptions(now.terminal);
        }
    }
    if (now.scrollOnOutput != previous.scrollOnOutput || now.scrollOnInput != previous.scrollOnInput) {
        for (Pane* p : findChildren<Pane*>()) {
            p->setScrollPolicy(now.scrollOnOutput, now.scrollOnInput);
        }
    }
    m_blinkTimer.setInterval(now.cursorBlinkInterval);
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
    if (m_settings.settings().keybinds.match(e) == Action::CopyMode) { stopCopyMode(); return true; }
    if (m_settings.settings().keybinds.match(e) == Action::Copy) { performAction(Action::Copy); stopCopyMode(); return true; }
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
    const quint32 physicalKey = e->nativeScanCode() ? e->nativeScanCode() : quint32(e->key());
    if (!e->isAutoRepeat()) m_consumedKeys.remove(physicalKey);
    if (copyModeKey(e)) { m_consumedKeys.insert(physicalKey); return; }
    if (m_overview.open) {
        m_consumedKeys.insert(physicalKey);
        overviewKey(e);
        return;
    }
    if (m_urlMode.active) {
        m_consumedKeys.insert(physicalKey);
        urlModeKey(e);
        return;
    }
    if (m_unicodeInput) {
        m_consumedKeys.insert(physicalKey);
        unicodeInputKey(e);
        return;
    }
    if (e->key() == Qt::Key_Control || e->key() == Qt::Key_Shift || e->key() == Qt::Key_Alt) {
        updateLinkHover();
    }
    if (m_controller->wantsKeyboard()) {
        m_consumedKeys.insert(physicalKey);
        cancelKeyMode();
        // The palette and search keybinds still toggle while the chrome has the keyboard.
        const Action a = m_settings.settings().keybinds.match(e);
        if (!m_scriptDialog && (a == Action::CommandPalette || a == Action::Search)) {
            performAction(a);
            return;
        }
        m_chrome->deliver(e);
        return;
    }
    if (handleShortcut(e)) {
        m_consumedKeys.insert(physicalKey);
        return;
    }
    sendKey(e);
}

void TerminalWindow::keyReleaseEvent(QKeyEvent* e)
{
    const quint32 physicalKey = e->nativeScanCode() ? e->nativeScanCode() : quint32(e->key());
    if (m_consumedKeys.contains(physicalKey)) {
        if (!e->isAutoRepeat()) m_consumedKeys.remove(physicalKey);
        if (m_controller->wantsKeyboard()) m_chrome->deliver(e);
        return;
    }
    if (m_urlMode.active || m_unicodeInput || m_overview.open) {
        return; // the mode took the press
    }
    const Qt::KeyboardModifiers linkModifier = m_settings.settings().links.modifier;
    if ((e->key() == Qt::Key_Control && linkModifier == Qt::ControlModifier)
        || (e->key() == Qt::Key_Shift && linkModifier == Qt::ShiftModifier)
        || (e->key() == Qt::Key_Alt && linkModifier == Qt::AltModifier)) {
        // queryKeyboardModifiers() still reports the key during its own release.
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
    if (ev.action != AfterglowVt::KeyAction::Release && m_focused->isHeld()) {
        closePane(m_focused); // --hold: its program has ended; any key closes it
        return;
    }
    if (ev.action != AfterglowVt::KeyAction::Release) {
        m_focused->clearSelection();
        restartBlink();
        if (m_settings.settings().hideMouseWhileTyping && !m_pointerHidden) {
            m_pointerHidden = true;
            setCursor(Qt::BlankCursor);
        }
    }
    m_focused->sendInput(bytes);
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
    if (!m_focused || !m_focused->hasSelection()) {
        return;
    }
    QGuiApplication::clipboard()->setText(m_focused->selectedText(),
                                          toClipboard ? QClipboard::Clipboard : QClipboard::Selection);
}

void TerminalWindow::clipboardRequest(Pane* pane, const AfterglowVt::ClipboardRequest& r)
{
    using AfterglowVt::ClipboardAssembler;
    QClipboard* clipboard = QGuiApplication::clipboard();
    if (r.osc52) {
        // An OSC 52 read: no name or password to remember a grant by, so
        // the setting decides, or the user does each time.
        const QString policy = m_settings.settings().clipboardRead;
        if (policy == QLatin1String("deny") || (r.primary && !clipboard->supportsSelection()) || m_pendingClipboardRead) {
            pane->sendInput(r.osc52Reply({}));
        } else if (policy == QLatin1String("allow")) {
            answerClipboardRead(pane, r);
        } else {
            m_pendingClipboardRead = PendingClipboardRead{pane, r};
            const QString who = pane->title().isEmpty() ? tr("A program") : pane->title();
            m_controller->askClipboard(
                tr("%1 wants to read the %2.").arg(who, r.primary ? tr("primary selection") : tr("clipboard")), {});
            if (pane != m_focused || !isActive()) {
                revealPane(pane);
            }
        }
        return;
    }
    if (r.primary && !clipboard->supportsSelection()) {
        pane->sendInput(r.kind == AfterglowVt::ClipboardRequest::Kind::Read ? ClipboardAssembler::readStatus("ENOSYS", r)
                                                                             : ClipboardAssembler::writeStatus("ENOSYS", r.id));
        return;
    }
    if (r.kind == AfterglowVt::ClipboardRequest::Kind::Write) {
        // Writes are allowed, as OSC 52 writes are.
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
        clipboard->setMimeData(data, r.primary ? QClipboard::Selection : QClipboard::Clipboard);
        pane->sendInput(ClipboardAssembler::writeStatus("DONE", r.id));
        return;
    }

    // Listing the types needs no permission.
    if (r.mimes.size() == 1 && r.mimes[0] == ".") {
        answerClipboardRead(pane, r);
        return;
    }
    auto& grants = m_clipboardGrants[pane];
    const QByteArray password = QByteArray::fromStdString(r.password);
    if (!password.isEmpty()) {
        if (const auto it = grants.find(password); it != grants.end()) {
            if (it->oneTime) {
                grants.erase(it);
            }
            answerClipboardRead(pane, r);
            return;
        }
    }
    if (m_pendingClipboardRead) {
        pane->sendInput(ClipboardAssembler::readStatus("EBUSY", r));
        return;
    }
    // Ask. The name, when the program gave one with a password, can be remembered.
    QStringList types;
    for (const std::string& m : r.mimes) {
        types.append(QString::fromStdString(m));
    }
    const QString name = r.password.empty() ? QString() : QString::fromStdString(r.name).left(80);
    const QString who = name.isEmpty() ? (pane->title().isEmpty() ? tr("A program") : pane->title()) : name;
    m_pendingClipboardRead = PendingClipboardRead{pane, r};
    m_controller->askClipboard(tr("%1 wants to read the %2 (%3).")
                                   .arg(who, r.primary ? tr("primary selection") : tr("clipboard"),
                                        types.join(QStringLiteral(", ")).left(200)),
                               name);
    if (pane != m_focused || !isActive()) {
        revealPane(pane);
    }
}

void TerminalWindow::answerClipboardRead(Pane* pane, const AfterglowVt::ClipboardRequest& r, std::string_view oneTimePassword)
{
    using AfterglowVt::ClipboardAssembler;
    if (r.osc52) {
        const QByteArray text = QGuiApplication::clipboard()->text(r.primary ? QClipboard::Selection : QClipboard::Clipboard)
                                    .toUtf8()
                                    .left(8 * 1024 * 1024);
        pane->sendInput(r.osc52Reply(std::string_view(text.constData(), std::size_t(text.size()))));
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
        out += ClipboardAssembler::readData(mime, std::string_view(bytes.constData(), std::size_t(bytes.size())), r,
                                            oneTimePassword);
    }
    out += ClipboardAssembler::readStatus("DONE", r, oneTimePassword);
    pane->sendInput(out);
}

bool TerminalWindow::sendPasteEvent(QClipboard::Mode mode)
{
    if (!m_focused || !m_focused->withTerminal([](Terminal& t) { return t.modes().pasteEvents; })) {
        return false;
    }
    // The program gets the list of types and a one-time password to read them.
    const QByteArray password = QUuid::createUuid().toByteArray(QUuid::WithoutBraces);
    m_clipboardGrants[m_focused][password] = ClipboardGrant{true};
    AfterglowVt::ClipboardRequest listing;
    listing.primary = mode == QClipboard::Selection;
    listing.mimes = {"."};
    answerClipboardRead(m_focused, listing, std::string_view(password.constData(), std::size_t(password.size())));
    return true;
}

void TerminalWindow::paste(QClipboard::Mode mode)
{
    if (!m_focused) {
        return;
    }
    if (sendPasteEvent(mode)) {
        return;
    }
    const QByteArray text = QGuiApplication::clipboard()->text(mode).toUtf8();
    if (text.isEmpty()) {
        return;
    }
    // Any raw line terminator can execute input without bracketed paste.
    const bool bracketed = m_focused->withTerminal([](Terminal& t) { return t.modes().bracketedPaste; });
    if (m_settings.settings().confirmUnsafePaste && !bracketed && (text.contains('\n') || text.contains('\r'))) {
        m_pendingPaste = text;
        m_pendingPastePane = m_focused;
        const QByteArray normalized = QByteArray(text).replace("\r\n", "\n").replace('\r', '\n');
        const QString preview = QString::fromUtf8(normalized.left(4000));
        m_controller->askPaste(preview, int(normalized.count('\n')) + 1);
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
    if (!m_splits) return;
    if (m_overview.open) {
        // The cards are where the shader draws them: a click takes the one in front.
        closeOverview(e->button() == Qt::LeftButton ? m_overview.highlighted.data() : nullptr);
        return;
    }
    if (m_pointerHidden) {
        m_pointerHidden = false;
        applyPointerShape();
    }
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
    const Qt::KeyboardModifiers linkModifier = m_settings.settings().links.modifier;
    if (e->button() == Qt::LeftButton && linkModifier != Qt::NoModifier && (e->modifiers() & linkModifier)
        && !m_hover.url.isEmpty()) {
        openLink(m_hover.url);
        return;
    }
    if (e->button() == Qt::LeftButton) m_focused->dragDrop()->press(e->position().toPoint());
    const int code = mouseButtonCode(e->button());
    if (code >= 0 && sendMouse(m_focused, e->position(), code, int(AfterglowVt::MouseAction::Press), e->modifiers())) {
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
        case MouseSettings::RightClick::Menu:
            openContextMenu(e->position());
            break;
        case MouseSettings::RightClick::Paste:
            paste(QClipboard::Clipboard);
            break;
        case MouseSettings::RightClick::CopyOrPaste:
            if (m_focused->hasSelection()) {
                copySelection(true);
                m_focused->clearSelection();
                scheduleRender();
            } else {
                paste(QClipboard::Clipboard);
            }
            break;
        case MouseSettings::RightClick::Extend:
            if (m_focused->hasSelection()) {
                const QPoint cell = cellAt(m_focused, e->position());
                m_focused->extendSelection(m_focused->selectionPointAt(cell.x(), cell.y()));
                copyOnSelect(m_focused);
                scheduleRender();
            }
            break;
        }
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
    if (m_overview.open || m_overview.amount.isAnimating()) return;
    if (e->button() == Qt::LeftButton) for (auto* pane : panes()) pane->dragDrop()->release();
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
            copyOnSelect(m_selectionPane);
        } else {
            m_selectionPane->clearSelection();
            const bool plain = !(e->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier));
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
    // to move the cursor: as many arrow keys as cells between there and here.
    const QPoint cell = cellAt(pane, pos);
    const std::string keys = pane->withTerminal([&cell](Terminal& t) -> std::string {
        if (!t.promptTakesClicks() || t.screen().viewportOffset() != 0
            || t.modes().mouseTracking != AfterglowVt::MouseTracking::None) {
            return {};
        }
        const auto& cur = t.screen().cursor;
        // Only on the prompt's own lines: from where it starts to the bottom.
        int promptRow = cur.y;
        while (promptRow > 0 && !t.screen().row(promptRow).promptStart) {
            --promptRow;
        }
        if (cell.y() < promptRow) {
            return {};
        }
        const long delta = long(cell.y() - cur.y) * t.cols() + (cell.x() - cur.x);
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
    m_pointerHidden = false;
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
    if (!m_selectionPane && routePointerToChrome(e, e->position(), false, false)) {
        setCursor(m_chrome->cursorAt(m_pointer));
        return;
    }
    if (chromeVisible()) {
        // Keep hover state in the chrome current as the pointer leaves it.
        QMouseEvent outside(e->type(), QPointF(-1, -1), QPointF(-1, -1), e->button(), e->buttons(), e->modifiers());
        m_chrome->deliver(&outside);
    }
    // After the chrome has seen the pointer leave, which may reset the cursor.
    if (auto divider = m_splits->dividerAt(px, int(std::lround(4 * devicePixelRatio())))) {
        setCursor(divider->vertical ? Qt::SplitVCursor : Qt::SplitHCursor);
    } else {
        applyPointerShape();
    }
    if (m_focused && (e->buttons() & Qt::LeftButton)) {
        const QPoint pixel = px - contentRect(m_splits->rectOf(m_focused)).topLeft();
        if (m_focused->dragDrop()->move(this, e->position().toPoint(), cellAt(m_focused, e->position()), pixel)) {
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
    sendMouse(m_focused, e->position(), code, int(AfterglowVt::MouseAction::Motion), e->modifiers());
}

void TerminalWindow::wheelEvent(QWheelEvent* e)
{
    if (!m_splits) return;
    if (m_overview.open) {
        m_overviewWheel += e->angleDelta().y() ? e->angleDelta().y() : e->angleDelta().x();
        const auto panes = overviewPanes();
        const auto at = std::find(panes.begin(), panes.end(), m_overview.highlighted.data());
        const int current = at == panes.end() ? 0 : int(at - panes.begin());
        const int steps = m_overviewWheel / 120;
        m_overviewWheel -= steps * 120;
        if (steps) moveOverview(current - steps);
        return;
    }
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
        for (int i = 0; i < std::abs(steps) * m_settings.settings().scrollLines; ++i) {
            bytes += AfterglowVt::encodeKey(key, modes);
        }
        p->sendInput(bytes);
        return;
    }
    scrollPane(p, double(steps) * m_settings.settings().scrollLines);
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
QRect TerminalWindow::accessiblePaneRect(Pane* pane) const
{
    if (!m_splits || tabIdOf(pane) != activeTabId() || m_splits->rectOf(pane).isEmpty()) return {};
    const QRect px = contentRect(m_splits->rectOf(pane));
    const qreal dpr = devicePixelRatio();
    return QRect(mapToGlobal((QPointF(px.topLeft()) / dpr).toPoint()), (QSizeF(px.size()) / dpr).toSize());
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
        return QRectF(area.x() + pane->snapshot().visualColumn(point.col, int(row)) * cw, area.y() + row * ch, width * cw, ch).toAlignedRect();
    });
}
}

namespace Afterglow {
QAccessibleInterface* TerminalWindow::accessibleRoot() const
{
    return QAccessible::queryAccessibleInterface(const_cast<TerminalWindow*>(this));
}
}
