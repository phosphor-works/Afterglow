// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Application.h"
#include "InstanceSocket.h"

#include "NotificationCenter.h"
#include "ScriptSettings.h"
#include "WindowStart.h"
#include "../chrome/ChromeLayer.h"
#include "../desktop/DragDrop.h"
#include "../mux/MuxClient.h"
#include "../mux/MuxRoute.h"
#include "../control/SessionControl.h"

#include <QIcon>
#include <QStandardPaths>
#include "../scripting/ScriptControl.h"

#include <PhosphorShortcuts/Factory.h>
#include <PhosphorShortcuts/IBackend.h>
#include <PhosphorShortcuts/Registry.h>

#include <QFileInfo>
#include <QGuiApplication>
#include <QKeySequence>
#include <QLoggingCategory>
#include <QQuickStyle>

#include <algorithm>
#include <functional>
#include <initializer_list>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

Q_LOGGING_CATEGORY(lcApp, "afterglow.app", QtInfoMsg)

namespace Afterglow {

/// What came of the calls of a session window's attach that wait for the
/// daemon: the way there and the connection that answered, or an error.
struct SessionAttached
{
    AfterglowMux::Route route;
    std::shared_ptr<SessionLink> link;
    QString error;
};

/// Where the attaches that run off the GUI thread give their outcome: the
/// application while it lives. An attach over ssh can wait for minutes, and
/// a quit waits for it only a bounded time (MuxClient::waitForCallThreads).
struct LaunchMailbox
{
    std::mutex mutex;
    Application* application = nullptr;
};

namespace {
const QString QuickShortcutId = QStringLiteral("toggle-quick-terminal");

/// What the attach of a session's window needs, read on the GUI thread,
/// which changes the environment that muxSockets() reads (an activation
/// token).
struct SessionAttach
{
    QString session;
    QString host; ///< the ssh destination, empty for this computer
    bool takeover = true;
    AfterglowProto::MuxSockets sockets; ///< for this computer
    QString remoteCommand;
    int controlPersist = 0;
    std::size_t eventQueue = 0;
    std::size_t history = 0;
    /// Taken on the GUI thread, which changes it (an activation token).
    AfterglowMux::Environment environment;
};

SessionAttach sessionAttach(const Settings& settings, const QString& session, const QString& host, bool takeover)
{
    return {session,
            host,
            takeover,
            host.isEmpty() ? AfterglowProto::muxSockets() : AfterglowProto::MuxSockets{},
            settings.mux.remoteCommand,
            settings.sshControlPersist,
            settings.mux.eventQueue,
            std::size_t(settings.terminal.maxScrollback),
            AfterglowMux::currentEnvironment()};
}

/// The calls of the attach that wait for the daemon, with no Qt object: for
/// any thread.
SessionAttached attachOnAnyThread(const SessionAttach& job)
{
    SessionAttached attached;
    if (job.host.isEmpty()) {
        attached.route.sockets = job.sockets;
    } else if (auto remote =
                   muxRoute(job.host, job.remoteCommand, job.controlPersist, job.environment, &attached.error)) {
        attached.route = std::move(*remote);
        // A window has no terminal: ssh joins the connection that a terminal
        // signed in (afterglow mux --ssh), or signs in with keys alone. It
        // never asks on the terminal that started this process.
        std::vector<std::string>& proxy = attached.route.proxy;
        if (!proxy.empty())
            proxy.insert(proxy.begin() + 1, std::initializer_list<std::string>{"-o", "BatchMode=yes"});
    } else {
        return attached;
    }
    // The first answer gets a call's wait: with BatchMode, ssh waits for no
    // one to sign in, and the launcher waits for this answer.
    if (auto link = attachSession(job.session, job.takeover, &attached.error, attached.route, job.eventQueue,
                                  job.history, AfterglowMux::CallTimeoutMs, &job.environment)) {
        attached.link = std::make_shared<SessionLink>(std::move(*link));
    }
    return attached;
}
} // namespace

Application::Application(InstanceSocket& socket, const QString& configPath, const QJsonObject& overrides,
                         ScriptDirectories scriptDirectories)
    : m_settings(configPath.isEmpty() ? SettingsStore::defaultPath() : configPath, overrides)
    , m_device(m_settings.settings().font)
    , m_chromeComponent(&m_engine)
    // Packs and presets beside the config file, as its scripts and themes are.
    , m_shaders(QFileInfo(m_settings.path()).absolutePath())
    , m_control(m_settings,
                [this](const QString& cwd, const QStringList& command, bool empty) {
                    return newWindow({cwd, command, false, !cwd.isEmpty(), {}, false, empty});
                })
    , m_socket(socket)
    , m_controlSocket(m_control, m_settings, socket.router())
    , m_scripts(scriptCallbacks(m_control))
    , m_commandSocket(m_control, m_scripts, m_settings, socket.router())
    , m_remoteSockets(
          m_control, m_scripts, m_settings,
          [this](quint32 pane, const AfterglowProto::Remote::Answer& answer,
                 const std::optional<AfterglowProto::Remote::Answer>& overlay) {
              return m_control.setRemoteSocket(pane, answer, overlay);
          },
          // One budget for every command socket: the limits hold for the process.
          m_commandSocket.budget())
    , m_services{m_device, m_settings, m_chromeComponent, m_shaders, m_scripts, m_audio}
    , m_launch(*this)
    , m_mailbox(std::make_shared<LaunchMailbox>())
{
    m_mailbox->application = this;
    // Panes that leave a session's window go to a new session, in a window
    // of its own, and a project's session whose window closed gets a new
    // one: no other window shows the session, which stays with another
    // Afterglow that shows it. The attach waits off the GUI thread, as a
    // launch's does, and Control finishes the move or the open then.
    m_control.setSessionWindowOpener([this](const QString& session, const QString& host, Control::SessionOpened done) {
        openLaunch({}, {},
                   {{QStringLiteral("session"), session},
                    {QStringLiteral("sessionHost"), host},
                    {QStringLiteral("takeover"), false}},
                   std::move(done));
    });
    m_control.setSessionLinkShower([this](const QString& session, SessionLink&& link) {
        WindowOptions window;
        QString error;
        window.session = MuxClient::adopt(session, std::move(link), &error, m_settings.settings().mux.eventQueue);
        if (!window.session)
            return error;
        return newWindow(window) ? QString() : QStringLiteral("cannot show session '%1'").arg(session);
    });
    // Some platform-theme versions leave Qt Quick at its default Fusion style.
    // Prefer the desktop bridge on Plasma, preserving explicit style choices
    // and falling back normally when the optional module is not installed.
    const QString desktop = qEnvironmentVariable("XDG_CURRENT_DESKTOP").toLower();
    if (qEnvironmentVariableIsEmpty("QT_QUICK_CONTROLS_STYLE") && qEnvironmentVariableIsEmpty("QT_QUICK_CONTROLS_CONF")
        && QQuickStyle::name() == QLatin1String("Fusion")
        && (desktop.contains(QLatin1String("kde")) || desktop.contains(QLatin1String("plasma")))) {
        for (const QString& path : m_engine.importPathList()) {
            if (QFileInfo::exists(path + QStringLiteral("/org/kde/desktop/qmldir"))) {
                QQuickStyle::setStyle(QStringLiteral("org.kde.desktop"));
                break;
            }
        }
    }
    // The chrome type, its imports and their plugins load on the QML loader
    // thread from now on, with the style chosen, for every window to come.
    m_chromeComponent.loadUrl(ChromeLayer::sceneUrl(), QQmlComponent::Asynchronous);

    m_controlSocket.setRemoteSockets(&m_remoteSockets);
    connect(&m_control, &Control::paneClosed, &m_remoteSockets, &RemoteSockets::paneClosed);

    connect(&m_audio, &AudioController::spectrumChanged, &m_device, [this](const QVector<float>& spectrum) {
        m_device.setAudioSpectrum(std::span<const float>(spectrum.constData(), std::size_t(spectrum.size())));
    });
    m_audio.configure(m_settings.settings().audio);
    NotificationCenter::instance().setRateLimit(m_settings.settings().notificationRateLimit,
                                                qint64(m_settings.settings().notificationRateWindow) * 1000);
    NotificationCenter::instance().setFilters(m_settings.settings().notificationFilters);
    DragTrees::instance().setLifetime(std::chrono::minutes(m_settings.settings().dragTreeLifetime));
    QStringList iconPaths = QIcon::themeSearchPaths();
    for (const auto& path : QStandardPaths::locateAll(QStandardPaths::GenericDataLocation, QStringLiteral("icons"),
                                                      QStandardPaths::LocateDirectory))
        if (!iconPaths.contains(path))
            iconPaths.append(path);
    QIcon::setThemeSearchPaths(iconPaths);
    if (QIcon::themeName().isEmpty())
        QIcon::setThemeName(QStringLiteral("breeze"));
    if (QIcon::fallbackThemeName().isEmpty())
        QIcon::setFallbackThemeName(QStringLiteral("hicolor"));
    connectScriptEvents(m_scripts, m_control);
    // Script packages bring their shader packs with them.
    connect(&m_scripts, &ScriptHost::packageShadersChanged, &m_shaders, &ShaderLibrary::setPackageShaders);
    m_control.setScriptCommandRunner([this](const QString& command, quint32 pane, const QString& source) {
        // Off, the scripts have no commands: "no command" would hide why.
        if (!m_scripts.isEnabled())
            return QStringLiteral("scripts are off (Scripts.enabled)");
        return m_scripts.runCommand(command, pane, source);
    });
    m_control.setScriptDiagnostics([this] {
        return m_scripts.diagnostics();
    });
    const QString ownScripts = QFileInfo(m_settings.path()).absolutePath() + QStringLiteral("/scripts");
    QStringList scriptPaths{ownScripts};
    if (scriptDirectories == ScriptDirectories::System) {
        scriptPaths = ScriptHost::defaultSearchPaths();
        scriptPaths.last() = ownScripts;
    }
    m_scripts.setSearchPaths(scriptPaths);
    m_scripts.setStateDirectory(QFileInfo(m_settings.path()).absolutePath() + QStringLiteral("/state/scripts"));
    // After the command socket, whose slot for the same settings runs first.
    followScriptSettings(m_scripts, m_settings);

    connect(&m_settings, &SettingsStore::changed, this, [this](const Settings& previous) {
        if (m_settings.settings().font != previous.font) {
            m_device.setFontSpec(m_settings.settings().font);
        }
        if (m_settings.settings().quickTerminal.shortcut != previous.quickTerminal.shortcut) {
            bindShortcuts();
        }
        QTimer::singleShot(0, &m_scripts, [this] {
            m_scripts.dispatch(QStringLiteral("on_config_changed"), 0);
        });
        if (const AudioSettings& audio = m_settings.settings().audio; audio != previous.audio) {
            m_audio.configure(audio);
        }
        NotificationCenter::instance().setRateLimit(m_settings.settings().notificationRateLimit,
                                                    qint64(m_settings.settings().notificationRateWindow) * 1000);
        NotificationCenter::instance().setFilters(m_settings.settings().notificationFilters);
        DragTrees::instance().setLifetime(std::chrono::minutes(m_settings.settings().dragTreeLifetime));
    });

    // Hidden windows (a tucked-away quick terminal) still count as open.
    QGuiApplication::setQuitOnLastWindowClosed(false);
    // A quit closes the windows: their sessions must know it first.
    QCoreApplication::instance()->installEventFilter(this);

    bindShortcuts();
    // Last: from here launches that waited on the socket get their windows.
    // The router is this process's, and nothing else registers the target:
    // a refusal is a bug, which launches would see as NO_SUCH_TARGET.
    if (!m_socket.router().registerTarget(QLatin1String(LaunchApi::Target), &m_launch)) {
        qCWarning(lcApp) << "another object holds the launch target";
    }
}

Application::~Application()
{
    {
        // The attaches that still wait drop their answers from here.
        const std::lock_guard lock(m_mailbox->mutex);
        m_mailbox->application = nullptr;
    }
    QCoreApplication::instance()->removeEventFilter(this);
    // What quitting ends is no news: the script command runs without a
    // client, which the scripts end as they go, show no notice.
    m_control.endNotices();
    // No new windows while the others close.
    m_socket.router().unregisterTarget(QLatin1String(LaunchApi::Target), &m_launch);
    m_controlSocket.setRemoteSockets(nullptr);
    // A quit (logout, a signal) is not the user closing a session's window:
    // every session keeps running, whatever Mux.onClose says.
    for (TerminalWindow* window : TerminalWindow::windows())
        window->keepSessionOnClose();
    // Windows render on the device and the chrome uses the QML engine.
    qDeleteAll(TerminalWindow::windows());
    // The scripts go before Control: its callbacks into them must not outlive them.
    m_control.setScriptCommandRunner({});
    m_control.setScriptDiagnostics({});
    // Nor its callbacks into this object, which open windows.
    m_control.setSessionWindowOpener({});
    m_control.setSessionLinkShower({});
}

bool Application::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() != QEvent::Quit || watched != QCoreApplication::instance())
        return QObject::eventFilter(watched, event);
    // A quit (QuitSignals, the end of the desktop session) is not the user
    // closing a session's window: every session keeps running, whatever
    // Mux.onClose says. Each window learns it before it closes: in
    // ~Application, as this process is a QApplication, which closes only
    // its widgets on a Quit.
    const QList<TerminalWindow*> windows = TerminalWindow::windows();
    for (TerminalWindow* window : windows)
        window->keepSessionOnClose();
    // Qt's own handling, here so that its outcome shows: a widget that does
    // not close ignores the event and interrupts the quit. The windows then
    // stay open.
    watched->event(event);
    if (!event->isAccepted()) {
        // No quit: Mux.onClose decides again for the windows that stay open.
        for (TerminalWindow* window : TerminalWindow::windows()) {
            if (!window->closing())
                window->keepSessionOnClose(false);
        }
    }
    return true;
}

void Application::bindShortcuts()
{
    if (!m_shortcutBackend) {
        m_shortcutBackend = PhosphorShortcuts::createBackend(PhosphorShortcuts::BackendHint::Auto, this);
        if (!m_shortcutBackend) {
            qCWarning(lcApp) << "no global shortcut backend; use `afterglow --quick-terminal` instead";
            return;
        }
        m_shortcuts = std::make_unique<PhosphorShortcuts::Registry>(m_shortcutBackend.get(), this);
    }
    const QString seq = m_settings.settings().quickTerminal.shortcut;
    if (seq == m_boundQuickShortcut) {
        return;
    }
    if (seq.isEmpty()) {
        m_shortcuts->unbind(QuickShortcutId);
    } else {
        m_shortcuts->bind(QuickShortcutId, QKeySequence::fromString(seq, QKeySequence::PortableText),
                          QStringLiteral("Toggle the quick terminal"), [this] {
                              if (const QString error = toggleQuickTerminal(); !error.isEmpty())
                                  qCWarning(lcApp).noquote() << error;
                          });
    }
    m_shortcuts->flush();
    m_boundQuickShortcut = seq;
}

TerminalWindow* Application::newWindow(const WindowOptions& options)
{
    auto* window = new TerminalWindow(m_services, options);
    if (!options.empty && !window->focusedPane()) {
        delete window;
        return nullptr;
    }
    m_control.attachWindow(window);
    // `afterglow ctl` for the session: the calls that only this window can
    // answer, which the daemon passes it.
    if (options.session) {
        options.session->setControlHandler(
            [this, shown = QPointer<TerminalWindow>(window)](const QString& fn, const QJsonArray& args, qint64 client,
                                                             MuxClient::ControlReply reply) {
                if (!shown)
                    return reply({{}, QStringLiteral("the window of the session closed")});
                answerSessionCall(m_control, m_settings.settings(), *shown, fn, args, client, std::move(reply));
            });
        // `aglow run` in a session pane: the commands target of this
        // process serves it, as it serves a pane outside a session. A
        // client of another host gets a socket of a remote session.
        MuxCommands::Host commands;
        commands.socket = [](QString* error) {
            const QString path = InstanceSocket::processPath();
            if (path.isEmpty())
                *error =
                    QStringLiteral("this afterglow listens on no socket, so it runs no commands for session panes");
            return path;
        };
        commands.remote = [this](quint32 pane, const QString& host, bool commandsTarget, QString* error) {
            return m_remoteSockets.endpoint(pane, host, commandsTarget, error);
        };
        commands.pane = [shown = QPointer<TerminalWindow>(window)](std::uint32_t id) -> std::optional<quint32> {
            const Pane* pane = shown ? shown->sessionPane(id) : nullptr;
            if (!pane)
                return std::nullopt;
            return pane->id();
        };
        commands.focused = [shown = QPointer<TerminalWindow>(window)]() -> std::optional<quint32> {
            const Pane* pane = shown ? shown->focusedPane() : nullptr;
            if (!pane || !pane->isSessionPane())
                return std::nullopt;
            return pane->id();
        };
        options.session->setCommandsHost(std::move(commands));
    }
    connect(window, &TerminalWindow::newWindowRequested, this, [this](const QString& cwd) {
        newWindow({cwd, {}, false, false, {}, false});
    });
    connect(window, &QObject::destroyed, this, &Application::quitWhenNoWindowsRemain, Qt::QueuedConnection);
    if (!options.quick) {
        // Before the first show, also for the empty windows that ctl,
        // scripts, projects and moved panes fill and show later.
        const WindowSettings& ws = m_settings.settings().window;
        window->setFlag(Qt::FramelessWindowHint, !ws.decorations);
        if (!options.empty)
            showForStartMode(*window, ws);
    }
    return window;
}

namespace {

/// Runs @p activate with the launcher's activation token of @p options in the
/// environment, where Qt reads it when a window asks for the focus. An unused
/// token must not reach the shells of later panes.
void withActivationToken(const QVariantMap& options, const std::function<void()>& activate)
{
    const QByteArray token = options.value(QStringLiteral("activationToken")).toString().toUtf8();
    if (!token.isEmpty())
        qputenv("XDG_ACTIVATION_TOKEN", token);
    activate();
    if (!token.isEmpty())
        qunsetenv("XDG_ACTIVATION_TOKEN");
}

} // namespace

void Application::openLaunch(const QString& workingDirectory, const QStringList& command, const QVariantMap& options,
                             LaunchDone done)
{
    WindowOptions window{workingDirectory,
                         command,
                         false,
                         options.value(QStringLiteral("directoryGiven")).toBool(),
                         options.value(QStringLiteral("title")).toString(),
                         options.value(QStringLiteral("hold")).toBool()};
    // afterglow mux attach: the window shows a session of the mux daemon.
    const QString session = options.value(QStringLiteral("session")).toString();
    if (session.isEmpty())
        return done(showLaunch(window, options));
    const QString host = options.value(QStringLiteral("sessionHost")).toString();
    // A window of this process shows it already: that window comes to the
    // front. An attach would take the session from it into a new window. A
    // window that closes is no longer the session's: the attach goes on.
    // Another name of the same host makes the attach take the session,
    // which works too.
    if (TerminalWindow* shown = sessionWindow(session, host)) {
        raiseForLaunch(shown, options);
        return done({});
    }
    // An attach of this session waits already, for a launch a moment ago: its
    // window answers this launch too. A second attach would take the session
    // from that window, or fail for it. The attach that waits keeps its
    // takeover.
    for (SessionLaunch& waiting : m_sessionLaunches) {
        if (waiting.session != session || waiting.host != host)
            continue;
        waiting.joined.append({workingDirectory, command, options, std::move(done)});
        return;
    }
    // The daemon of this computer, or of the ssh destination: the connection,
    // which ssh can make, and the attach call wait off the GUI thread, which
    // goes on drawing the other windows meanwhile.
    SessionAttach job =
        sessionAttach(m_settings.settings(), session, host, options.value(QStringLiteral("takeover"), true).toBool());
    const quint64 id = ++m_lastSessionLaunch;
    m_sessionLaunches.insert(id, SessionLaunch{window, options, std::move(done), session, host, {}});
    try {
        std::thread([counted = MuxClient::CallThread(), mailbox = m_mailbox, id, job = std::move(job)]() mutable {
            // Counted to the end of the body: a quit waits for it
            // (MuxClient::waitForCallThreads), before the statics go.
            const MuxClient::CallThread running = std::move(counted);
            auto attached = std::make_shared<SessionAttached>(attachOnAnyThread(job));
            const std::lock_guard lock(mailbox->mutex);
            if (!mailbox->application)
                return;
            QMetaObject::invokeMethod(
                mailbox->application,
                [application = mailbox->application, id, attached] {
                    application->sessionAttached(id, *attached);
                },
                Qt::QueuedConnection);
        }).detach();
    } catch (const std::system_error& e) {
        m_sessionLaunches.take(id).done(QStringLiteral("cannot start the attach of session '%1': %2")
                                            .arg(session, QString::fromLocal8Bit(e.what())));
    }
}

void Application::sessionAttached(quint64 id, SessionAttached& attached)
{
    SessionLaunch launch = m_sessionLaunches.take(id);
    if (!launch.done)
        return;
    const QString error = showAttached(launch.session, attached, std::move(launch.window), launch.options);
    launch.done(error);
    const auto takesOver = [](const QVariantMap& options) {
        return options.value(QStringLiteral("takeover"), true).toBool();
    };
    for (auto& [workingDirectory, command, options, done] : launch.joined) {
        if (!error.isEmpty()) {
            // An attach without the takeover that this launch asks for can
            // fail where this launch's own would not (another window shows
            // the session): it tries again on its own.
            if (takesOver(options) && !takesOver(launch.options)) {
                openLaunch(workingDirectory, command, options, std::move(done));
            } else {
                done(error);
            }
            continue;
        }
        // Each launcher's token lets the window take the focus for it. The
        // window can be gone again: its program ended at once.
        TerminalWindow* shown = sessionWindow(launch.session, launch.host);
        if (shown)
            raiseForLaunch(shown, options);
        done(shown ? QString() : QStringLiteral("cannot show session '%1'").arg(launch.session));
    }
}

TerminalWindow* Application::sessionWindow(const QString& session, const QString& host)
{
    // Panes of tabs only: a closed pane waits for its delete as a child too.
    for (TerminalWindow* shown : TerminalWindow::windows()) {
        if (shown->closing())
            continue;
        const QList<Pane*> panes = shown->panes();
        if (std::any_of(panes.cbegin(), panes.cend(), [&](const Pane* pane) {
                return pane->showsSession(session, host);
            }))
            return shown;
    }
    return nullptr;
}

void Application::raiseForLaunch(TerminalWindow* window, const QVariantMap& options)
{
    withActivationToken(options, [window] {
        if (!window->isVisible())
            window->show();
        window->raise();
        window->requestActivate();
    });
}

QString Application::showAttached(const QString& session, SessionAttached& attached, WindowOptions window,
                                  const QVariantMap& options)
{
    if (!attached.link)
        return attached.error;
    QString error;
    window.session = MuxClient::adopt(session, std::move(*attached.link), &error, m_settings.settings().mux.eventQueue);
    if (!window.session)
        return error;
    return showLaunch(window, options);
}

QString Application::showLaunch(const WindowOptions& window, const QVariantMap& options)
{
    TerminalWindow* w = newWindow(window);
    if (!w && window.session)
        return QStringLiteral("cannot show session '%1'").arg(window.session->session());
    if (!w)
        return QStringLiteral("cannot start %1")
            .arg(window.command.isEmpty() ? QStringLiteral("the shell") : window.command.first());
    withActivationToken(options, [w] {
        w->requestActivate();
    });
    return {};
}

QString Application::toggleQuickTerminal()
{
    if (!m_quick) {
        m_quick = newWindow({{}, {}, true, false, {}, false});
        if (!m_quick)
            return QStringLiteral("cannot start the shell of the quick terminal");
    }
    m_quick->toggleQuick();
    return {};
}

void Application::quitWhenNoWindowsRemain()
{
    if (TerminalWindow::windows().isEmpty()) {
        QGuiApplication::quit();
    }
}

LaunchApi::LaunchApi(Application& application)
    : m_application(application)
{
}

void LaunchApi::newWindow(const QString& workingDirectory, const QStringList& command, const QVariantMap& options)
{
    // The launcher waits for the window, which for a session shows after
    // this returns. A pending reply goes on the GUI thread, as the
    // application's answers do.
    auto reply = std::make_shared<PhosphorIpc::IpcPendingReply>(deferReply());
    // Past the router's limit of waiting calls, the router fails the call,
    // and the launcher reports that failure: no window opens for it. A call
    // from no connection has nobody to answer, and opens its window.
    if (!reply->isValid() && connectionId() != 0)
        return;
    m_application.openLaunch(workingDirectory, command, options, [reply](const QString& error) {
        if (error.isEmpty())
            reply->finish();
        else
            reply->fail(error);
    });
}

void LaunchApi::toggleQuickTerminal()
{
    if (const QString error = m_application.toggleQuickTerminal(); !error.isEmpty()) {
        sendErrorReply(error);
    }
}

} // namespace Afterglow
